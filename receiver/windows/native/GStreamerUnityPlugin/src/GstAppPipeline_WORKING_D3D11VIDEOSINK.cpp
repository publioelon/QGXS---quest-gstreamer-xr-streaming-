#include "GstAppPipeline.h"

#include <string>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <mutex>
#include <cstdint>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>
#include <dxgiformat.h>
#include <d3d11_1.h>

#include <glib-object.h>
#include <glib.h>

namespace
{
	std::mutex gDecodedFpsMutex;
	std::mutex gDrawFpsMutex;

	uint64_t gDecodedFrameCount = 0;
	uint64_t gDrawFrameCount = 0;

	std::chrono::steady_clock::time_point gDecodedLastTime = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point gDrawLastTime = std::chrono::steady_clock::now();

	void LogNativeFps(
		const char* label,
		std::mutex& mutex,
		uint64_t& frameCount,
		std::chrono::steady_clock::time_point& lastTime)
	{
		std::lock_guard<std::mutex> guard(mutex);

		frameCount++;

		auto now = std::chrono::steady_clock::now();
		double elapsed = std::chrono::duration<double>(now - lastTime).count();

		if (elapsed >= 1.0)
		{
			double fps = static_cast<double>(frameCount) / elapsed;

			std::stringstream ss;
			ss << label << ": " << std::fixed << std::setprecision(1) << fps;

			Debug::Log(ss.str());

			frameCount = 0;
			lastTime = now;
		}
	}

	static GstPadProbeReturn NativeDecodedFpsProbe(GstPad* pad, GstPadProbeInfo* info, gpointer data)
	{
		if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) == 0)
		{
			return GST_PAD_PROBE_OK;
		}

		LogNativeFps(
			"Native decoded/converted FPS",
			gDecodedFpsMutex,
			gDecodedFrameCount,
			gDecodedLastTime
		);

		return GST_PAD_PROBE_OK;
	}
}


gpointer GstMainLoopFunction(gpointer data)
{
	Debug::Log("Entering main loop");

	GstAppPipeline* pipeline = static_cast<GstAppPipeline*>(data);
	pipeline->mainLoop = g_main_loop_new(nullptr, FALSE);
	g_main_loop_run(pipeline->mainLoop);

	Debug::Log("Quitting main loop");

	return NULL;
}


static gboolean bus_call(GstBus* bus, GstMessage* msg, gpointer data)
{
	switch (GST_MESSAGE_TYPE(msg))
	{
	case GST_MESSAGE_EOS:
		Debug::Log("End of stream");
		return false;

	case GST_MESSAGE_ERROR:
	{
		gchar* debug = nullptr;
		GError* error = nullptr;

		gst_message_parse_error(msg, &error, &debug);

		if (debug != nullptr)
		{
			Debug::LogNoNewLine("Debug:");
			Debug::Log(debug);
			g_free(debug);
		}

		if (error != nullptr)
		{
			Debug::LogNoNewLine("Error:");
			Debug::Log(error->message);
			g_error_free(error);
		}

		return false;
	}

	default:
		Debug::LogNoNewLine("Received message of type:");
		Debug::Log(GST_MESSAGE_TYPE_NAME(msg));
		break;
	}

	return true;
}


static GstFlowReturn OnBeginDraw(GstElement* videosink, gpointer data)
{
	GstAppPipeline* pipeline = static_cast<GstAppPipeline*>(data);

	HANDLE sharedHandle = pipeline->GetSharedHandle();

	if (sharedHandle == nullptr)
	{
		return GST_FLOW_OK;
	}

	LogNativeFps(
		"Native texture draw FPS",
		gDrawFpsMutex,
		gDrawFrameCount,
		gDrawLastTime
	);

	g_signal_emit_by_name(
		videosink,
		"draw",
		(gpointer)sharedHandle,
		D3D11_RESOURCE_MISC_SHARED,
		0,
		0,
		nullptr
	);

	return GST_FLOW_OK;
}


GstAppPipeline::GstAppPipeline(u32 pipelineID, const GstApp* gstApp, u32 width, u32 height, char* uri)
	: id(pipelineID), app(gstApp), textureWidth(width), textureHeight(height)
{
	mainLoop = nullptr;
	pipelineLoopThread = nullptr;
	texture = nullptr;
	sharedHandle = nullptr;
	bus = nullptr;
	busWatchId = 0;

	Debug::LogNoNewLine("Creating GStreamer D3D11 RTP/H264 pipeline. URI=");
	Debug::Log(uri);

	auto pipelineName = std::string("udp-d3d11-decoder-") + std::to_string(id);
	pipeline = gst_pipeline_new(pipelineName.c_str());

	GstElement* udpsrc = gst_element_factory_make("udpsrc", NULL);
	GstElement* rtpjitterbuffer = gst_element_factory_make("rtpjitterbuffer", NULL);
	GstElement* rtph264depay = gst_element_factory_make("rtph264depay", NULL);
	GstElement* h264parse = gst_element_factory_make("h264parse", NULL);
	GstElement* d3d11h264dec = gst_element_factory_make("d3d11h264dec", NULL);
	GstElement* d3d11convert = gst_element_factory_make("d3d11convert", NULL);
	GstElement* d3d11videosink = gst_element_factory_make("d3d11videosink", NULL);

	if (!pipeline ||
		!udpsrc ||
		!rtpjitterbuffer ||
		!rtph264depay ||
		!h264parse ||
		!d3d11h264dec ||
		!d3d11convert ||
		!d3d11videosink)
	{
		Debug::Log("Failed to create all GStreamer elements.");

		if (!pipeline) Debug::Log("Missing element: pipeline");
		if (!udpsrc) Debug::Log("Missing element: udpsrc");
		if (!rtpjitterbuffer) Debug::Log("Missing element: rtpjitterbuffer");
		if (!rtph264depay) Debug::Log("Missing element: rtph264depay");
		if (!h264parse) Debug::Log("Missing element: h264parse");
		if (!d3d11h264dec) Debug::Log("Missing element: d3d11h264dec");
		if (!d3d11convert) Debug::Log("Missing element: d3d11convert");
		if (!d3d11videosink) Debug::Log("Missing element: d3d11videosink");

		if (pipeline)
		{
			gst_object_unref(pipeline);
			pipeline = nullptr;
		}

		return;
	}

	GstCaps* rtpCaps = gst_caps_from_string(
		"application/x-rtp, "
		"media=(string)video, "
		"encoding-name=(string)H264, "
		"payload=(int)96, "
		"clock-rate=(int)90000"
	);

	if (rtpCaps == nullptr)
	{
		Debug::Log("Failed to create RTP caps.");

		gst_object_unref(pipeline);
		pipeline = nullptr;
		return;
	}

	g_object_set(
		G_OBJECT(udpsrc),
		"uri",
		uri,
		"caps",
		rtpCaps,
		"buffer-size",
		67108864,
		NULL
	);

	gst_caps_unref(rtpCaps);

	g_object_set(
		G_OBJECT(rtpjitterbuffer),
		"latency",
		50,
		"drop-on-latency",
		TRUE,
		NULL
	);

	g_object_set(
		G_OBJECT(d3d11videosink),
		"draw-on-shared-texture",
		TRUE,
		NULL
	);

	g_signal_connect(
		G_OBJECT(d3d11videosink),
		"begin_draw",
		G_CALLBACK(OnBeginDraw),
		this
	);

	gst_bin_add_many(
		GST_BIN(pipeline),
		udpsrc,
		rtpjitterbuffer,
		rtph264depay,
		h264parse,
		d3d11h264dec,
		d3d11convert,
		d3d11videosink,
		NULL
	);

	if (!gst_element_link_many(
		udpsrc,
		rtpjitterbuffer,
		rtph264depay,
		h264parse,
		d3d11h264dec,
		d3d11convert,
		d3d11videosink,
		NULL))
	{
		Debug::Log("Elements could not be linked. Exiting.");

		gst_object_unref(pipeline);
		pipeline = nullptr;
		return;
	}

	GstPad* convertSrcPad = gst_element_get_static_pad(d3d11convert, "src");

	if (convertSrcPad != nullptr)
	{
		gst_pad_add_probe(
			convertSrcPad,
			GST_PAD_PROBE_TYPE_BUFFER,
			NativeDecodedFpsProbe,
			this,
			nullptr
		);

		gst_object_unref(convertSrcPad);

		Debug::Log("Native decoded FPS probe attached to d3d11convert src pad.");
	}
	else
	{
		Debug::Log("Could not attach native decoded FPS probe: d3d11convert src pad not found.");
	}

	bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
	busWatchId = gst_bus_add_watch(bus, bus_call, nullptr);
	gst_object_unref(bus);

	auto state = gst_element_set_state(pipeline, GstState::GST_STATE_PLAYING);

	if (state == GstStateChangeReturn::GST_STATE_CHANGE_FAILURE)
	{
		Debug::Log("Failed to set pipeline to PLAYING.");

		gst_object_unref(pipeline);
		pipeline = nullptr;
		return;
	}

	pipelineLoopThread = g_thread_new("GstUnityBridge Main Thread", GstMainLoopFunction, this);

	if (!pipelineLoopThread)
	{
		Debug::Log("Failed to create GLib main thread.");
		return;
	}

	Debug::Log("GStreamer D3D11 RTP/H264 pipeline started.");
}


GstAppPipeline::~GstAppPipeline()
{
	if (mainLoop != nullptr)
	{
		g_main_loop_quit(mainLoop);
		mainLoop = nullptr;
	}

	if (pipelineLoopThread != nullptr)
	{
		g_thread_join(pipelineLoopThread);
		pipelineLoopThread = nullptr;
	}

	if (pipeline != nullptr)
	{
		gst_element_set_state(pipeline, GstState::GST_STATE_NULL);
		gst_object_unref(pipeline);
		pipeline = nullptr;
	}
}


HANDLE GstAppPipeline::GetSharedHandle()
{
	return sharedHandle;
}


ID3D11Texture2D* GstAppPipeline::GetTexturePtr()
{
	return texture;
}


bool GstAppPipeline::CreateTexture()
{
	auto device = app->getD3D11Device();
	HRESULT hr = S_OK;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = textureWidth;
	desc.Height = textureHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

	hr = device->CreateTexture2D(&desc, nullptr, &texture);

	if (FAILED(hr))
	{
		Debug::Log("Could not create texture.");
		return false;
	}

	IDXGIResource* pDXGITexture = nullptr;

	hr = texture->QueryInterface(__uuidof(IDXGIResource), (void**)&pDXGITexture);

	if (FAILED(hr))
	{
		Debug::Log("Could not query IDXGIResource from texture.");
		return false;
	}

	HANDLE sharedHandle = nullptr;

	hr = pDXGITexture->GetSharedHandle(&sharedHandle);

	if (FAILED(hr))
	{
		Debug::Log("Could not create shared handle.");
		pDXGITexture->Release();
		return false;
	}

	pDXGITexture->Release();

	this->sharedHandle = sharedHandle;

	Debug::LogNoNewLine("Created texture for pipeline:");
	Debug::Log(static_cast<int>(id));

	return true;
}


void GstAppPipeline::ReleaseTexture(ID3D11Texture2D* texture)
{
	if (texture == nullptr)
	{
		return;
	}

	texture->Release();
	texture = nullptr;
}