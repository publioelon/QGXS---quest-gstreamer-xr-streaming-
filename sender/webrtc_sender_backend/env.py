from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path
from typing import Optional


# ============================================================
# GStreamer runtime detection
# ============================================================

def _path_exists(path: Path) -> bool:
    try:
        return path.exists()
    except OSError:
        return False


def _is_valid_gstreamer_root(path: Path) -> bool:
    """
    A valid GStreamer root usually contains:

        bin/
        lib/gstreamer-1.0/

    On Windows, bin usually contains gst-launch-1.0.exe.
    On Linux/macOS, it may contain gst-launch-1.0.
    """
    try:
        if not path.is_dir():
            return False

        bin_dir = path / "bin"
        plugin_dir = path / "lib" / "gstreamer-1.0"

        gst_launch_win = bin_dir / "gst-launch-1.0.exe"
        gst_launch_unix = bin_dir / "gst-launch-1.0"

        return (
            bin_dir.is_dir()
            and plugin_dir.is_dir()
            and (gst_launch_win.is_file() or gst_launch_unix.is_file())
        )
    except OSError:
        return False


def _project_root_from_this_file() -> Path:
    """
    This file is expected to be located at:

        Sender/webrtc_sender_backend/env.py

    Therefore:
        parents[0] -> webrtc_sender_backend
        parents[1] -> Sender
        parents[2] -> launcher/project root

    If the folder structure changes, this still safely falls back to the
    current file parent.
    """
    here = Path(__file__).resolve()

    try:
        return here.parents[2]
    except IndexError:
        return here.parent


def _candidate_roots_from_environment() -> list[Path]:
    candidates: list[Path] = []

    env_names = [
        "GST_ROOT",
        "GSTREAMER_ROOT",
        "GSTREAMER_ROOT_X86_64",
        "GSTREAMER_1_0_ROOT_X86_64",
        "GSTREAMER_1_0_ROOT_MSVC_X86_64",
        "GSTREAMER_1_0_ROOT_MINGW_X86_64",
    ]

    for name in env_names:
        value = os.environ.get(name, "").strip()

        if value:
            candidates.append(Path(value))

    return candidates


def _candidate_roots_from_program_files() -> list[Path]:
    """
    Windows-only convenience search.

    This does not hardcode a personal/developer path. It uses the current
    user's ProgramFiles environment variables.
    """
    candidates: list[Path] = []

    for env_name in ("ProgramFiles", "ProgramFiles(x86)"):
        root_text = os.environ.get(env_name, "").strip()

        if not root_text:
            continue

        root = Path(root_text)

        candidates.extend(
            [
                root / "gstreamer" / "1.0" / "msvc_x86_64",
                root / "GStreamer" / "1.0" / "msvc_x86_64",
                root / "gstreamer" / "1.0" / "mingw_x86_64",
                root / "GStreamer" / "1.0" / "mingw_x86_64",
            ]
        )

    return candidates


def _candidate_roots_from_path() -> list[Path]:
    candidates: list[Path] = []

    for exe_name in ("gst-launch-1.0.exe", "gst-launch-1.0"):
        found = shutil.which(exe_name)

        if not found:
            continue

        exe_path = Path(found)

        # Typical layout:
        #   <gst_root>/bin/gst-launch-1.0.exe
        if exe_path.parent.name.lower() == "bin":
            candidates.append(exe_path.parent.parent)

    return candidates


def _candidate_roots_from_project(project_root: Path) -> list[Path]:
    """
    Portable/bundled layouts.

    These allow you to distribute the launcher with a GStreamer runtime next
    to the project without requiring absolute paths.
    """
    return [
        project_root / "Runtime" / "GStreamer",
        project_root / "runtime" / "gstreamer",
        project_root / "GStreamer",
        project_root / "gstreamer",
    ]


def find_gstreamer_root() -> Optional[Path]:
    project_root = _project_root_from_this_file()

    candidates: list[Path] = []
    candidates.extend(_candidate_roots_from_project(project_root))
    candidates.extend(_candidate_roots_from_environment())
    candidates.extend(_candidate_roots_from_path())
    candidates.extend(_candidate_roots_from_program_files())

    seen: set[str] = set()

    for candidate in candidates:
        try:
            resolved = candidate.resolve()
        except OSError:
            resolved = candidate

        key = str(resolved).lower()

        if key in seen:
            continue

        seen.add(key)

        if _is_valid_gstreamer_root(resolved):
            return resolved

    return None


# ============================================================
# Environment modification helpers
# ============================================================

def prepend_path_if_possible(path_to_add: Path) -> None:
    if not _path_exists(path_to_add):
        return

    path_text = str(path_to_add)
    current_path = os.environ.get("PATH", "")

    parts = [part for part in current_path.split(os.pathsep) if part]
    lower_parts = {part.lower() for part in parts}

    if path_text.lower() not in lower_parts:
        os.environ["PATH"] = path_text + (os.pathsep + current_path if current_path else "")

    if os.name == "nt" and hasattr(os, "add_dll_directory"):
        try:
            os.add_dll_directory(path_text)
        except OSError:
            pass


def prepend_env_path_if_possible(name: str, path_to_add: Path) -> None:
    if not _path_exists(path_to_add):
        return

    path_text = str(path_to_add)
    current = os.environ.get(name, "")

    parts = [part for part in current.split(os.pathsep) if part]
    lower_parts = {part.lower() for part in parts}

    if path_text.lower() not in lower_parts:
        os.environ[name] = path_text + (os.pathsep + current if current else "")


def set_env_if_missing(name: str, value: str | Path) -> None:
    if not os.environ.get(name):
        os.environ[name] = str(value)


def set_env(name: str, value: str | Path) -> None:
    os.environ[name] = str(value)


# ============================================================
# Optional Python binding path support
# ============================================================

def add_possible_python_binding_paths(gst_root: Path) -> None:
    """
    Optional support for GStreamer-provided Python bindings.

    By default, this function does nothing because mixing GStreamer's own
    lib/site-packages with a Conda/venv PyGObject installation can cause gi/_gi
    import conflicts.

    To explicitly enable this behavior, set:

        GST_ADD_RUNTIME_PYTHON_BINDINGS=1

    before running the sender.
    """
    if os.environ.get("GST_ADD_RUNTIME_PYTHON_BINDINGS", "").strip() != "1":
        return

    candidates = [
        gst_root / "lib" / "site-packages",
        gst_root / "lib" / "python3" / "site-packages",
    ]

    lib_dir = gst_root / "lib"

    if lib_dir.is_dir():
        try:
            for item in lib_dir.iterdir():
                if item.is_dir() and item.name.lower().startswith("python"):
                    candidates.append(item / "site-packages")
        except OSError:
            pass

    for candidate in candidates:
        if (candidate / "gi" / "__init__.py").is_file():
            candidate_text = str(candidate)

            if candidate_text not in sys.path:
                sys.path.insert(0, candidate_text)


# ============================================================
# Main setup function
# ============================================================

def configure_gstreamer_environment() -> None:
    """
    Configure the GStreamer runtime without hardcoding personal machine paths.

    Detection order:
        1. Project-local runtime folders, such as Runtime/GStreamer
        2. Environment variables, such as GST_ROOT
        3. Existing PATH entry containing gst-launch-1.0
        4. Windows ProgramFiles-based common install folders

    If no runtime is found, this function returns without raising. The later
    GStreamer/PyGObject import step will then show the actual missing dependency
    error.
    """
    gst_root_text = os.environ.get("GST_ROOT", "").strip()

    if gst_root_text:
        gst_root = Path(gst_root_text)
    else:
        detected = find_gstreamer_root()

        if detected is None:
            set_env_if_missing("GST_DEBUG_NO_COLOR", "1")
            return

        gst_root = detected
        set_env("GST_ROOT", gst_root)

    if not _is_valid_gstreamer_root(gst_root):
        set_env_if_missing("GST_DEBUG_NO_COLOR", "1")
        return

    bin_path = gst_root / "bin"
    plugin_path = gst_root / "lib" / "gstreamer-1.0"

    if os.name == "nt":
        scanner_path = gst_root / "libexec" / "gstreamer-1.0" / "gst-plugin-scanner.exe"
    else:
        scanner_path = gst_root / "libexec" / "gstreamer-1.0" / "gst-plugin-scanner"

    typelib_path = gst_root / "lib" / "girepository-1.0"

    prepend_path_if_possible(bin_path)
    add_possible_python_binding_paths(gst_root)

    if plugin_path.exists():
        set_env_if_missing("GST_PLUGIN_PATH", plugin_path)
        set_env_if_missing("GST_PLUGIN_SYSTEM_PATH_1_0", plugin_path)

    if scanner_path.is_file():
        set_env_if_missing("GST_PLUGIN_SCANNER", scanner_path)

    if typelib_path.exists():
        prepend_env_path_if_possible("GI_TYPELIB_PATH", typelib_path)

    set_env_if_missing("GST_DEBUG_NO_COLOR", "1")

    # Local registry cache next to the project. This avoids using a stale global
    # registry cache if the user has multiple GStreamer installations.
    project_root = _project_root_from_this_file()
    set_env_if_missing("GST_REGISTRY", project_root / "gst-registry-python-sender.bin")