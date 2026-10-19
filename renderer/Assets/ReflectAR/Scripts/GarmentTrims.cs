using System.Collections.Generic;
using UnityEngine;

namespace ReflectAR
{
    /// <summary>
    /// Front closures as real geometry riding on the simulated cloth: a raised button placket with
    /// sewn buttons (or metal shanks), or a zipper with tape, teeth and slider.
    ///
    /// They hang off a chain of points down the centre front of the rest mesh, each found by a ray
    /// and stored as a triangle + barycentrics. Every frame each chain point follows its triangle
    /// on the cloth and carries a frame (right, up, normal), and every trim vertex sits at a fixed
    /// offset in the frame of its place along the chain, so the closure bends with the fabric.
    /// Submeshes: 0 fabric (placket band), 1 plastic (buttons / zipper tape), 2 metal.
    /// </summary>
    public class GarmentTrims
    {
        public const int Fabric = 0, Plastic = 1, Metal = 2;

        // chain: anchor triangle + barycentrics, arc length from the top
        int[] ta, tb, tc;
        float[] wa, wb, wc, arc;
        Vector3[] cp, cr, cu, cn;
        // trim vertices: place along the chain + offset (x right, z out) and normal in its frame
        int[] vChain;
        float[] vF;
        Vector3[] vLocal, vLocalN, pos, nrm;
        Vector4[] tan;

        /// <summary>Builds the closure for this garment into <paramref name="mesh"/>, or returns null
        /// if it has none (or the centre front can't be found).</summary>
        public static GarmentTrims Build(GarmentMeshData d, RestSkeleton s, GarmentSpec g, Mesh mesh)
        {
            if (g.placket == 0) return null;
            float u = s.u;
            float yTop = s.L + (g.neck == Neckline.Collar ? 0.075f : 0.03f) * u;
            float yBottom = Mathf.Lerp(g.hem * s.L, yTop, 1f - Mathf.Clamp01(g.placketLen)) + 0.012f * u;

            // ---- chain down the centre front, from the first hit below the top to the first miss
            var front = FrontTriangles(d);
            var pts = new List<(int a, int b, int c, float wa, float wb, float wc)>();
            var restP = new List<Vector3>();
            var restN = new List<Vector3>();
            for (float y = yTop; y >= yBottom; y -= 0.01f)
            {
                if (!RayFront(d, front, 0f, y, out var hit, out Vector3 p))
                {
                    if (pts.Count > 0) break;
                    continue;
                }
                pts.Add(hit);
                restP.Add(p);
                restN.Add((d.restN[hit.a] * hit.wa + d.restN[hit.b] * hit.wb + d.restN[hit.c] * hit.wc).normalized);
            }
            int n = pts.Count;
            if (n < 3) return null;

            var t = new GarmentTrims
            {
                ta = new int[n], tb = new int[n], tc = new int[n],
                wa = new float[n], wb = new float[n], wc = new float[n], arc = new float[n],
                cp = new Vector3[n], cr = new Vector3[n], cu = new Vector3[n], cn = new Vector3[n],
            };
            for (int i = 0; i < n; i++)
            {
                (t.ta[i], t.tb[i], t.tc[i], t.wa[i], t.wb[i], t.wc[i]) = pts[i];
                t.arc[i] = i == 0 ? 0f : t.arc[i - 1] + Vector3.Distance(restP[i], restP[i - 1]);
            }
            var rr = new Vector3[n]; var ru = new Vector3[n];
            Frames(restP, restN, rr, ru);

            // ---- geometry, in the chain's frames
            var b = new Builder(t, restP.ToArray(), restN.ToArray(), rr, ru);
            float end = t.arc[n - 1];
            if (g.placket == 1)
            {
                float hw = 0.017f * u, hz = 0.0012f * u;
                b.Strip(0f, end, hw, hz, Fabric);
                bool shank = g.metalButtons;
                float r = (shank ? 0.008f : 0.0065f) * u, pitch = 0.085f * u;
                for (float sc = 0.03f * u; sc < end - r; sc += pitch)
                    b.Button(sc, r, (shank ? 0.0032f : 0.0022f) * u, hz, shank, shank ? Metal : Plastic);
            }
            else
            {
                float tape = 0.007f * u, hz = 0.0008f * u;
                b.Strip(0f, end, tape, hz, Plastic);
                float pitch = 0.0028f * u;
                int k = 0;
                for (float sc = 0.026f * u; sc < end - 0.004f * u; sc += pitch, k++)
                    b.Box(sc, (k % 2 == 0 ? -1f : 1f) * 0.0013f * u, 0.0022f * u, 0.0008f * u, hz, hz + 0.0014f * u, Metal);
                b.Box(0.016f * u, 0f, 0.0055f * u, 0.009f * u, hz, hz + 0.0045f * u, Metal);          // slider
                b.Box(0.032f * u, 0f, 0.003f * u, 0.011f * u, hz + 0.0045f * u, hz + 0.006f * u, Metal); // pull tab
            }
            b.Finish(mesh);
            return t;
        }

        /// <summary>Moves the closure with the cloth. x: simulated positions, n: their normals.</summary>
        public void Update(Vector3[] x, Vector3[] n, Mesh mesh)
        {
            int c = cp.Length;
            for (int i = 0; i < c; i++)
            {
                cp[i] = x[ta[i]] * wa[i] + x[tb[i]] * wb[i] + x[tc[i]] * wc[i];
                cn[i] = (n[ta[i]] * wa[i] + n[tb[i]] * wb[i] + n[tc[i]] * wc[i]).normalized;
            }
            for (int i = 0; i < c; i++)
            {
                Vector3 up = cp[Mathf.Max(i - 1, 0)] - cp[Mathf.Min(i + 1, c - 1)];
                cr[i] = Vector3.Cross(up, cn[i]).normalized;
                cu[i] = Vector3.Cross(cn[i], cr[i]);
            }
            for (int v = 0; v < pos.Length; v++)
            {
                int i = vChain[v], j = Mathf.Min(i + 1, c - 1);
                float f = vF[v];
                Vector3 r = Vector3.LerpUnclamped(cr[i], cr[j], f), up = Vector3.LerpUnclamped(cu[i], cu[j], f), nn = Vector3.LerpUnclamped(cn[i], cn[j], f);
                Vector3 l = vLocal[v], ln = vLocalN[v];
                pos[v] = Vector3.LerpUnclamped(cp[i], cp[j], f) + r * l.x + up * l.y + nn * l.z;
                nrm[v] = (r * ln.x + up * ln.y + nn * ln.z).normalized;
                tan[v] = new Vector4(r.x, r.y, r.z, 1f);
            }
            mesh.SetVertices(pos);
            mesh.SetNormals(nrm);
            mesh.SetTangents(tan);
            mesh.RecalculateBounds();
        }

        // ---------------------------------------------------------------- rest mesh queries
        static List<int> FrontTriangles(GarmentMeshData d)
        {
            var list = new List<int>();
            var r = d.rest; var tr = d.tris;
            for (int t = 0; t < tr.Length; t += 3)
            {
                Vector3 a = r[tr[t]], b = r[tr[t + 1]], c = r[tr[t + 2]];
                if (Vector3.Cross(b - a, c - a).normalized.z > 0.2f && Mathf.Max(a.z, Mathf.Max(b.z, c.z)) > 0f) list.Add(t);
            }
            return list;
        }

        /// <summary>First front-facing hit of the ray from (x, y, +inf) toward -z.</summary>
        static bool RayFront(GarmentMeshData d, List<int> front, float x, float y,
            out (int a, int b, int c, float wa, float wb, float wc) hit, out Vector3 p)
        {
            var r = d.rest; var tr = d.tris;
            float bestZ = float.NegativeInfinity;
            hit = default; p = default;
            foreach (int t in front)
            {
                Vector3 a = r[tr[t]], b = r[tr[t + 1]], c = r[tr[t + 2]];
                // barycentrics in the xy plane
                float det = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
                if (Mathf.Abs(det) < 1e-12f) continue;
                float l0 = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / det;
                float l1 = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / det;
                float l2 = 1f - l0 - l1;
                if (l0 < 0f || l1 < 0f || l2 < 0f) continue;
                float z = a.z * l0 + b.z * l1 + c.z * l2;
                if (z <= bestZ) continue;
                bestZ = z;
                hit = (tr[t], tr[t + 1], tr[t + 2], l0, l1, l2);
                p = new Vector3(x, y, z);
            }
            return bestZ > 0f;
        }

        /// <summary>Right and up axes along a chain of points with normals (top first).</summary>
        static void Frames(List<Vector3> p, List<Vector3> n, Vector3[] right, Vector3[] up)
        {
            int c = p.Count;
            for (int i = 0; i < c; i++)
            {
                Vector3 u = p[Mathf.Max(i - 1, 0)] - p[Mathf.Min(i + 1, c - 1)];
                right[i] = Vector3.Cross(u, n[i]).normalized;
                up[i] = Vector3.Cross(n[i], right[i]);
            }
        }

        // ---------------------------------------------------------------- geometry
        /// <summary>Collects trim vertices at (arc length s, local x, z) along the chain.</summary>
        class Builder
        {
            readonly GarmentTrims t;
            readonly Vector3[] restP, restN, restR, restU;
            readonly List<int> chain = new List<int>();
            readonly List<float> frac = new List<float>();
            readonly List<Vector3> local = new List<Vector3>(), localN = new List<Vector3>();
            readonly List<Vector3> rp = new List<Vector3>(), rn = new List<Vector3>();
            readonly List<Vector2> uv = new List<Vector2>();
            readonly List<Vector3> flat = new List<Vector3>();         // (x, -s, z): to check winding
            readonly List<int>[] sub = { new List<int>(), new List<int>(), new List<int>() };

            public Builder(GarmentTrims t, Vector3[] p, Vector3[] n, Vector3[] r, Vector3[] u)
            {
                this.t = t; restP = p; restN = n; restR = r; restU = u;
            }

            int V(float s, float x, float z, Vector3 ln)
            {
                var arc = t.arc;
                int last = arc.Length - 1;
                s = Mathf.Clamp(s, 0f, arc[last]);
                int i = 0;
                while (i < last - 1 && arc[i + 1] <= s) i++;
                float f = Mathf.Clamp01((s - arc[i]) / Mathf.Max(arc[i + 1] - arc[i], 1e-6f));
                chain.Add(i); frac.Add(f);
                local.Add(new Vector3(x, 0f, z));
                localN.Add(ln.normalized);
                flat.Add(new Vector3(x, -s, z));
                Vector3 R = Vector3.Lerp(restR[i], restR[i + 1], f), U = Vector3.Lerp(restU[i], restU[i + 1], f), N = Vector3.Lerp(restN[i], restN[i + 1], f);
                Vector3 p = Vector3.Lerp(restP[i], restP[i + 1], f) + R * x + N * z;
                rp.Add(p);
                rn.Add((R * ln.x + U * ln.y + N * ln.z).normalized);
                uv.Add(new Vector2(x, p.y));                       // centre-front fabric coordinates (metres)
                return chain.Count - 1;
            }

            /// <summary>Triangle facing along its vertices' normals.</summary>
            void Tri(int a, int b, int c, int m)
            {
                Vector3 fn = Vector3.Cross(flat[b] - flat[a], flat[c] - flat[a]);
                if (Vector3.Dot(fn, localN[a] + localN[b] + localN[c]) < 0f) { int k = b; b = c; c = k; }
                sub[m].Add(a); sub[m].Add(b); sub[m].Add(c);
            }

            void Quad(int a, int b, int c, int d, int m) { Tri(a, b, c, m); Tri(a, c, d, m); }

            /// <summary>Raised band of half-width hw and height hz from arc length s0 to s1.</summary>
            public void Strip(float s0, float s1, float hw, float hz, int m)
            {
                var arc = t.arc;
                int pL = -1, pR = -1, pwl = -1, pwl0 = -1, pwr = -1, pwr0 = -1;
                for (int i = 0; i < arc.Length; i++)
                {
                    float s = arc[i];
                    if (s < s0 || s > s1) continue;
                    int L = V(s, -hw, hz, Vector3.forward), R = V(s, hw, hz, Vector3.forward);
                    int wl = V(s, -hw, hz, Vector3.left), wl0 = V(s, -hw, 0f, Vector3.left);
                    int wr = V(s, hw, hz, Vector3.right), wr0 = V(s, hw, 0f, Vector3.right);
                    if (pL >= 0)
                    {
                        Quad(pL, pR, R, L, m);
                        Quad(pwl, wl, wl0, pwl0, m);
                        Quad(pwr, wr, wr0, pwr0, m);
                    }
                    pL = L; pR = R; pwl = wl; pwl0 = wl0; pwr = wr; pwr0 = wr0;
                }
            }

            /// <summary>Box centred at arc length s and offset x, half-sizes hx (across) and hs (along),
            /// from height z0 to z1 above the cloth. Top and four sides.</summary>
            public void Box(float s, float x, float hx, float hs, float z0, float z1, int m)
            {
                float sa = s - hs, sb = s + hs, xa = x - hx, xb = x + hx;
                Quad(V(sa, xa, z1, Vector3.forward), V(sa, xb, z1, Vector3.forward), V(sb, xb, z1, Vector3.forward), V(sb, xa, z1, Vector3.forward), m);
                Quad(V(sa, xa, z1, Vector3.up), V(sa, xb, z1, Vector3.up), V(sa, xb, z0, Vector3.up), V(sa, xa, z0, Vector3.up), m);
                Quad(V(sb, xa, z1, Vector3.down), V(sb, xb, z1, Vector3.down), V(sb, xb, z0, Vector3.down), V(sb, xa, z0, Vector3.down), m);
                Quad(V(sa, xa, z1, Vector3.left), V(sb, xa, z1, Vector3.left), V(sb, xa, z0, Vector3.left), V(sa, xa, z0, Vector3.left), m);
                Quad(V(sa, xb, z1, Vector3.right), V(sb, xb, z1, Vector3.right), V(sb, xb, z0, Vector3.right), V(sa, xb, z0, Vector3.right), m);
            }

            /// <summary>Round button of radius r and height h on top of z0: dished face and rim
            /// (sewn button), or a dome (metal shank).</summary>
            public void Button(float s, float r, float h, float z0, bool dome, int m)
            {
                const int N = 16;
                var rim = new int[N]; var inner = new int[N]; var side = new int[N]; var foot = new int[N];
                float ri = dome ? 0.6f * r : 0.72f * r, zi = dome ? z0 + 0.95f * h : z0 + 0.8f * h;
                for (int k = 0; k < N; k++)
                {
                    float a = k * Mathf.PI * 2f / N, ca = Mathf.Cos(a), sa = Mathf.Sin(a);
                    // local y (up) is -s along the chain
                    rim[k] = V(s - sa * r, ca * r, z0 + (dome ? 0.55f : 1f) * h, new Vector3(ca * 0.6f, sa * 0.6f, 0.8f));
                    inner[k] = V(s - sa * ri, ca * ri, zi, dome ? new Vector3(ca * 0.35f, sa * 0.35f, 0.94f) : new Vector3(-ca * 0.3f, -sa * 0.3f, 0.95f));
                    side[k] = V(s - sa * r, ca * r, z0 + (dome ? 0.55f : 1f) * h, new Vector3(ca, sa, 0f));
                    foot[k] = V(s - sa * r, ca * r, z0, new Vector3(ca, sa, 0f));
                }
                int centre = V(s, 0f, dome ? z0 + h : z0 + 0.75f * h, Vector3.forward);
                for (int k = 0; k < N; k++)
                {
                    int k1 = (k + 1) % N;
                    Tri(centre, inner[k], inner[k1], m);
                    Quad(inner[k], rim[k], rim[k1], inner[k1], m);
                    Quad(side[k], foot[k], foot[k1], side[k1], m);
                }
            }

            public void Finish(Mesh mesh)
            {
                int n = chain.Count;
                t.vChain = chain.ToArray(); t.vF = frac.ToArray();
                t.vLocal = local.ToArray(); t.vLocalN = localN.ToArray();
                t.pos = rp.ToArray(); t.nrm = rn.ToArray(); t.tan = new Vector4[n];
                mesh.Clear();
                mesh.SetVertices(rp);
                mesh.SetNormals(rn);
                mesh.SetUVs(0, uv);
                mesh.SetUVs(2, rp);                                // rest position / normal, for the URP shader
                mesh.SetUVs(3, rn);
                mesh.subMeshCount = 3;
                for (int m = 0; m < 3; m++) mesh.SetTriangles(sub[m], m);
                mesh.RecalculateBounds();
            }
        }
    }
}
