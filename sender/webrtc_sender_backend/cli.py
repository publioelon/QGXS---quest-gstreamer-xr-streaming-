from __future__ import annotations

import sys
from typing import Optional

from .config import (
    SenderConfig,
    normalize_path_for_gstreamer,
    parse_positive_int,
    validate_runtime_parameters,
)


CODEC_NAMES = ("h264", "avc", "h265", "hevc", "av1")
INPUT_MODES = ("video-file", "image-folder")
IMAGE_FORMATS = ("auto", "jpg", "jpeg", "png", "webp", "avif")


def print_usage() -> None:
    print("\nUsage:")
    print(
        "  python webrtc_sender.py <codec> --input-mode <video-file|image-folder> "
        "--input <path> [--image-format <auto|jpg|png|webp|avif>] "
        "[--loop|--no-loop] "
        "<host> <port> [width] [height] [fps] [bitrate_kbps]\n"
    )

    print("Old video-file syntax still works:")
    print(
        "  python webrtc_sender.py <codec> <video_path> <host> <port> "
        "[width] [height] [fps] [bitrate_kbps]\n"
    )

    print("Old H.265-only syntax also still works:")
    print(
        "  python webrtc_sender.py <video_path> <host> <port> "
        "[width] [height] [fps] [bitrate_kbps]\n"
    )

    print("Codecs:")
    print("  h265 | hevc")
    print("  h264 | avc")
    print("  av1\n")

    print("Input modes:")
    print("  video-file")
    print("  image-folder\n")

    print("Image formats:")
    print("  auto | jpg | jpeg | png | webp | avif\n")

    print("Loop playback:")
    print("  --loop       Enable looping when the input reaches EOS.")
    print("  --no-loop    Disable looping and stop at EOS.\n")

    print("Examples:")
    print(
        '  python webrtc_sender.py h265 --input-mode video-file '
        '--input "./Samples/videos/sample.mp4" --loop '
        "127.0.0.1 9001 1920 960 30 8000"
    )
    print(
        '  python webrtc_sender.py h264 --input-mode video-file '
        '--input "./Samples/videos/sample.mp4" --no-loop '
        "127.0.0.1 9001 1920 960 30 8000"
    )
    print(
        '  python webrtc_sender.py av1 --input-mode video-file '
        '--input "./Samples/videos/sample.mp4" --loop '
        "127.0.0.1 9001 1920 960 30 8000"
    )
    print(
        '  python webrtc_sender.py h265 --input-mode image-folder '
        '--input "./Samples/frames" --image-format auto --loop '
        "127.0.0.1 9001 1920 960 30 8000\n"
    )

    print("Backward-compatible examples:")
    print(
        '  python webrtc_sender.py h265 "./Samples/videos/sample.mp4" '
        "127.0.0.1 9001 1920 960 30 8000"
    )
    print(
        '  python webrtc_sender.py "./Samples/videos/sample.mp4" '
        "127.0.0.1 9001\n"
    )

    print("Notes:")
    print("  Replace ./Samples/videos/sample.mp4 with your own video file path.")
    print("  Replace ./Samples/frames with your own image-folder path.")
    print("  Both relative and absolute paths are supported.")
    print("  Unity receiver width/height must match the sender width/height.")
    print("  Image-folder mode requires backend support in config.py, pipeline_builder.py, and gst_app.py.")
    print("  AV1 uses CPU encoding through svtav1enc unless GPU AV1 support is added later.")
    print("  --loop is enabled by default unless --no-loop is provided.\n")


def _ensure_loop_field(config: SenderConfig) -> None:
    if not hasattr(config, "loop"):
        config.loop = True


def _set_input_fields(
    config: SenderConfig,
    input_mode: str,
    input_path: str,
    image_format: str,
) -> None:
    input_path = normalize_path_for_gstreamer(input_path)

    config.input_mode = input_mode
    config.input_path = input_path
    config.image_format = image_format

    config.video_path = input_path


def _parse_optional_stream_values(
    config: SenderConfig,
    values: list[str],
    expected_context: str,
) -> bool:
    if len(values) < 2:
        print(
            f"Missing host/port after {expected_context}. "
            "Expected: <host> <port> [width] [height] [fps] [bitrate_kbps]",
            file=sys.stderr,
        )
        return False

    config.host = values[0]

    try:
        config.port = int(values[1])
    except ValueError:
        print(f"Invalid signaling port: {values[1]}", file=sys.stderr)
        return False

    try:
        if len(values) >= 3:
            config.width = parse_positive_int(values[2], "width")
        if len(values) >= 4:
            config.height = parse_positive_int(values[3], "height")
        if len(values) >= 5:
            config.fps = parse_positive_int(values[4], "fps")
        if len(values) >= 6:
            config.bitrate_kbps = parse_positive_int(values[5], "bitrate_kbps")
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return False

    if len(values) > 6:
        print(f"Too many arguments after {expected_context}: {values[6:]}", file=sys.stderr)
        return False

    return True


def _parse_new_flag_syntax(config: SenderConfig, args: list[str]) -> bool:
    input_mode = "video-file"
    input_path: Optional[str] = None
    image_format = "auto"

    _ensure_loop_field(config)

    remaining: list[str] = []
    i = 0

    while i < len(args):
        token = args[i]

        if token == "--input-mode":
            if i + 1 >= len(args):
                print("Missing value after --input-mode", file=sys.stderr)
                return False

            input_mode = args[i + 1].strip().lower()

            if input_mode not in INPUT_MODES:
                print(
                    f"Invalid input mode: {input_mode}. "
                    "Use video-file or image-folder.",
                    file=sys.stderr,
                )
                return False

            i += 2
            continue

        if token == "--input":
            if i + 1 >= len(args):
                print("Missing value after --input", file=sys.stderr)
                return False

            input_path = args[i + 1]
            i += 2
            continue

        if token == "--image-format":
            if i + 1 >= len(args):
                print("Missing value after --image-format", file=sys.stderr)
                return False

            image_format = args[i + 1].strip().lower()

            if image_format not in IMAGE_FORMATS:
                print(
                    f"Invalid image format: {image_format}. "
                    "Use auto, jpg, jpeg, png, webp, or avif.",
                    file=sys.stderr,
                )
                return False

            i += 2
            continue

        if token == "--loop":
            config.loop = True
            i += 1
            continue

        if token == "--no-loop":
            config.loop = False
            i += 1
            continue

        if token in ("--help", "-h", "/?"):
            print_usage()
            return False

        if token.startswith("--"):
            print(f"Unknown option: {token}", file=sys.stderr)
            return False

        remaining = args[i:]
        break

    if input_path is None:
        print("Missing --input <path>.", file=sys.stderr)
        return False

    _set_input_fields(config, input_mode, input_path, image_format)

    return _parse_optional_stream_values(
        config,
        remaining,
        expected_context="--input <path>",
    )


def _parse_old_codec_syntax(config: SenderConfig, args: list[str]) -> bool:
    _ensure_loop_field(config)

    if len(args) < 3:
        print(
            "Missing arguments. Expected: <video_path> <host> <port> "
            "[width] [height] [fps] [bitrate_kbps]",
            file=sys.stderr,
        )
        return False

    video_path = args[0]
    _set_input_fields(config, "video-file", video_path, "auto")

    return _parse_optional_stream_values(
        config,
        args[1:],
        expected_context="<video_path>",
    )


def _parse_old_no_codec_syntax(config: SenderConfig, args: list[str]) -> bool:
    _ensure_loop_field(config)
    config.codec = "h265"

    if len(args) < 3:
        print(
            "Missing arguments. Expected: <video_path> <host> <port> "
            "[width] [height] [fps] [bitrate_kbps]",
            file=sys.stderr,
        )
        return False

    video_path = args[0]
    _set_input_fields(config, "video-file", video_path, "auto")

    return _parse_optional_stream_values(
        config,
        args[1:],
        expected_context="<video_path>",
    )


def parse_arguments(argv: list[str]) -> Optional[SenderConfig]:
    config = SenderConfig()
    _ensure_loop_field(config)

    if len(argv) <= 1:
        print_usage()
        return None

    first = argv[1].strip().lower()

    if first in ("--help", "-h", "/?"):
        print_usage()
        return None

    if first in CODEC_NAMES:
        config.codec = first
        rest = argv[2:]

        if not rest:
            print_usage()
            return None

        if "--input" in rest or "--input-mode" in rest:
            ok = _parse_new_flag_syntax(config, rest)
        else:
            ok = _parse_old_codec_syntax(config, rest)

        if not ok:
            return None

    else:
        ok = _parse_old_no_codec_syntax(config, argv[1:])

        if not ok:
            return None

    config.codec = config.codec.strip().lower()

    if not hasattr(config, "input_mode"):
        config.input_mode = "video-file"

    if not hasattr(config, "input_path"):
        config.input_path = normalize_path_for_gstreamer(config.video_path)

    if not hasattr(config, "image_format"):
        config.image_format = "auto"

    _ensure_loop_field(config)

    config.input_mode = str(config.input_mode).strip().lower()
    config.input_path = normalize_path_for_gstreamer(str(config.input_path))
    config.image_format = str(config.image_format).strip().lower()
    config.video_path = config.input_path

    if not validate_runtime_parameters(config):
        return None

    return config