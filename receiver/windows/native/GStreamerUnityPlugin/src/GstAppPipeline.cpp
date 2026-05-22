#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include "GstAppPipeline.h"

#include <string>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <mutex>
#include <cstdint>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <memory>
#include <atomic>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/gstsdpmessage.h>

#include <gst/d3d11/gstd3d11.h>
#include <gst/d3d11/gstd3d11memory.h>

#include <d3d11.h>
#include <dxgi.h>
#include <dxgiformat.h>

#include <glib-object.h>
#include <glib.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "gstwebrtc-1.0.lib")
#pragma comment(lib, "gstsdp-1.0.lib")

namespace
{
	std::mutex gDecodedFpsMutex;
	std::mutex gCopyFpsMutex;

	uint64_t gDecodedFrameCount = 0;
	uint64_t gCopiedFrameCount = 0;

	std::chrono::steady_clock::time_point gDecodedLastTime = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point gCopyLastTime = std::chrono::steady_clock::now();

	struct WebRtcRuntime
	{
		GstElement* webrtcbin = nullptr;
		GThread* signalingThread = nullptr;

		SOCKET serverSocket = INVALID_SOCKET;
		SOCKET clientSocket = INVALID_SOCKET;

		int signalingPort = 9001;
		std::mutex socketMutex;
		std::atomic<bool> stopping{ false };
	};

	std::mutex gWebRtcRuntimeMutex;
	std::unordered_map<GstAppPipeline*, std::unique_ptr<WebRtcRuntime>> gWebRtcRuntimes;

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
			"A2f FIX16 DIRECT RING NO FLUSH native decoded/converted FPS",
			gDecodedFpsMutex,
			gDecodedFrameCount,
			gDecodedLastTime
		);

		return GST_PAD_PROBE_OK;
	}

	std::string LowerString(const char* value)
	{
		if (value == nullptr)
		{
			return "h264";
		}

		std::string result(value);
		std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});

		return result;
	}

	bool StartsWith(const std::string& value, const std::string& prefix)
	{
		return value.rfind(prefix, 0) == 0;
	}

	bool IsWebRtcUri(const char* uri)
	{
		if (uri == nullptr)
		{
			return false;
		}

		return StartsWith(std::string(uri), "webrtc://");
	}

	int ParsePortFromWebRtcUri(const char* uri)
	{
		if (uri == nullptr)
		{
			return 9001;
		}

		std::string value(uri);
		const std::string prefix = "webrtc://";

		if (!StartsWith(value, prefix))
		{
			return 9001;
		}

		std::string hostPort = value.substr(prefix.size());
		size_t colon = hostPort.rfind(':');

		if (colon == std::string::npos)
		{
			return 9001;
		}

		try
		{
			return std::stoi(hostPort.substr(colon + 1));
		}
		catch (...)
		{
			return 9001;
		}
	}

	WebRtcRuntime* CreateWebRtcRuntime(GstAppPipeline* pipeline, int port)
	{
		if (pipeline == nullptr)
		{
			return nullptr;
		}

		std::lock_guard<std::mutex> guard(gWebRtcRuntimeMutex);

		auto runtime = std::make_unique<WebRtcRuntime>();
		runtime->signalingPort = port;

		WebRtcRuntime* ptr = runtime.get();
		gWebRtcRuntimes[pipeline] = std::move(runtime);

		return ptr;
	}

	WebRtcRuntime* GetWebRtcRuntime(GstAppPipeline* pipeline)
	{
		std::lock_guard<std::mutex> guard(gWebRtcRuntimeMutex);

		auto it = gWebRtcRuntimes.find(pipeline);
		if (it == gWebRtcRuntimes.end())
		{
			return nullptr;
		}

		return it->second.get();
	}

	std::unique_ptr<WebRtcRuntime> RemoveWebRtcRuntime(GstAppPipeline* pipeline)
	{
		std::lock_guard<std::mutex> guard(gWebRtcRuntimeMutex);

		auto it = gWebRtcRuntimes.find(pipeline);
		if (it == gWebRtcRuntimes.end())
		{
			return nullptr;
		}

		std::unique_ptr<WebRtcRuntime> runtime = std::move(it->second);
		gWebRtcRuntimes.erase(it);

		return runtime;
	}

	std::string Base64EncodeString(const std::string& text)
	{
		gchar* encoded = g_base64_encode(
			reinterpret_cast<const guchar*>(text.data()),
			static_cast<gsize>(text.size())
		);

		if (encoded == nullptr)
		{
			return "";
		}

		std::string result(encoded);
		g_free(encoded);
		return result;
	}

	std::string Base64DecodeString(const std::string& text)
	{
		gsize outLen = 0;
		guchar* decoded = g_base64_decode(text.c_str(), &outLen);

		if (decoded == nullptr)
		{
			return "";
		}

		std::string result(reinterpret_cast<char*>(decoded), static_cast<size_t>(outLen));
		g_free(decoded);
		return result;
	}

	bool SendSignalingLine(GstAppPipeline* pipeline, const std::string& line)
	{
		WebRtcRuntime* runtime = GetWebRtcRuntime(pipeline);
		if (runtime == nullptr)
		{
			return false;
		}

		std::lock_guard<std::mutex> guard(runtime->socketMutex);

		if (runtime->clientSocket == INVALID_SOCKET)
		{
			return false;
		}

		std::string msg = line + "\n";
		const char* data = msg.c_str();
		int total = static_cast<int>(msg.size());
		int sentTotal = 0;

		while (sentTotal < total)
		{
			int sent = send(runtime->clientSocket, data + sentTotal, total - sentTotal, 0);

			if (sent <= 0)
			{
				return false;
			}

			sentTotal += sent;
		}

		return true;
	}

	static void OnWebRtcAnswerCreated(GstPromise* promise, gpointer userData)
	{
		GstAppPipeline* self = static_cast<GstAppPipeline*>(userData);
		WebRtcRuntime* runtime = GetWebRtcRuntime(self);

		if (runtime == nullptr || runtime->webrtcbin == nullptr)
		{
			Debug::Log("WEBRTC: answer callback without runtime/webrtcbin.");
			gst_promise_unref(promise);
			return;
		}

		const GstStructure* reply = gst_promise_get_reply(promise);
		GstWebRTCSessionDescription* answer = nullptr;

		if (reply != nullptr)
		{
			gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
		}

		gst_promise_unref(promise);

		if (answer == nullptr)
		{
			Debug::Log("WEBRTC: failed to create SDP answer.");
			return;
		}

		GstPromise* localPromise = gst_promise_new();
		g_signal_emit_by_name(runtime->webrtcbin, "set-local-description", answer, localPromise);
		gst_promise_interrupt(localPromise);
		gst_promise_unref(localPromise);

		gchar* sdpText = gst_sdp_message_as_text(answer->sdp);

		if (sdpText != nullptr)
		{
			std::string answerMessage = std::string("ANSWER|") + Base64EncodeString(sdpText);
			SendSignalingLine(self, answerMessage);

			Debug::Log("WEBRTC: sent SDP answer.");
			g_free(sdpText);
		}
		else
		{
			Debug::Log("WEBRTC: could not convert SDP answer to text.");
		}

		gst_webrtc_session_description_free(answer);
	}

	static void OnWebRtcIceCandidate(GstElement* webrtc, guint mlineIndex, const gchar* candidate, gpointer userData)
	{
		GstAppPipeline* self = static_cast<GstAppPipeline*>(userData);

		if (candidate == nullptr)
		{
			return;
		}

		std::string msg =
			std::string("ICE|") +
			std::to_string(static_cast<int>(mlineIndex)) +
			"|" +
			Base64EncodeString(candidate);

		SendSignalingLine(self, msg);
		Debug::Log("WEBRTC: sent local ICE candidate.");
	}

	static void OnWebRtcPadAdded(GstElement* webrtc, GstPad* newPad, gpointer userData)
	{
		GstElement* depay = static_cast<GstElement*>(userData);

		if (depay == nullptr)
		{
			Debug::Log("WEBRTC: pad-added without depay element.");
			return;
		}

		GstPad* sinkPad = gst_element_get_static_pad(depay, "sink");

		if (sinkPad == nullptr)
		{
			Debug::Log("WEBRTC: depay sink pad not found.");
			return;
		}

		if (gst_pad_is_linked(sinkPad))
		{
			Debug::Log("WEBRTC: depay sink pad already linked.");
			gst_object_unref(sinkPad);
			return;
		}

		GstCaps* caps = gst_pad_get_current_caps(newPad);
		if (caps == nullptr)
		{
			caps = gst_pad_query_caps(newPad, nullptr);
		}

		bool isRtp = false;

		if (caps != nullptr)
		{
			gchar* capsText = gst_caps_to_string(caps);
			if (capsText != nullptr)
			{
				Debug::LogNoNewLine("WEBRTC: pad-added caps: ");
				Debug::Log(capsText);
				g_free(capsText);
			}

			GstStructure* st = gst_caps_get_structure(caps, 0);
			if (st != nullptr)
			{
				const gchar* name = gst_structure_get_name(st);
				if (name != nullptr && std::string(name) == "application/x-rtp")
				{
					isRtp = true;
				}
			}

			gst_caps_unref(caps);
		}

		if (!isRtp)
		{
			Debug::Log("WEBRTC: ignoring non-RTP pad.");
			gst_object_unref(sinkPad);
			return;
		}

		GstPadLinkReturn ret = gst_pad_link(newPad, sinkPad);

		if (GST_PAD_LINK_FAILED(ret))
		{
			Debug::Log("WEBRTC: failed to link webrtcbin src pad to depay.");
		}
		else
		{
			Debug::Log("WEBRTC: linked webrtcbin src pad to depay.");
		}

		gst_object_unref(sinkPad);
	}

	void HandleRemoteOfferBase64(GstAppPipeline* pipeline, const std::string& offerBase64)
	{
		WebRtcRuntime* runtime = GetWebRtcRuntime(pipeline);

		if (runtime == nullptr || runtime->webrtcbin == nullptr)
		{
			Debug::Log("WEBRTC: cannot handle offer because runtime/webrtcbin is null.");
			return;
		}

		std::string offerText = Base64DecodeString(offerBase64);

		if (offerText.empty())
		{
			Debug::Log("WEBRTC: decoded offer is empty.");
			return;
		}

		GstSDPMessage* sdp = nullptr;
		GstSDPResult result = gst_sdp_message_new(&sdp);

		if (result != GST_SDP_OK || sdp == nullptr)
		{
			Debug::Log("WEBRTC: gst_sdp_message_new failed.");
			return;
		}

		result = gst_sdp_message_parse_buffer(
			reinterpret_cast<const guint8*>(offerText.data()),
			static_cast<guint>(offerText.size()),
			sdp
		);

		if (result != GST_SDP_OK)
		{
			Debug::Log("WEBRTC: SDP offer parse failed.");
			gst_sdp_message_free(sdp);
			return;
		}

		GstWebRTCSessionDescription* offer =
			gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);

		if (offer == nullptr)
		{
			Debug::Log("WEBRTC: failed to create WebRTC offer description.");
			gst_sdp_message_free(sdp);
			return;
		}

		GstPromise* remotePromise = gst_promise_new();
		g_signal_emit_by_name(runtime->webrtcbin, "set-remote-description", offer, remotePromise);
		gst_promise_interrupt(remotePromise);
		gst_promise_unref(remotePromise);

		gst_webrtc_session_description_free(offer);

		GstPromise* answerPromise = gst_promise_new_with_change_func(OnWebRtcAnswerCreated, pipeline, nullptr);
		g_signal_emit_by_name(runtime->webrtcbin, "create-answer", nullptr, answerPromise);

		Debug::Log("WEBRTC: remote offer set; answer requested.");
	}

	void HandleRemoteIceBase64(GstAppPipeline* pipeline, int mline, const std::string& candidateBase64)
	{
		WebRtcRuntime* runtime = GetWebRtcRuntime(pipeline);

		if (runtime == nullptr || runtime->webrtcbin == nullptr)
		{
			return;
		}

		std::string candidate = Base64DecodeString(candidateBase64);

		if (candidate.empty())
		{
			return;
		}

		g_signal_emit_by_name(
			runtime->webrtcbin,
			"add-ice-candidate",
			static_cast<guint>(mline),
			candidate.c_str()
		);

		Debug::Log("WEBRTC: added remote ICE candidate.");
	}

	std::string ReadLineFromSocket(SOCKET sock)
	{
		std::string line;
		char c = 0;

		while (true)
		{
			int r = recv(sock, &c, 1, 0);

			if (r <= 0)
			{
				return "";
			}

			if (c == '\n')
			{
				break;
			}

			if (c != '\r')
			{
				line.push_back(c);
			}
		}

		return line;
	}

	gpointer WebRtcSignalingThread(gpointer data)
	{
		GstAppPipeline* self = static_cast<GstAppPipeline*>(data);
		WebRtcRuntime* runtime = GetWebRtcRuntime(self);

		if (runtime == nullptr)
		{
			Debug::Log("WEBRTC: signaling thread started without runtime.");
			return nullptr;
		}

		WSADATA wsaData = {};
		int wsaOk = WSAStartup(MAKEWORD(2, 2), &wsaData);

		if (wsaOk != 0)
		{
			Debug::Log("WEBRTC: WSAStartup failed.");
			return nullptr;
		}

		SOCKET serverSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

		if (serverSock == INVALID_SOCKET)
		{
			Debug::Log("WEBRTC: socket creation failed.");
			WSACleanup();
			return nullptr;
		}

		{
			std::lock_guard<std::mutex> guard(runtime->socketMutex);
			runtime->serverSocket = serverSock;
		}

		int opt = 1;
		setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		InetPtonA(AF_INET, "127.0.0.1", &addr.sin_addr);
		addr.sin_port = htons(static_cast<u_short>(runtime->signalingPort));

		if (bind(serverSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
		{
			Debug::Log("WEBRTC: bind failed. Is port already in use?");

			{
				std::lock_guard<std::mutex> guard(runtime->socketMutex);
				runtime->serverSocket = INVALID_SOCKET;
			}

			closesocket(serverSock);
			WSACleanup();
			return nullptr;
		}

		if (listen(serverSock, 1) == SOCKET_ERROR)
		{
			Debug::Log("WEBRTC: listen failed.");

			{
				std::lock_guard<std::mutex> guard(runtime->socketMutex);
				runtime->serverSocket = INVALID_SOCKET;
			}

			closesocket(serverSock);
			WSACleanup();
			return nullptr;
		}

		Debug::LogNoNewLine("WEBRTC: signaling server listening on 127.0.0.1:");
		Debug::Log(runtime->signalingPort);

		SOCKET clientSock = accept(serverSock, nullptr, nullptr);

		if (clientSock == INVALID_SOCKET)
		{
			if (!runtime->stopping.load())
			{
				Debug::Log("WEBRTC: accept failed.");
			}

			{
				std::lock_guard<std::mutex> guard(runtime->socketMutex);
				runtime->serverSocket = INVALID_SOCKET;
			}

			closesocket(serverSock);
			WSACleanup();
			return nullptr;
		}

		{
			std::lock_guard<std::mutex> guard(runtime->socketMutex);
			runtime->clientSocket = clientSock;
		}

		Debug::Log("WEBRTC: signaling client connected.");

		while (!runtime->stopping.load())
		{
			std::string line = ReadLineFromSocket(clientSock);

			if (line.empty())
			{
				if (!runtime->stopping.load())
				{
					Debug::Log("WEBRTC: signaling client disconnected.");
				}
				break;
			}

			if (StartsWith(line, "OFFER|"))
			{
				std::string offer64 = line.substr(6);
				Debug::Log("WEBRTC: received SDP offer.");
				HandleRemoteOfferBase64(self, offer64);
			}
			else if (StartsWith(line, "ICE|"))
			{
				size_t first = line.find('|');
				size_t second = line.find('|', first + 1);

				if (first != std::string::npos && second != std::string::npos)
				{
					try
					{
						int mline = std::stoi(line.substr(first + 1, second - first - 1));
						std::string candidate64 = line.substr(second + 1);
						HandleRemoteIceBase64(self, mline, candidate64);
					}
					catch (...)
					{
						Debug::Log("WEBRTC: failed to parse ICE signaling line.");
					}
				}
			}
			else
			{
				Debug::LogNoNewLine("WEBRTC: unknown signaling line: ");
				Debug::Log(line);
			}
		}

		{
			std::lock_guard<std::mutex> guard(runtime->socketMutex);

			if (runtime->clientSocket != INVALID_SOCKET)
			{
				closesocket(runtime->clientSocket);
				runtime->clientSocket = INVALID_SOCKET;
			}

			if (runtime->serverSocket != INVALID_SOCKET)
			{
				closesocket(runtime->serverSocket);
				runtime->serverSocket = INVALID_SOCKET;
			}
		}

		WSACleanup();

		Debug::Log("WEBRTC: signaling thread finished.");

		return nullptr;
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


static GstFlowReturn OnNewSample(GstElement* appsink, gpointer data)
{
	GstAppPipeline* pipeline = static_cast<GstAppPipeline*>(data);
	(void)pipeline;

	GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink));
	if (sample == nullptr)
	{
		return GST_FLOW_OK;
	}

	GstBuffer* buffer = gst_sample_get_buffer(sample);
	if (buffer == nullptr || gst_buffer_n_memory(buffer) == 0)
	{
		gst_sample_unref(sample);
		return GST_FLOW_OK;
	}

	GstMemory* memory = gst_buffer_peek_memory(buffer, 0);
	if (memory == nullptr)
	{
		gst_sample_unref(sample);
		return GST_FLOW_OK;
	}

	static bool loggedFirstSample = false;
	if (!loggedFirstSample)
	{
		GstCaps* caps = gst_sample_get_caps(sample);
		if (caps != nullptr)
		{
			gchar* capsString = gst_caps_to_string(caps);
			if (capsString != nullptr)
			{
				Debug::LogNoNewLine("A2f FIX16 DIRECT RING NO FLUSH first appsink caps: ");
				Debug::Log(capsString);
				g_free(capsString);
			}
		}

		Debug::LogNoNewLine("A2f FIX16 DIRECT RING NO FLUSH memory is D3D11: ");
		Debug::Log(gst_is_d3d11_memory(memory));
	}

	if (gst_is_d3d11_memory(memory))
	{
		GstD3D11Memory* d3d11Memory = (GstD3D11Memory*)memory;
		ID3D11Resource* sourceResource = (ID3D11Resource*)gst_d3d11_memory_get_resource_handle(d3d11Memory);
		UINT sourceSubresource = gst_d3d11_memory_get_subresource_index(d3d11Memory);

		if (!loggedFirstSample)
		{
			if (sourceResource == nullptr)
			{
				Debug::Log("A2f FIX16 DIRECT RING NO FLUSH resource handle is NULL.");
			}
			else
			{
				ID3D11Texture2D* sourceTexture2D = nullptr;
				HRESULT qiHr = sourceResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&sourceTexture2D);

				if (SUCCEEDED(qiHr) && sourceTexture2D != nullptr)
				{
					D3D11_TEXTURE2D_DESC sourceDesc = {};
					sourceTexture2D->GetDesc(&sourceDesc);

					ID3D11Device* sourceDevice = nullptr;
					sourceTexture2D->GetDevice(&sourceDevice);

					std::stringstream ss;
					ss << "A2f FIX16 DIRECT RING NO FLUSH source texture: "
						<< sourceDesc.Width << "x" << sourceDesc.Height
						<< " format=" << static_cast<unsigned int>(sourceDesc.Format)
						<< " misc=" << sourceDesc.MiscFlags
						<< " bind=" << sourceDesc.BindFlags
						<< " usage=" << sourceDesc.Usage
						<< " array=" << sourceDesc.ArraySize
						<< " mips=" << sourceDesc.MipLevels
						<< " sampleCount=" << sourceDesc.SampleDesc.Count
						<< " subresource=" << sourceSubresource
						<< " device=" << sourceDevice;

					Debug::Log(ss.str());

					if (sourceDevice != nullptr)
					{
						sourceDevice->Release();
					}

					sourceTexture2D->Release();
				}
				else
				{
					Debug::Log("A2f FIX16 DIRECT RING NO FLUSH source resource is not ID3D11Texture2D.");
				}
			}

			Debug::Log("A2f FIX16 DIRECT RING NO FLUSH will copy into opened Unity shared textures and display lagged ring entries directly.");
			loggedFirstSample = true;
		}

		if (sourceResource != nullptr)
		{
			pipeline->CopyD3D11FrameToSharedTexture(sourceResource, sourceSubresource);
		}
	}
	else if (!loggedFirstSample)
	{
		Debug::Log("A2f FIX16 DIRECT RING NO FLUSH received non-D3D11 memory.");
		loggedFirstSample = true;
	}

	LogNativeFps(
		"A2f FIX16 DIRECT RING NO FLUSH appsink callback FPS",
		gCopyFpsMutex,
		gCopiedFrameCount,
		gCopyLastTime
	);

	gst_sample_unref(sample);
	return GST_FLOW_OK;
}


GstAppPipeline::GstAppPipeline(u32 pipelineID, const GstApp* gstApp, u32 width, u32 height, char* uri, char* codec)
	: id(pipelineID),
	app(gstApp),
	pipeline(nullptr),
	pipelineLoopThread(nullptr),
	bus(nullptr),
	busWatchId(0),
	textureWidth(width),
	textureHeight(height),
	codecName(LowerString(codec)),
	gstDevice(nullptr),
	gstContext(nullptr),
	latestReadyTextureIndex(-1),
	latestFrameId(0),
	copiedFrameId(0),
	openedSharedTexturesOnGstDevice(false),
	loggedFirstFrame(false)
{
	mainLoop = nullptr;
	displayTexture = nullptr;
	displayedFrameId.store(0);

	for (int i = 0; i < kTextureRingSize; ++i)
	{
		unityTextures[i] = nullptr;
		sharedHandles[i] = nullptr;
		unityKeyedMutexes[i] = nullptr;
		gstOpenedTextures[i] = nullptr;
		gstKeyedMutexes[i] = nullptr;
		frameIds[i].store(0);
	}

	bool useH265 = codecName == "h265" || codecName == "hevc";
	bool useWebRTC = IsWebRtcUri(uri);

	const char* depayFactory = useH265 ? "rtph265depay" : "rtph264depay";
	const char* parserFactory = useH265 ? "h265parse" : "h264parse";
	const char* decoderFactory = useH265 ? "d3d11h265dec" : "d3d11h264dec";
	const char* encodingName = useH265 ? "H265" : "H264";

	WebRtcRuntime* webRtcRuntime = nullptr;

	if (useWebRTC)
	{
		webRtcRuntime = CreateWebRtcRuntime(this, ParsePortFromWebRtcUri(uri));
	}

	Debug::LogNoNewLine("Creating A2f D3D11 appsink pipeline. URI=");
	Debug::Log(uri);
	Debug::LogNoNewLine("A2f codec=");
	Debug::Log(codecName);
	Debug::LogNoNewLine("A2f transport=");
	Debug::Log(useWebRTC ? "webrtc" : "udp");

	auto pipelineName =
		std::string(useWebRTC ? "webrtc-a2f-d3d11-appsink-" : "udp-a2f-d3d11-appsink-")
		+ std::to_string(id);

	pipeline = gst_pipeline_new(pipelineName.c_str());

	GstElement* udpsrc = nullptr;
	GstElement* rtpjitterbuffer = nullptr;
	GstElement* webrtcbin = nullptr;

	if (useWebRTC)
	{
		webrtcbin = gst_element_factory_make("webrtcbin", "webrtcbin");
		if (webRtcRuntime != nullptr)
		{
			webRtcRuntime->webrtcbin = webrtcbin;
		}
	}
	else
	{
		udpsrc = gst_element_factory_make("udpsrc", NULL);
		rtpjitterbuffer = gst_element_factory_make("rtpjitterbuffer", NULL);
	}

	GstElement* depay = gst_element_factory_make(depayFactory, NULL);
	GstElement* parser = gst_element_factory_make(parserFactory, NULL);
	GstElement* decoder = gst_element_factory_make(decoderFactory, NULL);
	GstElement* d3d11convert = gst_element_factory_make("d3d11convert", NULL);
	GstElement* capsfilter = gst_element_factory_make("capsfilter", NULL);
	GstElement* queue = gst_element_factory_make("queue", NULL);
	GstElement* appsink = gst_element_factory_make("appsink", NULL);

	if (!pipeline ||
		(!useWebRTC && !udpsrc) ||
		(!useWebRTC && !rtpjitterbuffer) ||
		(useWebRTC && !webrtcbin) ||
		!depay ||
		!parser ||
		!decoder ||
		!d3d11convert ||
		!capsfilter ||
		!queue ||
		!appsink)
	{
		Debug::Log("A2f failed to create all GStreamer elements.");

		if (!pipeline) Debug::Log("Missing element: pipeline");
		if (!useWebRTC && !udpsrc) Debug::Log("Missing element: udpsrc");
		if (!useWebRTC && !rtpjitterbuffer) Debug::Log("Missing element: rtpjitterbuffer");
		if (useWebRTC && !webrtcbin) Debug::Log("Missing element: webrtcbin");
		if (!depay) Debug::LogNoNewLine("Missing element: "), Debug::Log(depayFactory);
		if (!parser) Debug::LogNoNewLine("Missing element: "), Debug::Log(parserFactory);
		if (!decoder) Debug::LogNoNewLine("Missing element: "), Debug::Log(decoderFactory);
		if (!d3d11convert) Debug::Log("Missing element: d3d11convert");
		if (!capsfilter) Debug::Log("Missing element: capsfilter");
		if (!queue) Debug::Log("Missing element: queue");
		if (!appsink) Debug::Log("Missing element: appsink");

		if (pipeline)
		{
			gst_object_unref(pipeline);
			pipeline = nullptr;
		}

		if (useWebRTC)
		{
			RemoveWebRtcRuntime(this);
		}

		return;
	}

	if (!useWebRTC)
	{
		std::string rtpCapsString =
			std::string("application/x-rtp, ")
			+ "media=(string)video, "
			+ "encoding-name=(string)" + encodingName + ", "
			+ "payload=(int)96, "
			+ "clock-rate=(int)90000";

		GstCaps* rtpCaps = gst_caps_from_string(rtpCapsString.c_str());

		if (rtpCaps == nullptr)
		{
			Debug::Log("A2f failed to create RTP caps.");

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
			20,
			"drop-on-latency",
			TRUE,
			NULL
		);
	}
	else
	{
		g_object_set(
			G_OBJECT(webrtcbin),
			"bundle-policy",
			3,
			"latency",
			20,
			NULL
		);

		g_signal_connect(G_OBJECT(webrtcbin), "on-ice-candidate", G_CALLBACK(OnWebRtcIceCandidate), this);
		g_signal_connect(G_OBJECT(webrtcbin), "pad-added", G_CALLBACK(OnWebRtcPadAdded), depay);
	}

	GstCaps* appSinkCaps = gst_caps_from_string(
		"video/x-raw(memory:D3D11Memory), "
		"format=(string)BGRA"
	);

	if (appSinkCaps == nullptr)
	{
		Debug::Log("A2f failed to create appsink caps.");

		gst_object_unref(pipeline);
		pipeline = nullptr;

		if (useWebRTC)
		{
			RemoveWebRtcRuntime(this);
		}

		return;
	}

	g_object_set(G_OBJECT(capsfilter), "caps", appSinkCaps, NULL);
	gst_caps_unref(appSinkCaps);

	g_object_set(
		G_OBJECT(queue),
		"max-size-buffers",
		2,
		"max-size-bytes",
		0,
		"max-size-time",
		0,
		"leaky",
		2,
		NULL
	);

	g_object_set(
		G_OBJECT(appsink),
		"emit-signals",
		TRUE,
		"sync",
		FALSE,
		"max-buffers",
		2,
		"drop",
		TRUE,
		NULL
	);

	g_signal_connect(G_OBJECT(appsink), "new-sample", G_CALLBACK(OnNewSample), this);

	if (useWebRTC)
	{
		gst_bin_add_many(
			GST_BIN(pipeline),
			webrtcbin,
			depay,
			parser,
			decoder,
			d3d11convert,
			capsfilter,
			queue,
			appsink,
			NULL
		);
	}
	else
	{
		gst_bin_add_many(
			GST_BIN(pipeline),
			udpsrc,
			rtpjitterbuffer,
			depay,
			parser,
			decoder,
			d3d11convert,
			capsfilter,
			queue,
			appsink,
			NULL
		);
	}

	bool linked = false;

	if (useWebRTC)
	{
		linked = gst_element_link_many(
			depay,
			parser,
			decoder,
			d3d11convert,
			capsfilter,
			queue,
			appsink,
			NULL
		);
	}
	else
	{
		linked = gst_element_link_many(
			udpsrc,
			rtpjitterbuffer,
			depay,
			parser,
			decoder,
			d3d11convert,
			capsfilter,
			queue,
			appsink,
			NULL
		);
	}

	if (!linked)
	{
		Debug::Log("A2f elements could not be linked. Exiting.");

		gst_object_unref(pipeline);
		pipeline = nullptr;

		if (useWebRTC)
		{
			RemoveWebRtcRuntime(this);
		}

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

		Debug::Log("A2f native decoded FPS probe attached to d3d11convert src pad.");
	}
	else
	{
		Debug::Log("A2f could not attach native decoded FPS probe: d3d11convert src pad not found.");
	}

	bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
	busWatchId = gst_bus_add_watch(bus, bus_call, nullptr);
	gst_object_unref(bus);

	auto state = gst_element_set_state(pipeline, GstState::GST_STATE_PLAYING);

	if (state == GstStateChangeReturn::GST_STATE_CHANGE_FAILURE)
	{
		Debug::Log("A2f failed to set pipeline to PLAYING.");

		gst_object_unref(pipeline);
		pipeline = nullptr;

		if (useWebRTC)
		{
			RemoveWebRtcRuntime(this);
		}

		return;
	}

	pipelineLoopThread = g_thread_new("GstUnityA2f Main Thread", GstMainLoopFunction, this);

	if (!pipelineLoopThread)
	{
		Debug::Log("A2f failed to create GLib main thread.");
		return;
	}

	if (useWebRTC && webRtcRuntime != nullptr)
	{
		webRtcRuntime->signalingThread = g_thread_new("GstUnityWebRTCSignaling", WebRtcSignalingThread, this);

		if (!webRtcRuntime->signalingThread)
		{
			Debug::Log("WEBRTC: failed to start signaling thread.");
		}
		else
		{
			Debug::Log("WEBRTC: signaling thread started.");
		}
	}

	Debug::Log("A2f D3D11 appsink pipeline started.");
}


GstAppPipeline::~GstAppPipeline()
{
	std::unique_ptr<WebRtcRuntime> webRtcRuntime = RemoveWebRtcRuntime(this);

	if (webRtcRuntime)
	{
		webRtcRuntime->stopping.store(true);

		{
			std::lock_guard<std::mutex> guard(webRtcRuntime->socketMutex);

			if (webRtcRuntime->clientSocket != INVALID_SOCKET)
			{
				closesocket(webRtcRuntime->clientSocket);
				webRtcRuntime->clientSocket = INVALID_SOCKET;
			}

			if (webRtcRuntime->serverSocket != INVALID_SOCKET)
			{
				closesocket(webRtcRuntime->serverSocket);
				webRtcRuntime->serverSocket = INVALID_SOCKET;
			}
		}

		if (webRtcRuntime->signalingThread != nullptr)
		{
			g_thread_join(webRtcRuntime->signalingThread);
			webRtcRuntime->signalingThread = nullptr;
		}
	}

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

	std::lock_guard<std::mutex> guard(textureMutex);

	for (int i = 0; i < kTextureRingSize; ++i)
	{
		if (gstKeyedMutexes[i] != nullptr)
		{
			gstKeyedMutexes[i]->Release();
			gstKeyedMutexes[i] = nullptr;
		}

		if (gstOpenedTextures[i] != nullptr)
		{
			gstOpenedTextures[i]->Release();
			gstOpenedTextures[i] = nullptr;
		}

		if (unityKeyedMutexes[i] != nullptr)
		{
			unityKeyedMutexes[i]->Release();
			unityKeyedMutexes[i] = nullptr;
		}

		if (unityTextures[i] != nullptr)
		{
			unityTextures[i]->Release();
			unityTextures[i] = nullptr;
		}

		sharedHandles[i] = nullptr;
		frameIds[i].store(0);
	}

	if (displayTexture != nullptr)
	{
		displayTexture->Release();
		displayTexture = nullptr;
	}

	if (gstContext != nullptr)
	{
		gstContext->Release();
		gstContext = nullptr;
	}

	if (gstDevice != nullptr)
	{
		gstDevice->Release();
		gstDevice = nullptr;
	}
}


bool GstAppPipeline::CreateTexture()
{
	std::lock_guard<std::mutex> guard(textureMutex);

	auto device = app->getD3D11Device();
	HRESULT hr = S_OK;

	if (displayTexture == nullptr)
	{
		D3D11_TEXTURE2D_DESC displayDesc = {};
		displayDesc.Width = textureWidth;
		displayDesc.Height = textureHeight;
		displayDesc.MipLevels = 1;
		displayDesc.ArraySize = 1;
		displayDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		displayDesc.SampleDesc.Count = 1;
		displayDesc.SampleDesc.Quality = 0;
		displayDesc.Usage = D3D11_USAGE_DEFAULT;
		displayDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		displayDesc.CPUAccessFlags = 0;
		displayDesc.MiscFlags = 0;

		hr = device->CreateTexture2D(&displayDesc, nullptr, &displayTexture);

		if (FAILED(hr) || displayTexture == nullptr)
		{
			Debug::Log("A2f FIX16 DIRECT RING NO FLUSH could not create Unity display texture.");
			return false;
		}
	}

	for (int i = 0; i < kTextureRingSize; ++i)
	{
		if (unityTextures[i] != nullptr)
		{
			continue;
		}

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = textureWidth;
		desc.Height = textureHeight;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

		hr = device->CreateTexture2D(&desc, nullptr, &unityTextures[i]);

		if (FAILED(hr) || unityTextures[i] == nullptr)
		{
			std::stringstream ss;
			ss << "A2f FIX16 DIRECT RING NO FLUSH could not create Unity shared ring texture. hr=0x" << std::hex << hr;
			Debug::Log(ss.str());
			return false;
		}

		IDXGIResource* dxgiResource = nullptr;
		hr = unityTextures[i]->QueryInterface(__uuidof(IDXGIResource), (void**)&dxgiResource);

		if (FAILED(hr) || dxgiResource == nullptr)
		{
			std::stringstream ss;
			ss << "A2f FIX16 DIRECT RING NO FLUSH could not query IDXGIResource. hr=0x" << std::hex << hr;
			Debug::Log(ss.str());
			return false;
		}

		hr = dxgiResource->GetSharedHandle(&sharedHandles[i]);
		dxgiResource->Release();

		if (FAILED(hr) || sharedHandles[i] == nullptr)
		{
			std::stringstream ss;
			ss << "A2f FIX16 DIRECT RING NO FLUSH could not get shared handle. hr=0x" << std::hex << hr;
			Debug::Log(ss.str());
			return false;
		}
	}

	latestReadyTextureIndex.store(-1);

	Debug::LogNoNewLine("A2f FIX16 DIRECT RING NO FLUSH created Unity display texture + shared-handle ring for pipeline:");
	Debug::Log(static_cast<int>(id));

	return true;
}


bool GstAppPipeline::OpenSharedTexturesOnGstDevice(ID3D11Device* sourceDevice)
{
	if (sourceDevice == nullptr)
	{
		Debug::Log("A2f FIX16 DIRECT RING NO FLUSH sourceDevice is NULL.");
		return false;
	}

	if (openedSharedTexturesOnGstDevice)
	{
		return true;
	}

	for (int i = 0; i < kTextureRingSize; ++i)
	{
		if (sharedHandles[i] == nullptr)
		{
			Debug::Log("A2f FIX16 DIRECT RING NO FLUSH missing Unity shared handle.");
			return false;
		}
	}

	sourceDevice->AddRef();
	gstDevice = sourceDevice;
	gstDevice->GetImmediateContext(&gstContext);

	if (gstContext == nullptr)
	{
		Debug::Log("A2f FIX16 DIRECT RING NO FLUSH could not get GStreamer source D3D11 immediate context.");
		return false;
	}

	Debug::Log("A2f FIX16 DIRECT RING NO FLUSH captured source D3D11 device/context. About to OpenSharedResource.");

	for (int i = 0; i < kTextureRingSize; ++i)
	{
		std::stringstream before;
		before << "A2f FIX16 DIRECT RING NO FLUSH opening shared texture index=" << i
			<< " handle=" << sharedHandles[i];
		Debug::Log(before.str());

		HRESULT hr = gstDevice->OpenSharedResource(
			sharedHandles[i],
			__uuidof(ID3D11Texture2D),
			(void**)&gstOpenedTextures[i]
		);

		if (FAILED(hr) || gstOpenedTextures[i] == nullptr)
		{
			std::stringstream ss;
			ss << "A2f FIX16 DIRECT RING NO FLUSH OpenSharedResource failed at index=" << i
				<< " hr=0x" << std::hex << hr;
			Debug::Log(ss.str());
			return false;
		}

		D3D11_TEXTURE2D_DESC openedDesc = {};
		gstOpenedTextures[i]->GetDesc(&openedDesc);

		std::stringstream ss;
		ss << "A2f FIX16 DIRECT RING NO FLUSH opened texture index=" << i
			<< " desc=" << openedDesc.Width << "x" << openedDesc.Height
			<< " format=" << static_cast<unsigned int>(openedDesc.Format)
			<< " misc=" << openedDesc.MiscFlags
			<< " bind=" << openedDesc.BindFlags
			<< " usage=" << openedDesc.Usage;
		Debug::Log(ss.str());
	}

	openedSharedTexturesOnGstDevice = true;
	Debug::Log("A2f FIX16 DIRECT RING NO FLUSH successfully opened all Unity shared textures on GStreamer D3D11 device. Cross-device copy plus direct external texture ring display will be tested in this build.");

	return true;
}


bool GstAppPipeline::CopyD3D11FrameToSharedTexture(ID3D11Resource* sourceResource, UINT sourceSubresource)
{
	if (sourceResource == nullptr)
	{
		return false;
	}

	std::lock_guard<std::mutex> guard(textureMutex);

	ID3D11Texture2D* sourceTexture2D = nullptr;
	HRESULT qiHr = sourceResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&sourceTexture2D);

	if (FAILED(qiHr) || sourceTexture2D == nullptr)
	{
		Debug::Log("A2f FIX16 DIRECT RING NO FLUSH could not query source ID3D11Texture2D.");
		return false;
	}

	if (!loggedFirstFrame)
	{
		D3D11_TEXTURE2D_DESC sourceDesc = {};
		sourceTexture2D->GetDesc(&sourceDesc);

		std::stringstream ss;
		ss << "A2f FIX16 DIRECT RING NO FLUSH source texture: "
			<< sourceDesc.Width << "x" << sourceDesc.Height
			<< " format=" << static_cast<unsigned int>(sourceDesc.Format)
			<< " misc=" << sourceDesc.MiscFlags
			<< " bind=" << sourceDesc.BindFlags
			<< " usage=" << sourceDesc.Usage
			<< " subresource=" << sourceSubresource;
		Debug::Log(ss.str());
		loggedFirstFrame = true;
	}

	if (!openedSharedTexturesOnGstDevice)
	{
		ID3D11Device* sourceDevice = nullptr;
		sourceTexture2D->GetDevice(&sourceDevice);

		if (sourceDevice == nullptr)
		{
			sourceTexture2D->Release();
			Debug::Log("A2f FIX16 DIRECT RING NO FLUSH could not get source D3D11 device.");
			return false;
		}

		bool opened = OpenSharedTexturesOnGstDevice(sourceDevice);
		sourceDevice->Release();

		if (!opened)
		{
			sourceTexture2D->Release();
			return false;
		}
	}

	int writeIndex = static_cast<int>(latestFrameId.load() % kTextureRingSize);

	if (gstContext == nullptr || gstOpenedTextures[writeIndex] == nullptr)
	{
		sourceTexture2D->Release();
		Debug::Log("A2f FIX16 DIRECT RING NO FLUSH missing GStreamer context or opened shared texture.");
		return false;
	}

	D3D11_TEXTURE2D_DESC sourceDesc = {};
	sourceTexture2D->GetDesc(&sourceDesc);

	D3D11_BOX srcBox = {};
	srcBox.left = 0;
	srcBox.top = 0;
	srcBox.front = 0;
	srcBox.right = sourceDesc.Width;
	srcBox.bottom = sourceDesc.Height;
	srcBox.back = 1;

	gstContext->CopySubresourceRegion(
		gstOpenedTextures[writeIndex],
		0,
		0,
		0,
		0,
		sourceTexture2D,
		sourceSubresource,
		&srcBox
	);

	unsigned long long frameNumber = latestFrameId.load() + 1;
	frameIds[writeIndex].store(frameNumber, std::memory_order_release);
	latestFrameId.store(frameNumber);
	copiedFrameId.store(frameNumber);

	LogNativeFps(
		"A2f FIX16 DIRECT RING NO FLUSH copy-into-opened-shared FPS",
		gCopyFpsMutex,
		gCopiedFrameCount,
		gCopyLastTime
	);

	sourceTexture2D->Release();
	return true;
}


bool GstAppPipeline::CopyLatestSharedTextureToDisplayTextureOnRenderThread()
{
	return false;
}


ID3D11Texture2D* GstAppPipeline::GetLatestReadyUnityTexturePtr()
{
	unsigned long long latest = latestFrameId.load();
	if (latest < 4)
	{
		return nullptr;
	}

	unsigned long long frameToDisplay = latest - 8;
	int readIndex = static_cast<int>((frameToDisplay - 1) % kTextureRingSize);

	if (readIndex < 0 || readIndex >= kTextureRingSize)
	{
		return nullptr;
	}

	if (frameIds[readIndex].load(std::memory_order_acquire) != frameToDisplay)
	{
		return nullptr;
	}

	displayedFrameId.store(frameToDisplay);
	return unityTextures[readIndex];
}


ID3D11Texture2D* GstAppPipeline::GetTexturePtr()
{
	ID3D11Texture2D* ready = GetLatestReadyUnityTexturePtr();
	if (ready != nullptr)
	{
		return ready;
	}

	return displayTexture;
}


unsigned long long GstAppPipeline::GetLatestFrameId() const
{
	return latestFrameId.load();
}


unsigned long long GstAppPipeline::GetCopiedFrameId() const
{
	return copiedFrameId.load();
}


void GstAppPipeline::ReleaseTexture(ID3D11Texture2D* texture)
{
	(void)texture;
}