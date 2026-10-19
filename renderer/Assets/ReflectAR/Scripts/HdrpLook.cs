using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.HighDefinition;

namespace ReflectAR
{
    public static class RenderLook
    {
        /// <summary>True when HDRP is the active render pipeline (see Editor/PipelineSetup.cs to switch).</summary>
        public static bool HDRP => GraphicsSettings.currentRenderPipeline is HDRenderPipelineAsset;
    }

    /// <summary>
    /// HDRP scene setup, done at runtime so the scene works under either pipeline:
    /// - camera: solid black background (the mirror's "transparent"), SMAA, no dithering. Not TAA: cloth and
    ///   the body proxies have no per-vertex motion vectors, so TAA would smear them.
    /// - a global volume: fixed exposure (the feed and proxy shaders divide it back out), neutral
    ///   tonemapping, ambient light from a soft gradient "room", SSAO and contact shadows (the
    ///   body proxies are in the depth buffer, so arms and hands shade the shirt), short shadow
    ///   distance for crisp cascades on a body 1-3 m away, no bloom, motion blur or fog.
    /// - physical light intensities (lux) and soft shadows for the key light.
    /// </summary>
    public static class HdrpLook
    {
        public const float Ev100 = 10f, KeyLux = 3000f, FillLux = 700f;

        public static void SetupCamera(Camera cam)
        {
            var hd = cam.GetComponent<HDAdditionalCameraData>();
            if (hd == null) hd = cam.gameObject.AddComponent<HDAdditionalCameraData>();
            hd.clearColorMode = HDAdditionalCameraData.ClearColorMode.Color;
            hd.backgroundColorHDR = Color.black;
            hd.antialiasing = HDAdditionalCameraData.AntialiasingMode.SubpixelMorphologicalAntiAliasing;
            hd.SMAAQuality = HDAdditionalCameraData.SMAAQualityLevel.High;
            hd.dithering = false;                                // keep black exactly black for the mirror
        }

        public static Volume CreateVolume()
        {
            var go = new GameObject("HDRP Look");
            var v = go.AddComponent<Volume>();
            v.isGlobal = true;
            v.priority = 100f;
            var p = ScriptableObject.CreateInstance<VolumeProfile>();
            p.name = "ReflectAR HDRP";
            v.sharedProfile = p;

            var exposure = p.Add<Exposure>(true);
            exposure.mode.Override(ExposureMode.Fixed);
            exposure.fixedExposure.Override(Ev100);
            p.Add<Tonemapping>(true).mode.Override(TonemappingMode.Neutral);

            var env = p.Add<VisualEnvironment>(true);
            env.skyType.Override((int)SkyType.Gradient);
            env.skyAmbientMode.Override(SkyAmbientMode.Dynamic);
            var sky = p.Add<GradientSky>(true);
            sky.top.Override(new Color(0.86f, 0.88f, 0.94f));
            sky.middle.Override(new Color(0.72f, 0.7f, 0.66f));
            sky.bottom.Override(new Color(0.32f, 0.3f, 0.28f));
            sky.gradientDiffusion.Override(1f);
            sky.skyIntensityMode.Override(SkyIntensityMode.Exposure);
            sky.exposure.Override(8f);                           // ~400 lux of ambient

            var ao = p.Add<ScreenSpaceAmbientOcclusion>(true);
            ao.intensity.Override(1.2f);
            ao.radius.Override(0.25f);
            var contact = p.Add<ContactShadows>(true);
            contact.enable.Override(true);
            contact.length.Override(0.08f);
            p.Add<HDShadowSettings>(true).maxShadowDistance.Override(6f);
            p.Add<Bloom>(true).intensity.Override(0f);           // a glow on black would show on the mirror
            p.Add<MotionBlur>(true).intensity.Override(0f);
            p.Add<Fog>(true).enabled.Override(false);
            return v;
        }

        /// <summary>Directional light in lux; softDeg = angular diameter (shadow penumbra).</summary>
        public static void SetupLight(Light l, float lux, bool shadows, float softDeg)
        {
            var hd = l.GetComponent<HDAdditionalLightData>();
            if (hd == null) hd = l.gameObject.AddComponent<HDAdditionalLightData>();
            l.type = LightType.Directional;
            l.intensity = lux;
            l.shadows = shadows ? LightShadows.Soft : LightShadows.None;
            if (!shadows) return;
            hd.SetShadowResolution(2048);
            hd.angularDiameter = softDeg;
            hd.useContactShadow.useOverride = true;
            hd.useContactShadow.@override = true;
        }
    }
}
