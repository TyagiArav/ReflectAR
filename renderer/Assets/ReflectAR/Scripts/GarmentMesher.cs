using System.Collections.Generic;
using UnityEngine;

namespace ReflectAR
{
    /// <summary>Canonical rest pose built from the user's measurements: hips at the origin,
    /// +X = user's right, +Y up, +Z = forward (toward the mirror), arms 40 degrees below horizontal.</summary>
    public struct RestSkeleton
    {
        public float L, sw, hipW, u, upperL, upperR, foreL, foreR;
        public Vector3 hipC, shC, lSh, rSh, lEl, rEl, lWr, rWr;
        public float[] profile;                  // measured torso widths (BodyRig.Profile), copied
        public const float ArmDropDeg = 40f;

        public static RestSkeleton From(BodyRig b)
        {
            var r = new RestSkeleton
            {
                L = b.TorsoLen, sw = b.ShoulderW, hipW = b.HipW, u = b.Unit,
                upperL = b.UpperL, upperR = b.UpperR, foreL = b.ForeL, foreR = b.ForeR,
                profile = (float[])b.Profile.Clone(),
            };
            r.hipC = Vector3.zero;
            r.shC = new Vector3(0, r.L, 0);
            r.lSh = r.shC + Vector3.left * (r.sw / 2);
            r.rSh = r.shC + Vector3.right * (r.sw / 2);
            r.lEl = r.lSh + ArmDir(-1) * r.upperL; r.lWr = r.lEl + ArmDir(-1) * r.foreL;
            r.rEl = r.rSh + ArmDir(+1) * r.upperR; r.rWr = r.rEl + ArmDir(+1) * r.foreR;
            return r;
        }

        public static Vector3 ArmDir(float side)
        {
            float a = ArmDropDeg * Mathf.Deg2Rad;
            return new Vector3(side * Mathf.Cos(a), -Mathf.Sin(a), 0f);
        }

        /// <summary>Body (not garment) torso half-width a and half-depth d at height y: the generic
        /// model, reshaped by the measured profile. Wider than average also means deeper (chest,
        /// belly), at 60 % of the width change.</summary>
        public void TorsoRadii(float y, out float a, out float d)
        {
            float t = y / L;
            BodyModel(t, hipW, sw, u, out a, out d);
            if (profile == null) return;
            float r = BodyShape.ProfileAt(profile, t);
            a *= r;
            d *= 1f + (r - 1f) * 0.6f;
        }

        /// <summary>Generic torso half-width / half-depth at t = height / torso length, from hip and
        /// shoulder width (an average build).</summary>
        public static void BodyModel(float t, float hipW, float sw, float u, out float a, out float d)
        {
            float hipA = hipW * 0.68f + 0.02f * u, chestA = sw * 0.5f * 0.9f;
            float waistA = Mathf.Lerp(hipA, chestA, 0.45f) * 0.93f;
            a = Pw(t, hipA * 1.04f, hipA, waistA, chestA, chestA * 0.92f);
            d = Pw(t, 0.11f, 0.105f, 0.098f, 0.108f, 0.075f) * u;
        }

        // piecewise linear through knots t = -0.3, 0, 0.3, 0.7, 1
        static float Pw(float t, float v0, float v1, float v2, float v3, float v4)
        {
            if (t <= -0.3f) return v0;
            if (t <= 0f) return Mathf.Lerp(v0, v1, (t + 0.3f) / 0.3f);
            if (t <= 0.3f) return Mathf.Lerp(v1, v2, t / 0.3f);
            if (t <= 0.7f) return Mathf.Lerp(v2, v3, (t - 0.3f) / 0.4f);
            if (t <= 1f) return Mathf.Lerp(v3, v4, (t - 0.7f) / 0.3f);
            return v4;
        }
    }

    /// <summary>Generated garment: one welded, seamless surface plus per-vertex skinning and anchoring.</summary>
    public class GarmentMeshData
    {
        public Vector3[] rest, restN;
        public int[] tris;
        public BoneWeight[] weights;     // bones: 0 pelvis, 1 chest, 2 L upper, 3 R upper, 4 L fore, 5 R fore
        public float[] looseness;        // 0 = pinned at the shoulders/collar, 1 = free at the hem/cuffs

        // rendering: seams of the UV layout split vertices
        public int[] renderToSim;        // render vertex -> simulated vertex
        public Vector2[] uv;             // per render vertex, metres along the fabric
        public int[] renderTris;
    }

    /// <summary>
    /// Builds a garment as the zero set of a signed distance field (torso, shoulders and sleeves
    /// blended with smooth unions, so armpits and shoulders are one continuous surface), meshed
    /// with surface nets, then cut open at the neck, hem and cuffs.
    ///
    /// The torso follows the user's measured build (RestSkeleton.TorsoRadii). Below the chest the
    /// cut hangs from whatever is widest above it (chest or belly) by GarmentSpec.fit, the way
    /// fabric falls instead of tucking into the waist; ribbed hem bands pull back in to the hips.
    /// </summary>
    public static class GarmentMesher
    {
        // Voxel size of the build; set per call (thread-static, since builds run on worker threads).
        [System.ThreadStatic] static float Cell;
        public const float DefaultCell = 0.017f;

        // ---------------------------------------------------------------- SDF helpers
        static float SMin(float a, float b, float k)
        {
            float h = Mathf.Clamp01(0.5f + 0.5f * (b - a) / k);
            return Mathf.Lerp(b, a, h) - k * h * (1f - h);
        }
        static float SMax(float a, float b, float k) => -SMin(-a, -b, k);

        static float Capsule(Vector3 p, Vector3 a, Vector3 b, float ra, float rb)
        {
            Vector3 ab = b - a;
            float t = Mathf.Clamp01(Vector3.Dot(p - a, ab) / Mathf.Max(ab.sqrMagnitude, 1e-8f));
            return Vector3.Distance(p, a + ab * t) - Mathf.Lerp(ra, rb, t);
        }

        struct Ctx
        {
            public RestSkeleton s;
            public GarmentSpec g;
            public float hemY, neckR, e;
            public Vector3 lEnd, rEnd, lDir, rDir;
            public bool lLong, rLong;
            // garment half-width / half-depth (ease included) vs height
            public float[] ga, gd;
            public float y0, dy;

            public readonly void Radii(float y, out float a, out float d)
            {
                float f = Mathf.Clamp((y - y0) / dy, 0f, ga.Length - 1.001f);
                int i = (int)f; f -= i;
                a = Mathf.Lerp(ga[i], ga[i + 1], f);
                d = Mathf.Lerp(gd[i], gd[i + 1], f);
            }
        }

        /// <summary>Garment cross-section vs height: the body plus ease, hanging straight down
        /// from the widest point above (by fit) below the chest, pulled in at a hem band.</summary>
        static void BuildTables(ref Ctx c)
        {
            const int N = 128;
            var s = c.s; var g = c.g;
            c.y0 = -0.4f * s.L; c.dy = (s.L - c.y0) / (N - 1);
            c.ga = new float[N]; c.gd = new float[N];
            var ba = new float[N]; var bd = new float[N];
            for (int i = 0; i < N; i++) s.TorsoRadii(c.y0 + i * c.dy, out ba[i], out bd[i]);
            float hangTop = 0.72f * s.L, band = 0.06f * s.u, fit = Mathf.Clamp01(g.fit);
            float maxA = 0f, maxD = 0f;
            for (int i = N - 1; i >= 0; i--)
            {
                float y = c.y0 + i * c.dy;
                if (y >= hangTop) { maxA = ba[i]; maxD = bd[i]; }
                else { maxA = Mathf.Max(maxA, ba[i]); maxD = Mathf.Max(maxD, bd[i]); }
                float a = Mathf.Lerp(ba[i], maxA, fit) + c.e, d = Mathf.Lerp(bd[i], maxD, fit) + c.e;
                if (g.hemBand > 0f)
                {
                    float k = (1f - Mathf.SmoothStep(0f, 1f, (y - c.hemY) / band)) * Mathf.Clamp01(g.hemBand);
                    a = Mathf.Lerp(a, ba[i] + 0.35f * c.e, k);
                    d = Mathf.Lerp(d, bd[i] + 0.35f * c.e, k);
                }
                c.ga[i] = a; c.gd[i] = d;
            }
        }

        static Vector3 SleeveEnd(RestSkeleton s, float side, float frac, out Vector3 dir, out bool reachesForearm)
        {
            float up = side < 0 ? s.upperL : s.upperR, fo = side < 0 ? s.foreL : s.foreR;
            Vector3 sh = side < 0 ? s.lSh : s.rSh;
            float len = frac * (up + fo);
            dir = RestSkeleton.ArmDir(side);
            reachesForearm = len > up;
            return sh + dir * len;
        }

        static float Sdf(in Ctx c, Vector3 p)
        {
            var s = c.s; var g = c.g; float u = s.u, e = c.e;
            // torso: elliptic cylinder with the garment's cross-section, rounded into the shoulders
            c.Radii(p.y, out float a, out float d);
            float torso = (Mathf.Sqrt(p.x * p.x / (a * a) + p.z * p.z / (d * d)) - 1f) * Mathf.Min(a, d);
            torso = SMax(torso, p.y - (s.L - 0.02f * u), 0.05f * u);
            float shoulders = Capsule(p, s.lSh + Vector3.up * 0.005f, s.rSh + Vector3.up * 0.005f, 0.058f * u + e, 0.058f * u + e);
            float body = SMin(torso, shoulders, 0.07f * u);
            // neck band rising slightly above the shoulders (collar base)
            float bandTop = g.neck == Neckline.Collar ? 0.075f * u : 0.025f * u;
            float flare = g.neck == Neckline.Collar ? Mathf.Clamp01((p.y - s.L) / (0.06f * u)) * 0.028f * u : 0f;  // collar rolls outward
            float neckBand = Mathf.Max(new Vector2(p.x, p.z + 0.012f * u).magnitude - (c.neckR + 0.012f * u + flare),
                                       Mathf.Abs(p.y - (s.L + bandTop * 0.5f)) - (bandTop * 0.5f + 0.012f * u));
            body = SMin(body, neckBand, 0.03f * u);

            if (g.tank)
            {
                body = SMax(body, p.y - (s.L - 0.075f * u), 0.02f * u);
                for (int side = -1; side <= 1; side += 2)
                {
                    float x = side * s.sw * 0.27f;
                    Vector3 f = new Vector3(x, s.L - 0.1f * u, 0.09f * u + e), top = new Vector3(x, s.L + 0.045f * u, -0.005f), b = new Vector3(x, s.L - 0.1f * u, -0.08f * u - e);
                    float strap = Mathf.Min(Capsule(p, f, top, 0.018f * u, 0.018f * u), Capsule(p, top, b, 0.018f * u, 0.018f * u));
                    body = SMin(body, strap, 0.02f * u);
                }
            }
            else if (g.sleeve > 0f)
            {
                float se = g.sleeveEase * u;
                for (int side = -1; side <= 1; side += 2)
                {
                    Vector3 sh = side < 0 ? s.lSh : s.rSh, end = side < 0 ? c.lEnd : c.rEnd, dir = side < 0 ? c.lDir : c.rDir;
                    Vector3 start = sh - Vector3.right * side * 0.03f * u + Vector3.up * 0.01f * u;
                    float r0 = 0.062f * u + se, r1 = Mathf.Lerp(r0, 0.043f * u + se, g.sleeve) + g.cuffFlare * u;
                    float arm;
                    if (g.cuffBand && (side < 0 ? c.lLong : c.rLong))
                    {
                        // gathered cuff at the wrist; the sleeve above blouses over it
                        Vector3 bandStart = end - dir * (0.055f * u);
                        float rb = 0.041f * u + 0.25f * se;
                        arm = SMin(Capsule(p, start, bandStart, r0, r1), Capsule(p, bandStart, end, rb, rb), 0.008f * u);
                    }
                    else arm = Capsule(p, start, end, r0, r1);
                    arm = Mathf.Max(arm, Vector3.Dot(p - end, dir));             // flat cuff
                    body = SMin(body, arm, 0.06f * u);
                }
            }

            if (g.neck == Neckline.HoodDown)                                     // rolled hood lying on the upper back
            {
                Vector3 q = p - new Vector3(0, s.L + 0.02f * u, -0.075f * u);
                float ca = Mathf.Cos(-35f * Mathf.Deg2Rad), sa = Mathf.Sin(-35f * Mathf.Deg2Rad);
                q = new Vector3(q.x, ca * q.y - sa * q.z, sa * q.y + ca * q.z);           // rotate about X (worker-thread safe)
                float ring = new Vector2(new Vector2(q.x, q.z).magnitude - 0.105f * u, q.y).magnitude - 0.045f * u;
                ring = Mathf.Max(ring, p.z - (0.01f * u));                        // back half only
                body = SMin(body, ring, 0.03f * u);
            }
            if (g.quilt > 0f)                                                    // puffer baffles
                body -= g.quilt * u * 0.5f * (1f + Mathf.Cos(p.y * Mathf.PI * 2f / g.tile));

            return Mathf.Max(body, c.hemY - p.y);                                // hem cut
        }

        static Vector3 Grad(in Ctx c, Vector3 p)
        {
            float h = Cell * 0.35f;
            return new Vector3(Sdf(c, p + Vector3.right * h) - Sdf(c, p - Vector3.right * h),
                               Sdf(c, p + Vector3.up * h) - Sdf(c, p - Vector3.up * h),
                               Sdf(c, p + Vector3.forward * h) - Sdf(c, p - Vector3.forward * h)) / (2f * h);
        }

        // ---------------------------------------------------------------- build
        public static GarmentMeshData Build(RestSkeleton s, GarmentSpec g, float cell = DefaultCell)
        {
            Cell = cell;
            var c = new Ctx { s = s, g = g, e = g.ease * s.u, hemY = g.hem * s.L, neckR = 0.068f * s.u };
            c.lEnd = SleeveEnd(s, -1, g.sleeve, out c.lDir, out c.lLong);
            c.rEnd = SleeveEnd(s, +1, g.sleeve, out c.rDir, out c.rLong);
            BuildTables(ref c);

            // ---- sample grid
            float xr = g.sleeve > 0f ? Mathf.Max(Mathf.Abs(c.lEnd.x), c.rEnd.x) + 0.1f : s.sw * 0.5f + 0.15f;
            Vector3 min = new Vector3(-xr, Mathf.Min(c.hemY, Mathf.Min(c.lEnd.y, c.rEnd.y)) - 0.1f, -0.25f * s.u);
            Vector3 max = new Vector3(xr, s.L + 0.15f * s.u, 0.25f * s.u);
            int nx = Mathf.CeilToInt((max.x - min.x) / Cell), ny = Mathf.CeilToInt((max.y - min.y) / Cell), nz = Mathf.CeilToInt((max.z - min.z) / Cell);
            int sx = nx + 1, sy = ny + 1, sz = nz + 1;
            var f = new float[sx * sy * sz];
            for (int k = 0; k < sz; k++)
            for (int j = 0; j < sy; j++)
            for (int i = 0; i < sx; i++)
                f[i + sx * (j + sy * k)] = Sdf(c, min + new Vector3(i, j, k) * Cell);

            // ---- surface nets: one vertex per sign-changing cell
            var cellVert = new int[nx * ny * nz];
            var verts = new List<Vector3>(8192);
            int[,] edges = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
            var cv = new float[8]; var cp = new Vector3[8];
            for (int k = 0; k < nz; k++)
            for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++)
            {
                int ci = i + nx * (j + ny * k);
                cellVert[ci] = -1;
                int mask = 0;
                for (int q = 0; q < 8; q++)
                {
                    int di = q & 1, dj = (q >> 1) & 1, dk = (q >> 2) & 1;
                    cv[q] = f[(i + di) + sx * ((j + dj) + sy * (k + dk))];
                    cp[q] = min + new Vector3(i + di, j + dj, k + dk) * Cell;
                    if (cv[q] < 0) mask |= 1 << q;
                }
                if (mask == 0 || mask == 255) continue;
                Vector3 sum = Vector3.zero; int n = 0;
                for (int ei = 0; ei < 12; ei++)
                {
                    float a = cv[edges[ei, 0]], b = cv[edges[ei, 1]];
                    if ((a < 0) == (b < 0)) continue;
                    sum += Vector3.Lerp(cp[edges[ei, 0]], cp[edges[ei, 1]], a / (a - b)); n++;
                }
                cellVert[ci] = verts.Count;
                verts.Add(sum / n);
            }
            var quads = new List<int>(verts.Count * 6);
            for (int k = 1; k < nz; k++)
            for (int j = 1; j < ny; j++)
            for (int i = 1; i < nx; i++)
            {
                float v0 = f[i + sx * (j + sy * k)];
                // edges leaving corner (i,j,k) along +x, +y, +z
                for (int axis = 0; axis < 3; axis++)
                {
                    int i1 = i + (axis == 0 ? 1 : 0), j1 = j + (axis == 1 ? 1 : 0), k1 = k + (axis == 2 ? 1 : 0);
                    if (i1 > nx || j1 > ny || k1 > nz) continue;
                    float v1 = f[i1 + sx * (j1 + sy * k1)];
                    if ((v0 < 0) == (v1 < 0)) continue;
                    int a, b, cc, d;
                    if (axis == 0) { a = Cv(cellVert, nx, ny, i, j - 1, k - 1); b = Cv(cellVert, nx, ny, i, j, k - 1); cc = Cv(cellVert, nx, ny, i, j, k); d = Cv(cellVert, nx, ny, i, j - 1, k); }
                    else if (axis == 1) { a = Cv(cellVert, nx, ny, i - 1, j, k - 1); b = Cv(cellVert, nx, ny, i - 1, j, k); cc = Cv(cellVert, nx, ny, i, j, k); d = Cv(cellVert, nx, ny, i, j, k - 1); }
                    else { a = Cv(cellVert, nx, ny, i - 1, j - 1, k); b = Cv(cellVert, nx, ny, i, j - 1, k); cc = Cv(cellVert, nx, ny, i, j, k); d = Cv(cellVert, nx, ny, i - 1, j, k); }
                    if (a < 0 || b < 0 || cc < 0 || d < 0) continue;
                    if (v0 < 0) { quads.Add(a); quads.Add(b); quads.Add(cc); quads.Add(d); }
                    else { quads.Add(a); quads.Add(d); quads.Add(cc); quads.Add(b); }
                }
            }

            // ---- project vertices onto the true surface
            for (int vi = 0; vi < verts.Count; vi++)
            {
                Vector3 p = verts[vi];
                for (int it = 0; it < 3; it++)
                {
                    Vector3 gr = Grad(c, p);
                    float gg = gr.sqrMagnitude;
                    if (gg < 1e-8f) break;
                    p -= gr * (Sdf(c, p) / gg);
                }
                if ((p - verts[vi]).sqrMagnitude < Cell * Cell * 2f) verts[vi] = p;
            }

            // ---- triangulate quads (shorter diagonal) and orient outward
            var tris = new List<int>(quads.Count / 4 * 6);
            for (int q = 0; q < quads.Count; q += 4)
            {
                int a = quads[q], b = quads[q + 1], cc = quads[q + 2], d = quads[q + 3];
                if ((verts[a] - verts[cc]).sqrMagnitude < (verts[b] - verts[d]).sqrMagnitude)
                { tris.Add(a); tris.Add(b); tris.Add(cc); tris.Add(a); tris.Add(cc); tris.Add(d); }
                else
                { tris.Add(a); tris.Add(b); tris.Add(d); tris.Add(b); tris.Add(cc); tris.Add(d); }
            }
            float outward = 0f;
            for (int t = 0; t < tris.Count; t += 3)
            {
                Vector3 p0 = verts[tris[t]], n = Vector3.Cross(verts[tris[t + 1]] - p0, verts[tris[t + 2]] - p0);
                outward += Vector3.Dot(n, Grad(c, p0));
            }
            if (outward < 0) for (int t = 0; t < tris.Count; t += 3) { int tmp = tris[t + 1]; tris[t + 1] = tris[t + 2]; tris[t + 2] = tmp; }

            // ---- cut openings: remove the neck floor, hem cap and cuff caps
            var keep = new List<int>(tris.Count);
            float vDepth = 0.15f * s.u, scoopDepth = 0.12f * s.u, scoopW = s.sw * 0.2f;
            for (int t = 0; t < tris.Count; t += 3)
            {
                Vector3 p0 = verts[tris[t]], p1 = verts[tris[t + 1]], p2 = verts[tris[t + 2]];
                Vector3 cen = (p0 + p1 + p2) / 3f, n = Vector3.Cross(p1 - p0, p2 - p0).normalized;
                bool cut = false;
                if (cen.y < c.hemY + Cell * 0.4f && n.y < -0.6f) cut = true;                                  // hem
                float rXZ = new Vector2(cen.x, cen.z + 0.012f * s.u).magnitude;
                if (rXZ < c.neckR * 0.98f && cen.y > s.L - 0.08f * s.u && n.y > 0.25f) cut = true;            // neck floor
                if (g.neck == Neckline.VNeck && cen.z > 0 && n.z > 0.2f && cen.y > s.L - vDepth
                    && Mathf.Abs(cen.x) < (cen.y - (s.L - vDepth)) * 0.42f + 0.005f) cut = true;               // V
                if (g.neck == Neckline.Scoop && cen.z > 0 && n.z > 0.2f && Mathf.Abs(cen.x) < scoopW
                    && cen.y > s.L - scoopDepth * (1f - Mathf.Pow(cen.x / scoopW, 2f))) cut = true;           // scoop
                if (g.tank && n.y > 0.6f && cen.y > s.L - 0.075f * s.u - Cell * 0.6f && cen.y < s.L - 0.04f * s.u) cut = true; // armholes
                if (g.sleeve > 0f && !g.tank)
                {
                    if (Vector3.Dot(cen - c.lEnd, c.lDir) > -Cell * 0.4f && Vector3.Dot(n, c.lDir) > 0.6f) cut = true;
                    if (Vector3.Dot(cen - c.rEnd, c.rDir) > -Cell * 0.4f && Vector3.Dot(n, c.rDir) > 0.6f) cut = true;
                }
                if (!cut) { keep.Add(tris[t]); keep.Add(tris[t + 1]); keep.Add(tris[t + 2]); }
            }

            var data = Compact(verts, KeepLargestComponent(keep, verts.Count));
            SmoothBoundaries(data);
            data.restN = Normals(data.rest, data.tris);
            Skin(data, s, g, c);
            Unwrap(data, s, g, c);
            return data;
        }

        static int Cv(int[] cellVert, int nx, int ny, int i, int j, int k) => cellVert[i + nx * (j + ny * k)];

        static List<int> KeepLargestComponent(List<int> tris, int nv)
        {
            var parent = new int[nv];
            for (int i = 0; i < nv; i++) parent[i] = i;
            int Find(int x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; }
            for (int t = 0; t < tris.Count; t += 3)
            {
                int a = Find(tris[t]), b = Find(tris[t + 1]), cc = Find(tris[t + 2]);
                parent[b] = a; parent[Find(cc)] = a;
            }
            var count = new Dictionary<int, int>();
            for (int t = 0; t < tris.Count; t += 3) { int r = Find(tris[t]); count[r] = count.TryGetValue(r, out var n) ? n + 1 : 1; }
            int best = -1, bestN = 0;
            foreach (var kv in count) if (kv.Value > bestN) { best = kv.Key; bestN = kv.Value; }
            var outT = new List<int>(tris.Count);
            for (int t = 0; t < tris.Count; t += 3)
                if (Find(tris[t]) == best) { outT.Add(tris[t]); outT.Add(tris[t + 1]); outT.Add(tris[t + 2]); }
            return outT;
        }

        static GarmentMeshData Compact(List<Vector3> verts, List<int> tris)
        {
            var map = new int[verts.Count];
            for (int i = 0; i < map.Length; i++) map[i] = -1;
            var nv = new List<Vector3>();
            var nt = new int[tris.Count];
            for (int t = 0; t < tris.Count; t++)
            {
                int v = tris[t];
                if (map[v] < 0) { map[v] = nv.Count; nv.Add(verts[v]); }
                nt[t] = map[v];
            }
            return new GarmentMeshData { rest = nv.ToArray(), tris = nt };
        }

        /// <summary>Relax the jagged voxel edges of every opening along the boundary loop.</summary>
        static void SmoothBoundaries(GarmentMeshData d)
        {
            var edgeCount = new Dictionary<long, int>();
            long Key(int a, int b) => a < b ? ((long)a << 32) | (uint)b : ((long)b << 32) | (uint)a;
            for (int t = 0; t < d.tris.Length; t += 3)
                for (int e = 0; e < 3; e++)
                {
                    long k = Key(d.tris[t + e], d.tris[t + (e + 1) % 3]);
                    edgeCount[k] = edgeCount.TryGetValue(k, out var n) ? n + 1 : 1;
                }
            var nbr = new Dictionary<int, List<int>>();
            foreach (var kv in edgeCount)
            {
                if (kv.Value != 1) continue;
                int a = (int)(kv.Key >> 32), b = (int)(kv.Key & 0xffffffff);
                if (!nbr.ContainsKey(a)) nbr[a] = new List<int>();
                if (!nbr.ContainsKey(b)) nbr[b] = new List<int>();
                nbr[a].Add(b); nbr[b].Add(a);
            }
            for (int it = 0; it < 6; it++)
            {
                var moved = new Dictionary<int, Vector3>();
                foreach (var kv in nbr)
                {
                    if (kv.Value.Count != 2) continue;
                    Vector3 avg = (d.rest[kv.Value[0]] + d.rest[kv.Value[1]]) * 0.5f;
                    moved[kv.Key] = Vector3.Lerp(d.rest[kv.Key], avg, 0.5f);
                }
                foreach (var kv in moved) d.rest[kv.Key] = kv.Value;
            }
        }

        public static Vector3[] Normals(Vector3[] v, int[] tris)
        {
            var n = new Vector3[v.Length];
            for (int t = 0; t < tris.Length; t += 3)
            {
                int a = tris[t], b = tris[t + 1], c = tris[t + 2];
                Vector3 fn = Vector3.Cross(v[b] - v[a], v[c] - v[a]);
                n[a] += fn; n[b] += fn; n[c] += fn;
            }
            for (int i = 0; i < n.Length; i++) n[i] = n[i].normalized;
            return n;
        }

        static float SegDist(Vector3 p, Vector3 a, Vector3 b, out float t)
        {
            Vector3 ab = b - a;
            t = Mathf.Clamp01(Vector3.Dot(p - a, ab) / Mathf.Max(ab.sqrMagnitude, 1e-8f));
            return Vector3.Distance(p, a + ab * t);
        }

        /// <summary>Smooth inverse-distance skin weights to 6 bones, and how loose each vertex may hang.</summary>
        static void Skin(GarmentMeshData d, RestSkeleton s, GarmentSpec g, Ctx c)
        {
            int n = d.rest.Length;
            d.weights = new BoneWeight[n];
            d.looseness = new float[n];
            Vector3 mid = new Vector3(0, s.L * 0.45f, 0);
            var dist = new float[6];
            for (int i = 0; i < n; i++)
            {
                Vector3 p = d.rest[i];
                dist[0] = SegDist(p, new Vector3(0, c.hemY - 0.05f, 0), mid, out _);
                dist[1] = Mathf.Min(SegDist(p, mid, s.shC, out _), SegDist(p, s.lSh, s.rSh, out _));
                dist[2] = SegDist(p, s.lSh, s.lEl, out float tl);
                dist[3] = SegDist(p, s.rSh, s.rEl, out float tr);
                dist[4] = SegDist(p, s.lEl, s.lWr, out float tlf);
                dist[5] = SegDist(p, s.rEl, s.rWr, out float trf);
                // the arm bones only own vertices outside the shoulder joint
                float side = Mathf.Sign(p.x), outX = Mathf.Abs(p.x) - s.sw * 0.5f;
                if (outX < 0.0f) { dist[2] = dist[3] = dist[4] = dist[5] = 1f; }
                else if (side < 0) { dist[3] = dist[5] = 1f; } else { dist[2] = dist[4] = 1f; }

                var w = new float[6]; float sum = 0f;
                for (int b = 0; b < 6; b++) { float dd = Mathf.Max(dist[b], 0.01f); w[b] = 1f / (dd * dd * dd * dd); sum += w[b]; }
                // top 4
                var idx = new List<int> { 0, 1, 2, 3, 4, 5 };
                idx.Sort((x, y) => w[y].CompareTo(w[x]));
                float s4 = w[idx[0]] + w[idx[1]] + w[idx[2]] + w[idx[3]];
                d.weights[i] = new BoneWeight
                {
                    boneIndex0 = idx[0], weight0 = w[idx[0]] / s4, boneIndex1 = idx[1], weight1 = w[idx[1]] / s4,
                    boneIndex2 = idx[2], weight2 = w[idx[2]] / s4, boneIndex3 = idx[3], weight3 = w[idx[3]] / s4,
                };

                // looseness: torso -> grows from the shoulder line to the hem; sleeves -> from shoulder to cuff
                float armW = (w[2] + w[3] + w[4] + w[5]) / sum;
                float tTorso = Mathf.Clamp01((s.L * 0.9f - p.y) / Mathf.Max(s.L * 0.9f - c.hemY, 0.05f));
                float sleeveLen = g.sleeve * ((side < 0 ? s.upperL + s.foreL : s.upperR + s.foreR));
                float along = side < 0 ? (dist[2] < dist[4] ? tl * s.upperL : s.upperL + tlf * s.foreL)
                                       : (dist[3] < dist[5] ? tr * s.upperR : s.upperR + trf * s.foreR);
                float tArm = sleeveLen > 0.01f ? Mathf.Clamp01(along / sleeveLen) : 0f;
                float loose = Mathf.Lerp(tTorso, tArm, armW);
                // elastic bands hold on to the body
                if (g.hemBand > 0f && p.y < c.hemY + 0.06f * s.u) loose *= Mathf.Lerp(1f, 0.45f, Mathf.Clamp01(g.hemBand) * (1f - armW));
                if (g.cuffBand && armW > 0.5f && along > sleeveLen - 0.06f * s.u) loose *= 0.45f;
                if (p.y > s.L - 0.02f * s.u && Mathf.Abs(p.x) < s.sw * 0.5f) loose = 0f;      // collar / yoke
                d.looseness[i] = loose;
            }
        }

        // ---------------------------------------------------------------- UV layout
        const int ChFront = 0, ChBack = 1, ChYoke = 2, ChSleeveL = 3, ChSleeveR = 4;

        /// <summary>
        /// Lays the fabric out like sewn pattern pieces, so a 2D texture (HDRP/Lit) sits on it with
        /// little stretch: front and back panels unrolled around the torso (side seams), a yoke over
        /// the shoulder tops, and each sleeve unrolled around the arm (seam underneath). UVs are in
        /// metres of fabric. Seam vertices are duplicated for rendering; the solver keeps the welded mesh.
        /// </summary>
        static void Unwrap(GarmentMeshData d, RestSkeleton s, GarmentSpec g, Ctx c)
        {
            var tris = d.tris; var r = d.rest; var w = d.weights;
            int cap = r.Length * 5 / 4;
            var map = new Dictionary<long, int>(cap);
            var toSim = new List<int>(cap);
            var uvs = new List<Vector2>(cap);
            var rt = new int[tris.Length];
            var fu = new float[3]; var fv = new float[3]; var fr = new float[3];
            bool sleeves = g.sleeve > 0f && !g.tank;
            for (int t = 0; t < tris.Length; t += 3)
            {
                int i0 = tris[t], i1 = tris[t + 1], i2 = tris[t + 2];
                Vector3 cen = (r[i0] + r[i1] + r[i2]) / 3f;
                Vector3 n = Vector3.Cross(r[i1] - r[i0], r[i2] - r[i0]).normalized;
                float arm = (ArmW(w[i0]) + ArmW(w[i1]) + ArmW(w[i2])) / 3f;
                bool onArm = false;
                if (sleeves && arm > 0.5f)
                {
                    // not the armpit's body side, which faces along the arm and would smear in a sleeve chart
                    float sd = cen.x < 0 ? -1f : 1f;
                    Vector3 dir = RestSkeleton.ArmDir(sd), rel = cen - (sd < 0 ? s.lSh : s.rSh);
                    onArm = (rel - dir * Vector3.Dot(rel, dir)).magnitude < 1.5f * (0.062f + g.sleeveEase) * s.u;
                }
                int ch = onArm ? (cen.x < 0 ? ChSleeveL : ChSleeveR)
                       : n.y > 0.55f && cen.y > 0.8f * s.L ? ChYoke
                       : cen.z >= 0f ? ChFront : ChBack;
                for (int k = 0; k < 3; k++) ChartUV(ch, r[tris[t + k]], s, c, out fu[k], out fv[k], out fr[k]);
                // sleeves wrap around: a triangle across the seam moves its negative-angle corners one turn on
                bool cyl = ch >= ChSleeveL;
                bool across = cyl && Mathf.Max(fu[0], Mathf.Max(fu[1], fu[2])) - Mathf.Min(fu[0], Mathf.Min(fu[1], fu[2])) > Mathf.PI;
                for (int k = 0; k < 3; k++)
                {
                    int vi = tris[t + k];
                    int wrap = across && fu[k] < 0f ? 1 : 0;
                    float uu = cyl ? (fu[k] + wrap * 2f * Mathf.PI) * fr[k] : fu[k];
                    long key = ((long)vi << 4) | ((long)ch << 1) | (long)wrap;
                    if (!map.TryGetValue(key, out int ri))
                    {
                        ri = toSim.Count;
                        map[key] = ri;
                        toSim.Add(vi);
                        uvs.Add(new Vector2(uu, fv[k]));
                    }
                    rt[t + k] = ri;
                }
            }
            d.renderToSim = toSim.ToArray();
            d.uv = uvs.ToArray();
            d.renderTris = rt;
        }

        static float ArmW(BoneWeight b) => (b.boneIndex0 >= 2 ? b.weight0 : 0f) + (b.boneIndex1 >= 2 ? b.weight1 : 0f)
                                         + (b.boneIndex2 >= 2 ? b.weight2 : 0f) + (b.boneIndex3 >= 2 ? b.weight3 : 0f);

        /// <summary>Fabric coordinates of rest point p in chart ch. For sleeves u is the angle around
        /// the arm (seam underneath at +-pi) and rad the distance from the arm axis.</summary>
        static void ChartUV(int ch, Vector3 p, RestSkeleton s, Ctx c, out float u, out float v, out float rad)
        {
            rad = 0f;
            if (ch == ChFront || ch == ChBack)
            {
                // arc length around the garment's cross-section from the centre front / back,
                // scaled down where the surface is inside it (collar, hood)
                c.Radii(p.y, out float a, out float d);
                float x = p.x / a, z = (ch == ChFront ? p.z : -p.z) / d;
                u = EllipseArc(a, d, Mathf.Atan2(x, z)) * Mathf.Sqrt(x * x + z * z);
                v = p.y;
                return;
            }
            if (ch == ChYoke)
            {
                u = p.x;
                v = s.L + 0.3f + p.z;
                return;
            }
            float side = ch == ChSleeveL ? -1f : 1f;
            Vector3 dir = RestSkeleton.ArmDir(side);
            Vector3 rel = p - (side < 0 ? s.lSh : s.rSh);
            float along = Vector3.Dot(rel, dir);
            Vector3 rv = rel - dir * along;
            Vector3 top = (Vector3.up - dir * Vector3.Dot(Vector3.up, dir)).normalized;
            u = Mathf.Atan2(Vector3.Dot(rv, Vector3.forward), Vector3.Dot(rv, top));
            rad = rv.magnitude;
            v = along;
        }

        /// <summary>Signed arc length of the ellipse (x, z) = (a sin t, d cos t) from t = 0 to th (Simpson).</summary>
        static float EllipseArc(float a, float d, float th)
        {
            const int n = 8;
            float h = th / n, sum = 0f;
            for (int i = 0; i <= n; i++)
            {
                float t = i * h, ct = Mathf.Cos(t), st = Mathf.Sin(t);
                float sp = Mathf.Sqrt(a * a * ct * ct + d * d * st * st);
                sum += sp * (i == 0 || i == n ? 1f : (i % 2 == 1 ? 4f : 2f));
            }
            return sum * h / 3f;
        }
    }
}
