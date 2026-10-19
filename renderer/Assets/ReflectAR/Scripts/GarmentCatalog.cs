using System;
using System.Collections.Generic;
using UnityEngine;

namespace ReflectAR
{
    public enum Pattern { Solid, Stripes, Plaid, Quilt, Denim, Knit, Pique, Rib }
    public enum Neckline { Crew, VNeck, Collar, HoodDown, Scoop }

    /// <summary>
    /// One upper-body garment: its cut (used to generate a seamless mesh) and its fabric
    /// (used by the XPBD cloth solver). Lengths are in metres for a 0.36 m shoulder-joint span
    /// and scale with the user's body.
    /// </summary>
    [Serializable]
    public class GarmentSpec
    {
        public string name = "Garment";
        public Color colorA = Color.white, colorB = Color.gray;
        public Pattern pattern = Pattern.Solid;
        public float tile = 0.25f;               // metres per pattern repeat

        // ---- cut
        public float hem = -0.08f;               // hem height, fraction of torso length (0 = hip joints)
        public float ease = 0.015f;              // looseness around the torso
        public float sleeve = 0.3f;              // fraction of the arm covered (0 = sleeveless)
        public float sleeveEase = 0.008f;
        public Neckline neck = Neckline.Crew;
        public bool tank;                        // narrow straps instead of shoulders
        public float quilt;                      // puffer baffle bulge height (m)
        public float cuffFlare = 0f;             // extra radius at the sleeve opening
        public int placket;                      // front closure: 0 none, 1 buttons, 2 zipper
        public float placketLen = 1f;            // fraction of the front it runs down (polo = short)

        // ---- fabric (XPBD)
        public float stretch = 1e-6f;            // stretch compliance (lower = less stretchy)
        public float compress = 2e-4f;           // compression compliance (higher = buckles/wrinkles easily)
        public float bend = 2e-2f;               // bending compliance (higher = softer, drapier)
        public float damping = 0.04f;
        public float drape = 1f;                 // how far loose regions may hang from the body
        public float sheen = 0.25f;              // soft fabric rim highlight
        public float density = 1f;               // relative weight (gravity sag)

        // ---- fit: how the cut follows the measured body (GarmentMesher)
        public float fit = 0.7f;                 // below the chest: 0 = follows the waist, 1 = hangs straight from chest / belly
        public float hemBand;                    // ribbed hem band pulled in to the hips (0..1)
        public bool cuffBand;                    // gathered cuffs at the wrist (long sleeves)

        // ---- HDRP material (FabricMaps) and trims (GarmentTrims)
        public float smooth = 0.22f;             // perceptual smoothness of the fibres
        public float bump = 1f;                  // weave normal-map strength
        public Color trim = new Color(0.92f, 0.9f, 0.85f);   // buttons, or the zipper tape
        public Color metal = new Color(0.74f, 0.74f, 0.76f); // zipper teeth / slider, metal buttons
        public bool metalButtons;                // denim shanks instead of sewn plastic buttons
    }

    /// <summary>The 10 upper-body garments in the catalogue.</summary>
    public static class GarmentCatalog
    {
        static Color C(string hex) { ColorUtility.TryParseHtmlString(hex, out var c); return c; }

        public static GarmentSpec[] All => new[]
        {
            new GarmentSpec { name = "Classic Tee", colorA = C("#EDEBE6"), pattern = Pattern.Knit, sleeve = 0.34f,
                hem = -0.12f, ease = 0.018f, cuffFlare = 0.006f, fit = 0.75f, smooth = 0.2f },
            new GarmentSpec { name = "V-Neck Tee", colorA = C("#2B3A55"), pattern = Pattern.Knit, sleeve = 0.32f,
                hem = -0.12f, ease = 0.014f, neck = Neckline.VNeck, fit = 0.6f, smooth = 0.2f },
            new GarmentSpec { name = "Breton Long-Sleeve", colorA = C("#F4F1E8"), colorB = C("#1F2D5A"),
                pattern = Pattern.Stripes, tile = 0.07f, sleeve = 0.97f, hem = -0.12f, ease = 0.012f, fit = 0.45f, smooth = 0.24f, bump = 0.5f },
            new GarmentSpec { name = "Polo Shirt", colorA = C("#1D6B4E"), pattern = Pattern.Pique, sleeve = 0.3f,
                hem = -0.14f, ease = 0.016f, neck = Neckline.Collar, bend = 1.2e-2f, placket = 1, placketLen = 0.28f,
                fit = 0.7f, smooth = 0.26f, trim = C("#E9E5DA") },
            new GarmentSpec { name = "Flannel Shirt", colorA = C("#A3262A"), colorB = C("#1B1B1B"), pattern = Pattern.Plaid,
                tile = 0.16f, sleeve = 0.97f, hem = -0.2f, ease = 0.026f, neck = Neckline.Collar, bend = 1e-2f, drape = 1.2f, placket = 1,
                fit = 0.85f, smooth = 0.16f, bump = 0.8f, trim = C("#EDE6D6") },
            new GarmentSpec { name = "Pullover Hoodie", colorA = C("#8B8F96"), pattern = Pattern.Knit, sleeve = 0.98f,
                hem = -0.16f, ease = 0.035f, sleeveEase = 0.016f, neck = Neckline.HoodDown,
                bend = 6e-3f, compress = 8e-4f, density = 1.4f, drape = 1.3f,
                fit = 0.95f, hemBand = 1f, cuffBand = true, smooth = 0.1f, bump = 0.7f },
            new GarmentSpec { name = "Bomber Jacket", colorA = C("#4B5B3A"), pattern = Pattern.Solid, sleeve = 0.98f,
                hem = -0.03f, ease = 0.04f, sleeveEase = 0.02f, neck = Neckline.Collar,
                bend = 4e-3f, compress = 3e-4f, density = 1.5f, sheen = 0.45f, placket = 2,
                fit = 0.8f, hemBand = 1f, cuffBand = true, smooth = 0.5f, bump = 0.3f, trim = C("#2A3024"), metal = C("#8E9096") },
            new GarmentSpec { name = "Denim Jacket", colorA = C("#41628C"), pattern = Pattern.Denim, sleeve = 0.98f,
                hem = -0.04f, ease = 0.035f, sleeveEase = 0.018f, neck = Neckline.Collar,
                bend = 1.5e-3f, compress = 1e-4f, density = 1.8f, drape = 0.7f, sheen = 0.1f, placket = 1,
                fit = 0.7f, smooth = 0.2f, bump = 1.2f, metalButtons = true, metal = C("#B08A57") },
            new GarmentSpec { name = "Puffer Jacket", colorA = C("#1E1E24"), pattern = Pattern.Quilt, tile = 0.12f, sleeve = 0.98f,
                hem = -0.06f, ease = 0.038f, sleeveEase = 0.022f, quilt = 0.009f, neck = Neckline.Collar,
                bend = 8e-4f, compress = 5e-5f, density = 1.2f, drape = 0.5f, sheen = 0.6f, placket = 2,
                fit = 0.9f, hemBand = 0.6f, cuffBand = true, smooth = 0.58f, bump = 0.4f, trim = C("#121216"), metal = C("#5E6066") },
            new GarmentSpec { name = "Ribbed Tank", colorA = C("#E0533D"), pattern = Pattern.Rib, tile = 0.12f, sleeve = 0f,
                hem = -0.1f, ease = 0.006f, tank = true, neck = Neckline.Scoop, fit = 0.15f, smooth = 0.24f },
        };
    }

    /// <summary>Procedural fabric: one repeat of each pattern as colour + weave height.</summary>
    public static class PatternTex
    {
        static readonly Dictionary<string, Texture2D> cache = new Dictionary<string, Texture2D>();

        /// <summary>URP: colour in rgb, weave height in alpha (one texture = one repeat).</summary>
        public static Texture2D Make(GarmentSpec s)
        {
            if (cache.TryGetValue(s.name, out var t)) return t;
            const int N = 256;
            t = new Texture2D(N, N, TextureFormat.RGBA32, true)
                { wrapMode = TextureWrapMode.Repeat, filterMode = FilterMode.Trilinear, anisoLevel = 8 };
            var px = new Color[N * N];
            for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++)
            {
                Sample(s, x / (float)N, y / (float)N, out Color c, out float h);
                px[y * N + x] = new Color(c.r, c.g, c.b, h);
            }
            t.SetPixels(px);
            t.Apply(true);
            cache[s.name] = t;
            return t;
        }

        /// <summary>Colour and weave height (0..1) at (u, v) in [0, 1) of one repeat. Pixel-scale
        /// weaves (twill) are laid out on a 256 grid, whatever the texture size.</summary>
        public static void Sample(GarmentSpec s, float u, float v, out Color c, out float h)
        {
            int x = Mathf.FloorToInt(u * 256f), y = Mathf.FloorToInt(v * 256f);
            c = s.colorA;
            h = 0.5f;
            switch (s.pattern)
            {
                case Pattern.Stripes:
                    c = v < 0.5f ? s.colorA : s.colorB;
                    h = 0.5f + 0.5f * Mathf.Sin(u * Mathf.PI * 2f * 96f) * Mathf.Sin(v * Mathf.PI * 2f * 64f);   // fine jersey
                    break;
                case Pattern.Plaid:
                {
                    float bu = Mathf.Repeat(u, 1f) < 0.38f ? 1f : 0f, bv = Mathf.Repeat(v, 1f) < 0.38f ? 1f : 0f;
                    float thin = (Mathf.Abs(u - 0.7f) < 0.025f || Mathf.Abs(v - 0.7f) < 0.025f) ? 1f : 0f;
                    c = Color.Lerp(s.colorA, s.colorB, 0.5f * (bu + bv));
                    c = Color.Lerp(c, new Color(0.9f, 0.85f, 0.8f), thin * 0.5f);
                    h = ((x + y) % 4 < 2) ? 0.65f : 0.35f;         // twill
                    break;
                }
                case Pattern.Quilt:
                {
                    float d = Mathf.Abs(v - 0.5f) * 2f;              // horizontal baffle seams
                    c = Color.Lerp(s.colorA, s.colorA * 0.45f, Mathf.Pow(1f - d, 12f));
                    h = Mathf.Sqrt(1f - (1f - d) * (1f - d)); break;
                }
                case Pattern.Denim:
                {
                    // 3/1 twill: diagonal wales of white weft over indigo warp, slight slub
                    float w = Mathf.Repeat((x + y) / 5f, 1f);
                    float slub = Mathf.PerlinNoise(u * 6f, v * 90f) * 0.25f + Mathf.PerlinNoise(u * 60f, v * 60f) * 0.1f;
                    c = Color.Lerp(s.colorA * 0.8f, Color.Lerp(s.colorA, Color.white, 0.35f), (w < 0.35f ? 0.55f : 0.1f) + slub);
                    h = w < 0.35f ? 0.75f : 0.35f; break;
                }
                case Pattern.Knit:
                {
                    float cu = Mathf.Repeat(u * 64f, 1f), cv = Mathf.Repeat(v * 96f + (cu < 0.5f ? cu : 1f - cu), 1f);
                    h = 0.5f + 0.5f * Mathf.Sin(cv * Mathf.PI * 2f) * Mathf.Sin(cu * Mathf.PI);
                    c = s.colorA * (0.92f + 0.08f * h); break;
                }
                case Pattern.Pique:
                {
                    float cu = Mathf.Repeat(u * 48f, 1f) - 0.5f, cv = Mathf.Repeat(v * 48f, 1f) - 0.5f;
                    h = 1f - Mathf.Clamp01((cu * cu + cv * cv) * 5f);
                    c = s.colorA * (0.88f + 0.12f * h); break;
                }
                case Pattern.Rib:
                {
                    h = 0.5f + 0.5f * Mathf.Sin(u * Mathf.PI * 2f * 16f);
                    c = s.colorA * (0.9f + 0.1f * h); break;
                }
                default:                                             // plain weave: faint grain
                    h = 0.5f + 0.25f * Mathf.Sin(u * Mathf.PI * 2f * 128f) * Mathf.Sin(v * Mathf.PI * 2f * 128f);
                    break;
            }
            float noise = Mathf.PerlinNoise(u * 120f, v * 120f) * 0.06f - 0.03f;
            c = new Color(c.r + noise, c.g + noise, c.b + noise, 1f);
        }
    }

    /// <summary>
    /// HDRP/Lit maps for one repeat of a fabric: base colour (sRGB), tangent-space normal map from
    /// the weave height, and a mask map (R metallic, G ambient occlusion in the weave's valleys,
    /// B detail mask, A smoothness).
    /// </summary>
    public class FabricMaps
    {
        public Texture2D baseColor, normal, mask;
        static readonly Dictionary<string, FabricMaps> cache = new Dictionary<string, FabricMaps>();

        public static FabricMaps Make(GarmentSpec s)
        {
            if (cache.TryGetValue(s.name, out var m)) return m;
            const int N = 512;
            var col = new Color[N * N];
            var h = new float[N * N];
            for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++)
            {
                PatternTex.Sample(s, x / (float)N, y / (float)N, out Color c, out float hh);
                col[y * N + x] = c;
                h[y * N + x] = hh;
            }
            var nrm = new Color[N * N];
            var msk = new Color[N * N];
            // slope per texel of height: weaves are ~1 mm high over a few texels
            float k = 2.5f * s.bump;
            for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++)
            {
                float H(int i, int j) => h[((j + N) % N) * N + (i + N) % N];
                float dx = (H(x + 1, y) - H(x - 1, y)) * 0.5f * k, dy = (H(x, y + 1) - H(x, y - 1)) * 0.5f * k;
                Vector3 n = new Vector3(-dx, -dy, 1f).normalized;
                nrm[y * N + x] = new Color(n.x * 0.5f + 0.5f, n.y * 0.5f + 0.5f, n.z * 0.5f + 0.5f, 1f);
                float hv = H(x, y);
                msk[y * N + x] = new Color(0f, Mathf.Lerp(0.72f, 1f, hv), 0f, Mathf.Clamp01(s.smooth * Mathf.Lerp(0.8f, 1.15f, hv)));
            }
            m = new FabricMaps
            {
                baseColor = Tex(col, N, false),
                normal = Tex(nrm, N, true),
                mask = Tex(msk, N, true),
            };
            cache[s.name] = m;
            return m;
        }

        static Texture2D Tex(Color[] px, int n, bool linear)
        {
            var t = new Texture2D(n, n, TextureFormat.RGBA32, true, linear)
                { wrapMode = TextureWrapMode.Repeat, filterMode = FilterMode.Trilinear, anisoLevel = 8 };
            t.SetPixels(px);
            t.Apply(true, true);
            return t;
        }
    }
}
