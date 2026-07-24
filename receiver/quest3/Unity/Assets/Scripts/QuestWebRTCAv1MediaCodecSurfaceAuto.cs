using System;
using System.Runtime.InteropServices;
using UnityEngine;

public static class QuestWebRTCAv1MediaCodecSurfaceAuto
{
    [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
    private static void Boot()
    {
        Application.SetStackTraceLogType(LogType.Log, StackTraceLogType.None);
        Application.SetStackTraceLogType(LogType.Warning, StackTraceLogType.None);

        Debug.Log("[QSXRUnifiedReceiver] BOOT METHOD RAN");
        GameObject go = new GameObject("QSXR_UNIFIED_WEBRTC_RECEIVER");
        go.AddComponent<QuestWebRTCAv1MediaCodecSurfaceRunner>();
        UnityEngine.Object.DontDestroyOnLoad(go);
    }
}

public class QuestWebRTCAv1MediaCodecSurfaceRunner : MonoBehaviour
{
    private const int EventInitOes = 3000;
    private const int EventBlitOes = 3001;

    private int streamWidth;
    private int streamHeight;
    private int streamFps;
    private int requestedCodec;
    private int signalingPort;

    private GameObject panorama;
    private Renderer panoramaRenderer;
    private Texture2D externalTexture;
    private IntPtr renderEventFunc = IntPtr.Zero;
    private IntPtr lastTargetTexturePtr = IntPtr.Zero;

    private AndroidJavaObject surfaceTexture;
    private AndroidJavaObject surface;
    private bool surfaceRegistered;

    private float lastLogTime;
    private ulong lastInputs;
    private ulong lastDecoded;
    private ulong lastDrops;
    private int lastBlitFrame;
    private int lastUnityFrame;
    private int lastDetectedCodec = -1;
    private int lastActiveCodec = -1;
    private ulong lastGeneration;

#if UNITY_ANDROID && !UNITY_EDITOR
    [DllImport("libGstQuestInit.so")]
    private static extern void gst_quest_oes_configure(int width, int height);

    [DllImport("libGstQuestInit.so")]
    private static extern UIntPtr gst_quest_oes_get_external_texture_id();

    [DllImport("libGstQuestInit.so")]
    private static extern UIntPtr gst_quest_oes_get_target_texture_id();

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_oes_get_width();

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_oes_get_height();

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_oes_get_frame_id();

    [DllImport("libGstQuestInit.so")]
    private static extern void gst_quest_oes_set_surface_texture(IntPtr surfaceTextureObj);

    [DllImport("libGstQuestInit.so")]
    private static extern IntPtr gst_quest_oes_get_render_event_func();

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_webrtc_video_register_surface(
        IntPtr surfaceObj,
        int width,
        int height,
        int fps,
        int requestedCodec
    );

    [DllImport("libGstQuestInit.so")]
    private static extern void gst_quest_webrtc_video_stop_surface();

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_webrtc_video_get_requested_codec();

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_webrtc_video_get_detected_codec();

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_webrtc_video_get_active_codec();

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_webrtc_video_get_running();

    [DllImport("libGstQuestInit.so")]
    private static extern ulong gst_quest_webrtc_video_get_generation();

    [DllImport("libGstQuestInit.so")]
    private static extern ulong gst_quest_webrtc_video_get_input_units();

    [DllImport("libGstQuestInit.so")]
    private static extern ulong gst_quest_webrtc_video_get_decoded_frames();

    [DllImport("libGstQuestInit.so")]
    private static extern ulong gst_quest_webrtc_video_get_drops();

    [DllImport("libGstQuestInit.so")]
    private static extern ulong gst_quest_webrtc_video_get_backlog();
#endif

    private static int CodecToId(string codec)
    {
        switch (codec)
        {
            case "h264": return 1;
            case "h265": return 2;
            case "av1": return 3;
            default: return 0;
        }
    }

    private static string CodecName(int codec)
    {
        switch (codec)
        {
            case 1: return "h264";
            case 2: return "h265";
            case 3: return "av1";
            default: return "auto";
        }
    }

    private void Awake()
    {
        streamWidth = QuestRuntimeConfig.Width;
        streamHeight = QuestRuntimeConfig.Height;
        streamFps = QuestRuntimeConfig.Fps;
        signalingPort = QuestRuntimeConfig.SignalingPort;
        requestedCodec = CodecToId(QuestRuntimeConfig.Codec);

        Debug.Log(
            "[QSXRUnifiedReceiver] AWAKE size=" + streamWidth + "x" + streamHeight +
            " fps=" + streamFps +
            " signalingPort=" + signalingPort +
            " requestedCodec=" + CodecName(requestedCodec) +
            " config=" + QuestRuntimeConfig.Summary
        );

        CreatePanoramaSphere();
    }

    private void Start()
    {
#if UNITY_ANDROID && !UNITY_EDITOR
        gst_quest_oes_configure(streamWidth, streamHeight);
        renderEventFunc = gst_quest_oes_get_render_event_func();

        Debug.Log(
            "[QSXRUnifiedReceiver] Native render event func=" + renderEventFunc
        );

        if (renderEventFunc != IntPtr.Zero)
        {
            GL.IssuePluginEvent(renderEventFunc, EventInitOes);
        }
#endif
    }

    private void Update()
    {
        PositionPanorama();

#if UNITY_ANDROID && !UNITY_EDITOR
        if (renderEventFunc != IntPtr.Zero)
        {
            GL.IssuePluginEvent(renderEventFunc, EventInitOes);
        }

        int oesId = (int)gst_quest_oes_get_external_texture_id().ToUInt64();
        IntPtr targetPtr = (IntPtr)(long)gst_quest_oes_get_target_texture_id().ToUInt64();
        int width = gst_quest_oes_get_width();
        int height = gst_quest_oes_get_height();
        int blitFrame = gst_quest_oes_get_frame_id();

        if (!surfaceRegistered && oesId != 0)
        {
            RegisterSurface(oesId, width, height);
        }

        if (renderEventFunc != IntPtr.Zero)
        {
            GL.IssuePluginEvent(renderEventFunc, EventBlitOes);
        }

        if (targetPtr != IntPtr.Zero)
        {
            if (externalTexture == null || targetPtr != lastTargetTexturePtr)
            {
                externalTexture = Texture2D.CreateExternalTexture(
                    width,
                    height,
                    TextureFormat.RGBA32,
                    false,
                    false,
                    targetPtr
                );
                externalTexture.wrapMode = TextureWrapMode.Clamp;
                externalTexture.filterMode = FilterMode.Bilinear;
                panoramaRenderer.material.mainTexture = externalTexture;
                lastTargetTexturePtr = targetPtr;

                Debug.Log(
                    "[QSXRUnifiedReceiver] Unity texture created ptr=" + targetPtr +
                    " OES=" + oesId + " size=" + width + "x" + height
                );
            }
            else
            {
                externalTexture.UpdateExternalTexture(targetPtr);
            }
        }

        int detectedCodec = gst_quest_webrtc_video_get_detected_codec();
        int activeCodec = gst_quest_webrtc_video_get_active_codec();
        ulong generation = gst_quest_webrtc_video_get_generation();

        if (
            detectedCodec != lastDetectedCodec ||
            activeCodec != lastActiveCodec ||
            generation != lastGeneration
        )
        {
            Debug.Log(
                "[QSXRUnifiedReceiver] codec state requested=" +
                CodecName(gst_quest_webrtc_video_get_requested_codec()) +
                " detected=" + CodecName(detectedCodec) +
                " active=" + CodecName(activeCodec) +
                " running=" + gst_quest_webrtc_video_get_running() +
                " generation=" + generation
            );

            lastDetectedCodec = detectedCodec;
            lastActiveCodec = activeCodec;
            lastGeneration = generation;
        }

        float now = Time.realtimeSinceStartup;
        if (now - lastLogTime >= 1.0f)
        {
            ulong inputs = gst_quest_webrtc_video_get_input_units();
            ulong decoded = gst_quest_webrtc_video_get_decoded_frames();
            ulong drops = gst_quest_webrtc_video_get_drops();
            ulong backlog = gst_quest_webrtc_video_get_backlog();
            float dt = Mathf.Max(0.001f, now - lastLogTime);

            float inputRate = (float)(inputs - lastInputs) / dt;
            float decodedRate = (float)(decoded - lastDecoded) / dt;
            float dropRate = (float)(drops - lastDrops) / dt;
            float blitRate = (float)(blitFrame - lastBlitFrame) / dt;
            float unityRate = (float)(Time.frameCount - lastUnityFrame) / dt;

            Debug.Log(
                "[QSXRUnifiedReceiver] STATS codec=" + CodecName(activeCodec) +
                " UnityFPS=" + unityRate.ToString("F1") +
                " OESblit/s=" + blitRate.ToString("F1") +
                " input/s=" + inputRate.ToString("F1") +
                " decoded/s=" + decodedRate.ToString("F1") +
                " drops/s=" + dropRate.ToString("F1") +
                " totalInput=" + inputs +
                " totalDecoded=" + decoded +
                " totalDrops=" + drops +
                " backlog=" + backlog +
                " size=" + width + "x" + height +
                " configuredFps=" + streamFps
            );

            lastInputs = inputs;
            lastDecoded = decoded;
            lastDrops = drops;
            lastBlitFrame = blitFrame;
            lastUnityFrame = Time.frameCount;
            lastLogTime = now;
        }
#endif
    }

#if UNITY_ANDROID && !UNITY_EDITOR
    private void RegisterSurface(int oesId, int width, int height)
    {
        try
        {
            surfaceTexture = new AndroidJavaObject(
                "android.graphics.SurfaceTexture",
                oesId
            );
            surfaceTexture.Call("setDefaultBufferSize", width, height);
            surface = new AndroidJavaObject(
                "android.view.Surface",
                surfaceTexture
            );

            gst_quest_oes_set_surface_texture(surfaceTexture.GetRawObject());

            int result = gst_quest_webrtc_video_register_surface(
                surface.GetRawObject(),
                width,
                height,
                streamFps,
                requestedCodec
            );

            surfaceRegistered = true;

            Debug.Log(
                "[QSXRUnifiedReceiver] Surface registration result=" + result +
                " requestedCodec=" + CodecName(requestedCodec) +
                " size=" + width + "x" + height +
                " fps=" + streamFps
            );
        }
        catch (Exception ex)
        {
            Debug.LogError(
                "[QSXRUnifiedReceiver] Surface registration failed: " + ex
            );
        }
    }
#endif

    private void CreatePanoramaSphere()
    {
        Shader shader = Shader.Find("Unlit/Texture");
        if (shader == null) shader = Shader.Find("Sprites/Default");
        if (shader == null) shader = Shader.Find("Standard");

        panorama = new GameObject("QSXR_360_VIDEO_SPHERE");
        MeshFilter meshFilter = panorama.AddComponent<MeshFilter>();
        panoramaRenderer = panorama.AddComponent<MeshRenderer>();

        const int longitudeSegments = 96;
        const int latitudeSegments = 48;
        const float radius = 25.0f;

        Vector3[] vertices = new Vector3[
            (longitudeSegments + 1) * (latitudeSegments + 1)
        ];
        Vector2[] uvs = new Vector2[vertices.Length];
        int[] triangles = new int[
            longitudeSegments * latitudeSegments * 6
        ];

        int vertex = 0;
        for (int y = 0; y <= latitudeSegments; y++)
        {
            float v = (float)y / latitudeSegments;
            float theta = v * Mathf.PI;

            for (int x = 0; x <= longitudeSegments; x++)
            {
                float u = (float)x / longitudeSegments;
                float phi = u * Mathf.PI * 2.0f;
                float sx = Mathf.Sin(theta) * Mathf.Cos(phi);
                float sy = Mathf.Cos(theta);
                float sz = Mathf.Sin(theta) * Mathf.Sin(phi);

                vertices[vertex] = new Vector3(sx, sy, sz) * radius;
                uvs[vertex] = new Vector2(1.0f - u, 1.0f - v);
                vertex++;
            }
        }

        int triangle = 0;
        for (int y = 0; y < latitudeSegments; y++)
        {
            for (int x = 0; x < longitudeSegments; x++)
            {
                int i0 = y * (longitudeSegments + 1) + x;
                int i1 = i0 + 1;
                int i2 = i0 + longitudeSegments + 1;
                int i3 = i2 + 1;

                triangles[triangle++] = i0;
                triangles[triangle++] = i1;
                triangles[triangle++] = i2;
                triangles[triangle++] = i1;
                triangles[triangle++] = i3;
                triangles[triangle++] = i2;
            }
        }

        Mesh mesh = new Mesh();
        mesh.indexFormat = UnityEngine.Rendering.IndexFormat.UInt32;
        mesh.vertices = vertices;
        mesh.uv = uvs;
        mesh.triangles = triangles;
        mesh.RecalculateBounds();
        meshFilter.mesh = mesh;

        Material material = new Material(shader);
        material.mainTextureScale = Vector2.one;
        material.mainTextureOffset = Vector2.zero;
        material.SetInt(
            "_Cull",
            (int)UnityEngine.Rendering.CullMode.Front
        );
        panoramaRenderer.material = material;

        Debug.Log("[QSXRUnifiedReceiver] 360 panorama sphere created.");
    }

    private void PositionPanorama()
    {
        if (panorama == null) return;

        Camera camera = Camera.main;
        if (camera == null) camera = FindObjectOfType<Camera>();

        panorama.transform.position = camera != null
            ? camera.transform.position
            : Vector3.zero;
        panorama.transform.rotation = Quaternion.identity;
        panorama.transform.localScale = Vector3.one;
    }

    private void OnDestroy()
    {
#if UNITY_ANDROID && !UNITY_EDITOR
        gst_quest_webrtc_video_stop_surface();

        try
        {
            gst_quest_oes_set_surface_texture(IntPtr.Zero);

            if (surface != null)
            {
                surface.Call("release");
                surface.Dispose();
                surface = null;
            }

            if (surfaceTexture != null)
            {
                surfaceTexture.Call("release");
                surfaceTexture.Dispose();
                surfaceTexture = null;
            }
        }
        catch (Exception ex)
        {
            Debug.LogWarning(
                "[QSXRUnifiedReceiver] Surface cleanup warning: " + ex.Message
            );
        }
#endif

        Debug.Log("[QSXRUnifiedReceiver] Receiver stopped.");
    }
}
