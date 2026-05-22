#!/usr/bin/env python3
from __future__ import annotations

import configparser
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import Optional


APP_TITLE = "GStreamer Unity WebRTC Launcher"
CONFIG_FILE = "config.ini"

CODECS = ["h265", "h264", "av1"]
INPUT_MODES = ["video-file", "image-folder"]
IMAGE_FORMATS = ["auto", "jpg", "jpeg", "png", "webp", "avif"]
SUPPORTED_IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".webp", ".avif"}
SUPPORTED_VIDEO_EXTS = {".mp4", ".mov", ".mkv", ".avi", ".webm"}


def _existing_path_or_empty(path: Optional[Path]) -> str:
    if path is None:
        return ""

    try:
        if path.exists():
            return str(path)
    except OSError:
        pass

    return ""


def _path_from_env(name: str) -> Optional[Path]:
    value = os.environ.get(name, "").strip()

    if not value:
        return None

    path = Path(value)

    try:
        if path.exists():
            return path
    except OSError:
        return None

    return None


def _program_files_roots() -> list[Path]:
    roots: list[Path] = []

    for env_name in ("ProgramFiles", "ProgramFiles(x86)"):
        value = os.environ.get(env_name, "").strip()

        if not value:
            continue

        path = Path(value)

        try:
            if path.exists() and path not in roots:
                roots.append(path)
        except OSError:
            pass

    return roots


def _is_valid_gstreamer_root(path: Path) -> bool:
    try:
        if not path.exists() or not path.is_dir():
            return False

        bin_dir = path / "bin"
        plugin_dir = path / "lib" / "gstreamer-1.0"

        gst_launch_exe = bin_dir / "gst-launch-1.0.exe"
        gst_launch_no_ext = bin_dir / "gst-launch-1.0"

        return bin_dir.exists() and plugin_dir.exists() and (
            gst_launch_exe.exists() or gst_launch_no_ext.exists()
        )
    except OSError:
        return False


def detect_gstreamer_root(base_dir: Path) -> Optional[Path]:
    candidates: list[Path] = []

    candidates.append(base_dir / "Runtime" / "GStreamer")

    for env_name in (
        "GST_ROOT",
        "GSTREAMER_ROOT",
        "GSTREAMER_1_0_ROOT_MSVC_X86_64",
        "GSTREAMER_1_0_ROOT_X86_64",
    ):
        env_path = _path_from_env(env_name)
        if env_path is not None:
            candidates.append(env_path)

    for root in _program_files_roots():
        candidates.append(root / "gstreamer" / "1.0" / "msvc_x86_64")
        candidates.append(root / "GStreamer" / "1.0" / "msvc_x86_64")

    for exe_name in ("gst-launch-1.0.exe", "gst-launch-1.0"):
        found = shutil.which(exe_name)
        if found:
            exe_path = Path(found)
            if exe_path.parent.name.lower() == "bin":
                candidates.append(exe_path.parent.parent)

    seen: set[str] = set()

    for candidate in candidates:
        try:
            resolved = str(candidate.resolve()).lower()
        except OSError:
            resolved = str(candidate).lower()

        if resolved in seen:
            continue

        seen.add(resolved)

        if _is_valid_gstreamer_root(candidate):
            return candidate

    return None


def _unity_version_key(unity_exe: Path) -> tuple:
    try:
        version_text = unity_exe.parent.parent.name
    except Exception:
        version_text = ""

    numbers: list[int] = []
    current = ""

    for ch in version_text:
        if ch.isdigit():
            current += ch
        else:
            if current:
                numbers.append(int(current))
                current = ""

    if current:
        numbers.append(int(current))

    while len(numbers) < 4:
        numbers.append(0)

    return tuple(numbers[:4]) + (version_text,)


def _is_unity_exe(path: Path) -> bool:
    try:
        return path.is_file() and path.name.lower() == "unity.exe"
    except OSError:
        return False


def detect_unity_exe(base_dir: Path) -> Optional[Path]:
    candidates: list[Path] = []

    candidates.append(base_dir / "Runtime" / "Unity" / "Editor" / "Unity.exe")
    candidates.append(base_dir / "Unity" / "Editor" / "Unity.exe")

    env_unity = _path_from_env("UNITY_EXE")
    if env_unity is not None:
        candidates.append(env_unity)

    found = shutil.which("Unity.exe")
    if found:
        candidates.append(Path(found))

    for root in _program_files_roots():
        hub_root = root / "Unity" / "Hub" / "Editor"

        try:
            if hub_root.exists():
                candidates.extend(hub_root.glob("*/Editor/Unity.exe"))
        except OSError:
            pass

    valid = [candidate for candidate in candidates if _is_unity_exe(candidate)]

    if not valid:
        return None

    valid = sorted(valid, key=_unity_version_key)
    return valid[-1]


def _is_valid_unity_project(path: Path) -> bool:
    try:
        return (
            path.exists()
            and path.is_dir()
            and (path / "Assets").is_dir()
            and (path / "ProjectSettings").is_dir()
        )
    except OSError:
        return False


def detect_unity_project(base_dir: Path) -> Optional[Path]:
    candidates = [
        base_dir / "UnityReceiver" / "UnityProject",
        base_dir / "UnityProject",
        base_dir / "Receiver" / "UnityProject",
        base_dir,
    ]

    for candidate in candidates:
        if _is_valid_unity_project(candidate):
            return candidate

    return None


def detect_sender_backend(base_dir: Path) -> Optional[Path]:
    candidates = [
        base_dir / "Sender" / "webrtc_sender.py",
        base_dir / "Sender" / "webrtc_sender.exe",
        base_dir / "webrtc_sender.py",
        base_dir / "webrtc_sender.exe",
    ]

    for candidate in candidates:
        try:
            if candidate.is_file():
                return candidate
        except OSError:
            pass

    return None


def detect_python_exe(base_dir: Path) -> Optional[Path]:
    candidates = [
        base_dir / ".venv" / "Scripts" / "python.exe",
        base_dir / "venv" / "Scripts" / "python.exe",
        Path(sys.executable),
    ]

    for candidate in candidates:
        try:
            if candidate.is_file():
                return candidate
        except OSError:
            pass

    found = shutil.which("python")
    if found:
        path = Path(found)
        try:
            if path.is_file():
                return path
        except OSError:
            pass

    return None


def detect_default_video_or_folder(base_dir: Path) -> str:
    sample_dirs = [
        base_dir / "Samples" / "frames",
        base_dir / "Samples" / "Images",
        base_dir / "Samples" / "images",
        base_dir / "Samples" / "Videos",
        base_dir / "Samples" / "videos",
    ]

    for folder in sample_dirs:
        try:
            if not folder.is_dir():
                continue

            image_files = [
                p for p in folder.iterdir()
                if p.is_file() and p.suffix.lower() in SUPPORTED_IMAGE_EXTS
            ]

            if image_files:
                return str(folder)

            video_files = [
                p for p in folder.iterdir()
                if p.is_file() and p.suffix.lower() in SUPPORTED_VIDEO_EXTS
            ]

            if video_files:
                return str(sorted(video_files, key=lambda p: p.name.lower())[0])
        except OSError:
            pass

    return ""


class ProcessHandle:
    def __init__(self, name: str):
        self.name = name
        self.process: Optional[subprocess.Popen] = None
        self.reader_thread: Optional[threading.Thread] = None

    def is_running(self) -> bool:
        return self.process is not None and self.process.poll() is None


class LauncherApp(tk.Tk):
    def __init__(self):
        super().__init__()

        self.title(APP_TITLE)
        self.geometry("1280x820")
        self.minsize(1120, 740)

        self.base_dir = Path(__file__).resolve().parent
        self.config_path = self.base_dir / CONFIG_FILE
        self.log_queue: queue.Queue[str] = queue.Queue()

        self.unity = ProcessHandle("Unity")
        self.sender = ProcessHandle("Sender")

        self._create_variables()
        self._load_config_or_defaults()
        self._create_ui()
        self._poll_log_queue()
        self._update_status_loop()

    def _create_variables(self) -> None:
        self.gst_root_var = tk.StringVar()
        self.unity_exe_var = tk.StringVar()
        self.unity_project_var = tk.StringVar()
        self.sender_backend_var = tk.StringVar()
        self.python_exe_var = tk.StringVar()

        self.input_mode_var = tk.StringVar(value="video-file")
        self.input_path_var = tk.StringVar(value="")
        self.image_format_var = tk.StringVar(value="auto")

        self.codec_var = tk.StringVar(value="h265")
        self.host_var = tk.StringVar(value="127.0.0.1")
        self.port_var = tk.StringVar(value="9001")
        self.width_var = tk.StringVar(value="1920")
        self.height_var = tk.StringVar(value="960")
        self.fps_var = tk.StringVar(value="30")
        self.bitrate_var = tk.StringVar(value="8000")
        self.loop_var = tk.BooleanVar(value=True)

        self.unity_status_var = tk.StringVar(value="Unity: stopped")
        self.sender_status_var = tk.StringVar(value="Sender: stopped")
        self.input_hint_var = tk.StringVar(value="Select an MP4/MOV/MKV/AVI/WEBM video file.")

        self.dashboard_vars: dict[str, tk.StringVar] = {}
        dashboard_defaults = {
            "signaling_state": "n/a",
            "webrtc_state": "n/a",
            "ice_state": "n/a",
            "selected_route": "n/a",
            "rtt_ms": "n/a",
            "codec": "n/a",
            "resolution": "n/a",
            "target_fps": "n/a",
            "actual_fps": "n/a",
            "encoder": "n/a",
            "target_bitrate_mbps": "n/a",
            "actual_bitrate_mbps": "n/a",
            "estimated_bandwidth_mbps": "n/a",
            "packets_sent": "n/a",
            "bytes_sent": "n/a",
            "packet_loss_pct": "n/a",
            "jitter_ms": "n/a",
            "pipeline_state": "n/a",
            "last_error": "n/a",
            "receiver_alive": "0",
            "receiver_display_mode": "n/a",
            "receiver_unity_fps": "n/a",
            "receiver_copied_fps": "n/a",
            "receiver_frame_id": "0",
            "receiver_texture_attached": "0",
            "receiver_stall": "n/a",
        }

        for key, value in dashboard_defaults.items():
            self.dashboard_vars[key] = tk.StringVar(value=value)

    def _set_detected_defaults(self) -> None:
        gst_root = detect_gstreamer_root(self.base_dir)
        unity_exe = detect_unity_exe(self.base_dir)
        unity_project = detect_unity_project(self.base_dir)
        sender_backend = detect_sender_backend(self.base_dir)
        python_exe = detect_python_exe(self.base_dir)
        sample_input = detect_default_video_or_folder(self.base_dir)

        self.gst_root_var.set(_existing_path_or_empty(gst_root))
        self.unity_exe_var.set(_existing_path_or_empty(unity_exe))
        self.unity_project_var.set(_existing_path_or_empty(unity_project))
        self.sender_backend_var.set(_existing_path_or_empty(sender_backend))
        self.python_exe_var.set(_existing_path_or_empty(python_exe))
        self.input_path_var.set(sample_input)

        if sample_input:
            sample_path = Path(sample_input)
            if sample_path.is_dir():
                self.input_mode_var.set("image-folder")
            else:
                self.input_mode_var.set("video-file")

    @staticmethod
    def _config_path_value(section: configparser.SectionProxy, key: str) -> str:
        value = section.get(key, "").strip()

        if not value:
            return ""

        try:
            if Path(value).exists():
                return value
        except OSError:
            return ""

        return ""

    def _load_config_or_defaults(self) -> None:
        self._set_detected_defaults()

        if not self.config_path.exists():
            return

        parser = configparser.ConfigParser()
        parser.read(self.config_path, encoding="utf-8")

        if "Settings" not in parser:
            return

        s = parser["Settings"]

        gst_root = self._config_path_value(s, "gstRoot")
        unity_exe = self._config_path_value(s, "unityExe")
        unity_project = self._config_path_value(s, "unityProject")
        sender_backend = self._config_path_value(s, "senderBackend")
        python_exe = self._config_path_value(s, "pythonExe")

        input_path = self._config_path_value(s, "inputPath")
        if not input_path:
            input_path = self._config_path_value(s, "videoFile")

        if gst_root:
            self.gst_root_var.set(gst_root)
        if unity_exe:
            self.unity_exe_var.set(unity_exe)
        if unity_project:
            self.unity_project_var.set(unity_project)
        if sender_backend:
            self.sender_backend_var.set(sender_backend)
        if python_exe:
            self.python_exe_var.set(python_exe)
        if input_path:
            self.input_path_var.set(input_path)

        self.input_mode_var.set(s.get("inputMode", self.input_mode_var.get()).strip())
        self.image_format_var.set(s.get("imageFormat", self.image_format_var.get()).strip())

        self.codec_var.set(s.get("codec", self.codec_var.get()).strip())
        self.host_var.set(s.get("host", self.host_var.get()).strip())
        self.port_var.set(s.get("port", self.port_var.get()).strip())
        self.width_var.set(s.get("width", self.width_var.get()).strip())
        self.height_var.set(s.get("height", self.height_var.get()).strip())
        self.fps_var.set(s.get("fps", self.fps_var.get()).strip())
        self.bitrate_var.set(s.get("bitrate", self.bitrate_var.get()).strip())
        self.loop_var.set(s.getboolean("loop", fallback=True))

        if self.input_mode_var.get() not in INPUT_MODES:
            if self.input_path_var.get() and Path(self.input_path_var.get()).is_dir():
                self.input_mode_var.set("image-folder")
            else:
                self.input_mode_var.set("video-file")

        if self.image_format_var.get() not in IMAGE_FORMATS:
            self.image_format_var.set("auto")

    def save_config(self) -> None:
        parser = configparser.ConfigParser()
        parser["Settings"] = {
            "gstRoot": self.gst_root_var.get().strip(),
            "unityExe": self.unity_exe_var.get().strip(),
            "unityProject": self.unity_project_var.get().strip(),
            "senderBackend": self.sender_backend_var.get().strip(),
            "pythonExe": self.python_exe_var.get().strip(),
            "inputMode": self.input_mode_var.get().strip(),
            "inputPath": self.input_path_var.get().strip(),
            "imageFormat": self.image_format_var.get().strip(),
            "videoFile": self.input_path_var.get().strip(),
            "codec": self.codec_var.get().strip(),
            "host": self.host_var.get().strip(),
            "port": self.port_var.get().strip(),
            "width": self.width_var.get().strip(),
            "height": self.height_var.get().strip(),
            "fps": self.fps_var.get().strip(),
            "bitrate": self.bitrate_var.get().strip(),
            "loop": "true" if self.loop_var.get() else "false",
        }

        with self.config_path.open("w", encoding="utf-8") as f:
            parser.write(f)

        self.log(f"[gui] Settings saved to: {self.config_path}")

    def _create_ui(self) -> None:
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)

        root = ttk.Frame(self, padding=12)
        root.grid(row=0, column=0, sticky="nsew")
        root.columnconfigure(0, weight=1)
        root.rowconfigure(3, weight=1)

        title = ttk.Label(root, text=APP_TITLE, font=("Segoe UI", 16, "bold"))
        title.grid(row=0, column=0, sticky="w", pady=(0, 10))

        setup = ttk.LabelFrame(root, text="1. Setup", padding=10)
        setup.grid(row=1, column=0, sticky="ew", pady=(0, 10))
        setup.columnconfigure(1, weight=1)

        self._path_row(setup, 0, "GStreamer root", self.gst_root_var, self.browse_gst_root)
        self._path_row(setup, 1, "Unity.exe", self.unity_exe_var, self.browse_unity_exe)
        self._path_row(setup, 2, "Unity project", self.unity_project_var, self.browse_unity_project)
        self._path_row(setup, 3, "Sender backend", self.sender_backend_var, self.browse_sender_backend)
        self._path_row(setup, 4, "Python exe", self.python_exe_var, self.browse_python_exe)

        stream = ttk.LabelFrame(root, text="2. Stream", padding=10)
        stream.grid(row=2, column=0, sticky="ew", pady=(0, 10))
        stream.columnconfigure(1, weight=1)

        input_mode_row = ttk.Frame(stream)
        input_mode_row.grid(row=0, column=0, columnspan=4, sticky="ew", pady=(0, 8))
        input_mode_row.columnconfigure(7, weight=1)

        ttk.Label(input_mode_row, text="Input mode").grid(row=0, column=0, sticky="w", padx=(0, 6))
        input_mode_box = ttk.Combobox(
            input_mode_row,
            textvariable=self.input_mode_var,
            values=INPUT_MODES,
            width=14,
            state="readonly",
        )
        input_mode_box.grid(row=0, column=1, sticky="w", padx=(0, 14))
        input_mode_box.bind("<<ComboboxSelected>>", lambda _event: self.on_input_mode_changed())

        ttk.Label(input_mode_row, text="Image format").grid(row=0, column=2, sticky="w", padx=(0, 6))
        image_format_box = ttk.Combobox(
            input_mode_row,
            textvariable=self.image_format_var,
            values=IMAGE_FORMATS,
            width=8,
            state="readonly",
        )
        image_format_box.grid(row=0, column=3, sticky="w", padx=(0, 14))

        ttk.Label(input_mode_row, textvariable=self.input_hint_var, foreground="#444").grid(
            row=0,
            column=4,
            sticky="w",
        )

        self._input_path_row(stream, 1)

        row = ttk.Frame(stream)
        row.grid(row=2, column=0, columnspan=4, sticky="ew", pady=(8, 0))

        for i in range(16):
            row.columnconfigure(i, weight=0)
        row.columnconfigure(15, weight=1)

        ttk.Label(row, text="Codec").grid(row=0, column=0, sticky="w", padx=(0, 6))
        codec_box = ttk.Combobox(row, textvariable=self.codec_var, values=CODECS, width=8, state="readonly")
        codec_box.grid(row=0, column=1, sticky="w", padx=(0, 14))

        self._small_entry(row, "Host", self.host_var, 2, width=14)
        self._small_entry(row, "Port", self.port_var, 4, width=7)
        self._small_entry(row, "Width", self.width_var, 6, width=7)
        self._small_entry(row, "Height", self.height_var, 8, width=7)
        self._small_entry(row, "FPS", self.fps_var, 10, width=6)
        self._small_entry(row, "Bitrate kbps", self.bitrate_var, 12, width=9)

        ttk.Checkbutton(row, text="Loop playback", variable=self.loop_var).grid(
            row=0,
            column=14,
            sticky="w",
            padx=(0, 14),
        )

        controls = ttk.LabelFrame(root, text="3. Control", padding=10)
        controls.grid(row=3, column=0, sticky="nsew", pady=(0, 10))
        controls.columnconfigure(0, weight=1)
        controls.rowconfigure(2, weight=0)
        controls.rowconfigure(3, weight=1)

        button_row = ttk.Frame(controls)
        button_row.grid(row=0, column=0, sticky="ew")

        ttk.Button(button_row, text="Auto Detect Paths", command=self.auto_detect_paths).grid(
            row=0,
            column=0,
            padx=(0, 8),
        )
        ttk.Button(button_row, text="Validate", command=self.validate_all).grid(row=0, column=1, padx=(0, 8))
        ttk.Button(button_row, text="Detect Metadata", command=self.detect_metadata).grid(row=0, column=2, padx=(0, 8))
        ttk.Button(button_row, text="Save Settings", command=self.save_config).grid(row=0, column=3, padx=(0, 8))
        ttk.Button(button_row, text="Launch Unity Receiver", command=self.launch_unity).grid(row=0, column=4, padx=(0, 8))
        ttk.Button(button_row, text="Start Streaming", command=self.start_streaming).grid(row=0, column=5, padx=(0, 8))
        ttk.Button(button_row, text="Stop Streaming", command=self.stop_streaming).grid(row=0, column=6, padx=(0, 8))
        ttk.Button(button_row, text="Stop Unity", command=self.stop_unity).grid(row=0, column=7, padx=(0, 8))
        ttk.Button(button_row, text="Clear Log", command=self.clear_log).grid(row=0, column=8)

        status_row = ttk.Frame(controls)
        status_row.grid(row=1, column=0, sticky="ew", pady=(10, 8))
        ttk.Label(status_row, textvariable=self.unity_status_var).grid(row=0, column=0, sticky="w", padx=(0, 18))
        ttk.Label(status_row, textvariable=self.sender_status_var).grid(row=0, column=1, sticky="w")

        self._create_research_dashboard(controls)

        log_frame = ttk.LabelFrame(controls, text="Runtime Log", padding=6)
        log_frame.grid(row=3, column=0, sticky="nsew")
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)

        self.log_text = tk.Text(log_frame, wrap="word", height=18, font=("Consolas", 10))
        self.log_text.grid(row=0, column=0, sticky="nsew")

        scrollbar = ttk.Scrollbar(log_frame, orient="vertical", command=self.log_text.yview)
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.log_text.configure(yscrollcommand=scrollbar.set)

        note = (
            "Workflow reminder: Launch Unity Receiver → press Play in Unity → "
            "wait for the WebRTC receiver to listen → Start Streaming here."
        )
        ttk.Label(root, text=note, foreground="#444").grid(row=4, column=0, sticky="w")

        self.on_input_mode_changed()

        self.log("[gui] Ready.")
        self.log("[gui] Paths are auto-detected when possible. Missing fields can be filled with Browse.")
        self.log("[gui] This launcher uses the sender backend selected in the Sender backend field.")

    def _create_research_dashboard(self, parent) -> None:
        dashboard = ttk.LabelFrame(parent, text="Research Dashboard", padding=8)
        dashboard.grid(row=2, column=0, sticky="ew", pady=(0, 8))

        for col in range(6):
            dashboard.columnconfigure(col, weight=1)

        self._dashboard_group(
            dashboard,
            0,
            "Connection",
            [
                ("Signaling", "signaling_state", ""),
                ("WebRTC", "webrtc_state", ""),
                ("ICE", "ice_state", ""),
                ("Route", "selected_route", ""),
                ("RTT", "rtt_ms", " ms"),
            ],
        )

        self._dashboard_group(
            dashboard,
            1,
            "Stream",
            [
                ("Codec", "codec", ""),
                ("Resolution", "resolution", ""),
                ("Target FPS", "target_fps", ""),
                ("Actual FPS", "actual_fps", ""),
                ("Encoder", "encoder", ""),
            ],
        )

        self._dashboard_group(
            dashboard,
            2,
            "Bitrate",
            [
                ("Target Bitrate", "target_bitrate_mbps", " Mbps"),
                ("Actual Bitrate", "actual_bitrate_mbps", " Mbps"),
                ("Estimated BW", "estimated_bandwidth_mbps", " Mbps"),
            ],
        )

        self._dashboard_group(
            dashboard,
            3,
            "Network",
            [
                ("Packets Sent", "packets_sent", ""),
                ("Bytes Sent", "bytes_sent", ""),
                ("Packet Loss", "packet_loss_pct", " %"),
                ("Jitter", "jitter_ms", " ms"),
            ],
        )

        self._dashboard_group(
            dashboard,
            4,
            "Receiver",
            [
                ("Alive", "receiver_alive", ""),
                ("Display", "receiver_display_mode", ""),
                ("Unity FPS", "receiver_unity_fps", ""),
                ("Copied FPS", "receiver_copied_fps", ""),
                ("Frame ID", "receiver_frame_id", ""),
                ("Texture", "receiver_texture_attached", ""),
                ("Stall", "receiver_stall", ""),
            ],
        )

        self._dashboard_group(
            dashboard,
            5,
            "System",
            [
                ("Pipeline", "pipeline_state", ""),
                ("Last Warning/Error", "last_error", ""),
            ],
        )

    def _dashboard_group(self, parent, col: int, title: str, rows: list[tuple[str, str, str]]) -> None:
        frame = ttk.LabelFrame(parent, text=title, padding=8)
        frame.grid(row=0, column=col, sticky="nsew", padx=(0, 8 if col < 5 else 0))
        frame.columnconfigure(1, weight=1)

        for row_idx, (label, key, suffix) in enumerate(rows):
            ttk.Label(frame, text=f"{label}:").grid(row=row_idx, column=0, sticky="w", padx=(0, 8), pady=1)

            var = tk.StringVar()
            self.dashboard_vars[f"_display_{key}"] = var
            self._refresh_dashboard_display_value(key, suffix)

            ttk.Label(frame, textvariable=var).grid(row=row_idx, column=1, sticky="w", pady=1)

    def _refresh_dashboard_display_value(self, key: str, suffix: str = "") -> None:
        display_key = f"_display_{key}"

        if display_key not in self.dashboard_vars or key not in self.dashboard_vars:
            return

        value = self.dashboard_vars[key].get()

        if key == "receiver_alive":
            value = "yes" if value in ("1", "true", "True", "yes") else "no"
        elif key in ("receiver_texture_attached", "receiver_stall"):
            if value in ("1", "true", "True", "yes"):
                value = "yes"
            elif value in ("0", "false", "False", "no"):
                value = "no"
        elif suffix and value not in ("", "n/a", "unknown"):
            value = value + suffix

        self.dashboard_vars[display_key].set(value)

    def parse_metrics_line(self, line: str) -> dict[str, str]:
        if line.startswith("METRICS|"):
            line = line[len("METRICS|"):]

        values: dict[str, str] = {}

        for part in line.split("|"):
            if "=" not in part:
                continue

            key, value = part.split("=", 1)
            key = key.strip()
            value = value.strip()

            if key:
                values[key] = value

        return values

    def update_dashboard_from_metrics_line(self, line: str) -> None:
        values = self.parse_metrics_line(line)

        suffixes = {
            "rtt_ms": " ms",
            "target_bitrate_mbps": " Mbps",
            "actual_bitrate_mbps": " Mbps",
            "estimated_bandwidth_mbps": " Mbps",
            "packet_loss_pct": " %",
            "jitter_ms": " ms",
        }

        for key, value in values.items():
            if key not in self.dashboard_vars:
                self.dashboard_vars[key] = tk.StringVar(value="n/a")

            self.dashboard_vars[key].set(value)
            self._refresh_dashboard_display_value(key, suffixes.get(key, ""))

    def _path_row(self, parent, row_idx: int, label: str, var: tk.StringVar, command) -> None:
        ttk.Label(parent, text=label).grid(row=row_idx, column=0, sticky="w", padx=(0, 8), pady=4)
        entry = ttk.Entry(parent, textvariable=var)
        entry.grid(row=row_idx, column=1, sticky="ew", pady=4)
        ttk.Button(parent, text="Browse", command=command).grid(row=row_idx, column=2, padx=(8, 0), pady=4)

    def _input_path_row(self, parent, row_idx: int) -> None:
        ttk.Label(parent, text="Input path").grid(row=row_idx, column=0, sticky="w", padx=(0, 8), pady=4)

        entry = ttk.Entry(parent, textvariable=self.input_path_var)
        entry.grid(row=row_idx, column=1, sticky="ew", pady=4)

        ttk.Button(parent, text="Browse File", command=self.browse_input_file).grid(
            row=row_idx,
            column=2,
            padx=(8, 0),
            pady=4,
        )

        ttk.Button(parent, text="Browse Folder", command=self.browse_input_folder).grid(
            row=row_idx,
            column=3,
            padx=(8, 0),
            pady=4,
        )

    def _small_entry(self, parent, label: str, var: tk.StringVar, col: int, width: int) -> None:
        ttk.Label(parent, text=label).grid(row=0, column=col, sticky="w", padx=(0, 6))
        ttk.Entry(parent, textvariable=var, width=width).grid(row=0, column=col + 1, sticky="w", padx=(0, 14))

    def on_input_mode_changed(self) -> None:
        mode = self.input_mode_var.get().strip().lower()

        if mode == "image-folder":
            self.input_hint_var.set("Select a folder containing JPG, PNG, WEBP, or AVIF frames.")
        else:
            self.input_hint_var.set("Select an MP4/MOV/MKV/AVI/WEBM video file.")

    def auto_detect_paths(self) -> None:
        gst_root = detect_gstreamer_root(self.base_dir)
        unity_exe = detect_unity_exe(self.base_dir)
        unity_project = detect_unity_project(self.base_dir)
        sender_backend = detect_sender_backend(self.base_dir)
        python_exe = detect_python_exe(self.base_dir)

        if gst_root:
            self.gst_root_var.set(str(gst_root))
            self.log(f"[detect] GStreamer root: {gst_root}")
        else:
            self.log("[detect] GStreamer root was not found.")

        if unity_exe:
            self.unity_exe_var.set(str(unity_exe))
            self.log(f"[detect] Unity.exe: {unity_exe}")
        else:
            self.log("[detect] Unity.exe was not found.")

        if unity_project:
            self.unity_project_var.set(str(unity_project))
            self.log(f"[detect] Unity project: {unity_project}")
        else:
            self.log("[detect] Unity project was not found.")

        if sender_backend:
            self.sender_backend_var.set(str(sender_backend))
            self.log(f"[detect] Sender backend: {sender_backend}")
        else:
            self.log("[detect] Sender backend was not found.")

        if python_exe:
            self.python_exe_var.set(str(python_exe))
            self.log(f"[detect] Python exe: {python_exe}")
        else:
            self.log("[detect] Python exe was not found.")

        self.on_input_mode_changed()

    def browse_gst_root(self) -> None:
        value = filedialog.askdirectory(title="Select GStreamer root folder")
        if value:
            self.gst_root_var.set(value)

    def browse_unity_exe(self) -> None:
        value = filedialog.askopenfilename(
            title="Select Unity.exe",
            filetypes=[
                ("Unity executable", "Unity.exe"),
                ("Executables", "*.exe"),
                ("All files", "*.*"),
            ],
        )
        if value:
            self.unity_exe_var.set(value)

    def browse_unity_project(self) -> None:
        value = filedialog.askdirectory(title="Select Unity project folder")
        if value:
            self.unity_project_var.set(value)

    def browse_sender_backend(self) -> None:
        value = filedialog.askopenfilename(
            title="Select sender backend .py or .exe",
            filetypes=[
                ("Python or executable", "*.py *.exe"),
                ("Python", "*.py"),
                ("Executable", "*.exe"),
                ("All files", "*.*"),
            ],
        )
        if value:
            self.sender_backend_var.set(value)

    def browse_python_exe(self) -> None:
        value = filedialog.askopenfilename(
            title="Select python.exe",
            filetypes=[
                ("Python executable", "python.exe pythonw.exe"),
                ("Executables", "*.exe"),
                ("All files", "*.*"),
            ],
        )
        if value:
            self.python_exe_var.set(value)

    def browse_input_file(self) -> None:
        value = filedialog.askopenfilename(
            title="Select video file",
            filetypes=[
                ("Video files", "*.mp4 *.mov *.mkv *.avi *.webm"),
                ("All files", "*.*"),
            ],
        )
        if value:
            self.input_path_var.set(value)
            self.input_mode_var.set("video-file")
            self.on_input_mode_changed()

    def browse_input_folder(self) -> None:
        value = filedialog.askdirectory(title="Select image folder")
        if value:
            self.input_path_var.set(value)
            self.input_mode_var.set("image-folder")
            self.on_input_mode_changed()

    def log(self, text: str) -> None:
        timestamp = time.strftime("%H:%M:%S")
        self.log_queue.put(f"[{timestamp}] {text}\n")

    def _poll_log_queue(self) -> None:
        try:
            while True:
                line = self.log_queue.get_nowait()

                if isinstance(line, str) and line.startswith("__METRICS__"):
                    self.update_dashboard_from_metrics_line(line[len("__METRICS__"):])
                    continue

                self.log_text.insert("end", line)
                self.log_text.see("end")
        except queue.Empty:
            pass

        self.after(80, self._poll_log_queue)

    def clear_log(self) -> None:
        self.log_text.delete("1.0", "end")

    def make_gstreamer_env(self) -> dict[str, str]:
        env = os.environ.copy()

        gst_root_text = self.gst_root_var.get().strip()

        if gst_root_text:
            gst_root = Path(gst_root_text)

            gst_bin = str(gst_root / "bin")
            gst_plugins = str(gst_root / "lib" / "gstreamer-1.0")
            gst_scanner = str(gst_root / "libexec" / "gstreamer-1.0" / "gst-plugin-scanner.exe")
            gst_typelibs = str(gst_root / "lib" / "girepository-1.0")

            env["GST_ROOT"] = str(gst_root)
            env["PATH"] = gst_bin + os.pathsep + env.get("PATH", "")
            env["GST_PLUGIN_PATH"] = gst_plugins
            env["GST_PLUGIN_SYSTEM_PATH_1_0"] = gst_plugins
            env["GST_PLUGIN_SCANNER"] = gst_scanner

            if Path(gst_typelibs).exists():
                env["GI_TYPELIB_PATH"] = gst_typelibs + os.pathsep + env.get("GI_TYPELIB_PATH", "")

        env["GST_DEBUG_NO_COLOR"] = "1"
        env["PYTHONUNBUFFERED"] = "1"
        env["PYTHONIOENCODING"] = "utf-8"
        env["GST_REGISTRY"] = str(self.base_dir / "gst-registry-gui.bin")

        return env

    def validate_all(self) -> bool:
        ok = self._validate_common(show_popup=False, include_success=True)

        if ok:
            self.log("[gui] Validation passed.")
            messagebox.showinfo("Validation", "Validation passed.")

        return ok

    def _validate_common(self, show_popup: bool, include_success: bool = False) -> bool:
        errors: list[str] = []
        warnings: list[str] = []

        gst_root_text = self.gst_root_var.get().strip()
        unity_exe_text = self.unity_exe_var.get().strip()
        unity_project_text = self.unity_project_var.get().strip()
        sender_backend_text = self.sender_backend_var.get().strip()
        python_exe_text = self.python_exe_var.get().strip()
        input_path_text = self.input_path_var.get().strip()

        input_mode = self.input_mode_var.get().strip().lower()
        image_format = self.image_format_var.get().strip().lower()

        if not gst_root_text:
            errors.append("GStreamer root is empty. Click Auto Detect Paths or Browse.")
        else:
            gst_root = Path(gst_root_text)

            if not gst_root.exists():
                errors.append(f"GStreamer root does not exist:\n{gst_root}")
            else:
                if not (gst_root / "bin").exists():
                    errors.append(f"GStreamer bin folder missing:\n{gst_root / 'bin'}")
                if not (gst_root / "lib" / "gstreamer-1.0").exists():
                    errors.append(f"GStreamer plugin folder missing:\n{gst_root / 'lib' / 'gstreamer-1.0'}")
                if not (gst_root / "libexec" / "gstreamer-1.0" / "gst-plugin-scanner.exe").exists():
                    errors.append("GST_PLUGIN_SCANNER was not found inside the selected GStreamer root.")
                if not (gst_root / "lib" / "girepository-1.0").exists():
                    warnings.append("GI_TYPELIB_PATH folder was not found inside the selected GStreamer root.")

        if not unity_exe_text:
            errors.append("Unity.exe is empty. Click Auto Detect Paths or Browse.")
        else:
            unity_exe = Path(unity_exe_text)

            if not unity_exe.exists():
                errors.append(f"Unity executable does not exist:\n{unity_exe}")
            else:
                if unity_exe.name.lower() != "unity.exe":
                    warnings.append("The selected Unity executable does not look like Unity.exe.")
                if "unity hub" in str(unity_exe).lower():
                    errors.append("You selected Unity Hub. Select the real Editor\\Unity.exe instead.")

        if not unity_project_text:
            errors.append("Unity project is empty. Browse to a Unity project folder.")
        else:
            unity_project = Path(unity_project_text)

            if not unity_project.exists():
                errors.append(f"Unity project folder does not exist:\n{unity_project}")
            else:
                if not (unity_project / "Assets").exists() or not (unity_project / "ProjectSettings").exists():
                    errors.append("Unity project must contain both Assets and ProjectSettings folders.")

        if not sender_backend_text:
            errors.append("Sender backend is empty. Click Auto Detect Paths or Browse.")
        else:
            sender_backend = Path(sender_backend_text)

            if not sender_backend.exists():
                errors.append(f"Sender backend does not exist:\n{sender_backend}")
            else:
                suffix = sender_backend.suffix.lower()
                if suffix not in (".py", ".exe"):
                    warnings.append("Sender backend is not .py or .exe. The launcher may not know how to run it.")
                if suffix == ".py":
                    if not python_exe_text:
                        errors.append("Python executable is empty. Browse to python.exe or use Auto Detect Paths.")
                    elif not Path(python_exe_text).exists():
                        errors.append(f"Python executable does not exist:\n{python_exe_text}")

        if input_mode not in INPUT_MODES:
            errors.append("Input mode must be video-file or image-folder.")

        if image_format not in IMAGE_FORMATS:
            errors.append("Image format must be auto, jpg, jpeg, png, webp, or avif.")

        if not input_path_text:
            errors.append("Input path is empty. Select a video file or image folder.")
        else:
            input_path = Path(input_path_text)

            if input_mode == "video-file":
                if not input_path.is_file():
                    errors.append(f"Video input file does not exist:\n{input_path}")

            elif input_mode == "image-folder":
                if not input_path.is_dir():
                    errors.append(f"Image input folder does not exist:\n{input_path}")
                else:
                    images = self._list_supported_images(input_path, image_format)
                    if not images:
                        if image_format == "auto":
                            errors.append(
                                "Image folder does not contain supported images:\n"
                                f"{input_path}\n\n"
                                "Supported: .jpg, .jpeg, .png, .webp, .avif"
                            )
                        else:
                            errors.append(
                                f"Image folder does not contain .{image_format} images:\n{input_path}"
                            )

        for name, value in [
            ("port", self.port_var.get()),
            ("width", self.width_var.get()),
            ("height", self.height_var.get()),
            ("fps", self.fps_var.get()),
            ("bitrate", self.bitrate_var.get()),
        ]:
            try:
                parsed = int(value)
                if parsed <= 0:
                    raise ValueError
            except ValueError:
                errors.append(f"Invalid positive integer for {name}: {value}")

        try:
            width = int(self.width_var.get())
            height = int(self.height_var.get())
            if width % 2 != 0 or height % 2 != 0:
                errors.append("Width and height must be even numbers.")
        except ValueError:
            pass

        if self.codec_var.get().strip().lower() not in CODECS:
            errors.append("Codec must be h265, h264, or av1.")

        for warning in warnings:
            self.log(f"[warning] {warning}")

        if errors:
            text = "\n\n".join(errors)
            self.log("[gui] Validation failed.")
            messagebox.showerror("Validation failed", text)
            return False

        return True

    @staticmethod
    def _list_supported_images(folder: Path, image_format: str) -> list[Path]:
        if not folder.is_dir():
            return []

        if image_format == "auto":
            exts = SUPPORTED_IMAGE_EXTS
        else:
            fmt = image_format.lower()
            if fmt in ("jpg", "jpeg"):
                exts = {".jpg", ".jpeg"}
            else:
                exts = {f".{fmt}"}

        files = [p for p in folder.iterdir() if p.is_file() and p.suffix.lower() in exts]
        return sorted(files, key=lambda p: p.name.lower())

    def detect_metadata(self) -> None:
        input_mode = self.input_mode_var.get().strip().lower()
        input_path_text = self.input_path_var.get().strip()

        if not input_path_text:
            messagebox.showerror("Metadata", "Select an input file or folder first.")
            return

        input_path = Path(input_path_text)

        if input_mode == "video-file":
            self.detect_video_metadata(input_path)
        elif input_mode == "image-folder":
            self.detect_image_folder_metadata(input_path)
        else:
            messagebox.showerror("Metadata", "Unknown input mode.")

    def detect_video_metadata(self, video: Path) -> None:
        if not video.is_file():
            messagebox.showerror("Metadata", "Select a valid video file first.")
            return

        env = self.make_gstreamer_env()

        cmd = [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height,r_frame_rate",
            "-of",
            "default=noprint_wrappers=1:nokey=1",
            str(video),
        ]

        try:
            result = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=15)
        except FileNotFoundError:
            self.log("[metadata] ffprobe was not found in PATH. Metadata detection skipped.")
            messagebox.showwarning("Metadata", "ffprobe was not found. Fill width/height/FPS manually.")
            return
        except subprocess.TimeoutExpired:
            self.log("[metadata] ffprobe timed out.")
            messagebox.showwarning("Metadata", "ffprobe timed out. Fill width/height/FPS manually.")
            return

        if result.returncode != 0:
            self.log(f"[metadata] ffprobe failed: {result.stderr.strip()}")
            messagebox.showwarning("Metadata", "ffprobe failed. Fill width/height/FPS manually.")
            return

        lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
        if len(lines) < 3:
            self.log("[metadata] Could not parse ffprobe output.")
            messagebox.showwarning("Metadata", "Could not parse ffprobe output.")
            return

        width, height, fps_text = lines[0], lines[1], lines[2]
        fps = self._parse_fraction_fps(fps_text)

        self.width_var.set(width)
        self.height_var.set(height)
        if fps:
            self.fps_var.set(str(fps))

        self.log(f"[metadata] Detected video: {width}x{height} @ {self.fps_var.get()} FPS")

    def detect_image_folder_metadata(self, folder: Path) -> None:
        if not folder.is_dir():
            messagebox.showerror("Metadata", "Select a valid image folder first.")
            return

        images = self._list_supported_images(folder, self.image_format_var.get().strip().lower())

        if not images:
            messagebox.showerror(
                "Metadata",
                "No supported images found in folder.\n\nSupported: JPG, PNG, WEBP, AVIF",
            )
            return

        first_image = images[0]

        try:
            from PIL import Image
        except ImportError:
            self.log("[metadata] Pillow is not installed in the GUI Python environment.")
            self.log(f"[metadata] Found {len(images)} image(s), first image: {first_image.name}")
            messagebox.showwarning(
                "Metadata",
                "Pillow is not installed, so image dimensions could not be detected.\n\n"
                f"Found {len(images)} image(s).\n"
                "Fill width/height manually.",
            )
            return

        try:
            with Image.open(first_image) as img:
                width, height = img.size
        except Exception as exc:
            self.log(f"[metadata] Could not open first image: {exc}")
            messagebox.showwarning(
                "Metadata",
                f"Could not open first image:\n{first_image}\n\nFill width/height manually.",
            )
            return

        self.width_var.set(str(width))
        self.height_var.set(str(height))

        self.log(
            f"[metadata] Detected image folder: {len(images)} image(s), "
            f"first={first_image.name}, size={width}x{height}, fps={self.fps_var.get()}"
        )

    @staticmethod
    def _parse_fraction_fps(text: str) -> Optional[int]:
        try:
            if "/" in text:
                a, b = text.split("/", 1)
                value = float(a) / float(b)
            else:
                value = float(text)
            return max(1, int(round(value)))
        except Exception:
            return None

    def launch_unity(self) -> None:
        if self.unity.is_running():
            self.log("[unity] Unity is already running.")
            return

        if not self._validate_common(show_popup=True):
            return

        env = self.make_gstreamer_env()
        unity_exe = self.unity_exe_var.get().strip()
        unity_project = self.unity_project_var.get().strip()

        cmd = [unity_exe, "-projectPath", unity_project]

        self.log("[unity] Launching Unity with GStreamer environment...")
        self.log("[unity] " + " ".join(f'"{x}"' if " " in x else x for x in cmd))

        try:
            self.unity.process = subprocess.Popen(
                cmd,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
            )
        except Exception as exc:
            self.log(f"[unity] Failed to launch Unity: {exc}")
            messagebox.showerror("Unity", f"Failed to launch Unity:\n{exc}")
            return

        self._start_reader_thread(self.unity)
        self.log("[unity] Unity process started.")
        self.log("[unity] In Unity: press Play, wait for WebRTC receiver to listen, then Start Streaming here.")

    def start_streaming(self) -> None:
        if self.sender.is_running():
            self.log("[sender] Sender is already running.")
            return

        if not self._validate_common(show_popup=True):
            return

        env = self.make_gstreamer_env()

        sender_backend = Path(self.sender_backend_var.get().strip())

        codec = self.codec_var.get().strip().lower()
        input_mode = self.input_mode_var.get().strip().lower()
        input_path = self.input_path_var.get().strip()
        image_format = self.image_format_var.get().strip().lower()

        host = self.host_var.get().strip()
        port = self.port_var.get().strip()
        width = self.width_var.get().strip()
        height = self.height_var.get().strip()
        fps = self.fps_var.get().strip()
        bitrate = self.bitrate_var.get().strip()
        loop_flag = "--loop" if self.loop_var.get() else "--no-loop"

        python_backend_args = [
            codec,
            "--input-mode",
            input_mode,
            "--input",
            input_path,
            "--image-format",
            image_format,
            loop_flag,
            host,
            port,
            width,
            height,
            fps,
            bitrate,
        ]

        exe_backend_args = [
            codec,
            input_path,
            host,
            port,
            width,
            height,
            fps,
            bitrate,
        ]

        if sender_backend.suffix.lower() == ".py":
            python_exe = self.python_exe_var.get().strip()
            cmd = [python_exe, "-u", str(sender_backend), *python_backend_args]
        else:
            if input_mode != "video-file":
                messagebox.showerror(
                    "Sender backend",
                    "Image-folder input requires the Python sender backend.\n\n"
                    "The old .exe backend only supports video-file input.",
                )
                return
            cmd = [str(sender_backend), *exe_backend_args]

        self.log("[sender] Starting stream...")
        self.log("[sender] Loop playback: " + ("enabled" if self.loop_var.get() else "disabled"))
        self.log("[sender] " + " ".join(f'"{x}"' if " " in x else x for x in cmd))

        try:
            self.sender.process = subprocess.Popen(
                cmd,
                env=env,
                cwd=str(sender_backend.parent),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
            )
        except Exception as exc:
            self.log(f"[sender] Failed to start sender: {exc}")
            messagebox.showerror("Sender", f"Failed to start sender:\n{exc}")
            return

        self._start_reader_thread(self.sender)
        self.log("[sender] Sender process started.")

    def _start_reader_thread(self, handle: ProcessHandle) -> None:
        def reader() -> None:
            proc = handle.process
            if proc is None or proc.stdout is None:
                return

            for line in proc.stdout:
                clean = line.strip()

                if clean.startswith("METRICS|"):
                    self.log_queue.put("__METRICS__" + clean)
                    continue

                if "ICE candidate" in clean:
                    continue

                self.log_queue.put(f"[{handle.name.lower()}] {line}")

            code = proc.poll()
            self.log_queue.put(f"[{handle.name.lower()}] Process output ended. Return code: {code}\n")

        handle.reader_thread = threading.Thread(target=reader, daemon=True)
        handle.reader_thread.start()

    def stop_streaming(self) -> None:
        self._stop_process(self.sender, "sender")

    def stop_unity(self) -> None:
        self._stop_process(self.unity, "unity")

    def _stop_process(self, handle: ProcessHandle, label: str) -> None:
        proc = handle.process
        if proc is None or proc.poll() is not None:
            self.log(f"[{label}] Not running.")
            return

        self.log(f"[{label}] Stopping process...")

        try:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.log(f"[{label}] Process did not exit after terminate; killing...")
                proc.kill()
        except Exception as exc:
            self.log(f"[{label}] Failed to stop process: {exc}")

    def _update_status_loop(self) -> None:
        self.unity_status_var.set("Unity: running" if self.unity.is_running() else "Unity: stopped")
        self.sender_status_var.set("Sender: running" if self.sender.is_running() else "Sender: stopped")
        self.after(500, self._update_status_loop)

    def on_close(self) -> None:
        if self.sender.is_running() or self.unity.is_running():
            answer = messagebox.askyesno(
                "Quit",
                "Sender or Unity is still running. Stop them and quit?",
            )
            if not answer:
                return

        self.stop_streaming()
        self.stop_unity()
        self.destroy()


if __name__ == "__main__":
    app = LauncherApp()
    app.protocol("WM_DELETE_WINDOW", app.on_close)
    app.mainloop()