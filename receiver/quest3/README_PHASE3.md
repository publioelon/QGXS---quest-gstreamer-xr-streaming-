# Meta Quest 3 Unified Receiver

This directory contains the native Meta Quest 3 receiver for QGXS.

## Validated checkpoint

- Commit: `cf008f47c0eba97ff7c69c1f779fbb318e92adb4`
- Branch: `quest3-unified-receiver-phase3`
- Tag: `qsxr-quest3-phase3-validated-2026-07-24`

## Architecture

```text
WebRTC RTP
    -> GStreamer depayloader and parser
    -> Android MediaCodec hardware decoder
    -> Android Surface and SurfaceTexture
    -> OpenGL external OES texture
    -> Unity 360-degree sphere
```

The decoded frames are not copied through a full-resolution CPU-side RGBA buffer before entering Unity.

## Supported codecs

- H.264 through Android MediaCodec `video/avc`
- H.265 through Android MediaCodec `video/hevc`
- AV1 through Android MediaCodec `video/av01`

The receiver can automatically detect the incoming codec or enforce H.264, H.265, or AV1 manually.

## Runtime intent arguments

| Argument | Type | Description |
|---|---:|---|
| `qsxr_codec` | string | `auto`, `h264`, `h265`, or `av1` |
| `qsxr_width` | integer | Decoder and texture width |
| `qsxr_height` | integer | Decoder and texture height |
| `qsxr_fps` | integer | Configured stream frame rate |
| `qsxr_av1_fps` | integer | Legacy AV1 frame-rate argument |
| `qsxr_signaling_port` | integer | WebRTC signaling port |
| `qsxr_refresh_hz` | float | Requested display refresh rate |

## Default configuration

- Codec: automatic
- Resolution: 4096x2048
- Stream frame rate: 60 FPS
- Signaling port: 9001
- Display refresh selection: automatic

## Validated functionality

- H.264, H.265, and AV1 hardware decoding
- Automatic codec detection
- Manual codec acceptance and rejection
- Runtime resolution and frame-rate configuration
- Runtime signaling-port selection
- WebRTC destruction and reconnection
- Repeated codec sessions without restarting the application
- External OES texture rendering in Unity
- 2048x1024 at 30 FPS with the Quest display operating at 90 Hz
- 4096x2048 at 60 FPS with H.264, H.265, and AV1

## Known limitations

- `qsxr_refresh_hz` is parsed, but the APK does not yet issue the final native refresh-rate request.
- Some first statistics samples after decoder resets may underflow.
- Rejected-codec telemetry may retain the previously initialized decoder state.
- Receiver telemetry is not yet returned to the sender dashboard.
- Sender encoder output can temporarily exceed the configured target bitrate.

## Important SDP behavior

Generic NACK, RTX, and PLI must remain enabled. Removing PLI from the AV1 SDP caused severe recovery freezes during testing.

## Native libraries

- `libGstQuestInit.so`
- `libgstreamer_android.so`
- `libc++_shared.so`

Unity calls the exported native functions through C# `DllImport`.
