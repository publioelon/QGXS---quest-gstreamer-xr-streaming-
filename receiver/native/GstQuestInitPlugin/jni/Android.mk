LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := GstQuestInit
LOCAL_SRC_FILES := gst_quest_init.c

LOCAL_SHARED_LIBRARIES := gstreamer_android

LOCAL_LDLIBS := -llog -landroid -lGLESv3

include $(BUILD_SHARED_LIBRARY)

ifndef GSTREAMER_ROOT_ANDROID
$(error GSTREAMER_ROOT_ANDROID is not defined. Set it to your Android GStreamer arm64 folder, for example: C:/GStreamerAndroid/1.26.5/arm64)
endif

GSTREAMER_ROOT := $(GSTREAMER_ROOT_ANDROID)
GSTREAMER_NDK_BUILD_PATH := $(GSTREAMER_ROOT)/share/gst-android/ndk-build

GSTREAMER_PLUGINS := coreelements app playback videoparsersbad videoconvertscale androidmedia libav openh264 webrtc nice rtp rtpmanager rsrtp srtp dtls udp dav1d

GSTREAMER_EXTRA_DEPS := gstreamer-1.0 gstreamer-sdp-1.0 gstreamer-webrtc-1.0 gstreamer-video-1.0

include $(GSTREAMER_NDK_BUILD_PATH)/gstreamer-1.0.mk
