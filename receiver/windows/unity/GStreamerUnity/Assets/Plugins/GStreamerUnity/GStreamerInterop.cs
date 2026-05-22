using System;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.Rendering;

namespace GStreamerUnity
{
    public static class GStreamerInterop
    {
        const string dllName = "GStreamerUnityPlugin";
        const int maxDebugLogLength = 65536;

        public static void PrintLog()
        {
#if UNITY_EDITOR
            IntPtr output = Marshal.AllocHGlobal(maxDebugLogLength);
            int charCount = GetDebugLog(output, maxDebugLogLength);
            if (charCount > 0)
            {
                string[] stroutput = Marshal.PtrToStringAnsi(output, charCount).Split('\n', StringSplitOptions.RemoveEmptyEntries);
                foreach (string s in stroutput)
                {
                    Debug.Log("[NATIVE MESSAGE] " + s);
                }
            }
            Marshal.FreeHGlobal(output);
#endif
            ClearDebugLog();
        }

        [DllImport(dllName)]
        static extern private int GetDebugLog(IntPtr output, int maxLength);

        [DllImport(dllName)]
        static extern private void ClearDebugLog();

        [DllImport(dllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr CreatePipeline(uint id, uint width, uint height, string uri, string codec);

        public static void CreateTexture(GStreamerPipeline pipeline)
        {
            var cb = new CommandBuffer();
            cb.IssuePluginEventAndData(GetRenderEventFunc(), 1, pipeline.Ptr);
            Graphics.ExecuteCommandBuffer(cb);
        }

        public static void DisposeTexture(IntPtr textureNativePtr)
        {
            var cb = new CommandBuffer();
            cb.IssuePluginEventAndData(GetRenderEventFunc(), 2, textureNativePtr);
            Graphics.ExecuteCommandBuffer(cb);
        }

        public static void UpdateDisplayTexture(GStreamerPipeline pipeline)
        {
            if (pipeline == null || pipeline.Ptr == IntPtr.Zero) return;
            var cb = new CommandBuffer();
            cb.IssuePluginEventAndData(GetRenderEventFunc(), 3, pipeline.Ptr);
            Graphics.ExecuteCommandBuffer(cb);
        }

        [DllImport(dllName)]
        public static extern void DisposePipeline(IntPtr pipeline);

        public static IntPtr GetTexturePtr(GStreamerPipeline pipeline)
        {
            if (pipeline.Ptr == IntPtr.Zero) return IntPtr.Zero;
            return GetPipelineTexturePtr(pipeline.Ptr);
        }

        [DllImport(dllName)]
        private static extern IntPtr GetPipelineTexturePtr(IntPtr pipeline);

        [DllImport(dllName)]
        public static extern ulong GetPipelineCopiedFrameId(IntPtr pipeline);

        [DllImport(dllName)]
        public static extern ulong GetPipelineLatestFrameId(IntPtr pipeline);

        [DllImport(dllName)]
        private static extern IntPtr GetRenderEventFunc();
    }
}
