using UnityEngine;
using UnityEngine.Rendering.HighDefinition;

namespace ReflectAR
{
    /// <summary>
    /// The materials of one garment: fabric, plastic (buttons, zipper tape) and metal, each in an
    /// opaque and a fading variant, matching the submeshes of GarmentTrims.
    ///
    /// URP: the ReflectAR/Garment shader (triplanar on the rest pose); untextured for the trims.
    ///   It fades by _Alpha, so both variants are the same material.
    /// HDRP: HDRP/Lit with the garment's FabricMaps on its pattern-piece UVs. Opaque while fully
    ///   shown, so it gets SSAO, contact shadows and the deferred path; a transparent copy only
    ///   while it fades in or out, or is dimmed out of range.
    /// </summary>
    public class GarmentMaterials
    {
        /// <summary>Under Resources/: HDRP/Lit templates made by Editor/PipelineSetup.cs, so player
        /// builds keep their shader variants. Without them, materials are configured at runtime.</summary>
        public const string TemplateDir = "HDRP/";
        public const string FabricOpaque = "ReflectAR_FabricOpaque", FabricFade = "ReflectAR_FabricFade",
            TrimOpaque = "ReflectAR_TrimOpaque", TrimFade = "ReflectAR_TrimFade";

        readonly Material[] opaque = new Material[3], fade = new Material[3];
        readonly Color[] tint = new Color[3];
        readonly bool hd;
        bool? wasFading;

        static Material urpBase;

        public Material Fabric => opaque[0];

        public GarmentMaterials(GarmentSpec s)
        {
            hd = RenderLook.HDRP;
            tint[0] = Color.white; tint[1] = s.trim; tint[2] = s.metal;
            if (!hd)
            {
                if (urpBase == null) urpBase = new Material(Shader.Find("ReflectAR/Garment"));
                opaque[0] = Urp(s.name, PatternTex.Make(s), Color.white, 1f / Mathf.Max(s.tile, 0.01f), s.sheen);
                opaque[1] = Urp(s.name + " plastic", Texture2D.whiteTexture, s.trim, 1f, 0.15f);
                opaque[2] = Urp(s.name + " metal", Texture2D.whiteTexture, s.metal, 1f, 0.6f);
                for (int i = 0; i < 3; i++) fade[i] = opaque[i];
                return;
            }
            var maps = FabricMaps.Make(s);
            for (int f = 0; f < 2; f++)
            {
                bool transparent = f == 1;
                var set = transparent ? fade : opaque;
                set[0] = Template(transparent ? FabricFade : FabricOpaque, transparent, maps.baseColor, maps.normal, maps.mask);
                set[0].name = s.name + (transparent ? " (fade)" : "");
                set[0].SetTexture("_BaseColorMap", maps.baseColor);
                set[0].SetTexture("_NormalMap", maps.normal);
                set[0].SetTexture("_MaskMap", maps.mask);
                set[0].SetTextureScale("_BaseColorMap", Vector2.one / Mathf.Max(s.tile, 0.01f));   // UVs are metres of fabric
                set[1] = Template(transparent ? TrimFade : TrimOpaque, transparent, null, null, null);
                set[1].SetFloat("_Metallic", 0f);
                set[1].SetFloat("_Smoothness", 0.55f);
                set[2] = Template(transparent ? TrimFade : TrimOpaque, transparent, null, null, null);
                set[2].SetFloat("_Metallic", 1f);
                set[2].SetFloat("_Smoothness", 0.78f);
                for (int i = 0; i < 3; i++) set[i].SetColor("_BaseColor", tint[i]);
            }
        }

        static Material Urp(string name, Texture tex, Color c, float tile, float sheen)
        {
            var m = new Material(urpBase) { name = name };
            m.SetTexture("_BaseMap", tex);
            m.SetColor("_BaseColor", c);
            m.SetFloat("_Tile", tile);
            m.SetFloat("_Sheen", sheen);
            return m;
        }

        static Material Template(string name, bool transparent, Texture baseMap, Texture normal, Texture mask)
        {
            var t = Resources.Load<Material>(TemplateDir + name);
            return t != null ? new Material(t) : NewLit(transparent, baseMap, normal, mask);
        }

        /// <summary>A double-sided HDRP/Lit cloth material: opaque, or alpha-blended with depth
        /// write (so it still sorts against the body proxies and itself).</summary>
        public static Material NewLit(bool transparent, Texture baseMap, Texture normal, Texture mask)
        {
            var m = new Material(Shader.Find("HDRP/Lit"));
            m.SetFloat("_SurfaceType", transparent ? 1f : 0f);
            m.SetFloat("_BlendMode", 0f);                        // alpha
            m.SetFloat("_TransparentZWrite", 1f);
            m.SetFloat("_DoubleSidedEnable", 1f);
            // no back-face normal flip: CameraFeed mode mirrors the image with a flipped projection,
            // which reverses winding, so "back face" can't be told from the triangle order
            m.SetFloat("_DoubleSidedNormalMode", 2f);
            m.SetFloat("_MaterialID", 1f);                       // standard
            m.SetFloat("_SmoothnessRemapMin", 0f);
            m.SetFloat("_SmoothnessRemapMax", 1f);
            m.SetFloat("_AORemapMin", 0f);
            m.SetFloat("_AORemapMax", 1f);
            m.SetFloat("_NormalScale", 1f);
            if (baseMap != null) m.SetTexture("_BaseColorMap", baseMap);
            if (normal != null) m.SetTexture("_NormalMap", normal);
            if (mask != null) m.SetTexture("_MaskMap", mask);
            HDMaterial.ValidateMaterial(m);
            return m;
        }

        /// <summary>A plain opaque lit material in the active pipeline (cube test).</summary>
        public static Material Plain(Color c)
        {
            if (RenderLook.HDRP)
            {
                var m = Template(TrimOpaque, false, null, null, null);
                m.SetColor("_BaseColor", c);
                return m;
            }
            if (urpBase == null) urpBase = new Material(Shader.Find("ReflectAR/Garment"));
            return Urp("Plain", Texture2D.whiteTexture, c, 1f, 0.2f);
        }

        /// <summary>Puts the variant for this alpha on the garment and its trims.</summary>
        public void Apply(MeshRenderer body, MeshRenderer trims, float alpha)
        {
            if (!hd)
            {
                for (int i = 0; i < 3; i++) opaque[i].SetFloat("_Alpha", alpha);
                if (wasFading == null) { body.sharedMaterial = opaque[0]; trims.sharedMaterials = opaque; wasFading = false; }
                return;
            }
            bool fading = alpha < 0.999f;
            if (fading)
                for (int i = 0; i < 3; i++)
                {
                    Color c = tint[i]; c.a = alpha;
                    fade[i].SetColor("_BaseColor", c);
                }
            if (wasFading == fading) return;
            var set = fading ? fade : opaque;
            body.sharedMaterial = set[0];
            trims.sharedMaterials = set;
            wasFading = fading;
        }
    }
}
