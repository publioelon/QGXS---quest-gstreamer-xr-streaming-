using System;
using System.IO;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;

public static class BuildQuestAv1Test
{
    public static void Build()
    {
        const string scene = "Assets/Scenes/SampleScene.unity";
        const string packageName = "com.publio.qsxr.unified";
        const string apkName = "Quest_QSXR_Unified_WebRTC_TEST.apk";

        string outputPath = Path.Combine(Directory.GetCurrentDirectory(), apkName);

        if (!File.Exists(scene))
        {
            throw new FileNotFoundException("Required Android scene was not found.", scene);
        }

        string oldIdentifier = PlayerSettings.GetApplicationIdentifier(BuildTargetGroup.Android);
        string oldProductName = PlayerSettings.productName;
        string oldBundleVersion = PlayerSettings.bundleVersion;
        int oldVersionCode = PlayerSettings.Android.bundleVersionCode;
        AndroidArchitecture oldArchitectures = PlayerSettings.Android.targetArchitectures;
        AndroidSdkVersions oldMinSdk = PlayerSettings.Android.minSdkVersion;
        AndroidSdkVersions oldTargetSdk = PlayerSettings.Android.targetSdkVersion;
        ScriptingImplementation oldBackend = PlayerSettings.GetScriptingBackend(BuildTargetGroup.Android);
        bool oldBuildAppBundle = EditorUserBuildSettings.buildAppBundle;
        ColorSpace oldColorSpace = PlayerSettings.colorSpace;
        bool oldUseDefaultGraphicsApis = PlayerSettings.GetUseDefaultGraphicsAPIs(BuildTarget.Android);
        GraphicsDeviceType[] oldGraphicsApis = PlayerSettings.GetGraphicsAPIs(BuildTarget.Android);

        try
        {
            PlayerSettings.SetApplicationIdentifier(BuildTargetGroup.Android, packageName);
            PlayerSettings.productName = "QSXR Unified WebRTC Receiver";
            PlayerSettings.bundleVersion = "0.3-unified-codec";
            PlayerSettings.Android.bundleVersionCode = 3;
            PlayerSettings.Android.targetArchitectures = AndroidArchitecture.ARM64;
            PlayerSettings.Android.minSdkVersion = AndroidSdkVersions.AndroidApiLevel29;
            PlayerSettings.Android.targetSdkVersion = AndroidSdkVersions.AndroidApiLevelAuto;
            PlayerSettings.SetScriptingBackend(BuildTargetGroup.Android, ScriptingImplementation.IL2CPP);
            EditorUserBuildSettings.buildAppBundle = false;
            PlayerSettings.colorSpace = ColorSpace.Linear;
            PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.Android, false);
            PlayerSettings.SetGraphicsAPIs(BuildTarget.Android, new[] { GraphicsDeviceType.OpenGLES3 });

            BuildPlayerOptions options = new BuildPlayerOptions
            {
                scenes = new[] { scene },
                locationPathName = outputPath,
                target = BuildTarget.Android,
                targetGroup = BuildTargetGroup.Android,
                options = BuildOptions.None
            };

            Debug.Log("[BuildQSXRUnified] package=" + packageName + " output=" + outputPath);
            var report = BuildPipeline.BuildPlayer(options);
            var summary = report.summary;

            Debug.Log(
                "[BuildQSXRUnified] result=" + summary.result +
                " errors=" + summary.totalErrors +
                " warnings=" + summary.totalWarnings +
                " size=" + summary.totalSize +
                " output=" + outputPath
            );

            if (summary.result != UnityEditor.Build.Reporting.BuildResult.Succeeded)
            {
                throw new Exception(
                    "QSXR unified APK build failed: " + summary.result +
                    ", errors=" + summary.totalErrors
                );
            }
        }
        finally
        {
            PlayerSettings.SetApplicationIdentifier(BuildTargetGroup.Android, oldIdentifier);
            PlayerSettings.productName = oldProductName;
            PlayerSettings.bundleVersion = oldBundleVersion;
            PlayerSettings.Android.bundleVersionCode = oldVersionCode;
            PlayerSettings.Android.targetArchitectures = oldArchitectures;
            PlayerSettings.Android.minSdkVersion = oldMinSdk;
            PlayerSettings.Android.targetSdkVersion = oldTargetSdk;
            PlayerSettings.SetScriptingBackend(BuildTargetGroup.Android, oldBackend);
            EditorUserBuildSettings.buildAppBundle = oldBuildAppBundle;
            PlayerSettings.colorSpace = oldColorSpace;
            PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.Android, oldUseDefaultGraphicsApis);
            PlayerSettings.SetGraphicsAPIs(BuildTarget.Android, oldGraphicsApis);
            AssetDatabase.SaveAssets();
        }
    }
}
