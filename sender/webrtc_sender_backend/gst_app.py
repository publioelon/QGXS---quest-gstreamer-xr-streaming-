from __future__ import annotations

import threading
import time
from pathlib import Path
from typing import Any, Optional

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstSdp", "1.0")
gi.require_version("GstWebRTC", "1.0")

from gi.repository import GLib, Gst, GstSdp, GstWebRTC

from .config import (
    SenderConfig,
    codec_label,
    is_image_folder_mode,
    list_supported_images,
)
from .pipeline_builder import build_sender_pipeline_description
from .signaling import SignalingClient, base64_decode_to_string, base64_encode


def initialize_gstreamer(argv: list[str]) -> None:
    Gst.init(argv)


class WebRTCSender:
    def __init__(self, config: SenderConfig):
        self.config = config
        self.pipeline: Optional[Gst.Element] = None
        self.webrtc: Optional[Gst.Element] = None
        self.main_loop: Optional[GLib.MainLoop] = None
        self.frame_source: Optional[Gst.Element] = None
        self.image_files: list[Path] = []
        self.image_index: int = 0
        self.frame_number: int = 0
        self.frame_duration_ns: int = 0
        self.image_loader: Optional[Any] = None
        self.loop_image_folder: bool = True
        self.running = threading.Event()
        self.running.set()
        self.signaling = SignalingClient(self.running)
        self.signaling_thread: Optional[threading.Thread] = None
        self.loop_watchdog_id: Optional[int] = None
        self.loop_seek_margin_ns: int = int(0.75 * Gst.SECOND)
        self.loop_rearm_position_ns: int = int(3.0 * Gst.SECOND)
        self.loop_seek_in_progress: bool = False
        self.loop_armed: bool = True
        self.last_loop_seek_wall_time: float = 0.0
        self.loop_seek_cooldown_s: float = 2.0

    def on_bus_message(self, bus: Gst.Bus, message: Gst.Message) -> bool:
        src_name = message.src.get_name() if message.src else "unknown"

        if message.type == Gst.MessageType.ERROR:
            err, debug = message.parse_error()
            print(f"\n[ERROR from {src_name}] {err.message if err else 'unknown'}")
            if debug:
                print(f"[DEBUG] {debug}")
            if self.main_loop is not None:
                self.main_loop.quit()

        elif message.type == Gst.MessageType.WARNING:
            err, debug = message.parse_warning()
            print(f"\n[WARNING from {src_name}] {err.message if err else 'unknown'}")
            if debug:
                print(f"[DEBUG] {debug}")

        elif message.type == Gst.MessageType.EOS:
            print("[bus] EOS received.")
            if is_image_folder_mode(self.config):
                print("[bus] EOS received from image-folder mode.")
                if self.loop_image_folder:
                    print("[loop] Image-folder mode is configured to loop; keeping sender alive.")
                    return True
                print("[loop] Image-folder looping is disabled. Stopping sender.")
                if self.main_loop is not None:
                    self.main_loop.quit()
                return True

            if self.loop_video_file_from_beginning("eos"):
                return True

            print("[loop] Could not loop video-file pipeline. Stopping sender.")
            if self.main_loop is not None:
                self.main_loop.quit()

        return True

    def add_bus_watch(self, pipeline: Gst.Element) -> None:
        bus = pipeline.get_bus()
        if bus is None:
            print("[bus] Could not get pipeline bus.")
            return
        bus.add_signal_watch()
        bus.connect("message", self.on_bus_message)

    def loop_video_file_from_beginning(self, reason: str = "manual") -> bool:
        if self.pipeline is None:
            print("[loop] Pipeline is missing; cannot seek.")
            return False

        print(f"[loop] Seeking video-file pipeline back to the beginning. reason={reason}")

        flags = Gst.SeekFlags.FLUSH | Gst.SeekFlags.KEY_UNIT

        try:
            ok = self.pipeline.seek_simple(Gst.Format.TIME, flags, 0)
        except Exception as exc:
            print(f"[loop] seek_simple raised an exception: {exc}")
            ok = False

        if ok:
            self.loop_armed = False
            print("[loop] Video seeked back to the beginning with seek_simple().")
            return True

        print("[loop] seek_simple() failed. Trying full flushing seek...")

        try:
            ok = self.pipeline.seek(
                1.0,
                Gst.Format.TIME,
                flags,
                Gst.SeekType.SET,
                0,
                Gst.SeekType.NONE,
                Gst.CLOCK_TIME_NONE,
            )
        except Exception as exc:
            print(f"[loop] full seek raised an exception: {exc}")
            ok = False

        if ok:
            self.loop_armed = False
            print("[loop] Video seeked back to the beginning with full seek().")
            return True

        print("[loop] Full seek failed.")
        return False

    def loop_watchdog_tick(self) -> bool:
        if not self.running.is_set():
            return False

        if self.pipeline is None:
            return True

        if is_image_folder_mode(self.config):
            return True

        try:
            ok_pos, position_ns = self.pipeline.query_position(Gst.Format.TIME)
            ok_dur, duration_ns = self.pipeline.query_duration(Gst.Format.TIME)
        except Exception as exc:
            print(f"[loop-watchdog] Could not query position/duration: {exc}")
            return True

        if not ok_pos or not ok_dur:
            return True

        if duration_ns <= 0 or position_ns < 0:
            return True

        if not self.loop_armed:
            if position_ns <= self.loop_rearm_position_ns:
                self.loop_armed = True
                print(f"[loop-watchdog] Loop rearmed at position={position_ns / Gst.SECOND:.3f}s.")
            return True

        remaining_ns = duration_ns - position_ns

        if remaining_ns > self.loop_seek_margin_ns:
            return True

        if self.loop_seek_in_progress:
            return True

        now = time.monotonic()

        if now - self.last_loop_seek_wall_time < self.loop_seek_cooldown_s:
            return True

        self.last_loop_seek_wall_time = now
        self.loop_seek_in_progress = True

        print(
            "[loop-watchdog] Near end of video. "
            f"position={position_ns / Gst.SECOND:.3f}s "
            f"duration={duration_ns / Gst.SECOND:.3f}s "
            f"remaining={remaining_ns / Gst.SECOND:.3f}s. Looping..."
        )

        ok = self.loop_video_file_from_beginning("watchdog")

        if ok:
            print("[loop-watchdog] Loop seek succeeded.")
        else:
            print("[loop-watchdog] Loop seek failed.")

        self.loop_seek_in_progress = False
        return True

    def on_ice_candidate(self, webrtc: Gst.Element, mline_index: int, candidate: str) -> None:
        if not candidate:
            return
        line = f"ICE|{mline_index}|{base64_encode(candidate)}"
        self.signaling.send_line(line)
        print("[sender] ICE candidate sent to Unity receiver.")

    def on_offer_created(self, promise: Gst.Promise, *args) -> None:
        reply = promise.get_reply()

        if reply is None:
            print("[sender] Failed to create SDP offer: empty promise reply.")
            return

        try:
            offer = reply.get_value("offer")
        except Exception:
            offer = None

        if offer is None:
            print("[sender] Failed to create SDP offer.")
            return

        print("[sender] Created SDP offer.")

        if self.webrtc is None:
            print("[sender] webrtc element is missing.")
            return

        self.webrtc.emit("set-local-description", offer, None)

        try:
            sdp_text = offer.sdp.as_text()
        except Exception:
            sdp_text = None

        if sdp_text:
            encoded_offer = base64_encode(sdp_text)
            self.signaling.send_line("OFFER|" + encoded_offer)
            print("[sender] SDP offer sent to Unity receiver.")
        else:
            print("[sender] Could not convert SDP offer to text.")

    def on_negotiation_needed(self, webrtc: Gst.Element) -> None:
        print("[sender] Negotiation needed. Creating SDP offer...")

        if self.webrtc is None:
            print("[sender] webrtc element is missing.")
            return

        promise = Gst.Promise.new_with_change_func(self.on_offer_created, None, None)
        self.webrtc.emit("create-offer", None, promise)

    def handle_answer_message(self, encoded_answer: str) -> bool:
        sdp_text = base64_decode_to_string(encoded_answer)

        if sdp_text is None:
            print("[signaling] Could not decode SDP answer.")
            return False

        result, sdp = GstSdp.SDPMessage.new()

        if result != GstSdp.SDPResult.OK:
            print("[signaling] Could not allocate SDP answer message.")
            return False

        parse_result = GstSdp.sdp_message_parse_buffer(sdp_text.encode("utf-8"), sdp)

        if parse_result != GstSdp.SDPResult.OK:
            print("[signaling] Could not parse SDP answer.")
            return False

        answer = GstWebRTC.WebRTCSessionDescription.new(
            GstWebRTC.WebRTCSDPType.ANSWER,
            sdp,
        )

        if answer is None:
            print("[signaling] Could not create WebRTC answer description.")
            return False

        if self.webrtc is None:
            print("[signaling] webrtc element is missing.")
            return False

        self.webrtc.emit("set-remote-description", answer, None)

        print("[signaling] SDP answer received and applied.")
        return False

    def handle_ice_message(self, mline_text: str, encoded_candidate: str) -> bool:
        candidate = base64_decode_to_string(encoded_candidate)

        if candidate is None:
            print("[signaling] Could not decode ICE candidate.")
            return False

        try:
            mline_index = int(mline_text)
        except ValueError:
            print("[signaling] Invalid ICE mline index.")
            return False

        if self.webrtc is None:
            print("[signaling] webrtc element is missing.")
            return False

        self.webrtc.emit("add-ice-candidate", mline_index, candidate)
        print("[signaling] ICE candidate received from Unity receiver.")
        return False

    def handle_signaling_line(self, line: str) -> None:
        if line.startswith("ANSWER|"):
            encoded_answer = line[7:]
            GLib.idle_add(self.handle_answer_message, encoded_answer)
            return

        if line.startswith("ICE|"):
            parts = line.split("|", 2)

            if len(parts) != 3:
                print("[signaling] Malformed ICE message.")
                return

            _, mline, candidate = parts
            GLib.idle_add(self.handle_ice_message, mline, candidate)
            return

        print(f"[signaling] Unknown message from Unity: {line}")

    def signaling_read_thread(self) -> None:
        while self.running.is_set():
            line = self.signaling.recv_line()

            if line is None:
                print("[signaling] Signaling connection closed. Keeping WebRTC media pipeline alive.")
                break

            if line:
                self.handle_signaling_line(line)

    def setup_image_folder_source(self) -> bool:
        if self.pipeline is None:
            print("[image-folder] Pipeline is missing.")
            return False

        self.frame_source = self.pipeline.get_by_name("frame_source")

        if self.frame_source is None:
            print("[image-folder] Could not get appsrc element named frame_source.")
            return False

        self.image_files = list_supported_images(
            self.config.input_path,
            self.config.image_format,
        )

        if not self.image_files:
            print("[image-folder] No supported image files found.")
            print(f"[image-folder] Folder: {self.config.input_path}")
            print("[image-folder] Supported: .jpg, .jpeg, .png, .webp, .avif")
            return False

        try:
            try:
                import pillow_avif
            except ImportError:
                pass

            from PIL import Image
            self.image_loader = Image
        except ImportError:
            print("[image-folder] Pillow is not installed in the selected Python environment.")
            print("[image-folder] Install it with:")
            print("               conda install -n gstwebrtc -c conda-forge pillow pillow-avif-plugin -y")
            return False

        self.image_index = 0
        self.frame_number = 0
        self.frame_duration_ns = int(Gst.SECOND // max(1, self.config.fps))
        self.loop_image_folder = bool(getattr(self.config, "loop", True))

        caps = Gst.Caps.from_string(
            f"video/x-raw,format=RGB,"
            f"width={self.config.width},"
            f"height={self.config.height},"
            f"framerate={self.config.fps}/1"
        )

        self.frame_source.set_property("caps", caps)
        self.frame_source.set_property("is-live", True)
        self.frame_source.set_property("format", Gst.Format.TIME)
        self.frame_source.set_property("do-timestamp", False)
        self.frame_source.set_property("block", True)

        self.frame_source.connect("need-data", self.on_appsrc_need_data)

        print(
            f"[image-folder] Prepared {len(self.image_files)} image frame(s) "
            f"from {self.config.input_path}"
        )
        print(
            f"[image-folder] Streaming as RGB "
            f"{self.config.width}x{self.config.height} @ {self.config.fps} FPS"
        )

        return True

    def on_appsrc_need_data(self, appsrc: Gst.Element, length: int) -> None:
        if not self.running.is_set():
            return

        if not self.image_files:
            print("[image-folder] No image files available for appsrc.")
            appsrc.emit("end-of-stream")
            return

        if self.image_index >= len(self.image_files):
            if self.loop_image_folder:
                self.image_index = 0
            else:
                print("[image-folder] End of image sequence.")
                appsrc.emit("end-of-stream")
                return

        image_path = self.image_files[self.image_index]
        self.image_index += 1

        try:
            frame_bytes = self.load_image_as_rgb_bytes(image_path)
        except Exception as exc:
            print(f"[image-folder] Failed to load image: {image_path}")
            print(f"[image-folder] Error: {exc}")

            if self.main_loop is not None:
                self.main_loop.quit()
            return

        buffer = Gst.Buffer.new_allocate(None, len(frame_bytes), None)
        buffer.fill(0, frame_bytes)

        pts = self.frame_number * self.frame_duration_ns
        buffer.pts = pts
        buffer.dts = Gst.CLOCK_TIME_NONE
        buffer.duration = self.frame_duration_ns
        buffer.offset = self.frame_number
        buffer.offset_end = self.frame_number + 1

        self.frame_number += 1

        ret = appsrc.emit("push-buffer", buffer)

        if ret != Gst.FlowReturn.OK:
            print(f"[image-folder] appsrc push-buffer returned: {ret}")
            if self.main_loop is not None:
                self.main_loop.quit()

    def load_image_as_rgb_bytes(self, image_path: Path) -> bytes:
        if self.image_loader is None:
            raise RuntimeError("Pillow image loader is not initialized.")

        Image = self.image_loader

        with Image.open(image_path) as img:
            if img.mode != "RGB":
                img = img.convert("RGB")

            target_size = (self.config.width, self.config.height)

            if img.size != target_size:
                try:
                    resample = Image.Resampling.LANCZOS
                except AttributeError:
                    resample = Image.LANCZOS

                img = img.resize(target_size, resample=resample)

            return img.tobytes()

    def start_sender_pipeline(self) -> bool:
        desc = build_sender_pipeline_description(self.config)

        print(
            f"[sender] Starting GStreamer WebRTC "
            f"{codec_label(self.config.codec)} sender pipeline:\n{desc}"
        )

        try:
            self.pipeline = Gst.parse_launch(desc)
        except GLib.Error as exc:
            print(f"[sender] Pipeline parse error: {exc.message}")
            return False

        self.webrtc = self.pipeline.get_by_name("sender_webrtc") if self.pipeline else None

        if not self.webrtc:
            print("[sender] Could not get sender_webrtc element.")
            return False

        if is_image_folder_mode(self.config):
            if not self.setup_image_folder_source():
                return False

        self.webrtc.connect("on-negotiation-needed", self.on_negotiation_needed)
        self.webrtc.connect("on-ice-candidate", self.on_ice_candidate)

        self.add_bus_watch(self.pipeline)

        ret = self.pipeline.set_state(Gst.State.PLAYING)

        if ret == Gst.StateChangeReturn.FAILURE:
            print("[sender] Failed to set sender pipeline to PLAYING.")
            return False

        if not is_image_folder_mode(self.config):
            self.loop_watchdog_id = GLib.timeout_add(250, self.loop_watchdog_tick)
            print("[loop-watchdog] Enabled active video-file loop watchdog.")

        return True

    def cleanup(self) -> None:
        self.running.clear()

        if self.loop_watchdog_id is not None:
            try:
                GLib.source_remove(self.loop_watchdog_id)
            except Exception:
                pass
            self.loop_watchdog_id = None

        if self.pipeline is not None:
            self.pipeline.set_state(Gst.State.NULL)

        self.frame_source = None
        self.webrtc = None
        self.pipeline = None

        self.signaling.close()
        self.main_loop = None

    def run(self) -> int:
        if not self.signaling.connect(self.config.host, self.config.port):
            self.cleanup()
            return 1

        self.main_loop = GLib.MainLoop()

        self.signaling_thread = threading.Thread(
            target=self.signaling_read_thread,
            name="SignalingReadThread",
            daemon=False,
        )
        self.signaling_thread.start()

        if not self.start_sender_pipeline():
            self.running.clear()
            self.signaling.shutdown_to_unblock()

            if self.signaling_thread.is_alive():
                self.signaling_thread.join()

            self.cleanup()
            return 1

        print("[sender] Running. Unity should receive and display the stream.")

        try:
            self.main_loop.run()
        except KeyboardInterrupt:
            print("\n[sender] Keyboard interrupt received.")

        print("[sender] Stopping...")

        self.running.clear()
        self.signaling.shutdown_to_unblock()

        if self.signaling_thread.is_alive():
            self.signaling_thread.join()

        self.cleanup()
        return 0
