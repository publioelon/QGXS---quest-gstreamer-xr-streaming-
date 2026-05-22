# QGXS - Quest GStreamer XR Streaming

QGXS is an open-source XR streaming project built around **GStreamer**, **WebRTC**, **Unity**, and native hardware-decoding paths for real-time 2D and 360-degree video streaming.

The current public version includes a **Windows desktop sender/receiver pipeline** that streams video from a GStreamer WebRTC sender to a Unity receiver using a native D3D11 texture-sharing plugin.

The broader goal of QGXS is to provide a practical foundation for low-latency XR video streaming experiments, including 360-degree video, hardware decoding, codec evaluation, bandwidth-aware streaming, and future Meta Quest / Android-native receiver support.

---

## Current public release

This release focuses on the **Windows-to-Windows Unity WebRTC pipeline**.

It includes:

- Python launcher GUI
- GStreamer WebRTC sender
- Unity receiver project
- Native D3D11 GStreamer-to-Unity texture plugin source
- H.264 and H.265 streaming support
- 2D flat video display
- 360-degree equirectangular sphere display
- Automatic 2D/360 display selection based on aspect ratio
- Sender-side runtime metrics
- Receiver feedback metrics
- Native GPU texture path designed to avoid CPU-side RGBA frame uploads

The repository contains the Unity receiver project and the native plugin source code. Users do **not** need to download a separate Unity project or manually search for external plugin DLLs.

The native DLL is not committed to the repository. It is built locally from source using the provided script.

---

## Platform status

| Platform | Status |
|---|---|
| Windows sender | Supported |
| Windows Unity receiver | Supported |
| H.264 | Supported |
| H.265 | Supported |
| AV1 | Experimental / future work |
| Meta Quest 3 Android receiver | Future work |
| Linux receiver | Future work |

The current Windows Unity receiver uses a **D3D11 native texture-sharing path**. Linux and Android/Quest receivers require different graphics backends and are not included in this release.

---

## Repository structure

```text
QGXS---quest-gstreamer-xr-streaming-/
│
├── launcher/
│   └── launcher_gui.py
│
├── sender/
│   ├── webrtc_sender.py
│   └── webrtc_sender_backend/
│
├── receiver/
│   └── windows/
│       ├── unity/
│       │   └── GStreamerUnity/
│       └── native/
│           └── GStreamerUnityPlugin/
│
├── scripts/
│   └── windows/
│       ├── build_native_plugin.ps1
│       └── run_launcher.ps1
│
├── docs/
│   └── quick_start_windows.md
│
├── README.md
├── LICENSE
└── .gitignore