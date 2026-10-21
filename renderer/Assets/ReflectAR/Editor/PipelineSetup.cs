using System.IO;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.HighDefinition;

namespace ReflectAR.EditorTools
{
    /// <summary>
    /// Switches the project between HDRP and URP (menu ReflectAR > Render Pipeline). On the first
    /// load after HDRP was added it creates the HDRP asset, makes HDRP the active pipeline and
    /// writes the HDRP/Lit templates the garment materials are instanced from (GarmentMaterials),
    /// so player builds keep those shader variants. The URP assets stay in Assets/Settings.
    /// </summary>
    [InitializeOnLoad]
    static class PipelineSetup
    {
        const string HdrpAsset = "Assets/Settings/HDRP/ReflectAR_HDRP.asset";
        const string UrpPC = "Assets/Settings/PC_RPAsset.asset", UrpMobile = "Assets/Settings/Mobile_RPAsset.asset";
        const string TemplateDir = "Assets/ReflectAR/Resources/" + GarmentMaterials.TemplateDir;

        static PipelineSetup() => EditorApplication.delayCall += FirstRun;

        static void FirstRun()
        {
            if (EditorApplication.isPlayingOrWillChangePlaymode) return;
            if (AssetDatabase.LoadAssetAtPath<HDRenderPipelineAsset>(HdrpAsset) == null) UseHdrp();
            else if (RenderLook.HDRP) EnsureTemplates();
        }

        [MenuItem("ReflectAR/Render Pipeline/HDRP")]
        public static void UseHdrp()
        {
            Directory.CreateDirectory(Path.GetDirectoryName(HdrpAsset));
            var asset = AssetDatabase.LoadAssetAtPath<HDRenderPipelineAsset>(HdrpAsset);
            if (asset == null)
            {
                asset = ScriptableObject.CreateInstance<HDRenderPipelineAsset>();
                AssetDatabase.CreateAsset(asset, HdrpAsset);
            }
            Assign(asset, _ => asset);
            EnsureTemplates();
            AssetDatabase.SaveAssets();
            Debug.Log("ReflectAR: HDRP is now the render pipeline (ReflectAR > Render Pipeline > URP switches back). "
                + "If HDRP reports missing settings, run Window > Rendering > HDRP Wizard > Fix All.");
        }

        [MenuItem("ReflectAR/Render Pipeline/URP")]
        public static void UseUrp()
        {
            var pc = AssetDatabase.LoadAssetAtPath<RenderPipelineAsset>(UrpPC);
            var mobile = AssetDatabase.LoadAssetAtPath<RenderPipelineAsset>(UrpMobile);
            Assign(pc, level => level == "Mobile" && mobile != null ? mobile : pc);
            AssetDatabase.SaveAssets();
            Debug.Log("ReflectAR: URP is now the render pipeline.");
        }

        [MenuItem("ReflectAR/Render Pipeline/HDRP", true)] static bool CanUseHdrp() => !RenderLook.HDRP;
        [MenuItem("ReflectAR/Render Pipeline/URP", true)] static bool CanUseUrp() => RenderLook.HDRP;

        /// <summary>Sets the default pipeline and every quality level's pipeline (by level name).</summary>
        static void Assign(RenderPipelineAsset def, System.Func<string, RenderPipelineAsset> perLevel)
        {
            GraphicsSettings.defaultRenderPipeline = def;
            var qs = AssetDatabase.LoadAllAssetsAtPath("ProjectSettings/QualitySettings.asset");
            if (qs.Length == 0) return;
            var so = new SerializedObject(qs[0]);
            var levels = so.FindProperty("m_QualitySettings");
            for (int i = 0; i < levels.arraySize; i++)
            {
                var level = levels.GetArrayElementAtIndex(i);
                level.FindPropertyRelative("customRenderPipeline").objectReferenceValue = perLevel(level.FindPropertyRelative("name").stringValue);
            }
            so.ApplyModifiedProperties();
        }

        /// <summary>HDRP/Lit templates, with placeholder maps so the fabric ones keep their normal-
        /// and mask-map keywords. Instances get the garment's own maps at runtime.</summary>
        static void EnsureTemplates()
        {
            Directory.CreateDirectory(TemplateDir);
            var white = Placeholder("Placeholder_White", Color.white, false);
            var flat = Placeholder("Placeholder_Normal", new Color(0.5f, 0.5f, 1f, 1f), true);
            var mask = Placeholder("Placeholder_Mask", new Color(0f, 1f, 0f, 0.3f), true);
            Template(GarmentMaterials.FabricOpaque, false, white, flat, mask);
            Template(GarmentMaterials.FabricFade, true, white, flat, mask);
            Template(GarmentMaterials.TrimOpaque, false, null, null, null);
            Template(GarmentMaterials.TrimFade, true, null, null, null);
            AssetDatabase.SaveAssets();
        }

        static Texture2D Placeholder(string name, Color c, bool linear)
        {
            string path = TemplateDir + name + ".asset";
            var t = AssetDatabase.LoadAssetAtPath<Texture2D>(path);
            if (t != null) return t;
            t = new Texture2D(4, 4, TextureFormat.RGBA32, false, linear) { name = name };
            var px = new Color[16];
            for (int i = 0; i < px.Length; i++) px[i] = c;
            t.SetPixels(px);
            t.Apply();
            AssetDatabase.CreateAsset(t, path);
            return t;
        }

        static void Template(string name, bool transparent, Texture baseMap, Texture normal, Texture mask)
        {
            string path = TemplateDir + name + ".mat";
            if (AssetDatabase.LoadAssetAtPath<Material>(path) != null) return;
            var m = GarmentMaterials.NewLit(transparent, baseMap, normal, mask);
            m.name = name;
            AssetDatabase.CreateAsset(m, path);
        }
    }
}
