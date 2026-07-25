<div align="center">

# QGXS

### Hardware-Accelerated XR Video Streaming for Unity

Real-time 2D and 360° video streaming across **Windows**, **Ubuntu**, and **Meta Quest 3** using GStreamer, WebRTC, and native hardware decoding.

[![License](https://img.shields.io/badge/License-Apache--2.0-blue.svg)](LICENSE)
![Unity](https://img.shields.io/badge/Unity-2022.3%20LTS-black)
![GStreamer](https://img.shields.io/badge/GStreamer-WebRTC-purple)
![Codecs](https://img.shields.io/badge/Codecs-H.264%20%7C%20H.265%20%7C%20AV1-green)

## [Download QGXS v0.1.0](https://drive.google.com/file/d/1Jy8PrMn2373mifEb80dezbvN0fJgldyk/view?usp=sharing)

</div>

---

## Overview

QGXS is an open-source video-streaming tool built with GStreamer, WebRTC, Unity, and platform-specific native decoding backends.

It supports flat video and equirectangular 360° video through native GPU texture paths for Windows, Ubuntu, and Meta Quest 3.

## Features

- GStreamer/WebRTC transport
- H.264, H.265, and AV1
- Windows and Ubuntu sender workflows
- Windows Unity receiver
- Ubuntu Unity receiver
- Meta Quest 3 receiver APK
- NVIDIA hardware encoding and decoding
- Android MediaCodec hardware decoding
- D3D11, CUDA/OpenGL, and external OES texture paths
- Flat 2D and 360° rendering
- Configurable resolution, frame rate, bitrate, and codec
- Sender and receiver telemetry
- Unity diagnostics overlay

## Support

| Sender | Receiver | H.264 | H.265 | AV1 |
|---|---|:---:|:---:|:---:|
| Windows | Windows Unity | **Supported** | **Supported** | **Not Supported** |
| Ubuntu | Ubuntu Unity | **Supported** | **Supported** | **Supported** |
| Ubuntu | Meta Quest 3 | **Supported** | **Supported** | **Supported** |

## Architecture

### Windows Unity

```text
GStreamer WebRTC
→ D3D11 hardware decoder
→ native D3D11 texture
→ Unity
```

### Ubuntu Unity

```text
GStreamer WebRTC
→ NVIDIA hardware decoder
→ CUDAMemory
→ CUDA/OpenGL interoperability
→ Unity external texture
```

### Meta Quest 3

```text
GStreamer WebRTC
→ Android MediaCodec
→ SurfaceTexture
→ OpenGL external OES texture
→ Unity XR sphere
```

## Download

### [Download the complete QGXS v0.1.0 release](https://drive.google.com/file/d/1Jy8PrMn2373mifEb80dezbvN0fJgldyk/view?usp=sharing)

The release contains:

| File | Description |
|---|---|
| `QSXR-v0.1.0-Ubuntu-Sender-GUI.tar.gz` | Ubuntu sender GUI |
| `QSXR-v0.1.0-Ubuntu-Unity-Receiver.zip` | Ubuntu Unity receiver |
| `QSXR-v0.1.0-Quest3-Receiver.apk` | Meta Quest 3 receiver |
| `SHA256SUMS.txt` | Integrity checks |
| `RELEASE_NOTES.md` | Release information |

## Windows

### Requirements

- Windows 10 or 11
- Unity 2022.3 LTS
- Python 3
- 64-bit GStreamer
- Visual Studio C++ Build Tools
- D3D11-compatible GPU

Clone the repository:

```powershell
git clone https://github.com/publioelon/QGXS---quest-gstreamer-xr-streaming-.git
cd QGXS---quest-gstreamer-xr-streaming-
```

Build the native plugin:

```powershell
powershell -ExecutionPolicy Bypass `
    -File .\scripts\windows\build_native_plugin.ps1
```

Open the Unity project:

```text
receiver/windows/unity/GStreamerUnity
```

Enter Play Mode and launch the sender:

```powershell
powershell -ExecutionPolicy Bypass `
    -File .\scripts\windows\run_launcher.ps1
```

Same-machine connection:

```text
Receiver: 127.0.0.1
Port:     9001
```

## Ubuntu Unity

Extract these files from the release bundle:

```text
QSXR-v0.1.0-Ubuntu-Sender-GUI.tar.gz
QSXR-v0.1.0-Ubuntu-Unity-Receiver.zip
```

Launch Unity on the NVIDIA GPU:

```bash
UNITY="$HOME/Unity/Hub/Editor/2022.3.45f1/Editor/Unity"
PROJECT="/path/to/QSXR-v0.1.0-Ubuntu-Unity-Receiver"

prime-run "$UNITY" -projectPath "$PROJECT" -force-glcore
```

Open `Assets/Scenes/Main.unity`, enter Play Mode, and start the sender:

```bash
cd "/path/to/QSXR-v0.1.0-Ubuntu-Sender-GUI"
python3 launcher_gui_linux.py
```

Same-machine connection:

```text
Receiver: 127.0.0.1
Port:     9001
Feedback: 9101
```

Receiver controls:

| Key | Action |
|---|---|
| `F8` | Show or hide the HUD |
| `F9` | Pause or resume logging |
| `F10` | Save a diagnostic snapshot |

## Meta Quest 3

Requirements:

- Meta Quest 3 with Developer Mode
- ADB
- Ubuntu sender and Quest 3 on the same network

Install the APK:

```bash
adb install -r QSXR-v0.1.0-Quest3-Receiver.apk
```

Open QGXS on the headset and configure the Ubuntu sender:

```text
Receiver: Quest 3 IP address
Port:     9001
Codec:    H.264, H.265, or AV1
```

## Validated configurations

| Pipeline | Configuration |
|---|---|
| Ubuntu → Ubuntu Unity | 4096×2048 at 120 FPS |
| Ubuntu → Meta Quest 3 | 4096×2048 at 60 FPS with H.264, H.265, and AV1 |
| Windows → Windows Unity | H.264 and H.265 with D3D11 texture sharing |

## Repository structure

```text
QGXS/
├── launcher/
├── sender/
├── receiver/
│   ├── windows/
│   └── quest3/
├── scripts/
├── docs/
├── tools/
├── README.md
└── LICENSE
```

## License

Licensed under the [Apache License 2.0](LICENSE).

## Author

**Públio Elon Correa da Silva**
