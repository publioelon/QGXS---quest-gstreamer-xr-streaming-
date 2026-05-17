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


def escape_gst_string(value: str) -> str:
    return str(value).replace("\\", "\\\\").replace('"', '\\"')


def _caps_video_raw(config: SenderConfig) -> str:
    return (
        f"video/x-raw,width={config.width},height={config.height},"
        f"framerate={config.fps}/1"
    )


def build_video_file_source_fragment(config: SenderConfig) -> str:
    gst_path = escape_gst_string(normalize_path_for_gstreamer(config.input_path))

    parts: list[str] = []

    parts.append(f'filesrc location="{gst_path}"')
    parts.append("! qtdemux name=demux demux.video_0")
    parts.append("! queue max-size-buffers=4 max-size-bytes=0 max-size-time=0")
    parts.append("! decodebin")

    return " ".join(parts)


def build_image_folder_source_fragment(config: SenderConfig) -> str:
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


def build_common_processing_fragment(config: SenderConfig) -> str:
    parts: list[str] = []

    parts.append("! queue max-size-buffers=4 max-size-bytes=0 max-size-time=0")
    parts.append("! videoconvert")
    parts.append("! videoscale")
    parts.append("! videorate")
    parts.append(f"! {_caps_video_raw(config)}")
    parts.append("! identity name=loop_identity sync=true single-segment=true")
    parts.append("! videoconvert")

    return " ".join(parts)


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
    parts.append("! rtph264pay pt=96 config-interval=1 mtu=1200")
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
    parts.append("! rtph265pay pt=96 config-interval=1 mtu=1200")
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
    parts.append("! rtpav1pay pt=96 mtu=1200")
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


def build_sender_pipeline_description(config: SenderConfig) -> str:
    parts: list[str] = []

    parts.append("webrtcbin name=sender_webrtc bundle-policy=max-bundle latency=100")
    parts.append(build_source_fragment(config))
    parts.append(build_common_processing_fragment(config))
    parts.append(build_codec_fragment(config))
    parts.append("! sender_webrtc.")

    return " ".join(parts)
