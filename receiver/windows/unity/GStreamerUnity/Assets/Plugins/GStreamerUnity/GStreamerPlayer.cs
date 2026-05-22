using System;
using System.Globalization;
using System.Net.Sockets;
using System.Text;
using UnityEngine;
using UnityEngine.Rendering;

namespace GStreamerUnity
{
    public class GStreamerPlayer : MonoBehaviour
    {
        public enum DisplayMode
        {
            Auto,
            Flat2D,
            Sphere360
        }

        [Header("Display Targets")]
        [SerializeField] private Renderer flatTarget;
        [SerializeField] private Renderer sphereTarget;
        [SerializeField] private DisplayMode displayMode = DisplayMode.Auto;

        [Header("Auto Display Creation")]
        [SerializeField] private bool autoCreateDisplayTargets = true;
        [SerializeField] private bool autoPositionCameraFor360 = true;
        [SerializeField] private float flatDistanceFromCamera = 3.0f;
        [SerializeField] private float flatHeight = 2.0f;
        [SerializeField] private float sphereRadius = 20.0f;

        [Header("360 Image Quality")]
        [SerializeField] private bool useHighResolutionSphereMesh = true;
        [SerializeField] private int sphereLongitudeSegments = 256;
        [SerializeField] private int sphereLatitudeSegments = 128;
        [SerializeField] private FilterMode streamTextureFilterMode = FilterMode.Bilinear;
        [SerializeField] private int streamTextureAnisoLevel = 4;
        [SerializeField] private bool sphereHorizontalWrap = true;
        [SerializeField] private bool forceUnlitWhiteMaterial = true;
        [SerializeField] private bool disableMaterialBackfaceCulling = true;

        [Header("Auto Detection")]
        [SerializeField] private float equirectangularAspect = 2.0f;
        [SerializeField] private float aspectTolerance = 0.12f;

        [Header("Transport")]
        [SerializeField] private string transport = "webrtc";
        [SerializeField] private string udpUri = "127.0.0.1:5004";
        [SerializeField] private string webrtcUri = "127.0.0.1:9001";

        [Header("Stream")]
        [SerializeField] private uint width = 4096;
        [SerializeField] private uint height = 2048;
        [SerializeField] private string codec = "h264";

        [Header("Texture Orientation")]
        [SerializeField] private bool flatFlipVertical = true;
        [SerializeField] private bool flatFlipHorizontal = false;
        [SerializeField] private bool sphereFlipVertical = true;
        [SerializeField] private bool sphereFlipHorizontal = false;

        [Header("Mouse Drag Look")]
        [SerializeField] private bool enableMouseDragLook = true;
        [SerializeField] private int mouseButton = 0;
        [SerializeField] private float mouseSensitivity = 0.12f;
        [SerializeField] private bool invertMouseY = false;
        [SerializeField] private float minPitch = -89.0f;
        [SerializeField] private float maxPitch = 89.0f;
        [SerializeField] private bool resetCameraRotationOnStart = true;
        [SerializeField] private bool lockCursorWhileDragging = false;

        [Header("Debug")]
        [SerializeField] private bool printFrameCounters = true;

        [Header("Receiver Feedback")]
        [SerializeField] private bool enableReceiverFeedback = true;
        [SerializeField] private string receiverFeedbackHost = "127.0.0.1";
        [SerializeField] private int receiverFeedbackPort = 9101;
        [SerializeField] private float receiverFeedbackInterval = 1.0f;
        [SerializeField] private float receiverFeedbackReconnectInterval = 1.0f;
        [SerializeField] private float receiverStallTimeoutSeconds = 2.0f;

        private bool createdMaterial;
        private bool usingSphere;
        private GStreamerPipeline pipeline;
        private ulong lastPrintedFrameId;
        private float lastPrintTime;

        private Material flatMaterial;
        private Material sphereMaterial;

        private GameObject autoFlatObject;
        private GameObject autoSphereObject;

        private Renderer preparedInsideSphereRenderer;
        private Mesh generatedInsideSphereMesh;
        private int generatedSphereLongitudeSegments;
        private int generatedSphereLatitudeSegments;

        private Camera controlledCamera;
        private bool cameraLookInitialized;
        private bool draggingCamera;
        private Vector3 lastMousePosition;
        private float cameraYaw;
        private float cameraPitch;

        private TcpClient feedbackClient;
        private NetworkStream feedbackStream;
        private float lastFeedbackConnectAttemptTime = -1000.0f;
        private float lastFeedbackSendTime;
        private float unityFpsWindowStartTime;
        private int unityFpsFrameCount;
        private float measuredUnityFps;
        private float copiedFpsWindowStartTime;
        private ulong copiedFpsWindowStartFrameId;
        private float measuredCopiedFps;
        private ulong lastReceiverCopiedFrameId;
        private float lastReceiverFrameChangeTime;

        private string BuildNativeUri()
        {
            string mode = string.IsNullOrWhiteSpace(transport)
                ? "udp"
                : transport.Trim().ToLowerInvariant();

            if (mode == "webrtc")
            {
                return "webrtc://" + webrtcUri;
            }

            return "udp://" + udpUri;
        }

        private bool ShouldUseSphere()
        {
            if (displayMode == DisplayMode.Sphere360)
            {
                return true;
            }

            if (displayMode == DisplayMode.Flat2D)
            {
                return false;
            }

            if (height == 0)
            {
                return false;
            }

            float aspect = (float)width / height;
            return Mathf.Abs(aspect - equirectangularAspect) <= aspectTolerance;
        }

        private Shader FindUnlitTextureShader()
        {
            Shader shader = Shader.Find("Unlit/Texture");

            if (shader == null)
            {
                shader = Shader.Find("Universal Render Pipeline/Unlit");
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

        private Material CreateStreamMaterial(string materialName)
        {
            Shader shader = FindUnlitTextureShader();

            if (shader == null)
            {
                shader = Shader.Find("Standard");
            }

            Material material = new Material(shader);
            material.name = materialName;

            ConfigureMaterialForVideo(material);

            if (material.HasProperty("_MainTex"))
            {
                material.SetTexture("_MainTex", null);
            }

            if (material.HasProperty("_BaseMap"))
            {
                material.SetTexture("_BaseMap", null);
            }

            return material;
        }

        private void ConfigureMaterialForVideo(Material material)
        {
            if (material == null)
            {
                return;
            }

            if (forceUnlitWhiteMaterial)
            {
                if (material.HasProperty("_Color"))
                {
                    material.SetColor("_Color", Color.white);
                }

                if (material.HasProperty("_BaseColor"))
                {
                    material.SetColor("_BaseColor", Color.white);
                }

                if (material.HasProperty("_EmissionColor"))
                {
                    material.SetColor("_EmissionColor", Color.black);
                }

                if (material.HasProperty("_Metallic"))
                {
                    material.SetFloat("_Metallic", 0.0f);
                }

                if (material.HasProperty("_Glossiness"))
                {
                    material.SetFloat("_Glossiness", 0.0f);
                }

                if (material.HasProperty("_Smoothness"))
                {
                    material.SetFloat("_Smoothness", 0.0f);
                }

                if (material.HasProperty("_SpecColor"))
                {
                    material.SetColor("_SpecColor", Color.black);
                }
            }

            if (disableMaterialBackfaceCulling && material.HasProperty("_Cull"))
            {
                material.SetInt("_Cull", (int)CullMode.Off);
            }
        }

        private void ConfigureStreamTexture(Texture texture, bool forSphere)
        {
            if (texture == null)
            {
                return;
            }

            texture.filterMode = streamTextureFilterMode;
            texture.anisoLevel = Mathf.Clamp(streamTextureAnisoLevel, 0, 16);
            texture.mipMapBias = 0.0f;

            if (forSphere)
            {
                if (sphereHorizontalWrap)
                {
                    texture.wrapModeU = TextureWrapMode.Repeat;
                    texture.wrapModeV = TextureWrapMode.Clamp;
                }
                else
                {
                    texture.wrapMode = TextureWrapMode.Clamp;
                }
            }
            else
            {
                texture.wrapMode = TextureWrapMode.Clamp;
            }
        }

        private void AssignTextureToMaterial(Material material, Texture texture)
        {
            if (material == null || texture == null)
            {
                return;
            }

            ConfigureMaterialForVideo(material);

            if (material.HasProperty("_MainTex"))
            {
                material.SetTexture("_MainTex", texture);
            }

            if (material.HasProperty("_BaseMap"))
            {
                material.SetTexture("_BaseMap", texture);
            }
        }

        private void SetMaterialScaleOffset(Material material, Vector2 scale, Vector2 offset)
        {
            if (material == null)
            {
                return;
            }

            if (material.HasProperty("_MainTex"))
            {
                material.SetTextureScale("_MainTex", scale);
                material.SetTextureOffset("_MainTex", offset);
            }

            if (material.HasProperty("_BaseMap"))
            {
                material.SetTextureScale("_BaseMap", scale);
                material.SetTextureOffset("_BaseMap", offset);
            }
        }

        private void RemoveColliderIfPresent(GameObject obj)
        {
            if (obj == null)
            {
                return;
            }

            Collider collider = obj.GetComponent<Collider>();

            if (collider != null)
            {
                Destroy(collider);
            }
        }

        private Camera GetMainCamera()
        {
            Camera cam = Camera.main;

            if (cam != null)
            {
                return cam;
            }

#if UNITY_2023_1_OR_NEWER
            return FindFirstObjectByType<Camera>();
#else
            return FindObjectOfType<Camera>();
#endif
        }

        private float NormalizeAngle(float angle)
        {
            while (angle > 180.0f)
            {
                angle -= 360.0f;
            }

            while (angle < -180.0f)
            {
                angle += 360.0f;
            }

            return angle;
        }

        private void InitializeCameraLook(Camera cam)
        {
            if (cam == null)
            {
                return;
            }

            controlledCamera = cam;

            Vector3 euler = cam.transform.rotation.eulerAngles;
            cameraYaw = NormalizeAngle(euler.y);
            cameraPitch = NormalizeAngle(euler.x);
            cameraPitch = Mathf.Clamp(cameraPitch, minPitch, maxPitch);

            cameraLookInitialized = true;
        }

        private void ResetCameraLook(Camera cam)
        {
            if (cam == null)
            {
                return;
            }

            cam.transform.rotation = Quaternion.identity;
            cameraYaw = 0.0f;
            cameraPitch = 0.0f;
            controlledCamera = cam;
            cameraLookInitialized = true;
        }

        private void HandleMouseDragLook()
        {
            if (!enableMouseDragLook || !usingSphere)
            {
                return;
            }

            Camera cam = GetMainCamera();

            if (cam == null)
            {
                return;
            }

            if (!cameraLookInitialized || controlledCamera != cam)
            {
                InitializeCameraLook(cam);
            }

            if (Input.GetMouseButtonDown(mouseButton))
            {
                draggingCamera = true;
                lastMousePosition = Input.mousePosition;

                if (lockCursorWhileDragging)
                {
                    Cursor.lockState = CursorLockMode.Locked;
                    Cursor.visible = false;
                }
            }

            if (Input.GetMouseButtonUp(mouseButton))
            {
                draggingCamera = false;

                if (lockCursorWhileDragging)
                {
                    Cursor.lockState = CursorLockMode.None;
                    Cursor.visible = true;
                }
            }

            if (!draggingCamera || !Input.GetMouseButton(mouseButton))
            {
                return;
            }

            Vector3 currentMousePosition = Input.mousePosition;
            Vector3 delta = currentMousePosition - lastMousePosition;
            lastMousePosition = currentMousePosition;

            cameraYaw += delta.x * mouseSensitivity;

            if (invertMouseY)
            {
                cameraPitch += delta.y * mouseSensitivity;
            }
            else
            {
                cameraPitch -= delta.y * mouseSensitivity;
            }

            cameraPitch = Mathf.Clamp(cameraPitch, minPitch, maxPitch);

            cam.transform.rotation = Quaternion.Euler(cameraPitch, cameraYaw, 0.0f);

            if (sphereTarget != null)
            {
                sphereTarget.transform.position = cam.transform.position;
            }
        }

        private void EnsureDisplayTargets()
        {
            if (!autoCreateDisplayTargets)
            {
                return;
            }

            if (flatTarget == null)
            {
                autoFlatObject = GameObject.CreatePrimitive(PrimitiveType.Quad);
                autoFlatObject.name = "AutoDisplayFlat2D";
                RemoveColliderIfPresent(autoFlatObject);
                flatTarget = autoFlatObject.GetComponent<Renderer>();
            }

            if (sphereTarget == null)
            {
                autoSphereObject = new GameObject("AutoDisplaySphere360");
                MeshFilter meshFilter = autoSphereObject.AddComponent<MeshFilter>();
                MeshRenderer meshRenderer = autoSphereObject.AddComponent<MeshRenderer>();
                meshFilter.sharedMesh = CreateHighQualityInsideSphereMesh();
                sphereTarget = meshRenderer;
            }

            if (flatMaterial == null)
            {
                flatMaterial = CreateStreamMaterial("AutoStreamFlatMaterial");
            }

            if (sphereMaterial == null)
            {
                sphereMaterial = CreateStreamMaterial("AutoStreamSphereMaterial");
            }

            if (flatTarget != null)
            {
                flatTarget.sharedMaterial = flatMaterial;
            }

            if (sphereTarget != null)
            {
                sphereTarget.sharedMaterial = sphereMaterial;
                EnsureHighQualityInsideSphereMesh(sphereTarget);
            }

            PositionDisplayObjects();
        }

        private int NormalizeSegmentCount(int value, int minimum, int maximum)
        {
            int normalized = Mathf.Clamp(value, minimum, maximum);

            if (normalized % 2 != 0)
            {
                normalized++;
            }

            return Mathf.Clamp(normalized, minimum, maximum);
        }

        private Mesh CreateHighQualityInsideSphereMesh()
        {
            int lonSegments = NormalizeSegmentCount(sphereLongitudeSegments, 64, 768);
            int latSegments = NormalizeSegmentCount(sphereLatitudeSegments, 32, 384);

            int vertexCount = (lonSegments + 1) * (latSegments + 1);
            int indexCount = lonSegments * latSegments * 6;

            Vector3[] vertices = new Vector3[vertexCount];
            Vector3[] normals = new Vector3[vertexCount];
            Vector2[] uvs = new Vector2[vertexCount];
            int[] indices = new int[indexCount];

            int vertexIndex = 0;

            for (int lat = 0; lat <= latSegments; lat++)
            {
                float v = (float)lat / latSegments;
                float theta = Mathf.PI * v;
                float sinTheta = Mathf.Sin(theta);
                float cosTheta = Mathf.Cos(theta);

                for (int lon = 0; lon <= lonSegments; lon++)
                {
                    float u = (float)lon / lonSegments;
                    float phi = Mathf.PI * 2.0f * u;

                    float sinPhi = Mathf.Sin(phi);
                    float cosPhi = Mathf.Cos(phi);

                    Vector3 position = new Vector3(
                        sinPhi * sinTheta * 0.5f,
                        cosTheta * 0.5f,
                        cosPhi * sinTheta * 0.5f
                    );

                    vertices[vertexIndex] = position;

                    if (position.sqrMagnitude > 0.000001f)
                    {
                        normals[vertexIndex] = -position.normalized;
                    }
                    else
                    {
                        normals[vertexIndex] = Vector3.down;
                    }

                    uvs[vertexIndex] = new Vector2(u, 1.0f - v);
                    vertexIndex++;
                }
            }

            int index = 0;
            int row = lonSegments + 1;

            for (int lat = 0; lat < latSegments; lat++)
            {
                for (int lon = 0; lon < lonSegments; lon++)
                {
                    int i0 = lat * row + lon;
                    int i1 = i0 + 1;
                    int i2 = i0 + row;
                    int i3 = i2 + 1;

                    indices[index++] = i0;
                    indices[index++] = i1;
                    indices[index++] = i2;

                    indices[index++] = i1;
                    indices[index++] = i3;
                    indices[index++] = i2;
                }
            }

            Mesh mesh = new Mesh();
            mesh.name = "HighQualityInsideEquirectangularSphere";

            if (vertexCount > 65535)
            {
                mesh.indexFormat = IndexFormat.UInt32;
            }

            mesh.vertices = vertices;
            mesh.normals = normals;
            mesh.uv = uvs;
            mesh.triangles = indices;
            mesh.RecalculateBounds();

            generatedSphereLongitudeSegments = lonSegments;
            generatedSphereLatitudeSegments = latSegments;

            return mesh;
        }

        private void EnsureHighQualityInsideSphereMesh(Renderer renderer)
        {
            if (renderer == null)
            {
                return;
            }

            MeshFilter meshFilter = renderer.GetComponent<MeshFilter>();

            if (meshFilter == null)
            {
                meshFilter = renderer.gameObject.AddComponent<MeshFilter>();
            }

            if (!useHighResolutionSphereMesh)
            {
                EnsureInsideVisibleSphereMeshFromExisting(renderer);
                return;
            }

            int lonSegments = NormalizeSegmentCount(sphereLongitudeSegments, 64, 768);
            int latSegments = NormalizeSegmentCount(sphereLatitudeSegments, 32, 384);

            bool alreadyPrepared =
                preparedInsideSphereRenderer == renderer &&
                generatedInsideSphereMesh != null &&
                generatedSphereLongitudeSegments == lonSegments &&
                generatedSphereLatitudeSegments == latSegments &&
                meshFilter.sharedMesh == generatedInsideSphereMesh;

            if (alreadyPrepared)
            {
                return;
            }

            if (generatedInsideSphereMesh != null)
            {
                Destroy(generatedInsideSphereMesh);
                generatedInsideSphereMesh = null;
            }

            generatedInsideSphereMesh = CreateHighQualityInsideSphereMesh();
            meshFilter.sharedMesh = generatedInsideSphereMesh;
            preparedInsideSphereRenderer = renderer;

            Debug.Log(
                $"[GStreamerPlayer] Prepared high-quality inside-visible 360 sphere mesh. " +
                $"Segments={generatedSphereLongitudeSegments}x{generatedSphereLatitudeSegments}"
            );
        }

        private void EnsureInsideVisibleSphereMeshFromExisting(Renderer renderer)
        {
            if (renderer == null)
            {
                return;
            }

            if (preparedInsideSphereRenderer == renderer && generatedInsideSphereMesh != null)
            {
                return;
            }

            MeshFilter meshFilter = renderer.GetComponent<MeshFilter>();

            if (meshFilter == null || meshFilter.sharedMesh == null)
            {
                return;
            }

            if (generatedInsideSphereMesh != null)
            {
                Destroy(generatedInsideSphereMesh);
                generatedInsideSphereMesh = null;
            }

            Mesh sourceMesh = meshFilter.sharedMesh;
            Mesh insideMesh = Instantiate(sourceMesh);
            insideMesh.name = sourceMesh.name + "_InsideVisible";

            Vector3[] normals = insideMesh.normals;

            if (normals != null && normals.Length > 0)
            {
                for (int i = 0; i < normals.Length; i++)
                {
                    normals[i] = -normals[i];
                }

                insideMesh.normals = normals;
            }

            for (int submesh = 0; submesh < insideMesh.subMeshCount; submesh++)
            {
                int[] triangles = insideMesh.GetTriangles(submesh);

                for (int i = 0; i < triangles.Length; i += 3)
                {
                    int temp = triangles[i];
                    triangles[i] = triangles[i + 1];
                    triangles[i + 1] = temp;
                }

                insideMesh.SetTriangles(triangles, submesh);
            }

            insideMesh.RecalculateBounds();

            meshFilter.sharedMesh = insideMesh;
            generatedInsideSphereMesh = insideMesh;
            preparedInsideSphereRenderer = renderer;

            Debug.Log("[GStreamerPlayer] Prepared fallback inside-visible 360 sphere mesh.");
        }

        private void PositionDisplayObjects()
        {
            Camera cam = GetMainCamera();

            if (cam == null)
            {
                return;
            }

            float aspect = height > 0 ? (float)width / height : 16.0f / 9.0f;

            if (usingSphere && autoPositionCameraFor360)
            {
                cam.transform.position = Vector3.zero;
                cam.nearClipPlane = 0.01f;
                cam.farClipPlane = Mathf.Max(1000.0f, sphereRadius * 4.0f);

                if (!cameraLookInitialized)
                {
                    if (resetCameraRotationOnStart)
                    {
                        ResetCameraLook(cam);
                    }
                    else
                    {
                        InitializeCameraLook(cam);
                    }
                }
            }

            if (flatTarget != null)
            {
                Transform flat = flatTarget.transform;
                flat.position = cam.transform.position + cam.transform.forward * flatDistanceFromCamera;
                flat.rotation = cam.transform.rotation;

                float flatWidth = flatHeight * aspect;
                flat.localScale = new Vector3(flatWidth, flatHeight, 1.0f);
            }

            if (sphereTarget != null)
            {
                Transform sphere = sphereTarget.transform;
                sphere.position = cam.transform.position;
                sphere.rotation = Quaternion.identity;

                float diameter = sphereRadius * 2.0f;
                sphere.localScale = new Vector3(diameter, diameter, diameter);
            }
        }

        private void SetRendererVisible(Renderer renderer, bool visible)
        {
            if (renderer == null)
            {
                return;
            }

            renderer.enabled = visible;
        }

        private Renderer GetActiveTarget()
        {
            usingSphere = ShouldUseSphere();

            EnsureDisplayTargets();

            SetRendererVisible(flatTarget, !usingSphere);
            SetRendererVisible(sphereTarget, usingSphere);

            PositionDisplayObjects();

            return usingSphere ? sphereTarget : flatTarget;
        }

        private void ApplyTextureOrientation(Renderer targetRenderer)
        {
            if (targetRenderer == null)
            {
                return;
            }

            bool flipVertical = usingSphere ? sphereFlipVertical : flatFlipVertical;
            bool flipHorizontal = usingSphere ? sphereFlipHorizontal : flatFlipHorizontal;

            Vector2 scale = new Vector2(
                flipHorizontal ? -1.0f : 1.0f,
                flipVertical ? -1.0f : 1.0f
            );

            Vector2 offset = new Vector2(
                flipHorizontal ? 1.0f : 0.0f,
                flipVertical ? 1.0f : 0.0f
            );

            Material material = targetRenderer.material;
            SetMaterialScaleOffset(material, scale, offset);
        }

        private string ReceiverDisplayModeName()
        {
            return usingSphere ? "Sphere360" : "Flat2D";
        }

        private void UpdateReceiverFeedbackMetrics()
        {
            if (!enableReceiverFeedback)
            {
                return;
            }

            float now = Time.realtimeSinceStartup;

            if (unityFpsWindowStartTime <= 0.0f)
            {
                unityFpsWindowStartTime = now;
            }

            unityFpsFrameCount++;
            float unityWindow = now - unityFpsWindowStartTime;

            if (unityWindow >= 1.0f)
            {
                measuredUnityFps = unityFpsFrameCount / Mathf.Max(0.0001f, unityWindow);
                unityFpsFrameCount = 0;
                unityFpsWindowStartTime = now;
            }

            ulong copiedFrameId = pipeline != null ? pipeline.GetCopiedFrameId() : 0;

            if (copiedFpsWindowStartTime <= 0.0f)
            {
                copiedFpsWindowStartTime = now;
                copiedFpsWindowStartFrameId = copiedFrameId;
                lastReceiverCopiedFrameId = copiedFrameId;
                lastReceiverFrameChangeTime = now;
            }

            if (copiedFrameId != lastReceiverCopiedFrameId)
            {
                lastReceiverCopiedFrameId = copiedFrameId;
                lastReceiverFrameChangeTime = now;
            }

            float copiedWindow = now - copiedFpsWindowStartTime;

            if (copiedWindow >= 1.0f)
            {
                ulong frameDelta = copiedFrameId >= copiedFpsWindowStartFrameId
                    ? copiedFrameId - copiedFpsWindowStartFrameId
                    : 0;

                measuredCopiedFps = frameDelta / Mathf.Max(0.0001f, copiedWindow);
                copiedFpsWindowStartFrameId = copiedFrameId;
                copiedFpsWindowStartTime = now;
            }

            if (now - lastFeedbackSendTime >= Mathf.Max(0.25f, receiverFeedbackInterval))
            {
                lastFeedbackSendTime = now;
                SendReceiverFeedback(copiedFrameId, now);
            }
        }

        private void EnsureReceiverFeedbackConnected()
        {
            if (!enableReceiverFeedback)
            {
                return;
            }

            if (feedbackClient != null && feedbackClient.Connected && feedbackStream != null)
            {
                return;
            }

            float now = Time.realtimeSinceStartup;

            if (now - lastFeedbackConnectAttemptTime < receiverFeedbackReconnectInterval)
            {
                return;
            }

            lastFeedbackConnectAttemptTime = now;
            CloseReceiverFeedbackConnection();

            try
            {
                TcpClient client = new TcpClient();
                IAsyncResult connectResult = client.BeginConnect(receiverFeedbackHost, receiverFeedbackPort, null, null);
                bool connected = connectResult.AsyncWaitHandle.WaitOne(50);

                if (!connected)
                {
                    client.Close();
                    return;
                }

                client.EndConnect(connectResult);
                client.NoDelay = true;
                feedbackClient = client;
                feedbackStream = feedbackClient.GetStream();
                Debug.Log($"[GStreamerPlayer] Receiver feedback connected to {receiverFeedbackHost}:{receiverFeedbackPort}");
            }
            catch
            {
                CloseReceiverFeedbackConnection();
            }
        }

        private void SendReceiverFeedback(ulong copiedFrameId, float now)
        {
            EnsureReceiverFeedbackConnected();

            if (feedbackClient == null || !feedbackClient.Connected || feedbackStream == null)
            {
                return;
            }

            bool textureAttached = createdMaterial && pipeline != null && pipeline.GetTexture() != null;
            bool stalled = textureAttached && copiedFrameId > 0 && now - lastReceiverFrameChangeTime > receiverStallTimeoutSeconds;

            string message =
                "RECEIVER_METRICS|" +
                "receiver_alive=1|" +
                "display_mode=" + ReceiverDisplayModeName() + "|" +
                "unity_fps=" + measuredUnityFps.ToString("F1", CultureInfo.InvariantCulture) + "|" +
                "copied_fps=" + measuredCopiedFps.ToString("F1", CultureInfo.InvariantCulture) + "|" +
                "last_frame_id=" + copiedFrameId + "|" +
                "texture_attached=" + (textureAttached ? "1" : "0") + "|" +
                "stall=" + (stalled ? "1" : "0") + "\n";

            try
            {
                byte[] bytes = Encoding.UTF8.GetBytes(message);
                feedbackStream.Write(bytes, 0, bytes.Length);
                feedbackStream.Flush();
            }
            catch
            {
                CloseReceiverFeedbackConnection();
            }
        }

        private void CloseReceiverFeedbackConnection()
        {
            if (feedbackStream != null)
            {
                try
                {
                    feedbackStream.Close();
                }
                catch
                {
                }

                feedbackStream = null;
            }

            if (feedbackClient != null)
            {
                try
                {
                    feedbackClient.Close();
                }
                catch
                {
                }

                feedbackClient = null;
            }
        }

        private void OnEnable()
        {
            string nativeUri = BuildNativeUri();
            usingSphere = ShouldUseSphere();

            cameraLookInitialized = false;
            draggingCamera = false;
            unityFpsWindowStartTime = Time.realtimeSinceStartup;
            copiedFpsWindowStartTime = Time.realtimeSinceStartup;
            unityFpsFrameCount = 0;
            measuredUnityFps = 0.0f;
            measuredCopiedFps = 0.0f;
            lastReceiverCopiedFrameId = 0;
            copiedFpsWindowStartFrameId = 0;
            lastReceiverFrameChangeTime = Time.realtimeSinceStartup;

            EnsureDisplayTargets();

            Debug.Log(
                $"[GStreamerPlayer] Creating pipeline. " +
                $"Transport={transport} URI={nativeUri} " +
                $"Resolution={width}x{height} Codec={codec} " +
                $"Display={(usingSphere ? "Sphere360" : "Flat2D")}"
            );

            Renderer activeTarget = GetActiveTarget();

            if (activeTarget == null)
            {
                Debug.LogError("[GStreamerPlayer] Could not create or find a display target.");
                return;
            }

            pipeline = GStreamerPipeline.Create(width, height, nativeUri, codec);
            pipeline.CreateTexture();
        }

        private void Update()
        {
            HandleMouseDragLook();

            if (pipeline == null)
            {
                return;
            }

            pipeline.UpdateDisplayTexture();

            Texture texture = pipeline.GetTexture();

            if (texture != null && !createdMaterial)
            {
                Renderer activeTarget = GetActiveTarget();

                if (activeTarget == null)
                {
                    Debug.LogError("[GStreamerPlayer] Active display target is null.");
                    return;
                }

                ConfigureStreamTexture(texture, usingSphere);
                AssignTextureToMaterial(activeTarget.material, texture);
                ApplyTextureOrientation(activeTarget);

                createdMaterial = true;

                Debug.Log(
                    $"[GStreamerPlayer] Display texture attached. " +
                    $"URI={BuildNativeUri()} Resolution={width}x{height} " +
                    $"Codec={codec} Display={(usingSphere ? "Sphere360" : "Flat2D")} " +
                    $"TextureFilter={streamTextureFilterMode} Aniso={streamTextureAnisoLevel}"
                );
            }

            UpdateReceiverFeedbackMetrics();

            if (printFrameCounters && Time.realtimeSinceStartup - lastPrintTime >= 1.0f)
            {
                ulong frameId = pipeline.GetCopiedFrameId();

                if (frameId != lastPrintedFrameId)
                {
                    Debug.Log($"[GStreamerPlayer] A2f FIX16 copied frame id={frameId}");
                    lastPrintedFrameId = frameId;
                }

                lastPrintTime = Time.realtimeSinceStartup;
            }
        }

        private void OnDisable()
        {
            if (pipeline != null)
            {
                pipeline.Dispose();
                pipeline = null;
            }

            CloseReceiverFeedbackConnection();

            if (lockCursorWhileDragging)
            {
                Cursor.lockState = CursorLockMode.None;
                Cursor.visible = true;
            }

            draggingCamera = false;
            createdMaterial = false;
            lastPrintedFrameId = 0;
        }

        private void OnDestroy()
        {
            CloseReceiverFeedbackConnection();

            if (flatMaterial != null)
            {
                Destroy(flatMaterial);
                flatMaterial = null;
            }

            if (sphereMaterial != null)
            {
                Destroy(sphereMaterial);
                sphereMaterial = null;
            }

            if (generatedInsideSphereMesh != null)
            {
                Destroy(generatedInsideSphereMesh);
                generatedInsideSphereMesh = null;
            }
        }
    }
}