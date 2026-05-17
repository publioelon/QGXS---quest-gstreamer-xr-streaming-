#!/usr/bin/env python3
from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path
from typing import Optional


# ============================================================
# Path / environment helpers
# ============================================================

def _path_exists(path: Path) -> bool:
    try:
        return path.exists()
    except OSError:
        return False


def _is_valid_gstreamer_root(path: Path) -> bool:
    """
    Validate a GStreamer root folder without relying on machine-specific paths.

    Expected structure:
        <gst_root>/bin
        <gst_root>/lib/gstreamer-1.0

    On Windows, gst-launch is usually:
        <gst_root>/bin/gst-launch-1.0.exe

    On Linux/macOS, gst-launch is usually:
        <gst_root>/bin/gst-launch-1.0
    """
    try:
        if not path.is_dir():
            return False

        bin_dir = path / "bin"
        plugin_dir = path / "lib" / "gstreamer-1.0"

        if not bin_dir.is_dir():
            return False

        if not plugin_dir.is_dir():
            return False

        gst_launch_win = bin_dir / "gst-launch-1.0.exe"
        gst_launch_unix = bin_dir / "gst-launch-1.0"

        # Some minimal/bundled runtimes may omit gst-launch but still work.
        # Therefore gst-launch is treated as a strong signal, not a strict rule.
        if gst_launch_win.is_file() or gst_launch_unix.is_file():
            return True

        return True

    except OSError:
        return False


def _sender_dir_from_this_file() -> Path:
    return Path(__file__).resolve().parent


def _project_root_from_this_file() -> Path:
    """
    This file is normally located at:

        <project_root>/Sender/webrtc_sender.py

    Therefore:
        parent      -> Sender
        parent.parent -> project root

    If the layout changes, the function safely falls back to the sender folder.
    """
    sender_dir = _sender_dir_from_this_file()

    if sender_dir.name.lower() == "sender":
        return sender_dir.parent

    return sender_dir


def _candidate_roots_from_project(project_root: Path, sender_dir: Path) -> list[Path]:
    """
    Portable/bundled layouts.

    These allow public distribution without absolute developer-machine paths.
    """
    return [
        project_root / "Runtime" / "GStreamer",
        project_root / "runtime" / "gstreamer",
        project_root / "GStreamer",
        project_root / "gstreamer",

        sender_dir / "Runtime" / "GStreamer",
        sender_dir / "runtime" / "gstreamer",
        sender_dir / "GStreamer",
        sender_dir / "gstreamer",
    ]


def _candidate_roots_from_environment() -> list[Path]:
    """
    Read user/system-provided environment variables.

    No personal paths are assumed here. If a user installed GStreamer elsewhere,
    they can set one of these variables.
    """
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


def _candidate_roots_from_path() -> list[Path]:
    """
    Detect GStreamer if gst-launch-1.0 is already available in PATH.
    """
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


def _candidate_roots_from_program_files() -> list[Path]:
    """
    Windows convenience search using the current machine's ProgramFiles variables.

    This does not hardcode a developer/user path. It only uses environment
    variables from the machine running the program.
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


def find_gstreamer_root() -> Optional[Path]:
    sender_dir = _sender_dir_from_this_file()
    project_root = _project_root_from_this_file()

    candidates: list[Path] = []
    candidates.extend(_candidate_roots_from_project(project_root, sender_dir))
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


def _prepend_env_path(name: str, path: Path) -> None:
    if not _path_exists(path):
        return

    path_text = str(path)
    current = os.environ.get(name, "")

    parts = [p for p in current.split(os.pathsep) if p]
    lower_parts = {p.lower() for p in parts}

    if path_text.lower() not in lower_parts:
        os.environ[name] = path_text + (os.pathsep + current if current else "")


def _add_dll_directory(path: Path) -> None:
    if os.name != "nt":
        return

    if not _path_exists(path):
        return

    if hasattr(os, "add_dll_directory"):
        try:
            os.add_dll_directory(str(path))
        except OSError:
            pass


def _set_env(name: str, value: Path | str) -> None:
    os.environ[name] = str(value)


def _set_env_if_missing(name: str, value: Path | str) -> None:
    if not os.environ.get(name):
        os.environ[name] = str(value)


def _configure_gstreamer_runtime() -> Optional[Path]:
    """
    Configure the GStreamer runtime for the Python sender.

    Detection order:
        1. Existing GST_ROOT or related environment variables
        2. Project-local runtime folders
        3. Existing PATH entry containing gst-launch-1.0
        4. Windows ProgramFiles-based common install folders

    This function intentionally avoids hardcoded developer-machine paths.
    """
    gst_root_text = os.environ.get("GST_ROOT", "").strip()

    if gst_root_text:
        gst_root = Path(gst_root_text)
    else:
        detected = find_gstreamer_root()

        if detected is None:
            _set_env_if_missing("GST_DEBUG_NO_COLOR", "1")
            return None

        gst_root = detected
        _set_env("GST_ROOT", gst_root)

    if not _is_valid_gstreamer_root(gst_root):
        _set_env_if_missing("GST_DEBUG_NO_COLOR", "1")
        return gst_root

    gst_bin = gst_root / "bin"
    gst_plugins = gst_root / "lib" / "gstreamer-1.0"

    if os.name == "nt":
        gst_scanner = gst_root / "libexec" / "gstreamer-1.0" / "gst-plugin-scanner.exe"
    else:
        gst_scanner = gst_root / "libexec" / "gstreamer-1.0" / "gst-plugin-scanner"

    gst_typelibs = gst_root / "lib" / "girepository-1.0"

    _prepend_env_path("PATH", gst_bin)
    _add_dll_directory(gst_bin)

    if gst_plugins.exists():
        _set_env_if_missing("GST_PLUGIN_PATH", gst_plugins)
        _set_env_if_missing("GST_PLUGIN_SYSTEM_PATH_1_0", gst_plugins)

    if gst_scanner.is_file():
        _set_env_if_missing("GST_PLUGIN_SCANNER", gst_scanner)

    if gst_typelibs.exists():
        _prepend_env_path("GI_TYPELIB_PATH", gst_typelibs)

    _set_env_if_missing("GST_DEBUG_NO_COLOR", "1")

    local_registry = _sender_dir_from_this_file() / "gst-registry-python-sender.bin"
    _set_env_if_missing("GST_REGISTRY", local_registry)

    return gst_root


# ============================================================
# Python binding cleanup
# ============================================================

def _remove_gstreamer_site_packages_from_syspath(gst_root: Optional[Path]) -> None:
    """
    Prevent importing gi from the GStreamer runtime's lib/site-packages.

    Preferred design:
        - PyGObject should come from the selected Python environment.
        - GStreamer runtime should provide DLLs, plugins, scanner, and typelibs.

    This avoids mixed gi/_gi imports between different Python environments.
    """
    if gst_root is None:
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

    candidate_roots: list[Path] = []

    for candidate in candidates:
        try:
            if candidate.exists():
                candidate_roots.append(candidate.resolve())
        except OSError:
            pass

    if not candidate_roots:
        return

    cleaned: list[str] = []

    for entry in sys.path:
        try:
            resolved = Path(entry).resolve()
        except OSError:
            cleaned.append(entry)
            continue

        inside_gstreamer_site_packages = any(
            resolved == root or root in resolved.parents for root in candidate_roots
        )

        if not inside_gstreamer_site_packages:
            cleaned.append(entry)

    sys.path[:] = cleaned


def _patch_backend_env_module() -> None:
    """
    Patch the modular backend env module so it does not add GStreamer's
    lib/site-packages to sys.path.

    The backend should use PyGObject from the active Python environment.
    """
    try:
        import webrtc_sender_backend.env as backend_env
    except Exception:
        return

    def _do_not_add_gstreamer_python_bindings(gst_root: Path) -> None:
        return None

    backend_env.add_possible_python_binding_paths = _do_not_add_gstreamer_python_bindings


# ============================================================
# Diagnostics
# ============================================================

def _print_python_binding_help(error: BaseException) -> None:
    print("\n[fatal] Python could not import the GStreamer Python bindings correctly.")
    print("[fatal] Error:")
    print(f"        {error}")
    print()
    print("[fatal] The sender needs PyGObject, which provides the compiled gi._gi")
    print("[fatal] module for the Python environment used to run this file.")
    print()
    print("[fatal] Conda example:")
    print("        conda install -n <your_env_name> -c conda-forge pygobject gst-python -y")
    print()
    print("[fatal] venv/pip users may need to install PyGObject according to their OS.")
    print("[fatal] On Windows, using Conda is usually the simplest option.")
    print()
    print("[fatal] Then set the launcher field 'Python exe' to the python.exe from")
    print("[fatal] that environment, or run this sender with that Python interpreter.")
    print()


def _print_typelib_help(error: BaseException) -> None:
    gst_root = os.environ.get("GST_ROOT", "")
    gi_typelib_path = os.environ.get("GI_TYPELIB_PATH", "")

    print("\n[fatal] Python imported gi, but GStreamer typelibs were not found or not usable.")
    print("[fatal] Error:")
    print(f"        {error}")
    print()
    print("[fatal] The sender requires these GStreamer introspection namespaces:")
    print("        Gst 1.0")
    print("        GstSdp 1.0")
    print("        GstWebRTC 1.0")
    print()
    print("[fatal] Current runtime environment:")
    print(f"        GST_ROOT={gst_root or '<not set>'}")
    print(f"        GI_TYPELIB_PATH={gi_typelib_path or '<not set>'}")
    print()
    print("[fatal] Make sure GStreamer is installed or bundled with the project, and")
    print("[fatal] make sure the selected Python environment has working PyGObject.")
    print()


def _verify_gi_and_gstreamer_typelibs() -> None:
    """
    Fail early with a clear message if PyGObject or GstWebRTC typelibs are not usable.
    """
    try:
        import gi
    except Exception as exc:
        _print_python_binding_help(exc)
        raise SystemExit(1) from exc

    try:
        gi.require_version("Gst", "1.0")
        gi.require_version("GstSdp", "1.0")
        gi.require_version("GstWebRTC", "1.0")
    except Exception as exc:
        _print_typelib_help(exc)
        raise SystemExit(1) from exc


# ============================================================
# Main entry point
# ============================================================

def main_entry(argv: list[str]) -> int:
    gst_root = _configure_gstreamer_runtime()
    _remove_gstreamer_site_packages_from_syspath(gst_root)
    _patch_backend_env_module()
    _verify_gi_and_gstreamer_typelibs()

    from webrtc_sender_backend.main import main

    return main(argv)


if __name__ == "__main__":
    raise SystemExit(main_entry(sys.argv))