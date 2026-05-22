from __future__ import annotations

from .config import (
    SenderConfig,
    gop_size,
    is_av1,
    is_h264,
    is_image_folder_mode,
    is_video_file_mode,
    normalize_path_for_gstreamer,
)


# ============================================================
# Generic helpers
# ============================================================

def escape_gst_string(value: str) -> str:
    """
    Escape a string used inside a quoted gst_parse_launch property.
    """
    return str(value).replace("\\", "\\\\").replace('"', '\\"')


def _caps_video_raw(config: SenderConfig) -> str:
    return (
        f"video/x-raw,width={config.width},height={config.height},"
        f"framerate={config.fps}/1"
    )


# ============================================================
# Source builders
# ============================================================

def build_video_file_source_fragment(config: SenderConfig) -> str:
    """
    Build the source section for normal video files such as MP4/MOV/MKV/AVI/WEBM.

    Current behavior:
        filesrc -> qtdemux -> decodebin

    Note:
        qtdemux is mainly for MP4/MOV-style inputs. If later full MKV/AVI/WEBM
        autodetection is needed, this can be changed to uridecodebin.
    """
    gst_path = escape_gst_string(normalize_path_for_gstreamer(config.input_path))

    parts: list[str] = []

    parts.append(f'filesrc location="{gst_path}"')
    parts.append("! qtdemux name=demux demux.video_0")
    parts.append("! queue max-size-buffers=4 max-size-bytes=0 max-size-time=0")
    parts.append("! decodebin")

    return " ".join(parts)


def build_image_folder_source_fragment(config: SenderConfig) -> str:
    """
    Build the source section for arbitrary image folders.

    The actual image loading/decoding is done in Python inside gst_app.py
    using appsrc.

    Expected raw frames pushed by gst_app.py:
        RGB
        width=config.width
        height=config.height
        framerate=config.fps/1

    Supported sorted folder inputs:
        JPG/JPEG, PNG, WEBP, AVIF
    """
    parts: list[str] = []

    parts.append(
        "appsrc name=frame_source "
        "is-live=true "
        "format=time "
        "do-timestamp=false "
        "block=true "
        f'caps="video/x-raw,format=RGB,width={config.width},height={config.height},framerate={config.fps}/1"'
    )

    parts.append("! queue max-size-buffers=4 max-size-bytes=0 max-size-time=0")

    return " ".join(parts)


def build_source_fragment(config: SenderConfig) -> str:
    if is_image_folder_mode(config):
        return build_image_folder_source_fragment(config)

    if is_video_file_mode(config):
        return build_video_file_source_fragment(config)

    raise ValueError(f"Unsupported input mode: {config.input_mode}")


# ============================================================
# Common raw-video processing
# ============================================================

def build_common_processing_fragment(config: SenderConfig) -> str:
    """
    Normalize the decoded/raw stream before encoding.

    For video-file mode:
        The source produces decoded raw frames from decodebin.

    For image-folder mode:
        appsrc already pushes RGB raw frames, but this stage still guarantees
        the requested output size/framerate before encoding.

    Important:
        The identity element is intentionally named loop_guard. gst_app.py uses
        this element's src pad to intercept EOS before it reaches encoder/WebRTC,
        then seeks back to the beginning when loop playback is enabled.
    """
    parts: list[str] = []

    parts.append("! queue max-size-buffers=4 max-size-bytes=0 max-size-time=0")
    parts.append("! videoconvert")
    parts.append("! videoscale")
    parts.append("! videorate")
    parts.append(f"! {_caps_video_raw(config)}")

    # Live pacing and EOS interception point.
    parts.append("! identity name=loop_guard sync=true")
    parts.append("! videoconvert")

    return " ".join(parts)


# ============================================================
# Codec / encoder builders
# ============================================================

def build_h264_fragment(config: SenderConfig) -> str:
    parts: list[str] = []

    parts.append("! video/x-raw,format=NV12")
    parts.append(
        f"! nvh264enc bitrate={config.bitrate_kbps} "
        f"gop-size={gop_size(config)} "
        "zerolatency=true "
        "bframes=0 "
        "rc-lookahead=0 "
        "tune=ultra-low-latency "
        "preset=p1"
    )
    parts.append("! h264parse config-interval=-1")
    parts.append("! rtph264pay name=rtp_pay pt=96 config-interval=1 mtu=1200")
    parts.append(
        "! application/x-rtp,media=video,encoding-name=H264,"
        "payload=96,clock-rate=90000"
    )

    return " ".join(parts)


def build_h265_fragment(config: SenderConfig) -> str:
    parts: list[str] = []

    parts.append("! video/x-raw,format=NV12")
    parts.append(
        f"! nvh265enc bitrate={config.bitrate_kbps} "
        f"gop-size={gop_size(config)} "
        "zerolatency=true "
        "bframes=0 "
        "rc-lookahead=0 "
        "tune=ultra-low-latency "
        "preset=p1"
    )
    parts.append("! h265parse config-interval=-1")
    parts.append("! rtph265pay name=rtp_pay pt=96 config-interval=1 mtu=1200")
    parts.append(
        "! application/x-rtp,media=video,encoding-name=H265,"
        "payload=96,clock-rate=90000"
    )

    return " ".join(parts)


def build_av1_fragment(config: SenderConfig) -> str:
    parts: list[str] = []

    parts.append("! video/x-raw,format=I420")
    parts.append(
        f"! svtav1enc preset=13 "
        f"target-bitrate={config.bitrate_kbps} "
        f"intra-period-length={gop_size(config)}"
    )
    parts.append("! av1parse")
    parts.append("! video/x-av1,stream-format=obu-stream,alignment=tu,parsed=true")
    parts.append("! rtpav1pay name=rtp_pay pt=96 mtu=1200")
    parts.append(
        "! application/x-rtp,media=video,encoding-name=AV1,"
        "payload=96,clock-rate=90000"
    )

    return " ".join(parts)


def build_codec_fragment(config: SenderConfig) -> str:
    if is_h264(config.codec):
        return build_h264_fragment(config)

    if is_av1(config.codec):
        return build_av1_fragment(config)

    return build_h265_fragment(config)


# ============================================================
# Full sender pipeline builder
# ============================================================

def build_sender_pipeline_description(config: SenderConfig) -> str:
    """
    Build the full GStreamer sender pipeline.

    Supported input modes:
        video-file:
            filesrc -> qtdemux -> decodebin -> loop_guard -> encoder -> RTP -> webrtcbin

        image-folder:
            appsrc -> loop_guard -> encoder -> RTP -> webrtcbin

    Supported codecs:
        h264/h265/av1
    """
    parts: list[str] = []

    parts.append("webrtcbin name=sender_webrtc bundle-policy=max-bundle latency=100")

    parts.append(build_source_fragment(config))
    parts.append(build_common_processing_fragment(config))
    parts.append(build_codec_fragment(config))
    parts.append("! identity name=rtp_counter silent=true signal-handoffs=true")

    parts.append("! sender_webrtc.")

    return " ".join(parts)
