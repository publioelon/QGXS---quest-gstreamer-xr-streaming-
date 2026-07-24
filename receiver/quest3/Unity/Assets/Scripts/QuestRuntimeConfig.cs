using System;
using UnityEngine;

public static class QuestRuntimeConfig
{
    public const string CodecKey = "qsxr_codec";
    public const string WidthKey = "qsxr_width";
    public const string HeightKey = "qsxr_height";
    public const string FpsKey = "qsxr_fps";
    public const string LegacyAv1FpsKey = "qsxr_av1_fps";
    public const string SignalingPortKey = "qsxr_signaling_port";
    public const string RefreshHzKey = "qsxr_refresh_hz";

    private static bool loaded;
    private static string codec = "auto";
    private static int width = 4096;
    private static int height = 2048;
    private static int fps = 60;
    private static int signalingPort = 9001;
    private static float refreshHz = 0.0f;
    private static bool widthAuto = true;
    private static bool heightAuto = true;
    private static bool fpsAuto = true;
    private static bool refreshAuto = true;

    public static string Codec { get { EnsureLoaded(); return codec; } }
    public static int Width { get { EnsureLoaded(); return width; } }
    public static int Height { get { EnsureLoaded(); return height; } }
    public static int Fps { get { EnsureLoaded(); return fps; } }
    public static int SignalingPort { get { EnsureLoaded(); return signalingPort; } }
    public static float RefreshHz { get { EnsureLoaded(); return refreshHz; } }
    public static bool WidthIsAuto { get { EnsureLoaded(); return widthAuto; } }
    public static bool HeightIsAuto { get { EnsureLoaded(); return heightAuto; } }
    public static bool FpsIsAuto { get { EnsureLoaded(); return fpsAuto; } }
    public static bool RefreshIsAuto { get { EnsureLoaded(); return refreshAuto; } }

    public static string Summary
    {
        get
        {
            EnsureLoaded();
            return BuildSummary();
        }
    }

    private static void EnsureLoaded()
    {
        if (loaded) return;
        loaded = true;

#if UNITY_ANDROID && !UNITY_EDITOR
        try
        {
            using (AndroidJavaClass unityPlayer =
                new AndroidJavaClass("com.unity3d.player.UnityPlayer"))
            using (AndroidJavaObject activity =
                unityPlayer.GetStatic<AndroidJavaObject>("currentActivity"))
            using (AndroidJavaObject intent =
                activity.Call<AndroidJavaObject>("getIntent"))
            {
                if (intent != null)
                {
                    if (intent.Call<bool>("hasExtra", CodecKey))
                    {
                        string value = intent.Call<string>("getStringExtra", CodecKey);
                        if (!string.IsNullOrEmpty(value))
                        {
                            codec = value.Trim().ToLowerInvariant();
                        }
                    }

                    widthAuto = !intent.Call<bool>("hasExtra", WidthKey);
                    heightAuto = !intent.Call<bool>("hasExtra", HeightKey);
                    fpsAuto =
                        !intent.Call<bool>("hasExtra", FpsKey) &&
                        !intent.Call<bool>("hasExtra", LegacyAv1FpsKey);
                    refreshAuto = !intent.Call<bool>("hasExtra", RefreshHzKey);

                    width = intent.Call<int>("getIntExtra", WidthKey, width);
                    height = intent.Call<int>("getIntExtra", HeightKey, height);

                    if (intent.Call<bool>("hasExtra", FpsKey))
                    {
                        fps = intent.Call<int>("getIntExtra", FpsKey, fps);
                    }
                    else if (intent.Call<bool>("hasExtra", LegacyAv1FpsKey))
                    {
                        fps = intent.Call<int>(
                            "getIntExtra",
                            LegacyAv1FpsKey,
                            fps
                        );
                    }

                    signalingPort = intent.Call<int>(
                        "getIntExtra",
                        SignalingPortKey,
                        signalingPort
                    );

                    refreshHz = intent.Call<float>(
                        "getFloatExtra",
                        RefreshHzKey,
                        refreshHz
                    );
                }
            }
        }
        catch (Exception ex)
        {
            Debug.LogWarning(
                "[QuestRuntimeConfig] Could not read Android intent extras: " +
                ex.Message
            );
        }
#endif

        if (
            codec != "auto" &&
            codec != "h264" &&
            codec != "h265" &&
            codec != "av1"
        )
        {
            Debug.LogWarning(
                "[QuestRuntimeConfig] Unsupported codec '" + codec +
                "'; using auto."
            );
            codec = "auto";
        }

        width = Mathf.Clamp(width, 16, 8192);
        height = Mathf.Clamp(height, 16, 8192);
        fps = Mathf.Clamp(fps, 1, 240);
        signalingPort = Mathf.Clamp(signalingPort, 1, 65535);
        refreshHz = refreshHz <= 0.0f
            ? 0.0f
            : Mathf.Clamp(refreshHz, 60.0f, 144.0f);

        Debug.Log("[QuestRuntimeConfig] " + BuildSummary());
    }

    private static string BuildSummary()
    {
        return
            "codec=" + codec +
            " width=" + width + (widthAuto ? "(auto)" : "(user)") +
            " height=" + height + (heightAuto ? "(auto)" : "(user)") +
            " fps=" + fps + (fpsAuto ? "(auto)" : "(user)") +
            " signalingPort=" + signalingPort +
            " refreshHz=" + refreshHz.ToString("0.##") +
            (refreshAuto ? "(auto)" : "(user)");
    }
}
