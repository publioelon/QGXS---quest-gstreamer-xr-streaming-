# Meta Quest 3 Receiver Architecture

The Meta Quest 3 receiver combines Unity, GStreamer, WebRTC, Android MediaCodec, and OpenGL ES.

## Responsibilities

### Unity

- XR application lifecycle
- head tracking
- 360-degree sphere rendering
- materials and shaders
- runtime configuration
- application user interface

### Native GStreamer plugin

- WebRTC signaling
- RTP reception
- depayloading and parsing
- incoming-codec detection
- MediaCodec configuration
- decoder input and output handling
- SurfaceTexture and OES texture integration
- reconnection and native statistics

## Hardware decoding

The native plugin configures Android MediaCodec with an Android Surface as the decoder output.

Decoded frames are presented to a SurfaceTexture and exposed to Unity through an external OpenGL OES texture.

This avoids an application pipeline such as:

```text
decoder
    -> CPU YUV/RGBA buffer
    -> CPU memory copy
    -> Unity texture upload
```

The GPU samples the external texture when rendering the video on the Unity sphere.

## WebRTC recovery

NACK, RTX, and PLI are retained to support packet recovery and decoder resynchronization.

The receiver destroys disconnected WebRTC sessions and returns to the signaling accept loop without requiring an application restart.

## Future work

- automatic source resolution and frame-rate detection
- sender and receiver telemetry integration
- congestion control and bandwidth adaptation
- automatic display refresh-rate control
- packaged APK releases and installation tools
