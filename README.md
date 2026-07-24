# QGXS - Quest GStreamer XR Streaming

QGXS is an open-source XR streaming project built with GStreamer, WebRTC, Unity, and native hardware-decoding APIs.

It provides low-latency video streaming paths for desktop and Meta Quest applications, including flat video and 360-degree equirectangular content.

## Features

- GStreamer WebRTC video sender
- Windows Unity receiver using native D3D11 texture sharing
- Meta Quest 3 receiver using Android MediaCodec
- H.264 and H.265 support on Windows
- H.264, H.265, and AV1 hardware decoding on Meta Quest 3
- Flat and 360-degree video rendering
- Automatic incoming-codec detection on Quest
- Runtime stream resolution and frame-rate configuration
- Runtime signaling-port configuration
- WebRTC reconnection without restarting the Quest application
- Native texture paths that avoid full-resolution CPU-side RGBA uploads

## Supported components

| Component | Status | Codecs |
|---|---|---|
| Windows GStreamer sender | Supported | H.264, H.265 |
| Windows Unity receiver | Supported | H.264, H.265 |
| Meta Quest 3 receiver | Supported | H.264, H.265, AV1 |
| Linux sender setup | Development | H.264, H.265, AV1 |
| Linux desktop receiver | Planned | — |

Codec support is receiver-specific. AV1 support on Meta Quest 3 does not mean that the Windows receiver currently supports AV1.

## Architecture

### Windows receiver

```text
GStreamer WebRTC
    -> native D3D11 texture-sharing plugin
    -> Unity texture
    -> flat or 360-degree renderer
```

### Meta Quest 3 receiver

```text
WebRTC RTP
    -> GStreamer depayloader and parser
    -> Android MediaCodec hardware decoder
    -> Android SurfaceTexture
    -> OpenGL external OES texture
    -> Unity 360-degree renderer
```

The Quest path avoids copying every decoded full-resolution frame through application CPU memory before rendering it in Unity.

## Repository structure

```text
QGXS---quest-gstreamer-xr-streaming-/
|
|-- launcher/
|   `-- launcher_gui.py
|
|-- sender/
|   |-- webrtc_sender.py
|   `-- webrtc_sender_backend/
|
|-- receiver/
|   |-- windows/
|   |   |-- unity/
|   |   `-- native/
|   |
|   `-- quest3/
|       |-- Native/jni/
|       |-- Unity/Assets/
|       `-- README.md
|
|-- scripts/
|-- docs/
|-- README.md
|-- LICENSE
`-- .gitignore
```

## Documentation

- [Windows quick start](docs/quick_start_windows.md)
- [Meta Quest 3 receiver setup](receiver/quest3/README.md)
- [Meta Quest 3 receiver architecture](docs/quest3_receiver.md)

## Current limitations

- A prebuilt Quest APK has not yet been published as a GitHub Release.
- Quest display refresh-rate selection is not yet applied automatically by the application.
- Receiver telemetry is not yet fully returned to the sender dashboard.
- Automatic bandwidth adaptation and congestion control are still under development.
- Linux desktop receiver support requires a separate graphics backend.

## Project status

The Windows and Meta Quest 3 receiver source paths are functional and included in the repository. Packaging, installation automation, adaptive streaming, and additional platform support are under active development.

## License

Licensed under the [Apache License 2.0](LICENSE).
