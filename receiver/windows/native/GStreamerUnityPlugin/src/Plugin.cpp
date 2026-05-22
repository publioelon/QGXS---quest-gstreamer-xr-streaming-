#include <stdint.h>
#include <stdlib.h>
#include <string>
#include <time.h>
#include <d3d11.h>
#include <mutex>

#include "Utils.h"
#include "DebugCpp.h"
#include "GstApp.h"
#include "GstAppPipeline.h"
#include "../includes/IUnityInterface.h"
#include "../includes/IUnityGraphics.h"
#include "../includes/IUnityGraphicsD3D11.h"

std::mutex lock_;

UnityExport int GetDebugLog(char* message, int maxLength)
{
	return Debug::GetDebugLog(message, maxLength);
}

UnityExport void ClearDebugLog()
{
	Debug::ClearDebugLog();
}

UnityExport void* CreatePipeline(u32 id, u32 width, u32 height, char* uri, char* codec)
{
	return new GstAppPipeline(id, GstApp::_gstApp.get(), width, height, uri, codec);
}

UnityExport void DisposePipeline(void* pipelinePtr)
{
	if (pipelinePtr == nullptr)
	{
		return;
	}

	std::lock_guard<std::mutex> guard(lock_);

	GstAppPipeline* pipeline = (GstAppPipeline*)pipelinePtr;
	delete pipeline;
}

UnityExport void* GetPipelineTexturePtr(void* pipelinePtr)
{
	if (pipelinePtr == nullptr)
	{
		return nullptr;
	}

	GstAppPipeline* pipeline = (GstAppPipeline*)pipelinePtr;
	return pipeline->GetTexturePtr();
}

UnityExport unsigned long long GetPipelineCopiedFrameId(void* pipelinePtr)
{
	if (pipelinePtr == nullptr)
	{
		return 0;
	}

	GstAppPipeline* pipeline = (GstAppPipeline*)pipelinePtr;
	return pipeline->GetCopiedFrameId();
}

UnityExport unsigned long long GetPipelineLatestFrameId(void* pipelinePtr)
{
	if (pipelinePtr == nullptr)
	{
		return 0;
	}

	GstAppPipeline* pipeline = (GstAppPipeline*)pipelinePtr;
	return pipeline->GetLatestFrameId();
}

static void UNITY_INTERFACE_API OnRenderEvent(int eventID, void* dataPtr)
{
	if (dataPtr == nullptr)
	{
		return;
	}

	switch (eventID)
	{
	case EVENT_CREATE_TEXTURE:
	{
		std::lock_guard<std::mutex> guard(lock_);
		((GstAppPipeline*)dataPtr)->CreateTexture();
		break;
	}

	case EVENT_DISPOSE_TEXTURE:
	{
		GstAppPipeline::ReleaseTexture((ID3D11Texture2D*)dataPtr);
		break;
	}

	case 3:
	{
		((GstAppPipeline*)dataPtr)->CopyLatestSharedTextureToDisplayTextureOnRenderThread();
		break;
	}
	}
}

UnityExport UnityRenderingEventAndData UNITY_INTERFACE_API GetRenderEventFunc()
{
	return OnRenderEvent;
}

UnityExport void OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType)
{
	if (eventType == kUnityGfxDeviceEventShutdown && GstApp::_gstApp)
	{
		GstApp::_gstApp->Shutdown();
	}
}

UnityExport void UnityPluginLoad(IUnityInterfaces* interfaces)
{
	Debug::InitiateDebug();
	GstApp::_gstApp = std::make_unique<GstApp>(interfaces);
	GstApp::_gstApp->getGraphics()->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
}

UnityExport void UnityPluginUnload()
{
	if (GstApp::_gstApp)
	{
		GstApp::_gstApp->getGraphics()->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
		GstApp::_gstApp.reset();
	}

	Debug::ReleaseDebug();
}
