using System;
using System.Runtime.InteropServices;
using UnityEngine;

public class GStreamerReceiver : MonoBehaviour
{
    public enum StreamCodec
    {
        H265 = 0,
        H264 = 1,
        AV1 = 4
    }

    public enum ReceiverTransportMode
    {
    [InspectorName("RTP/UDP")]
    RTP_UDP = 0,

    [InspectorName("WebRTC Receiver")]
    WebRTC_ReceiverOnly = 1
    }   

    [Header("Receiver Mode")]
    public ReceiverTransportMode transportMode = ReceiverTransportMode.WebRTC_ReceiverOnly;

    [Header("Codec")]
    public StreamCodec codec = StreamCodec.H265;

    [Header("Safe Start")]
    public bool autoStartReceiver = false;
    public bool checkNativeSupportBeforeStart = false;
    public KeyCode startReceiverKey = KeyCode.S;
    public KeyCode stopReceiverKey = KeyCode.X;

    [Header("RTP/UDP Settings")]
    public int port = 5004;

    [Header("WebRTC Receiver-Only Signaling")]
    public int signalingPort = 9001;

    [Header("Resolution")]
    public int width = 1920;
    public int height = 960;

    [Header("Display Rate")]
    public int targetDisplayFps = 30;
    public bool lockUnityFrameRate = true;

    [Header("360 Sphere Settings")]
    public float sphereRadius = 50f;
    public int longitudeSegments = 96;
    public int latitudeSegments = 48;
    public bool followCameraPosition = true;

    [Header("Texture Orientation")]
    public bool flipVertical = true;
    public bool flipHorizontal = false;

    [Header("Initial View")]
    public float initialYawDegrees = 45f;
    public float initialPitchDegrees = 0f;

    [Header("Mouse Drag Look Controls")]
    public bool enableMouseDragLook = true;
    public float mouseSensitivity = 0.15f;
    public bool invertDragX = false;
    public bool invertDragY = false;
    public float minPitchDegrees = -85f;
    public float maxPitchDegrees = 85f;

    [Header("Stats / Debug")]
    public bool showStatsInConsole = true;
    public bool showStatsOverlay = true;
    public float statsIntervalSeconds = 1.0f;

    private Texture2D texture;
    private byte[] frameBuffer;

    private bool started = false;
    private bool nativeReceiverStarted = false;
    private bool startInProgress = false;

    private GameObject sphere360;
    private Renderer sphereRenderer;
    private Camera viewerCamera;

    private float yaw;
    private float pitch;
    private bool isDragging = false;
    private Vector3 lastMousePosition;

    private float statsTimer = 0f;
    private int unityRenderFrames = 0;
    private int textureUploadFrames = 0;
    private float lastUnityFps = 0f;
    private float lastTextureUploadRate = 0f;

    [DllImport("GstUnityReceiver")]
    private static extern bool GstReceiver_IsCodecReceiveSupported(int codec);

    [DllImport("GstUnityReceiver")]
    private static extern bool GstReceiver_StartCodec(
        int port,
        int width,
        int height,
        int codec
    );

    [DllImport("GstUnityReceiver")]
    private static extern bool GstReceiver_StartWebRTC_H265_ReceiverOnly(
        int width,
        int height,
        int signalingPort
    );

    [DllImport("GstUnityReceiver")]
    private static extern bool GstReceiver_StartWebRTC_H264_ReceiverOnly(
        int width,
        int height,
        int signalingPort
    );

    [DllImport("GstUnityReceiver")]
    private static extern bool GstReceiver_StartWebRTC_AV1_ReceiverOnly(
        int width,
        int height,
        int signalingPort
    );

    [DllImport("GstUnityReceiver")]
    private static extern bool GstReceiver_IsWebRTC_H265_ReceiverOnlySupported();

    [DllImport("GstUnityReceiver")]
    private static extern bool GstReceiver_IsWebRTC_H264_ReceiverOnlySupported();

    [DllImport("GstUnityReceiver")]
    private static extern bool GstReceiver_IsWebRTC_AV1_ReceiverOnlySupported();

    [DllImport("GstUnityReceiver")]
    private static extern bool GstReceiver_GetLatestFrame(
        [Out] byte[] dst,
        int dstSize
    );

    [DllImport("GstUnityReceiver")]
    private static extern void GstReceiver_Stop();

    void OnValidate()
    {
        if (!Enum.IsDefined(typeof(ReceiverTransportMode), transportMode))
            transportMode = ReceiverTransportMode.WebRTC_ReceiverOnly;

        if (!Enum.IsDefined(typeof(StreamCodec), codec))
            codec = StreamCodec.H265;

        width = Mathf.Max(16, width);
        height = Mathf.Max(16, height);
        port = Mathf.Clamp(port, 1, 65535);
        signalingPort = Mathf.Clamp(signalingPort, 1, 65535);
        targetDisplayFps = Mathf.Max(1, targetDisplayFps);
        sphereRadius = Mathf.Max(1f, sphereRadius);
        longitudeSegments = Mathf.Max(8, longitudeSegments);
        latitudeSegments = Mathf.Max(4, latitudeSegments);
        statsIntervalSeconds = Mathf.Max(0.1f, statsIntervalSeconds);
    }

    void Start()
    {
        Application.runInBackground = true;

        SanitizeSettings();
        ApplyFrameRateSettings();

        SetupCamera();
        CreateInsideOutSphere();
        CreateTextureAndFrameBuffer();
        ApplyTextureToSphere();

        if (autoStartReceiver)
        {
            Debug.Log("Auto-start is enabled. Starting GStreamer receiver now.");
            StartGStreamerReceiver();
        }
        else
        {
            Debug.Log(
                "GStreamer receiver auto-start is disabled. " +
                "Press " + startReceiverKey + " in the Game view to start manually. " +
                "Press " + stopReceiverKey + " to stop."
            );
        }
    }

    void Update()
    {
        unityRenderFrames++;

        if (!started && !startInProgress && Input.GetKeyDown(startReceiverKey))
        {
            Debug.Log("Manual receiver start requested.");
            StartGStreamerReceiver();
        }

        if ((started || nativeReceiverStarted) && Input.GetKeyDown(stopReceiverKey))
        {
            Debug.Log("Manual receiver stop requested.");
            StopReceiverSafely();
        }

        UpdateVideoFrame();
        HandleMouseDragLook();
        UpdateStatsCounter();
    }

    void LateUpdate()
    {
        if (followCameraPosition && sphere360 != null && viewerCamera != null)
        {
            sphere360.transform.position = viewerCamera.transform.position;
        }
    }

    void SanitizeSettings()
    {
        width = Mathf.Max(16, width);
        height = Mathf.Max(16, height);
        port = Mathf.Clamp(port, 1, 65535);
        signalingPort = Mathf.Clamp(signalingPort, 1, 65535);
        targetDisplayFps = Mathf.Max(1, targetDisplayFps);
        sphereRadius = Mathf.Max(1f, sphereRadius);
        longitudeSegments = Mathf.Max(8, longitudeSegments);
        latitudeSegments = Mathf.Max(4, latitudeSegments);
        statsIntervalSeconds = Mathf.Max(0.1f, statsIntervalSeconds);
    }

    void ApplyFrameRateSettings()
    {
        if (!lockUnityFrameRate)
            return;

        QualitySettings.vSyncCount = 0;
        Application.targetFrameRate = Mathf.Max(1, targetDisplayFps);

        Debug.Log("Unity target frame rate locked to: " + Application.targetFrameRate + " FPS");
    }

    void SetupCamera()
    {
        viewerCamera = Camera.main;

        if (viewerCamera == null)
        {
            GameObject camObj = new GameObject("Main Camera");
            viewerCamera = camObj.AddComponent<Camera>();
            camObj.tag = "MainCamera";
        }

        yaw = initialYawDegrees;
        pitch = initialPitchDegrees;

        viewerCamera.transform.position = Vector3.zero;
        viewerCamera.transform.rotation = Quaternion.Euler(pitch, yaw, 0f);
        viewerCamera.nearClipPlane = 0.01f;
        viewerCamera.farClipPlane = 1000f;
        viewerCamera.clearFlags = CameraClearFlags.SolidColor;
        viewerCamera.backgroundColor = Color.black;
    }

    void CreateTextureAndFrameBuffer()
    {
        int frameSize = width * height * 4;

        frameBuffer = new byte[frameSize];

        texture = new Texture2D(width, height, TextureFormat.BGRA32, false);
        texture.wrapMode = TextureWrapMode.Repeat;
        texture.filterMode = FilterMode.Bilinear;

        Debug.Log(
            "Created Unity texture/frame buffer: " +
            width + "x" + height +
            " | BGRA bytes: " + frameSize
        );
    }

    void ApplyTextureToSphere()
    {
        if (sphereRenderer == null)
        {
            Debug.LogError("Sphere renderer was not created.");
            return;
        }

        sphereRenderer.material.mainTexture = texture;

        float scaleX = flipHorizontal ? -1f : 1f;
        float scaleY = flipVertical ? -1f : 1f;
        float offsetX = flipHorizontal ? 1f : 0f;
        float offsetY = flipVertical ? 1f : 0f;

        sphereRenderer.material.mainTextureScale = new Vector2(scaleX, scaleY);
        sphereRenderer.material.mainTextureOffset = new Vector2(offsetX, offsetY);
    }

    void StartGStreamerReceiver()
    {
        if (started || startInProgress)
        {
            Debug.LogWarning("GStreamer receiver is already started or starting.");
            return;
        }

        startInProgress = true;

        try
        {
            SanitizeSettings();

            Debug.Log(
                "Starting receiver. Transport=" + transportMode +
                ", Codec=" + codec +
                ", Resolution=" + width + "x" + height +
                ", RTP port=" + port +
                ", WebRTC signaling port=" + signalingPort +
                ", Support check=" + checkNativeSupportBeforeStart
            );

            if (transportMode == ReceiverTransportMode.RTP_UDP)
            {
                StartRtpUdpReceiver();
            }
            else if (transportMode == ReceiverTransportMode.WebRTC_ReceiverOnly)
            {
                StartWebRTCReceiverOnly(codec);
            }
            else
            {
                Debug.LogError("Unknown receiver transport mode: " + transportMode);
                started = false;
            }
        }
        catch (DllNotFoundException e)
        {
            started = false;
            nativeReceiverStarted = false;

            Debug.LogError(
                "Could not find GstUnityReceiver.dll or one of its dependencies. " +
                "Make sure GstUnityReceiver.dll is inside Assets/Plugins/x86_64 " +
                "and Unity was opened through the GStreamer launcher.\n" +
                e.Message
            );
        }
        catch (EntryPointNotFoundException e)
        {
            started = false;
            nativeReceiverStarted = false;

            Debug.LogError(
                "The loaded GstUnityReceiver.dll does not contain one of the required exported functions. " +
                "Rebuild the C++ DLL and copy it into Assets/Plugins/x86_64.\n" +
                e.Message
            );
        }
        catch (Exception e)
        {
            started = false;
            nativeReceiverStarted = false;
            Debug.LogError("Unexpected managed error while starting GStreamer receiver:\n" + e);
        }
        finally
        {
            startInProgress = false;
        }
    }

    void StartRtpUdpReceiver()
    {
        if (checkNativeSupportBeforeStart)
        {
            bool supported = GstReceiver_IsCodecReceiveSupported((int)codec);

            if (!supported)
            {
                Debug.LogError(
                    "Selected RTP/UDP codec is not supported by this GStreamer DLL/runtime: " +
                    codec
                );

                started = false;
                nativeReceiverStarted = false;
                return;
            }
        }

        bool ok = GstReceiver_StartCodec(
            port,
            width,
            height,
            (int)codec
        );

        started = ok;
        nativeReceiverStarted = ok;

        if (!ok)
        {
            Debug.LogError("Failed to start GStreamer RTP/UDP receiver. Codec: " + codec);
        }
        else
        {
            Debug.Log(
                "GStreamer RTP/UDP receiver started. " +
                "Codec: " + codec +
                ", Resolution: " + width + "x" + height +
                ", Port: " + port
            );
        }
    }

    void StartWebRTCReceiverOnly(StreamCodec selectedCodec)
    {
        if (checkNativeSupportBeforeStart)
        {
            bool supported = false;

            if (selectedCodec == StreamCodec.H265)
                supported = GstReceiver_IsWebRTC_H265_ReceiverOnlySupported();
            else if (selectedCodec == StreamCodec.H264)
                supported = GstReceiver_IsWebRTC_H264_ReceiverOnlySupported();
            else if (selectedCodec == StreamCodec.AV1)
                supported = GstReceiver_IsWebRTC_AV1_ReceiverOnlySupported();

            Debug.Log("WebRTC " + selectedCodec + " receiver-only supported: " + supported);

            if (!supported)
            {
                Debug.LogError(
                    "WebRTC receiver-only mode is not supported for codec " +
                    selectedCodec +
                    ". Make sure Unity was launched through the GStreamer launcher."
                );

                started = false;
                nativeReceiverStarted = false;
                return;
            }
        }

        bool ok = false;

        if (selectedCodec == StreamCodec.H265)
        {
            ok = GstReceiver_StartWebRTC_H265_ReceiverOnly(
                width,
                height,
                signalingPort
            );
        }
        else if (selectedCodec == StreamCodec.H264)
        {
            ok = GstReceiver_StartWebRTC_H264_ReceiverOnly(
                width,
                height,
                signalingPort
            );
        }
        else if (selectedCodec == StreamCodec.AV1)
        {
            ok = GstReceiver_StartWebRTC_AV1_ReceiverOnly(
                width,
                height,
                signalingPort
            );
        }

        started = ok;
        nativeReceiverStarted = ok;

        if (!ok)
        {
            Debug.LogError(
                "Failed to start GStreamer WebRTC receiver-only mode. " +
                "Codec: " + selectedCodec +
                ", Resolution: " + width + "x" + height +
                ", Signaling port: " + signalingPort
            );
        }
        else
        {
            Debug.Log(
                "GStreamer WebRTC receiver-only started. " +
                "Codec: " + selectedCodec +
                ", Resolution: " + width + "x" + height +
                ", Signaling port: " + signalingPort +
                ". Now start the external sender with the same codec."
            );
        }
    }

    void UpdateVideoFrame()
    {
        if (!started || texture == null || frameBuffer == null)
            return;

        bool gotFrame = false;

        try
        {
            gotFrame = GstReceiver_GetLatestFrame(frameBuffer, frameBuffer.Length);
        }
        catch (Exception e)
        {
            Debug.LogError("Managed exception while getting latest frame:\n" + e);
            StopReceiverSafely();
            return;
        }

        if (gotFrame)
        {
            texture.LoadRawTextureData(frameBuffer);
            texture.Apply(false);

            textureUploadFrames++;
        }
    }

    void HandleMouseDragLook()
    {
        if (!enableMouseDragLook || viewerCamera == null)
            return;

        if (Input.GetMouseButtonDown(0))
        {
            isDragging = true;
            lastMousePosition = Input.mousePosition;
        }

        if (Input.GetMouseButtonUp(0))
        {
            isDragging = false;
        }

        if (isDragging && Input.GetMouseButton(0))
        {
            Vector3 delta = Input.mousePosition - lastMousePosition;
            lastMousePosition = Input.mousePosition;

            float xMultiplier = invertDragX ? -1f : 1f;
            float yMultiplier = invertDragY ? -1f : 1f;

            yaw += delta.x * mouseSensitivity * xMultiplier;
            pitch -= delta.y * mouseSensitivity * yMultiplier;

            pitch = Mathf.Clamp(pitch, minPitchDegrees, maxPitchDegrees);

            viewerCamera.transform.rotation = Quaternion.Euler(pitch, yaw, 0f);
        }
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
                Debug.Log(
                    "Transport: " + transportMode +
                    " | Codec: " + codec +
                    " | Native started: " + nativeReceiverStarted +
                    " | Unity render FPS: " + lastUnityFps.ToString("F1") +
                    " | Texture uploads/s: " + lastTextureUploadRate.ToString("F1")
                );
            }

            unityRenderFrames = 0;
            textureUploadFrames = 0;
            statsTimer = 0f;
        }
    }

    void OnGUI()
    {
        if (!showStatsOverlay)
            return;

        GUI.Label(
            new Rect(20, 20, 1600, 30),
            "Transport: " + transportMode +
            " | Codec: " + codec +
            " | Native started: " + nativeReceiverStarted +
            " | Unity FPS: " + lastUnityFps.ToString("F1") +
            " | Texture uploads/s: " + lastTextureUploadRate.ToString("F1") +
            " | Target: " + targetDisplayFps + " FPS" +
            " | Signaling port: " + signalingPort +
            " | Start: " + startReceiverKey +
            " | Stop: " + stopReceiverKey
        );
    }

    void CreateInsideOutSphere()
    {
        sphere360 = new GameObject("AutoCreated_InsideOut_360_Sphere");
        sphere360.transform.position = Vector3.zero;
        sphere360.transform.rotation = Quaternion.identity;
        sphere360.transform.localScale = Vector3.one;

        MeshFilter meshFilter = sphere360.AddComponent<MeshFilter>();
        MeshRenderer meshRenderer = sphere360.AddComponent<MeshRenderer>();

        Mesh mesh = CreateInwardFacingSphereMesh(
            sphereRadius,
            longitudeSegments,
            latitudeSegments
        );

        meshFilter.sharedMesh = mesh;

        Shader shader = Shader.Find("Unlit/Texture");

        if (shader == null)
        {
            Debug.LogWarning("Unlit/Texture shader not found. Falling back to Standard shader.");
            shader = Shader.Find("Standard");
        }

        Material mat = new Material(shader);
        mat.name = "Runtime_InsideOut360_Material";

        meshRenderer.material = mat;
        sphereRenderer = meshRenderer;
    }

    Mesh CreateInwardFacingSphereMesh(float radius, int lonSegments, int latSegments)
    {
        Mesh mesh = new Mesh();
        mesh.name = "Runtime_InwardFacing_360_Sphere";

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
                uvs[index] = new Vector2(u, 1f - v);

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
        StopReceiverSafely();
    }

    void OnDestroy()
    {
        StopReceiverSafely();
    }

    void OnApplicationQuit()
    {
        StopReceiverSafely();
    }

    void StopReceiverSafely()
    {
        if (!nativeReceiverStarted && !started)
            return;

        try
        {
            GstReceiver_Stop();
        }
        catch
        {
            // Ignore shutdown errors when Unity exits Play Mode.
        }

        started = false;
        nativeReceiverStarted = false;
    }
}