from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


# ============================================================
# Supported codecs / input modes / image formats
# ============================================================

CODEC_NAMES = ("h264", "avc", "h265", "hevc", "av1")

INPUT_MODE_VIDEO_FILE = "video-file"
INPUT_MODE_IMAGE_FOLDER = "image-folder"

INPUT_MODES = (
    INPUT_MODE_VIDEO_FILE,
    INPUT_MODE_IMAGE_FOLDER,
)

IMAGE_FORMAT_AUTO = "auto"

IMAGE_FORMATS = (
    "auto",
    "jpg",
    "jpeg",
    "png",
    "webp",
    "avif",
)

SUPPORTED_IMAGE_EXTS = {
    ".jpg",
    ".jpeg",
    ".png",
    ".webp",
    ".avif",
}


# ============================================================
# Runtime configuration
# ============================================================

@dataclass
class SenderConfig:
    # Codec / encoder settings.
    codec: str = "h265"

    # Generic input fields.
    #
    # input_mode:
    #   "video-file"    -> normal video input, such as MP4/MOV/MKV/AVI/WEBM
    #   "image-folder"  -> folder containing JPG/PNG/WEBP/AVIF images
    #
    # input_path:
    #   path to the video file or image folder.
    #
    # Important:
    #   This default is intentionally empty.
    #   The real input path should come from:
    #       - launcher_gui.py
    #       - config.ini generated on the user's machine
    #       - command-line arguments
    #
    # This avoids shipping the project with a hardcoded developer-machine path.
    input_mode: str = INPUT_MODE_VIDEO_FILE
    input_path: str = ""
    image_format: str = IMAGE_FORMAT_AUTO

    # Backward compatibility with the old backend.
    # Older modules may still read config.video_path.
    #
    # This is also intentionally empty and synchronized with input_path
    # inside normalize_config_paths().
    video_path: str = ""

    # Signaling / receiver settings.
    # These are network defaults, not personal file-system paths.
    host: str = "127.0.0.1"
    port: int = 9001

    # Output stream settings.
    width: int = 1920
    height: int = 960
    fps: int = 30
    bitrate_kbps: int = 8000

    # Image-folder behavior.
    # True means the image sequence repeats when it reaches the end.
    loop: bool = True


# ============================================================
# Path / parsing helpers
# ============================================================

def normalize_path_for_gstreamer(path: str) -> str:
    """
    Normalize Windows paths so GStreamer can consume them more reliably.

    Example:
        C:\\Users\\Example\\video.mp4
    becomes:
        C:/Users/Example/video.mp4
    """
    return str(path).strip().replace("\\", "/")


def parse_positive_int(text: str, label: str) -> int:
    try:
        value = int(text)
    except ValueError as exc:
        raise ValueError(f"Invalid {label}: {text}") from exc

    if value <= 0:
        raise ValueError(f"Invalid {label}: {text}")

    return value


def normalize_config_paths(config: SenderConfig) -> None:
    """
    Keep old and new input path fields synchronized.

    New code should use:
        config.input_path

    Old code may still use:
        config.video_path

    Behavior:
        - If input_path is empty but video_path exists, use video_path.
        - If video_path is empty but input_path exists, use input_path.
        - If both are empty, keep both empty and let validation fail clearly.
    """
    input_path = getattr(config, "input_path", "") or ""
    video_path = getattr(config, "video_path", "") or ""

    input_path = str(input_path).strip()
    video_path = str(video_path).strip()

    if not input_path and video_path:
        input_path = video_path

    if input_path:
        input_path = normalize_path_for_gstreamer(input_path)

    config.input_path = input_path
    config.video_path = input_path


# ============================================================
# Codec helpers
# ============================================================

def normalize_codec(codec: str) -> str:
    return str(codec).strip().lower()


def is_h265(codec: str) -> bool:
    codec = normalize_codec(codec)
    return codec in ("h265", "hevc")


def is_h264(codec: str) -> bool:
    codec = normalize_codec(codec)
    return codec in ("h264", "avc")


def is_av1(codec: str) -> bool:
    codec = normalize_codec(codec)
    return codec == "av1"


def codec_label(codec: str) -> str:
    if is_h264(codec):
        return "H.264 / AVC"

    if is_av1(codec):
        return "AV1"

    return "H.265 / HEVC"


def gop_size(config: SenderConfig) -> int:
    return config.fps if config.fps > 0 else 30


# ============================================================
# Input mode / image helpers
# ============================================================

def normalize_input_mode(input_mode: str) -> str:
    mode = str(input_mode).strip().lower()

    aliases = {
        "video": INPUT_MODE_VIDEO_FILE,
        "file": INPUT_MODE_VIDEO_FILE,
        "video_file": INPUT_MODE_VIDEO_FILE,
        "videofile": INPUT_MODE_VIDEO_FILE,
        "video-file": INPUT_MODE_VIDEO_FILE,

        "image": INPUT_MODE_IMAGE_FOLDER,
        "images": INPUT_MODE_IMAGE_FOLDER,
        "folder": INPUT_MODE_IMAGE_FOLDER,
        "image_folder": INPUT_MODE_IMAGE_FOLDER,
        "imagefolder": INPUT_MODE_IMAGE_FOLDER,
        "image-folder": INPUT_MODE_IMAGE_FOLDER,
    }

    return aliases.get(mode, mode)


def normalize_image_format(image_format: str) -> str:
    fmt = str(image_format).strip().lower()

    if fmt in ("", "*", "all"):
        return IMAGE_FORMAT_AUTO

    if fmt == "jpeg":
        return "jpeg"

    if fmt == "jpg":
        return "jpg"

    if fmt == "png":
        return "png"

    if fmt == "webp":
        return "webp"

    if fmt == "avif":
        return "avif"

    return fmt


def image_extensions_for_format(image_format: str) -> set[str]:
    fmt = normalize_image_format(image_format)

    if fmt == IMAGE_FORMAT_AUTO:
        return set(SUPPORTED_IMAGE_EXTS)

    if fmt in ("jpg", "jpeg"):
        return {".jpg", ".jpeg"}

    return {f".{fmt}"}


def list_supported_images(
    folder: str | Path,
    image_format: str = IMAGE_FORMAT_AUTO,
) -> list[Path]:
    folder_path = Path(folder)

    if not folder_path.is_dir():
        return []

    exts = image_extensions_for_format(image_format)

    images = [
        path
        for path in folder_path.iterdir()
        if path.is_file() and path.suffix.lower() in exts
    ]

    return sorted(images, key=lambda p: p.name.lower())


def is_video_file_mode(config: SenderConfig) -> bool:
    return (
        normalize_input_mode(
            getattr(config, "input_mode", INPUT_MODE_VIDEO_FILE)
        )
        == INPUT_MODE_VIDEO_FILE
    )


def is_image_folder_mode(config: SenderConfig) -> bool:
    return (
        normalize_input_mode(
            getattr(config, "input_mode", INPUT_MODE_VIDEO_FILE)
        )
        == INPUT_MODE_IMAGE_FOLDER
    )


# ============================================================
# Validation
# ============================================================

def validate_runtime_parameters(config: SenderConfig) -> bool:
    config.codec = normalize_codec(config.codec)
    config.input_mode = normalize_input_mode(
        getattr(config, "input_mode", INPUT_MODE_VIDEO_FILE)
    )
    config.image_format = normalize_image_format(
        getattr(config, "image_format", IMAGE_FORMAT_AUTO)
    )

    normalize_config_paths(config)

    if not (is_h264(config.codec) or is_h265(config.codec) or is_av1(config.codec)):
        print(f"Unsupported codec: {config.codec}")
        print("Use h264, h265, or av1.")
        return False

    if config.input_mode not in INPUT_MODES:
        print(f"Unsupported input mode: {config.input_mode}")
        print("Use video-file or image-folder.")
        return False

    if config.image_format not in IMAGE_FORMATS:
        print(f"Unsupported image format: {config.image_format}")
        print("Use auto, jpg, jpeg, png, webp, or avif.")
        return False

    if config.port <= 0 or config.port > 65535:
        print(f"Invalid signaling port: {config.port}")
        return False

    if (
        config.width <= 0
        or config.height <= 0
        or config.fps <= 0
        or config.bitrate_kbps <= 0
    ):
        print("Invalid width/height/fps/bitrate values.")
        return False

    if (config.width % 2) != 0 or (config.height % 2) != 0:
        print("Width and height must be even numbers for NV12/I420 video formats.")
        return False

    if not config.input_path:
        print("Input path is empty.")
        print("Provide a video file or image-folder path through the GUI, config.ini, or CLI.")
        return False

    input_path = Path(config.input_path)

    if is_video_file_mode(config):
        if not input_path.is_file():
            print("Video input file does not exist:")
            print(config.input_path)
            return False

    elif is_image_folder_mode(config):
        if not input_path.is_dir():
            print("Image input folder does not exist:")
            print(config.input_path)
            return False

        images = list_supported_images(input_path, config.image_format)

        if not images:
            if config.image_format == IMAGE_FORMAT_AUTO:
                print("Image input folder does not contain supported image files:")
                print(config.input_path)
                print("Supported extensions: .jpg, .jpeg, .png, .webp, .avif")
            else:
                print(f"Image input folder does not contain {config.image_format} files:")
                print(config.input_path)

            return False

    return True