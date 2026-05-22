#pragma once

#include "GstApp.h"
#include "Utils.h"

#include <gst/gst.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <mutex>
#include <string>

// A2f FIX16 direct ring no-flush prototype:
// GStreamer decodes into D3D11Memory, appsink receives the GPU texture,
// and the plugin copies it on-GPU into a ring of Unity-owned shared D3D11 textures.
// FIX16 displays by switching Unity external texture pointers to older ready ring entries.
// No render-thread copy, no event-query wait, and no explicit Flush in appsink.
class GstAppPipeline
{
private:
    static const int kTextureRingSize = 16;

    u32 id;
    const GstApp* app;
    GstElement* pipeline;
    GThread* pipelineLoopThread;
    GstBus* bus;
    u32 busWatchId;

    u32 textureWidth;
    u32 textureHeight;
    std::string codecName;

    ID3D11Texture2D* displayTexture;
    ID3D11Texture2D* unityTextures[kTextureRingSize];
    HANDLE sharedHandles[kTextureRingSize];
    IDXGIKeyedMutex* unityKeyedMutexes[kTextureRingSize];

    ID3D11Device* gstDevice;
    ID3D11DeviceContext* gstContext;
    ID3D11Texture2D* gstOpenedTextures[kTextureRingSize];
    IDXGIKeyedMutex* gstKeyedMutexes[kTextureRingSize];
    std::atomic<unsigned long long> frameIds[kTextureRingSize];

    std::mutex textureMutex;
    std::atomic<int> latestReadyTextureIndex;
    std::atomic<unsigned long long> latestFrameId;
    std::atomic<unsigned long long> copiedFrameId;
    std::atomic<unsigned long long> displayedFrameId;

    bool openedSharedTexturesOnGstDevice;
    bool loggedFirstFrame;

    bool useWebRTC;
    GstElement* webrtcbin;
    GThread* signalingThread;
    std::mutex signalingMutex;
    uintptr_t signalingClientSocket;
    int signalingPort;

    bool OpenSharedTexturesOnGstDevice(ID3D11Device* sourceDevice);

public:
    GMainLoop* mainLoop;

    GstAppPipeline(u32 id, const GstApp* app, u32 width, u32 height, char* uri, char* codec);
    ~GstAppPipeline();

    bool CreateTexture();
    ID3D11Texture2D* GetTexturePtr();

    bool CopyD3D11FrameToSharedTexture(ID3D11Resource* sourceResource, UINT sourceSubresource);
    bool CopyLatestSharedTextureToDisplayTextureOnRenderThread();
    ID3D11Texture2D* GetLatestReadyUnityTexturePtr();

    unsigned long long GetLatestFrameId() const;
    unsigned long long GetCopiedFrameId() const;

    bool SendSignalingLine(const std::string& line);
    void HandleRemoteOfferBase64(const std::string& offerBase64);
    void HandleRemoteIceBase64(int mline, const std::string& candidateBase64);

    static void ReleaseTexture(ID3D11Texture2D* texture);
};