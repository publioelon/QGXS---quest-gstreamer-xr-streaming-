from __future__ import annotations

import sys

from .cli import parse_arguments
from .config import codec_label
from .env import configure_gstreamer_environment


def main(argv: list[str]) -> int:
    # Important: configure the GStreamer environment before importing gi/GStreamer.
    configure_gstreamer_environment()

    from .gst_app import WebRTCSender, initialize_gstreamer

    initialize_gstreamer(argv)

    config = parse_arguments(argv)

    if config is None:
        return 1

    print("============================================================")
    print("GStreamer WebRTC CMD Sender")
    print("============================================================")
    print(f"Video path: {config.video_path}")
    print(f"Unity receiver: {config.host}:{config.port}")
    print(f"Codec: {codec_label(config.codec)}")
    print(f"Resolution: {config.width}x{config.height} @ {config.fps} FPS")
    print(f"Bitrate: {config.bitrate_kbps} kbps")
    print("GStreamer runtime prepared inside sender process.")
    print("============================================================")

    sender = WebRTCSender(config)
    return sender.run()


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
