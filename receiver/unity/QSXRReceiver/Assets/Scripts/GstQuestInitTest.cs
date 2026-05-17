using System;
using System.Runtime.InteropServices;
using System.Text;
using UnityEngine;

public class GstQuestInitTest : MonoBehaviour
{
#if UNITY_ANDROID && !UNITY_EDITOR
    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_init();

    [DllImport("libGstQuestInit.so")]
    private static extern IntPtr gst_quest_version();

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_has_webrtcbin();

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_start_signaling_test(int port);

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_is_signaling_running();

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_get_last_signaling_message(byte[] dst, int dstSize);

    [DllImport("libGstQuestInit.so")]
    private static extern void gst_quest_stop_signaling_test();

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_start_discovery_test(int discoveryPort);

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_is_discovery_running();

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_get_last_discovery_message(byte[] dst, int dstSize);

    [DllImport("libGstQuestInit.so")]
    private static extern void gst_quest_stop_discovery_test();
#endif

    [Header("Native Signaling Test")]
    public bool startSignalingServer = true;
    public int signalingPort = 9001;

    [Header("LAN Discovery Test")]
    public bool startDiscoveryServer = true;
    public int discoveryPort = 9010;

    private string status = "GstQuestInitTest loaded.";
    private string lastSignalingMessage = "";
    private string lastDiscoveryMessage = "";
    private float pollTimer = 0f;

    void Awake()
    {
        Debug.Log("[GstQuestInitTest] Awake called.");
    }

    void Start()
    {
        Debug.Log("[GstQuestInitTest] Start called.");

#if UNITY_ANDROID && !UNITY_EDITOR
        Debug.Log("[GstQuestInitTest] Android runtime detected. Calling native plugin...");

        try
        {
            int ok = gst_quest_init();
            Debug.Log("[GstQuestInitTest] gst_quest_init returned: " + ok);

            if (ok != 1)
            {
                status = "GStreamer initialization failed.";
                Debug.LogError("[GstQuestInitTest] " + status);
                return;
            }

            IntPtr ptr = gst_quest_version();
            string version = Marshal.PtrToStringAnsi(ptr);

            int hasWebrtc = gst_quest_has_webrtcbin();
            Debug.Log("[GstQuestInitTest] gst_quest_has_webrtcbin returned: " + hasWebrtc);

            status = "GStreamer initialized: " + version;

            if (hasWebrtc == 1)
            {
                status += " | webrtcbin found";
                Debug.Log("[GstQuestInitTest] " + status);
            }
            else
            {
                status += " | webrtcbin NOT found";
                Debug.LogError("[GstQuestInitTest] " + status);
            }

            if (startSignalingServer)
            {
                int signalingOk = gst_quest_start_signaling_test(signalingPort);
                Debug.Log("[GstQuestInitTest] gst_quest_start_signaling_test returned: " + signalingOk);

                if (signalingOk == 1)
                {
                    status += " | TCP signaling: " + signalingPort;
                }
                else
                {
                    status += " | TCP signaling FAILED";
                }
            }

            if (startDiscoveryServer)
            {
                int discoveryOk = gst_quest_start_discovery_test(discoveryPort);
                Debug.Log("[GstQuestInitTest] gst_quest_start_discovery_test returned: " + discoveryOk);

                if (discoveryOk == 1)
                {
                    status += " | UDP discovery: " + discoveryPort;
                }
                else
                {
                    status += " | UDP discovery FAILED";
                }
            }
        }
        catch (DllNotFoundException ex)
        {
            status = "Native plugin not found: " + ex.Message;
            Debug.LogError("[GstQuestInitTest] " + status);
        }
        catch (EntryPointNotFoundException ex)
        {
            status = "Native function not found: " + ex.Message;
            Debug.LogError("[GstQuestInitTest] " + status);
        }
        catch (Exception ex)
        {
            status = "Native plugin error: " + ex;
            Debug.LogError("[GstQuestInitTest] " + status);
        }
#else
        status = "This test runs only on Android/Quest APK, not in Unity Editor.";
        Debug.Log("[GstQuestInitTest] " + status);
#endif
    }

    void Update()
    {
#if UNITY_ANDROID && !UNITY_EDITOR
        pollTimer += Time.unscaledDeltaTime;

        if (pollTimer >= 1.0f)
        {
            pollTimer = 0f;

            try
            {
                int signalingRunning = gst_quest_is_signaling_running();
                int discoveryRunning = gst_quest_is_discovery_running();

                byte[] signalingBuffer = new byte[1024];
                int signalingOk = gst_quest_get_last_signaling_message(signalingBuffer, signalingBuffer.Length);

                if (signalingOk == 1)
                {
                    string msg = Encoding.UTF8.GetString(signalingBuffer).TrimEnd('\0');

                    if (!string.IsNullOrEmpty(msg))
                    {
                        lastSignalingMessage = msg;
                    }
                }

                byte[] discoveryBuffer = new byte[1024];
                int discoveryOk = gst_quest_get_last_discovery_message(discoveryBuffer, discoveryBuffer.Length);

                if (discoveryOk == 1)
                {
                    string msg = Encoding.UTF8.GetString(discoveryBuffer).TrimEnd('\0');

                    if (!string.IsNullOrEmpty(msg))
                    {
                        lastDiscoveryMessage = msg;
                    }
                }

                Debug.Log(
                    "[GstQuestInitTest] signaling running=" + signalingRunning +
                    " | discovery running=" + discoveryRunning +
                    " | last signaling=" + lastSignalingMessage +
                    " | last discovery=" + lastDiscoveryMessage
                );
            }
            catch (Exception ex)
            {
                Debug.LogError("[GstQuestInitTest] Polling failed: " + ex);
            }
        }
#endif
    }

    void OnGUI()
    {
        GUI.Label(
            new Rect(180, 180, 2100, 240),
            status +
            "\nLast signaling message: " + lastSignalingMessage +
            "\nLast discovery message: " + lastDiscoveryMessage
        );
    }

    void OnDestroy()
    {
#if UNITY_ANDROID && !UNITY_EDITOR
        try
        {
            gst_quest_stop_discovery_test();
            gst_quest_stop_signaling_test();
        }
        catch
        {
        }
#endif
    }

    void OnApplicationQuit()
    {
#if UNITY_ANDROID && !UNITY_EDITOR
        try
        {
            gst_quest_stop_discovery_test();
            gst_quest_stop_signaling_test();
        }
        catch
        {
        }
#endif
    }
}