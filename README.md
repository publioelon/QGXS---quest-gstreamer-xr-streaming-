# Quest GStreamer XR Streaming

This repository contains a Quest-native GStreamer receiver prototype integrated with Unity, plus a Windows desktop sender and launcher.

The project goal is to stream 360-degree video from a Windows sender to a Meta Quest 3 receiver while bypassing the codec limitations normally encountered in Unity Render Streaming.

## Repository layout

```text
launcher/
  Desktop launcher source files.

sender/
  Python sender backend and GStreamer/WebRTC sender code.

receiver/unity/QSXRReceiver/
  Unity receiver project files.

receiver/native/GstQuestInitPlugin/
  Android ARM64 native GStreamer plugin source.

tools/
  Helper scripts.

docs/
  Technical documentation.
```

## Codec status

H.264 is the stable baseline.

H.265 is an experimental optional path.

AV1 is experimental and may be slow depending on the Quest decoding path.

## Runtime dependencies

Bundled GStreamer runtimes and large binaries are intentionally not included in the source repository. They should be documented separately or distributed through releases/Git LFS if needed.
