# Meta Quest 3 Receiver

The QGXS Meta Quest 3 receiver uses a Unity application together with a native Android GStreamer plugin.

Compressed H.264, H.265, or AV1 video is received through WebRTC and decoded using Android MediaCodec hardware decoding.

## Video path

```text
WebRTC RTP
    -> GStreamer
    -> Android MediaCodec
    -> Android SurfaceTexture
    -> external OES texture
    -> Unity sphere material
```

The decoded image is not transferred to Unity as a full-resolution CPU-side RGBA array.

## Supported codecs

| Codec | Android MIME type | Status |
|---|---|---|
| H.264 | `video/avc` | Supported |
| H.265 | `video/hevc` | Supported |
| AV1 | `video/av01` | Supported |

## Requirements

- Meta Quest 3 with Developer Mode enabled
- Unity 2022.3 LTS
- Android ARM64 build target
- Android NDK
- GStreamer Android libraries
- ADB for installation and development
- Sender and headset connected to the same network

## Runtime options

| Argument | Type | Description |
|---|---:|---|
| `qsxr_codec` | string | `auto`, `h264`, `h265`, or `av1` |
| `qsxr_width` | integer | Stream and decoder width |
| `qsxr_height` | integer | Stream and decoder height |
| `qsxr_fps` | integer | Stream frame rate |
| `qsxr_signaling_port` | integer | WebRTC signaling port |
| `qsxr_refresh_hz` | float | Requested display refresh rate |

## Example launch

```bash
adb shell am start -S \
  -n com.publio.qsxr.unified/com.unity3d.player.UnityPlayerActivity \
  --es qsxr_codec auto \
  --ei qsxr_width 4096 \
  --ei qsxr_height 2048 \
  --ei qsxr_fps 60 \
  --ei qsxr_signaling_port 9001
```

## Native libraries

- `libGstQuestInit.so`
- `libgstreamer_android.so`
- `libc++_shared.so`

Unity loads the native plugin through C# `DllImport` calls.

## Limitations

- No prebuilt APK is currently published.
- The refresh-rate argument is parsed but is not yet applied automatically.
- Sender-to-receiver telemetry integration is still incomplete.
