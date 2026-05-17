# Architecture

The system has three parts: a Windows sender, a Quest-native Unity receiver, and a desktop launcher.

The sender is responsible for reading a local video file and sending encoded media through GStreamer/WebRTC.

The receiver is a Unity Android application that loads native ARM64 GStreamer libraries and displays the decoded 360-degree video in XR.

The launcher is the user-facing tool used to configure the sender, receiver IP address, codec, resolution, frame rate, and bitrate.
