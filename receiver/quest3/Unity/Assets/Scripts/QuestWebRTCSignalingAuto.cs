using System;
using System.Runtime.InteropServices;
using System.Text;
using UnityEngine;

public class QuestWebRTCSignalingAuto : MonoBehaviour
{
    private int signalingPort = 9001;

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_init();

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_start_signaling_test(int port);

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_is_signaling_running();

    [DllImport("libGstQuestInit.so")]
    private static extern int gst_quest_get_last_signaling_message(StringBuilder dst, int dstSize);

    [DllImport("libGstQuestInit.so")]
    private static extern void gst_quest_stop_signaling_test();

    [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
    private static void Boot()
    {
        GameObject go = new GameObject("QuestWebRTCSignalingAuto");
        DontDestroyOnLoad(go);
        go.AddComponent<QuestWebRTCSignalingAuto>();
    }

    private void Start()
    {
        signalingPort = QuestRuntimeConfig.SignalingPort;
        Debug.Log(
            "[QuestWebRTCSignalingAuto] Starting GStreamer + WebRTC signaling. " +
            QuestRuntimeConfig.Summary
        );

        int initOk = gst_quest_init();
        Debug.Log("[QuestWebRTCSignalingAuto] gst_quest_init=" + initOk);

        int startOk = gst_quest_start_signaling_test(signalingPort);
        Debug.Log("[QuestWebRTCSignalingAuto] gst_quest_start_signaling_test(" + signalingPort + ")=" + startOk);
    }

    private void Update()
    {
        if (Time.frameCount % 120 != 0) return;

        int running = gst_quest_is_signaling_running();

        StringBuilder sb = new StringBuilder(1024);
        int copied = gst_quest_get_last_signaling_message(sb, sb.Capacity);

        Debug.Log("[QuestWebRTCSignalingAuto] signaling running=" + running + " copied=" + copied + " | last message=" + sb.ToString());
    }

    private void OnApplicationQuit()
    {
        Debug.Log("[QuestWebRTCSignalingAuto] Stopping WebRTC signaling.");
        gst_quest_stop_signaling_test();
    }
}
