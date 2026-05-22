using System;
using UnityEngine;

namespace GStreamerUnity
{
    public class GStreamerPipeline : IDisposable
    {
        public IntPtr Ptr { get; private set; }
        public IntPtr TextureNativePtr { get; private set; }

        private uint id;
        private uint width;
        private uint height;
        private string codec;

        private static uint _totalId;

        private Texture2D texture;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
        static void Init()
        {
            _totalId = 0;
        }

        private GStreamerPipeline()
        {
        }

        public static GStreamerPipeline Create(uint width, uint height, string uri, string codec)
        {
            uint id = _totalId++;

            if (string.IsNullOrWhiteSpace(codec))
            {
                codec = "h264";
            }

            return new GStreamerPipeline
            {
                width = width,
                height = height,
                codec = codec.Trim().ToLowerInvariant(),
                Ptr = GStreamerInterop.CreatePipeline(id, width, height, uri, codec),
                id = id
            };
        }

        public void CreateTexture()
        {
            if (Ptr == IntPtr.Zero)
            {
                return;
            }

            GStreamerInterop.CreateTexture(this);
        }

        public Texture2D GetTexture()
        {
            if (Ptr == IntPtr.Zero)
            {
                return texture;
            }

            IntPtr nativePtr = GStreamerInterop.GetTexturePtr(this);

            if (nativePtr == IntPtr.Zero)
            {
                return texture;
            }

            if (texture == null)
            {
                TextureNativePtr = nativePtr;

                texture = Texture2D.CreateExternalTexture(
                    (int)width,
                    (int)height,
                    TextureFormat.BGRA32,
                    mipChain: false,
                    linear: true,
                    nativeTex: TextureNativePtr
                );

                ApplyVideoTextureQualitySettings(texture);
            }
            else if (nativePtr != TextureNativePtr)
            {
                TextureNativePtr = nativePtr;
                texture.UpdateExternalTexture(TextureNativePtr);
                ApplyVideoTextureQualitySettings(texture);
            }

            return texture;
        }

        private void ApplyVideoTextureQualitySettings(Texture2D tex)
        {
            if (tex == null)
            {
                return;
            }

            tex.name = $"GStreamerExternalTexture_{width}x{height}_{codec}";
            tex.filterMode = FilterMode.Bilinear;
            tex.wrapMode = TextureWrapMode.Clamp;
            tex.anisoLevel = 1;
            tex.mipMapBias = 0.0f;
        }

        public void UpdateDisplayTexture()
        {
        }

        public ulong GetCopiedFrameId()
        {
            if (Ptr == IntPtr.Zero)
            {
                return 0;
            }

            return GStreamerInterop.GetPipelineCopiedFrameId(Ptr);
        }

        public ulong GetLatestFrameId()
        {
            if (Ptr == IntPtr.Zero)
            {
                return 0;
            }

            return GStreamerInterop.GetPipelineLatestFrameId(Ptr);
        }

        public void Dispose()
        {
            if (TextureNativePtr != IntPtr.Zero)
            {
                GStreamerInterop.DisposeTexture(TextureNativePtr);
                TextureNativePtr = IntPtr.Zero;
            }

            if (Ptr != IntPtr.Zero)
            {
                GStreamerInterop.DisposePipeline(Ptr);
                Ptr = IntPtr.Zero;
            }

            if (texture != null)
            {
                UnityEngine.Object.Destroy(texture);
                texture = null;
            }
        }
    }
}