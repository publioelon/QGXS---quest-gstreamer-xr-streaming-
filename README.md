# QGXS - Quest GStreamer XR Streaming

QGXS is an open-source XR streaming research project built around GStreamer, WebRTC, Unity, and native hardware-decoding paths for real-time 2D and 360-degree video streaming.

The repository currently contains:

- a Python/GStreamer WebRTC sender;
- a Windows Unity receiver using native D3D11 texture sharing;
- a Meta Quest 3 Unity receiver using Android MediaCodec and an external OES texture;
- H.264 and H.265 support for the Windows receiver path;
- H.264, H.265, and AV1 hardware decoding for the validated Meta Quest 3 receiver path.

QGXS is intended as a practical foundation for low-latency XR streaming experiments, including 360-degree video delivery, codec evaluation, hardware decoding, bandwidth-aware streaming, congestion control, and viewport-aware adaptation.

---

## Repository status

The repository includes two native receiver architectures.

### Windows receiver path

```text
GStreamer WebRTC
    -> native D3D11 texture-sharing plugin
    -> Unity texture
    -> flat or 360-degree display
```

### Meta Quest 3 receiver path

```text
WebRTC RTP
    -> GStreamer depayloader and parser
    -> Android MediaCodec hardware decoder
    -> Android Surface and SurfaceTexture
    -> OpenGL external OES texture
    -> Unity 360-degree sphere
```

The Quest receiver avoids copying every decoded full-resolution frame through an application CPU-side RGBA buffer before rendering it in Unity.

This should be understood as an application-level GPU/display path rather than a claim that the entire SoC performs literally zero internal copies.

---

## Platform status

| Component | Status | Details |
|---|---|---|
| Windows GStreamer sender | Supported | Current desktop sender pipeline |
| Ubuntu/Linux sender | Development validated | Used during Quest receiver validation; packaging and setup documentation remain in progress |
| Windows Unity receiver | Supported | Native D3D11 texture-sharing path |
| Meta Quest 3 Android receiver | Validated Phase 3 | Native GStreamer, MediaCodec Surface decoding, and external OES rendering |
| Linux desktop receiver | Future work | A separate Linux graphics backend is still required |

## Codec status

| Codec | Windows Unity receiver | Meta Quest 3 receiver |
|---|---|---|
| H.264 | Supported | Validated hardware decoding |
| H.265 | Supported | Validated hardware decoding |
| AV1 | Not currently documented as supported | Validated hardware decoding |

Codec support is receiver-specific. AV1 support in the Quest path does not imply that every QGXS receiver supports AV1.

---

## Meta Quest 3 validated checkpoint

- Development commit: `cf008f47c0eba97ff7c69c1f779fbb318e92adb4`
- Development branch: `quest3-unified-receiver-phase3`
- Validation tag: `qsxr-quest3-phase3-validated-2026-07-24`
- Main integration commit: `cca01024659c9a943a11de06f63805aeb1c8b43f`
- Main integration tag: `qsxr-main-quest3-phase3-integrated-2026-07-24`

### Validated Quest functionality

- H.264 hardware decoding through Android MediaCodec;
- H.265 hardware decoding through Android MediaCodec;
- AV1 hardware decoding through Android MediaCodec;
- automatic incoming-codec detection;
- manual codec acceptance and rejection;
- runtime width and height configuration;
- runtime stream frame-rate configuration;
- runtime signaling-port selection;
- WebRTC session destruction and reconnection;
- repeated codec sessions without restarting the application;
- external OES texture rendering in Unity;
- 2048x1024 at 30 FPS while the Quest display operated at 90 Hz;
- 4096x2048 at 60 FPS with H.264, H.265, and AV1.

### Runtime Android intent arguments

| Argument | Type | Description |
|---|---:|---|
| `qsxr_codec` | string | `auto`, `h264`, `h265`, or `av1` |
| `qsxr_width` | integer | Decoder and OES texture width |
| `qsxr_height` | integer | Decoder and OES texture height |
| `qsxr_fps` | integer | Configured stream frame rate |
| `qsxr_av1_fps` | integer | Legacy AV1 frame-rate argument |
| `qsxr_signaling_port` | integer | WebRTC signaling port |
| `qsxr_refresh_hz` | float | Requested Quest display refresh rate |

---

## Known limitations

- The Quest refresh-rate argument is parsed, but the APK does not yet issue the final working native refresh-rate request.
- System-level 90 Hz operation was validated during development.
- The first statistics sample after some decoder resets may underflow.
- After rejecting a codec, some state fields may still describe the previously initialized decoder.
- Receiver telemetry is not yet fully returned to the sender dashboard.
- Encoder output bitrate may temporarily exceed the configured target.
- A packaged end-user Quest release and graphical installer are not yet published.

Generic NACK, RTX, and PLI must remain enabled in the WebRTC SDP. Removing PLI from the AV1 configuration caused severe recovery freezes during testing.

---

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
|       |-- Native/
|       |   `-- jni/
|       |       |-- gst_quest_init.c
|       |       |-- Android.mk
|       |       `-- Application.mk
|       |
|       |-- Unity/
|       |   `-- Assets/
|       |       |-- Scripts/
|       |       |-- Editor/
|       |       `-- Plugins/Android/libs/arm64-v8a/
|       |
|       |-- validation/
|       `-- README_PHASE3.md
|
|-- scripts/
|   `-- windows/
|
|-- docs/
|   |-- quick_start_windows.md
|   `-- quest3_phase3_validation.md
|
|-- README.md
|-- LICENSE
`-- .gitignore
```

---

## Documentation

- Windows quick start: [`docs/quick_start_windows.md`](docs/quick_start_windows.md)
- Quest receiver architecture and configuration: [`receiver/quest3/README_PHASE3.md`](receiver/quest3/README_PHASE3.md)
- Quest Phase 3 validation results: [`docs/quest3_phase3_validation.md`](docs/quest3_phase3_validation.md)

---

## Development status

QGXS is an active research and development project. The validated Quest receiver source is included, but installation, automated dependency setup, source-native configuration, receiver telemetry, congestion control, and packaged releases are still being developed.

## License

Licensed under the Apache License 2.0. See [`LICENSE`](LICENSE).
