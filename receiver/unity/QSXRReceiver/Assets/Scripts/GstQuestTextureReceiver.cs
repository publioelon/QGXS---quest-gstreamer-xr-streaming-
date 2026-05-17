using System;
using System.Runtime.InteropServices;
using System.Text;
using UnityEngine;
using UnityEngine.Rendering;

public class GstQuestTextureReceiver : MonoBehaviour
{
    [Header("Auto Bootstrap")]
    public bool autoStartReceiver = true;
    public bool dontDestroyOnLoad = true;
    public bool forceAutoStart = true;
    public bool retryStartIfFailed = true;

    [Header("Native Quest GStreamer")]
    public bool checkWebRTCBinBeforeStart = true;
    public KeyCode startReceiverKey = KeyCode.S;
    public KeyCode stopReceiverKey = KeyCode.X;

    [Header("Signaling / Discovery")]
    public int signalingPort = 9001;
    public int discoveryPort = 9010;
    public bool startDiscoveryServer = true;

    [Header("Expected Resolution")]
    public int width = 1920;
    public int height = 960;

    [Header("Display Rate")]
    public int targetDisplayFps = 72;
    public bool lockUnityFrameRate = true;

    [Header("360 Sphere Settings")]
    public float sphereRadius = 50f;
    public int longitudeSegments = 96;
    public int latitudeSegments = 48;
    public bool followCameraPosition = false;

    [Header("Texture Orientation")]
    public bool flipVertical = false;
    public bool flipHorizontal = false;

    [Header("Initial View")]
    public float initialYawDegrees = 45f;
    public float initialPitchDegrees = 0f;

    [Header("Mouse Drag Look Controls")]
    public bool enableMouseDragLook = false;
    public float mouseSensitivity = 0.15f;
    public bool invertDragX = false;
    public bool invertDragY = false;
    public float minPitchDegrees = -85f;
    public float maxPitchDegrees = 85f;

    [Header("Stats / Debug")]
    public bool showStatsInConsole = true;
    public bool showStatsOverlay = true;
    public float statsIntervalSeconds = 1.0f;

    [Header("Runtime Info")]
    public bool nativeInitialized;
    public bool nativeReceiverStarted;
    public bool receivingFrames;
    public int nativeFrameWidth;
    public int nativeFrameHeight;
    public int nativeFrameSize;
    public ulong nativeFrameId;
    public string gstreamerVersion = "";
    public string lastSignalingMessage = "";
    public string lastDiscoveryMessage = "";
    public string lastStartError = "";

    private Texture2D texture;
    private byte[] frameBuffer;

    private bool started = false;
    private bool startInProgress = false;
    private bool applicationQuitting = false;
    private float nextStartRetryTime = 0f;

    private GameObject sphere360;
    private Renderer sphereRenderer;
    private Camera viewerCamera;

    private float yaw;
    private float pitch;
    private bool isDragging = false;
    private Vector3 lastMousePosition;

    private ulong lastUploadedFrameId = 0;

    private float statsTimer = 0f;
    private int unityRenderFrames = 0;
    private int textureUploadFrames = 0;
    private float lastUnityFps = 0f;
    private float lastTextureUploadRate = 0f;

    private const string NativeLib = "libGstQuestInit.so";

    [DllImport(NativeLib)]
    private static extern int gst_quest_init();

    [DllImport(NativeLib)]
    private static extern IntPtr gst_quest_version();

    [DllImport(NativeLib)]
    private static extern int gst_quest_has_webrtcbin();

    [DllImport(NativeLib)]
    private static extern int gst_quest_start_signaling_test(int port);

    [DllImport(NativeLib)]
    private static extern int gst_quest_is_signaling_running();

    [DllImport(NativeLib)]
    private static extern int gst_quest_get_last_signaling_message([Out] byte[] dst, int dstSize);

    [DllImport(NativeLib)]
    private static extern void gst_quest_stop_signaling_test();

    [DllImport(NativeLib)]
    private static extern int gst_quest_start_discovery_test(int discoveryPort);

    [DllImport(NativeLib)]
    private static extern int gst_quest_is_discovery_running();

    [DllImport(NativeLib)]
    private static extern int gst_quest_get_last_discovery_message([Out] byte[] dst, int dstSize);

    [DllImport(NativeLib)]
    private static extern void gst_quest_stop_discovery_test();

    [DllImport(NativeLib)]
    private static extern int gst_quest_get_latest_frame_width();

    [DllImport(NativeLib)]
    private static extern int gst_quest_get_latest_frame_height();

    [DllImport(NativeLib)]
    private static extern int gst_quest_get_latest_frame_size();

    [DllImport(NativeLib)]
    private static extern ulong gst_quest_get_latest_frame_id();

    [DllImport(NativeLib)]
    private static extern int gst_quest_copy_latest_frame([Out] byte[] dst, int dstSize);

    [DllImport(NativeLib)]
    private static extern void gst_quest_clear_latest_frame();

    [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
    private static void AutoCreateReceiver()
    {
        GstQuestTextureReceiver existing = FindObjectOfType<GstQuestTextureReceiver>();

        if (existing != null)
        {
            existing.autoStartReceiver = true;
            existing.forceAutoStart = true;
            existing.followCameraPosition = false;
            existing.enableMouseDragLook = false;
            existing.flipVertical = false;
            existing.flipHorizontal = false;
            return;
        }

        GameObject obj = new GameObject("AutoCreated_GstQuestTextureReceiver");
        GstQuestTextureReceiver receiver = obj.AddComponent<GstQuestTextureReceiver>();
        receiver.autoStartReceiver = true;
        receiver.forceAutoStart = true;
        receiver.followCameraPosition = false;
        receiver.enableMouseDragLook = false;
        receiver.flipVertical = false;
        receiver.flipHorizontal = false;
    }

    void OnValidate()
    {
        SanitizeSettings();
    }

    void Awake()
    {
        autoStartReceiver = true;
        forceAutoStart = true;
        followCameraPosition = false;
        enableMouseDragLook = false;
        flipVertical = false;
        flipHorizontal = false;

        if (dontDestroyOnLoad)
        {
            DontDestroyOnLoad(gameObject);
        }
    }

    void Start()
    {
        Application.runInBackground = true;

        autoStartReceiver = true;
        forceAutoStart = true;
        followCameraPosition = false;
        enableMouseDragLook = false;
        flipVertical = false;
        flipHorizontal = false;

        SanitizeSettings();
        ApplyFrameRateSettings();

        SetupCamera();
        CreateInsideOutSphere();
        ApplyTextureToSphere();

        Debug.Log("[GstQuestTextureReceiver] Start called. autoStartReceiver=" + autoStartReceiver + " forceAutoStart=" + forceAutoStart);

        if (autoStartReceiver || forceAutoStart)
        {
            Debug.Log("[GstQuestTextureReceiver] Forced auto-start enabled. Starting Quest native receiver.");
            StartQuestNativeReceiver();
        }
        else
        {
            Debug.Log("[GstQuestTextureReceiver] Auto-start disabled. Press " + startReceiverKey + " to start and " + stopReceiverKey + " to stop.");
        }
    }

    void Update()
    {
        unityRenderFrames++;

        if (!started && !startInProgress && Input.GetKeyDown(startReceiverKey))
        {
            Debug.Log("[GstQuestTextureReceiver] Manual receiver start requested.");
            StartQuestNativeReceiver();
        }

        if ((started || nativeReceiverStarted) && Input.GetKeyDown(stopReceiverKey))
        {
            Debug.Log("[GstQuestTextureReceiver] Manual receiver stop requested.");
            StopReceiverSafely();
        }

        if (retryStartIfFailed && (autoStartReceiver || forceAutoStart) && !nativeReceiverStarted && !started && !startInProgress)
        {
            if (Time.unscaledTime >= nextStartRetryTime)
            {
                nextStartRetryTime = Time.unscaledTime + 3.0f;
                Debug.Log("[GstQuestTextureReceiver] Receiver is not started. Retrying native start.");
                StartQuestNativeReceiver();
            }
        }

        UpdateNativeStatus();
        UpdateVideoFrame();
        UpdateStatsCounter();
    }

    void LateUpdate()
    {
        if (sphere360 != null)
        {
            if (sphere360.transform.parent != null)
            {
                sphere360.transform.SetParent(null, true);
            }

            sphere360.transform.position = Vector3.zero;
            sphere360.transform.rotation = Quaternion.identity;
            sphere360.transform.localScale = Vector3.one;
        }
    }

    void SanitizeSettings()
    {
        width = Mathf.Max(16, width);
        height = Mathf.Max(16, height);
        signalingPort = Mathf.Clamp(signalingPort, 1, 65535);
        discoveryPort = Mathf.Clamp(discoveryPort, 1, 65535);
        targetDisplayFps = Mathf.Max(1, targetDisplayFps);
        sphereRadius = Mathf.Max(1f, sphereRadius);
        longitudeSegments = Mathf.Max(8, longitudeSegments);
        latitudeSegments = Mathf.Max(4, latitudeSegments);
        statsIntervalSeconds = Mathf.Max(0.1f, statsIntervalSeconds);
    }

    void ApplyFrameRateSettings()
    {
        if (!lockUnityFrameRate)
        {
            return;
        }

        QualitySettings.vSyncCount = 0;
        Application.targetFrameRate = Mathf.Max(1, targetDisplayFps);

        Debug.Log("[GstQuestTextureReceiver] Unity target frame rate locked to: " + Application.targetFrameRate + " FPS");
    }

    void SetupCamera()
    {
        viewerCamera = Camera.main;

        if (viewerCamera == null)
        {
            GameObject camObj = new GameObject("Main Camera");
            viewerCamera = camObj.AddComponent<Camera>();
            camObj.tag = "MainCamera";

            viewerCamera.transform.position = Vector3.zero;
            viewerCamera.transform.rotation = Quaternion.Euler(initialPitchDegrees, initialYawDegrees, 0f);
        }

        yaw = initialYawDegrees;
        pitch = initialPitchDegrees;

        viewerCamera.nearClipPlane = 0.01f;
        viewerCamera.farClipPlane = 1000f;
        viewerCamera.clearFlags = CameraClearFlags.SolidColor;
        viewerCamera.backgroundColor = Color.black;
    }

    Shader FindBestVideoShader()
    {
        Shader shader = Shader.Find("Universal Render Pipeline/Unlit");

        if (shader == null)
        {
            shader = Shader.Find("Unlit/Texture");
        }

        if (shader == null)
        {
            shader = Shader.Find("Sprites/Default");
        }

        if (shader == null)
        {
            shader = Shader.Find("Standard");
        }

        return shader;
    }

    Material CreateVideoMaterial()
    {
        Shader shader = FindBestVideoShader();

        if (shader == null)
        {
            Debug.LogError("[GstQuestTextureReceiver] No usable shader found. The sphere may render pink.");
            return null;
        }

        Material mat = new Material(shader);
        mat.name = "Runtime_Quest_InsideOut360_Material";

        if (mat.HasProperty("_BaseColor"))
        {
            mat.SetColor("_BaseColor", Color.white);
        }

        if (mat.HasProperty("_Color"))
        {
            mat.SetColor("_Color", Color.white);
        }

        if (texture != null)
        {
            AssignTextureToMaterial(mat, texture);
        }

        Debug.Log("[GstQuestTextureReceiver] Created video material with shader: " + shader.name);

        return mat;
    }

    void AssignTextureToMaterial(Material mat, Texture2D tex)
    {
        if (mat == null)
        {
            return;
        }

        if (tex != null)
        {
            mat.mainTexture = tex;

            if (mat.HasProperty("_MainTex"))
            {
                mat.SetTexture("_MainTex", tex);
            }

            if (mat.HasProperty("_BaseMap"))
            {
                mat.SetTexture("_BaseMap", tex);
            }
        }

        float scaleX = flipHorizontal ? -1f : 1f;
        float scaleY = flipVertical ? -1f : 1f;
        float offsetX = flipHorizontal ? 1f : 0f;
        float offsetY = flipVertical ? 1f : 0f;

        mat.mainTextureScale = new Vector2(scaleX, scaleY);
        mat.mainTextureOffset = new Vector2(offsetX, offsetY);

        if (mat.HasProperty("_MainTex"))
        {
            mat.SetTextureScale("_MainTex", new Vector2(scaleX, scaleY));
            mat.SetTextureOffset("_MainTex", new Vector2(offsetX, offsetY));
        }

        if (mat.HasProperty("_BaseMap"))
        {
            mat.SetTextureScale("_BaseMap", new Vector2(scaleX, scaleY));
            mat.SetTextureOffset("_BaseMap", new Vector2(offsetX, offsetY));
        }
    }

    void CreateInsideOutSphere()
    {
        if (sphere360 != null && sphereRenderer != null)
        {
            return;
        }

        if (sphere360 != null && sphereRenderer == null)
        {
            Destroy(sphere360);
            sphere360 = null;
        }

        GameObject existingSphere = GameObject.Find("AutoCreated_InsideOut_360_Sphere");

        if (existingSphere != null)
        {
            Destroy(existingSphere);
        }

        sphere360 = new GameObject("AutoCreated_InsideOut_360_Sphere");
        sphere360.transform.SetParent(null, false);
        sphere360.transform.position = Vector3.zero;
        sphere360.transform.rotation = Quaternion.identity;
        sphere360.transform.localScale = Vector3.one;

        MeshFilter meshFilter = sphere360.AddComponent<MeshFilter>();
        MeshRenderer meshRenderer = sphere360.AddComponent<MeshRenderer>();

        Mesh mesh = CreateInwardFacingSphereMesh(sphereRadius, longitudeSegments, latitudeSegments);
        meshFilter.sharedMesh = mesh;

        Material mat = CreateVideoMaterial();

        if (mat == null)
        {
            Shader fallbackShader = Shader.Find("Sprites/Default");

            if (fallbackShader != null)
            {
                mat = new Material(fallbackShader);
                mat.name = "Runtime_Quest_Fallback_Material";
            }
        }

        if (mat != null)
        {
            meshRenderer.material = mat;
        }

        meshRenderer.shadowCastingMode = ShadowCastingMode.Off;
        meshRenderer.receiveShadows = false;
        meshRenderer.lightProbeUsage = LightProbeUsage.Off;
        meshRenderer.reflectionProbeUsage = ReflectionProbeUsage.Off;

        sphereRenderer = meshRenderer;

        Debug.Log("[GstQuestTextureReceiver] Created automatic inside-out 360 sphere. Renderer=" + (sphereRenderer != null));
    }

    void ApplyTextureToSphere()
    {
        if (sphereRenderer == null)
        {
            CreateInsideOutSphere();
        }

        if (sphereRenderer == null)
        {
            Debug.LogError("[GstQuestTextureReceiver] Sphere renderer is still missing after recreate attempt.");
            return;
        }

        if (sphereRenderer.material == null)
        {
            Material mat = CreateVideoMaterial();

            if (mat != null)
            {
                sphereRenderer.material = mat;
            }
        }

        AssignTextureToMaterial(sphereRenderer.material, texture);
    }

    void CreateOrResizeTextureAndFrameBuffer(int nativeWidth, int nativeHeight, int nativeSize)
    {
        int expectedSize = nativeWidth * nativeHeight * 4;

        if (nativeSize != expectedSize)
        {
            Debug.LogWarning("[GstQuestTextureReceiver] Native frame size does not match RGBA32 expected size. nativeSize=" + nativeSize + " expected=" + expectedSize + " width=" + nativeWidth + " height=" + nativeHeight);
            return;
        }

        if (frameBuffer == null || frameBuffer.Length != nativeSize)
        {
            frameBuffer = new byte[nativeSize];

            Debug.Log("[GstQuestTextureReceiver] Allocated frame buffer: " + nativeWidth + "x" + nativeHeight + " | RGBA bytes: " + nativeSize);
        }

        if (texture == null || texture.width != nativeWidth || texture.height != nativeHeight)
        {
            texture = new Texture2D(nativeWidth, nativeHeight, TextureFormat.RGBA32, false);
            texture.wrapMode = TextureWrapMode.Repeat;
            texture.filterMode = FilterMode.Bilinear;

            CreateInsideOutSphere();
            ApplyTextureToSphere();

            Debug.Log("[GstQuestTextureReceiver] Created Unity texture: " + nativeWidth + "x" + nativeHeight + " | RGBA32");
        }
    }

    void StartQuestNativeReceiver()
    {
        if (started || startInProgress)
        {
            Debug.LogWarning("[GstQuestTextureReceiver] Receiver is already started or starting.");
            return;
        }

        startInProgress = true;
        lastStartError = "";

        try
        {
            SanitizeSettings();

            Debug.Log("[GstQuestTextureReceiver] Calling gst_quest_init().");

            int initOk = gst_quest_init();
            nativeInitialized = initOk != 0;

            IntPtr versionPtr = gst_quest_version();

            if (versionPtr != IntPtr.Zero)
            {
                gstreamerVersion = Marshal.PtrToStringAnsi(versionPtr);
            }

            Debug.Log("[GstQuestTextureReceiver] Native init=" + nativeInitialized + " | version=" + gstreamerVersion);

            if (!nativeInitialized)
            {
                lastStartError = "gst_quest_init returned false.";
                Debug.LogError("[GstQuestTextureReceiver] " + lastStartError);
                started = false;
                nativeReceiverStarted = false;
                return;
            }

            if (checkWebRTCBinBeforeStart)
            {
                Debug.Log("[GstQuestTextureReceiver] Checking webrtcbin.");

                int hasWebrtc = gst_quest_has_webrtcbin();

                if (hasWebrtc == 0)
                {
                    lastStartError = "webrtcbin not found in native GStreamer.";
                    Debug.LogError("[GstQuestTextureReceiver] " + lastStartError);
                    started = false;
                    nativeReceiverStarted = false;
                    return;
                }

                Debug.Log("[GstQuestTextureReceiver] webrtcbin found.");
            }

            Debug.Log("[GstQuestTextureReceiver] Starting native signaling server on port " + signalingPort);

            int signalingOk = gst_quest_start_signaling_test(signalingPort);

            if (signalingOk == 0)
            {
                lastStartError = "Failed to start native signaling server on port " + signalingPort;
                Debug.LogError("[GstQuestTextureReceiver] " + lastStartError);
                started = false;
                nativeReceiverStarted = false;
                return;
            }

            int signalingRunning = gst_quest_is_signaling_running();

            Debug.Log("[GstQuestTextureReceiver] Signaling start returned " + signalingOk + " running=" + signalingRunning);

            if (startDiscoveryServer)
            {
                Debug.Log("[GstQuestTextureReceiver] Starting UDP discovery server on port " + discoveryPort);

                int discoveryOk = gst_quest_start_discovery_test(discoveryPort);
                int discoveryRunning = gst_quest_is_discovery_running();

                Debug.Log("[GstQuestTextureReceiver] Discovery start returned " + discoveryOk + " running=" + discoveryRunning);

                if (discoveryOk == 0)
                {
                    Debug.LogWarning("[GstQuestTextureReceiver] Failed to start UDP discovery server on port " + discoveryPort);
                }
            }

            started = true;
            nativeReceiverStarted = true;

            Debug.Log("[GstQuestTextureReceiver] Quest native GStreamer WebRTC receiver started. Signaling port=" + signalingPort + " discovery=" + startDiscoveryServer + " discoveryPort=" + discoveryPort + ". Start the Windows sender with H.264.");
        }
        catch (DllNotFoundException e)
        {
            started = false;
            nativeReceiverStarted = false;
            lastStartError = "DllNotFoundException: " + e.Message;

            Debug.LogError("[GstQuestTextureReceiver] Could not find libGstQuestInit.so or one of its Android native dependencies. Make sure libGstQuestInit.so, libgstreamer_android.so, and libc++_shared.so are in Assets/Plugins/Android/libs/arm64-v8a.\n" + e.Message);
        }
        catch (EntryPointNotFoundException e)
        {
            started = false;
            nativeReceiverStarted = false;
            lastStartError = "EntryPointNotFoundException: " + e.Message;

            Debug.LogError("[GstQuestTextureReceiver] libGstQuestInit.so does not export one of the required native functions. Rebuild the native plugin and copy it again into Unity.\n" + e.Message);
        }
        catch (Exception e)
        {
            started = false;
            nativeReceiverStarted = false;
            lastStartError = "Exception: " + e;

            Debug.LogError("[GstQuestTextureReceiver] Unexpected managed error while starting receiver:\n" + e);
        }
        finally
        {
            startInProgress = false;
        }
    }

    void UpdateNativeStatus()
    {
        if (!started && !nativeReceiverStarted)
        {
            return;
        }

        try
        {
            lastSignalingMessage = ReadNativeString(gst_quest_get_last_signaling_message);
            lastDiscoveryMessage = ReadNativeString(gst_quest_get_last_discovery_message);
        }
        catch
        {
        }
    }

    delegate int NativeStringReader(byte[] dst, int dstSize);

    string ReadNativeString(NativeStringReader reader)
    {
        byte[] buffer = new byte[1024];
        int ok = reader(buffer, buffer.Length);

        if (ok == 0)
        {
            return "";
        }

        int len = 0;

        while (len < buffer.Length && buffer[len] != 0)
        {
            len++;
        }

        if (len <= 0)
        {
            return "";
        }

        return Encoding.UTF8.GetString(buffer, 0, len);
    }

    void UpdateVideoFrame()
    {
        if (!started)
        {
            return;
        }

        int w = gst_quest_get_latest_frame_width();
        int h = gst_quest_get_latest_frame_height();
        int size = gst_quest_get_latest_frame_size();
        ulong frameId = gst_quest_get_latest_frame_id();

        nativeFrameWidth = w;
        nativeFrameHeight = h;
        nativeFrameSize = size;
        nativeFrameId = frameId;

        if (w <= 0 || h <= 0 || size <= 0 || frameId == 0)
        {
            receivingFrames = false;
            return;
        }

        receivingFrames = true;

        if (frameId == lastUploadedFrameId)
        {
            return;
        }

        CreateOrResizeTextureAndFrameBuffer(w, h, size);

        if (texture == null || frameBuffer == null)
        {
            return;
        }

        int copied = 0;

        try
        {
            copied = gst_quest_copy_latest_frame(frameBuffer, frameBuffer.Length);
        }
        catch (Exception e)
        {
            Debug.LogError("[GstQuestTextureReceiver] Managed exception while copying latest native frame:\n" + e);
            StopReceiverSafely();
            return;
        }

        if (copied <= 0)
        {
            if (copied < 0)
            {
                Debug.LogWarning("[GstQuestTextureReceiver] Native frame buffer too small. Needed=" + (-copied));
            }

            return;
        }

        if (copied != frameBuffer.Length)
        {
            Debug.LogWarning("[GstQuestTextureReceiver] Copied frame size mismatch. copied=" + copied + " bufferLength=" + frameBuffer.Length);
            return;
        }

        texture.LoadRawTextureData(frameBuffer);
        texture.Apply(false, false);

        ApplyTextureToSphere();

        lastUploadedFrameId = frameId;
        textureUploadFrames++;
    }

    void UpdateStatsCounter()
    {
        statsTimer += Time.unscaledDeltaTime;

        if (statsTimer >= statsIntervalSeconds)
        {
            lastUnityFps = unityRenderFrames / statsTimer;
            lastTextureUploadRate = textureUploadFrames / statsTimer;

            if (showStatsInConsole)
            {
                Debug.Log("[GstQuestTextureReceiver] Native started: " + nativeReceiverStarted + " | Receiving frames: " + receivingFrames + " | Native frame: " + nativeFrameWidth + "x" + nativeFrameHeight + " | Native size: " + nativeFrameSize + " | Native frame id: " + nativeFrameId + " | Unity render FPS: " + lastUnityFps.ToString("F1") + " | Texture uploads/s: " + lastTextureUploadRate.ToString("F1") + " | Signaling: " + lastSignalingMessage + " | Last start error: " + lastStartError);
            }

            unityRenderFrames = 0;
            textureUploadFrames = 0;
            statsTimer = 0f;
        }
    }

    void OnGUI()
    {
        if (!showStatsOverlay)
        {
            return;
        }

        GUI.Label(
            new Rect(20, 20, 1800, 30),
            "Quest GStreamer WebRTC H.264 Receiver" +
            " | Native started: " + nativeReceiverStarted +
            " | Receiving: " + receivingFrames +
            " | Frame: " + nativeFrameWidth + "x" + nativeFrameHeight +
            " | Size: " + nativeFrameSize +
            " | Frame ID: " + nativeFrameId +
            " | Unity FPS: " + lastUnityFps.ToString("F1") +
            " | Uploads/s: " + lastTextureUploadRate.ToString("F1") +
            " | Signal port: " + signalingPort
        );

        GUI.Label(
            new Rect(20, 50, 1800, 30),
            "GStreamer: " + gstreamerVersion +
            " | Signaling: " + lastSignalingMessage +
            " | Discovery: " + lastDiscoveryMessage
        );

        GUI.Label(
            new Rect(20, 80, 1800, 30),
            "Last start error: " + lastStartError
        );
    }

    Mesh CreateInwardFacingSphereMesh(float radius, int lonSegments, int latSegments)
    {
        Mesh mesh = new Mesh();
        mesh.name = "Runtime_Quest_InwardFacing_360_Sphere";

        int vertCount = (lonSegments + 1) * (latSegments + 1);

        Vector3[] vertices = new Vector3[vertCount];
        Vector3[] normals = new Vector3[vertCount];
        Vector2[] uvs = new Vector2[vertCount];

        int index = 0;

        for (int lat = 0; lat <= latSegments; lat++)
        {
            float v = (float)lat / latSegments;
            float theta = v * Mathf.PI;

            float sinTheta = Mathf.Sin(theta);
            float cosTheta = Mathf.Cos(theta);

            for (int lon = 0; lon <= lonSegments; lon++)
            {
                float u = (float)lon / lonSegments;
                float phi = u * Mathf.PI * 2f;

                float sinPhi = Mathf.Sin(phi);
                float cosPhi = Mathf.Cos(phi);

                Vector3 pos = new Vector3(
                    sinTheta * cosPhi,
                    cosTheta,
                    sinTheta * sinPhi
                ) * radius;

                vertices[index] = pos;
                normals[index] = -pos.normalized;
                uvs[index] = new Vector2(u, v);

                index++;
            }
        }

        int[] triangles = new int[lonSegments * latSegments * 6];
        int tri = 0;

        for (int lat = 0; lat < latSegments; lat++)
        {
            for (int lon = 0; lon < lonSegments; lon++)
            {
                int a = lat * (lonSegments + 1) + lon;
                int b = a + 1;
                int c = a + (lonSegments + 1);
                int d = c + 1;

                triangles[tri++] = a;
                triangles[tri++] = c;
                triangles[tri++] = b;

                triangles[tri++] = b;
                triangles[tri++] = c;
                triangles[tri++] = d;
            }
        }

        mesh.vertices = vertices;
        mesh.normals = normals;
        mesh.uv = uvs;
        mesh.triangles = triangles;

        mesh.RecalculateBounds();

        return mesh;
    }

    void OnDisable()
    {
        if (!applicationQuitting)
        {
            StopReceiverSafely();
        }
    }

    void OnDestroy()
    {
        if (!applicationQuitting)
        {
            StopReceiverSafely();
        }
    }

    void OnApplicationQuit()
    {
        applicationQuitting = true;
        StopReceiverSafely();
    }

    void StopReceiverSafely()
    {
        if (!nativeReceiverStarted && !started)
        {
            return;
        }

        try
        {
            gst_quest_stop_signaling_test();
        }
        catch
        {
        }

        try
        {
            gst_quest_stop_discovery_test();
        }
        catch
        {
        }

        try
        {
            gst_quest_clear_latest_frame();
        }
        catch
        {
        }

        started = false;
        nativeReceiverStarted = false;
        receivingFrames = false;
    }
}