using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;

namespace ReflectAR
{
    /// <summary>
    /// A garment simulated as cloth. The seamless mesh from GarmentMesher is skinned to six bones
    /// driven by the tracked skeleton; that skinned shape is the "body-fitted" target. An XPBD
    /// solver then simulates the fabric: inextensible stretch, soft compression (so it buckles into
    /// wrinkles), bending, gravity, inertia, damping and collisions against capsules fitted to the
    /// user's body. Each vertex is tethered to its skinned target by a max distance that is ~0 at the
    /// collar and shoulders and grows toward the hem and cuffs, which is how real garments hang.
    ///
    /// The solver runs on the welded mesh. What is drawn is a copy split along the UV seams of the
    /// pattern-piece layout, with normals taken from the welded mesh (no shading seams) and
    /// tangents for HDRP's normal maps, plus the front closure (GarmentTrims) riding on it.
    /// </summary>
    [RequireComponent(typeof(MeshFilter), typeof(MeshRenderer))]
    public class ClothGarment : MonoBehaviour
    {
        public GarmentSpec spec;
        public BodyRig body;
        public int substeps = 2;                     // the cloth inherits body motion (follow), so 2 is enough
        [Range(0, 1)] public float follow = 0.9f;    // share of body motion the cloth inherits directly

        public float Alpha { get; set; } = 1f;
        public int VertexCount => x?.Length ?? 0;
        public float LastBuildMs { get; private set; }
        public float LastStepMs { get; private set; }

        Mesh mesh, trimMesh;
        MeshRenderer bodyRenderer, trimRenderer;
        GarmentMaterials mats;
        GarmentTrims trims;
        GarmentMeshData data;
        RestSkeleton built;
        bool hasMesh;

        // simulation state
        Vector3[] x, prev, target, lastTarget;
        float[] maxDist, skinK;
        int[] eA, eB, bA, bB;
        float[] eRest, bRest;
        readonly Matrix4x4[] restInv = new Matrix4x4[6];
        readonly Matrix4x4[] skin = new Matrix4x4[6];
        readonly List<Capsule> colliders = new List<Capsule>(16);
        Vector3[] colCenter = new Vector3[0];
        float[] colBound = new float[0];
        bool simInit;

        // render mesh (split at UV seams)
        int[] toSim;
        Vector3[] rPos, rNrm, simN, tAcc, bAcc;
        Vector4[] rTan;
        bool tangents;

        public void Init(GarmentSpec s, BodyRig b)
        {
            spec = s; body = b;
            mesh = new Mesh { name = s.name, indexFormat = IndexFormat.UInt32 };
            mesh.MarkDynamic();
            GetComponent<MeshFilter>().sharedMesh = mesh;
            bodyRenderer = GetComponent<MeshRenderer>();

            var tg = new GameObject("Trims", typeof(MeshFilter), typeof(MeshRenderer));
            tg.transform.SetParent(transform, false);
            trimMesh = new Mesh { name = s.name + " trims" };
            trimMesh.MarkDynamic();
            tg.GetComponent<MeshFilter>().sharedMesh = trimMesh;
            trimRenderer = tg.GetComponent<MeshRenderer>();
            tg.SetActive(false);

            mats = new GarmentMaterials(s);
            tangents = RenderLook.HDRP;                     // normal maps; the URP shader doesn't need them
            // HDRP: the garment shadows itself (sleeves on the torso, collar on the neck)
            var cast = RenderLook.HDRP ? ShadowCastingMode.On : ShadowCastingMode.Off;
            bodyRenderer.shadowCastingMode = trimRenderer.shadowCastingMode = cast;
            mats.Apply(bodyRenderer, trimRenderer, Alpha);
        }

        public void ResetCloth() => simInit = false;

        // ---------------------------------------------------------------- mesh (re)build
        // Generating a garment takes 100-250 ms, so the mesh and its constraints are built on a
        // worker thread; only the mesh upload and trims (Unity API) happen on the main thread.

        class Built
        {
            public RestSkeleton skel;
            public GarmentMeshData data;
            public float[] maxDist, skinK, eRest, bRest;
            public int[] eA, eB, bA, bB;
            public float ms;
        }

        System.Threading.Tasks.Task<Built> pending;
        float driftSince = -1f, lastApplied = -100f, firstApplied = -100f;
        const int MaxVerts = 4000;

        public int RebuildCount { get; private set; }

        bool Drifted()
        {
            float d(float a, float b) => Mathf.Abs(a - b) / Mathf.Max(b, 1e-3f);
            if (d(body.ShoulderW, built.sw) > 0.25f || d(body.TorsoLen, built.L) > 0.25f
                || d(body.UpperL, built.upperL) > 0.3f || d(body.UpperR, built.upperR) > 0.3f
                || d(body.ForeL, built.foreL) > 0.3f || d(body.ForeR, built.foreR) > 0.3f) return true;
            for (int i = 0; i < body.Profile.Length; i++)
                if (Mathf.Abs(body.Profile[i] - built.profile[i]) > 0.08f) return true;
            return false;
        }

        /// <summary>Start (re)generating this garment for the current body if needed. Cheap; call every frame.</summary>
        public void Prepare()
        {
            if (body == null || !body.Valid || pending != null) return;
            bool rebuild = !hasMesh;
            if (hasMesh)
            {
                // only a sustained change is worth a rebuild (it resets the cloth); tracking jitter is not
                bool drift = Drifted();
                if (!drift) driftSince = -1f;
                else if (driftSince < 0f) driftSince = Time.time;
                float cooldown = Time.time - firstApplied < 15f ? 3f : 10f;   // the measured profile settles first
                rebuild = drift && Time.time - driftSince > 1.5f && Time.time - lastApplied > cooldown;
            }
            if (!rebuild) return;
            var skel = RestSkeleton.From(body);
            var g = spec;
            pending = System.Threading.Tasks.Task.Run(() => Generate(skel, g));
        }

        static Built Generate(RestSkeleton skel, GarmentSpec spec)
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();
            // detail relative to the body, capped: solver cost grows with the vertex count
            float cell = GarmentMesher.DefaultCell * Mathf.Max(skel.u, 0.8f);
            var data = GarmentMesher.Build(skel, spec, cell);
            if (data.rest.Length > MaxVerts)
                data = GarmentMesher.Build(skel, spec, cell * Mathf.Sqrt(data.rest.Length / (MaxVerts * 0.9f)));
            var r = new Built { skel = skel, data = data };
            int n = data.rest.Length;
            r.maxDist = new float[n]; r.skinK = new float[n];
            for (int i = 0; i < n; i++)
            {
                float l = data.looseness[i];
                r.maxDist[i] = Mathf.Lerp(0.002f, (0.03f + spec.ease) * spec.drape * skel.u, Mathf.Pow(l, 1.4f));
                r.skinK[i] = Mathf.Lerp(0.6f, 0.015f, Mathf.Pow(l, 0.7f));
            }
            var tris = data.tris;
            var edgeSet = new Dictionary<long, int>();             // edge -> first opposite vertex
            var eAl = new List<int>(); var eBl = new List<int>(); var bAl = new List<int>(); var bBl = new List<int>();
            long Key(int a, int b) => a < b ? ((long)a << 32) | (uint)b : ((long)b << 32) | (uint)a;
            for (int t = 0; t < tris.Length; t += 3)
                for (int e = 0; e < 3; e++)
                {
                    int a = tris[t + e], b = tris[t + (e + 1) % 3], opp = tris[t + (e + 2) % 3];
                    long k = Key(a, b);
                    if (edgeSet.TryGetValue(k, out int o)) { if (o != opp) { bAl.Add(o); bBl.Add(opp); } }   // bending across the shared edge
                    else { edgeSet[k] = opp; eAl.Add(a); eBl.Add(b); }
                }
            r.eA = eAl.ToArray(); r.eB = eBl.ToArray(); r.bA = bAl.ToArray(); r.bB = bBl.ToArray();
            r.eRest = new float[r.eA.Length]; r.bRest = new float[r.bA.Length];
            for (int i = 0; i < r.eA.Length; i++) r.eRest[i] = Vector3.Distance(data.rest[r.eA[i]], data.rest[r.eB[i]]);
            for (int i = 0; i < r.bA.Length; i++) r.bRest[i] = Vector3.Distance(data.rest[r.bA[i]], data.rest[r.bB[i]]);
            r.ms = (float)sw.Elapsed.TotalMilliseconds;
            return r;
        }

        /// <summary>Swap in a finished background build (main thread).</summary>
        void ApplyPending()
        {
            if (pending == null || !pending.IsCompleted) return;
            var task = pending; pending = null;
            if (task.IsFaulted) { Debug.LogException(task.Exception); return; }
            var r = task.Result;
            built = r.skel; data = r.data;
            maxDist = r.maxDist; skinK = r.skinK;
            eA = r.eA; eB = r.eB; bA = r.bA; bB = r.bB; eRest = r.eRest; bRest = r.bRest;
            int n = data.rest.Length;
            x = new Vector3[n]; prev = new Vector3[n]; target = new Vector3[n]; lastTarget = new Vector3[n];

            toSim = data.renderToSim;
            int rn = toSim.Length;
            rPos = new Vector3[rn]; rNrm = new Vector3[rn]; rTan = new Vector4[rn];
            tAcc = new Vector3[rn]; bAcc = new Vector3[rn]; simN = new Vector3[n];
            for (int i = 0; i < rn; i++) { rPos[i] = data.rest[toSim[i]]; rNrm[i] = data.restN[toSim[i]]; }
            mesh.Clear();
            mesh.SetVertices(rPos);
            mesh.SetNormals(rNrm);
            mesh.SetUVs(0, data.uv);                            // pattern pieces, metres (HDRP/Lit)
            mesh.SetUVs(2, rPos);                               // rest position / normal -> seamless triplanar (URP)
            mesh.SetUVs(3, rNrm);
            mesh.SetTriangles(data.renderTris, 0);
            mesh.RecalculateBounds();

            trims = GarmentTrims.Build(data, built, spec, trimMesh);
            trimRenderer.gameObject.SetActive(trims != null);
            var rf = RestFrames(built);
            for (int b = 0; b < 6; b++) restInv[b] = rf[b].inverse;
            hasMesh = true;
            simInit = false;
            lastApplied = Time.time;
            if (RebuildCount == 0) firstApplied = Time.time;
            driftSince = -1f;
            LastBuildMs = r.ms;
            RebuildCount++;
        }

        // ---------------------------------------------------------------- bones
        static Matrix4x4 Frame(Vector3 origin, Vector3 axisY, Vector3 refZ)
        {
            Vector3 y = axisY.normalized;
            Vector3 z = Vector3.ProjectOnPlane(refZ, y);
            if (z.sqrMagnitude < 1e-6f) z = Vector3.ProjectOnPlane(Vector3.forward, y);
            return Matrix4x4.TRS(origin, Quaternion.LookRotation(z.normalized, y), Vector3.one);
        }

        static Matrix4x4[] RestFrames(RestSkeleton s)
        {
            Vector3 dl = RestSkeleton.ArmDir(-1), dr = RestSkeleton.ArmDir(1);
            var upL = Frame(s.lSh, dl, Vector3.forward);
            var upR = Frame(s.rSh, dr, Vector3.forward);
            return new[]
            {
                Frame(s.hipC, Vector3.up, Vector3.forward),
                Frame(s.shC, Vector3.up, Vector3.forward),
                upL, upR,
                Frame(s.lEl, dl, upL.GetColumn(2)), Frame(s.rEl, dr, upR.GetColumn(2)),
            };
        }

        Matrix4x4[] CurrentFrames()
        {
            var J = body.J;
            Vector3 dUL = J[LM.LElbow] - J[LM.LShoulder], dUR = J[LM.RElbow] - J[LM.RShoulder];
            var upL = Frame(J[LM.LShoulder], dUL, body.Fwd);
            var upR = Frame(J[LM.RShoulder], dUR, body.Fwd);
            return new[]
            {
                Matrix4x4.TRS(body.HipC, body.PelvisRot, Vector3.one),
                Matrix4x4.TRS(body.ShC, body.ChestRot, Vector3.one),
                upL, upR,
                Frame(J[LM.LElbow], J[LM.LWrist] - J[LM.LElbow], upL.GetColumn(2)),
                Frame(J[LM.RElbow], J[LM.RWrist] - J[LM.RElbow], upR.GetColumn(2)),
            };
        }

        void SkinTargets()
        {
            var cur = CurrentFrames();
            for (int b = 0; b < 6; b++) skin[b] = cur[b] * restInv[b];
            var w = data.weights; var r = data.rest;
            for (int i = 0; i < r.Length; i++)
            {
                var bw = w[i];
                target[i] = skin[bw.boneIndex0].MultiplyPoint3x4(r[i]) * bw.weight0
                          + skin[bw.boneIndex1].MultiplyPoint3x4(r[i]) * bw.weight1
                          + skin[bw.boneIndex2].MultiplyPoint3x4(r[i]) * bw.weight2
                          + skin[bw.boneIndex3].MultiplyPoint3x4(r[i]) * bw.weight3;
            }
        }

        /// <summary>Capsules approximating the actual body (no ease), for cloth collisions.</summary>
        void BuildColliders()
        {
            colliders.Clear();
            float u = body.Unit;
            for (int k = 0; k < 5; k++)
            {
                float y = Mathf.Lerp(spec.hem - 0.1f, 0.92f, k / 4f) * body.TorsoLen;
                built.TorsoRadii(y, out float a, out float d);
                a *= body.ShoulderW / built.sw; d *= 0.95f;
                float w = Mathf.SmoothStep(0f, 1f, (y / body.TorsoLen - 0.1f) / 0.7f);
                Vector3 r = Vector3.Slerp(body.PRight, body.Right, w).normalized;
                Vector3 c = body.HipC + body.Up * y;
                float half = Mathf.Max(a - d, 0.005f);
                colliders.Add(new Capsule(c - r * half, c + r * half, d));
            }
            var J = body.J;
            colliders.Add(new Capsule(J[LM.LShoulder], J[LM.RShoulder], 0.05f * u));
            colliders.Add(new Capsule(J[LM.LShoulder], J[LM.LElbow], 0.043f * u));
            colliders.Add(new Capsule(J[LM.RShoulder], J[LM.RElbow], 0.043f * u));
            colliders.Add(new Capsule(J[LM.LElbow], J[LM.LWrist], 0.036f * u));
            colliders.Add(new Capsule(J[LM.RElbow], J[LM.RWrist], 0.036f * u));
            colliders.Add(new Capsule(body.ShC, body.HeadC - body.Up * 0.04f * u, 0.055f * u));
        }

        /// <summary>How far the cloth may stand off the body (ease plus drape), with a little to spare.</summary>
        public float Room => ((0.03f + spec.ease) * spec.drape + 0.01f) * body.Unit;

        /// <summary>Where the garment covers the torso: its torso and shoulder colliders grown by
        /// <see cref="Room"/>. The occlusion prepass keeps oversized occluders off this area.</summary>
        public void Outline(List<Capsule> into)
        {
            float room = Room;
            for (int i = 0; i < 6 && i < colliders.Count; i++)      // 5 torso levels + the shoulder line
                into.Add(new Capsule(colliders[i].a, colliders[i].b, colliders[i].r + room));
        }

        // ---------------------------------------------------------------- simulation
        public void Step(float dt)
        {
            if (body == null || !body.Valid) return;
            ApplyPending();
            Prepare();
            bodyRenderer.enabled = hasMesh;
            if (!hasMesh) { trimRenderer.gameObject.SetActive(false); return; }
            var sw = System.Diagnostics.Stopwatch.StartNew();
            SkinTargets();
            BuildColliders();
            int n = x.Length;
            if (!simInit) { System.Array.Copy(target, x, n); System.Array.Copy(target, prev, n); System.Array.Copy(target, lastTarget, n); simInit = true; }
            // Carry the cloth along with the body's own motion (like Unity Cloth's world velocity
            // scale): only (1 - follow) of each movement is left to inertia, so the garment no
            // longer trails behind the user, but still swings and wrinkles locally.
            for (int i = 0; i < n; i++)
            {
                Vector3 moved = (target[i] - lastTarget[i]) * follow;
                x[i] += moved; prev[i] += moved;
                lastTarget[i] = target[i];
            }

            dt = Mathf.Min(dt, 1f / 30f);
            float h = dt / substeps;
            float h2 = h * h;
            Vector3 g = Physics.gravity * spec.density * h2;
            float keep = 1f - Mathf.Clamp01(spec.damping * h * 60f);
            float aS = spec.stretch / h2, aC = spec.compress / h2, aB = spec.bend / h2;
            float thick = 0.004f;
            var cols = colliders;
            if (colCenter.Length < cols.Count) { colCenter = new Vector3[cols.Count]; colBound = new float[cols.Count]; }
            for (int c = 0; c < cols.Count; c++)
            {
                colCenter[c] = (cols[c].a + cols[c].b) * 0.5f;
                float rr = Vector3.Distance(cols[c].a, cols[c].b) * 0.5f + cols[c].r + thick;
                colBound[c] = rr * rr;
            }
            float skinRate = h * 60f;

            for (int s = 0; s < substeps; s++)
            {
                // predict (Verlet with damping)
                for (int i = 0; i < n; i++)
                {
                    Vector3 v = (x[i] - prev[i]) * keep;
                    prev[i] = x[i];
                    x[i] += v + g;
                }
                // stretch / compression (XPBD, one iteration per small substep)
                for (int k = 0; k < eA.Length; k++)
                {
                    int a = eA[k], b = eB[k];
                    Vector3 d = x[a] - x[b];
                    float len = d.magnitude;
                    if (len < 1e-7f) continue;
                    float C = len - eRest[k];
                    float alpha = C > 0 ? aS : aC;
                    float dl = -C / (2f + alpha);
                    Vector3 corr = d * (dl / len);
                    x[a] += corr; x[b] -= corr;
                }
                // bending (distance across each pair of triangles)
                for (int k = 0; k < bA.Length; k++)
                {
                    int a = bA[k], b = bB[k];
                    Vector3 d = x[a] - x[b];
                    float len = d.magnitude;
                    if (len < 1e-7f) continue;
                    float C = len - bRest[k];
                    float dl = -C / (2f + aB);
                    Vector3 corr = d * (dl / len);
                    x[a] += corr; x[b] -= corr;
                }
                // tether to the body-fitted shape + body collisions
                for (int i = 0; i < n; i++)
                {
                    Vector3 p = x[i], t = target[i];
                    p = Vector3.LerpUnclamped(p, t, 1f - Mathf.Pow(1f - skinK[i], skinRate));
                    Vector3 off = p - t;
                    float m = maxDist[i];
                    if (off.sqrMagnitude > m * m) p = t + off.normalized * m;
                    for (int c = 0; c < cols.Count; c++)
                    {
                        var cap = cols[c];
                        // cheap reject: outside the capsule's bounding sphere
                        Vector3 bc = colCenter[c] - p;
                        if (bc.sqrMagnitude > colBound[c]) continue;
                        Vector3 q = cap.Closest(p);
                        Vector3 nrm = p - q;
                        float r = cap.r + thick, dd = nrm.sqrMagnitude;
                        if (dd < r * r && dd > 1e-10f)
                        {
                            float dist = Mathf.Sqrt(dd);
                            Vector3 push = nrm * ((r - dist) / dist);
                            p += push;
                            // friction: kill some tangential motion while in contact
                            Vector3 vel = p - prev[i];
                            Vector3 vn = nrm * (Vector3.Dot(vel, nrm) / dd);
                            prev[i] += (vel - vn) * 0.3f;
                        }
                    }
                    x[i] = p;
                }
            }
            UpdateRenderMesh();
            trims?.Update(x, simN, trimMesh);
            mats.Apply(bodyRenderer, trimRenderer, Alpha);
            LastStepMs = (float)sw.Elapsed.TotalMilliseconds;
        }

        /// <summary>Copies the simulated cloth into the seam-split render mesh.</summary>
        void UpdateRenderMesh()
        {
            // normals on the welded mesh, so UV seams don't show in the shading
            System.Array.Clear(simN, 0, simN.Length);
            var st = data.tris;
            for (int t = 0; t < st.Length; t += 3)
            {
                int a = st[t], b = st[t + 1], c = st[t + 2];
                Vector3 fn = Vector3.Cross(x[b] - x[a], x[c] - x[a]);
                simN[a] += fn; simN[b] += fn; simN[c] += fn;
            }
            for (int i = 0; i < simN.Length; i++) simN[i] = simN[i].normalized;
            for (int r = 0; r < toSim.Length; r++) { rPos[r] = x[toSim[r]]; rNrm[r] = simN[toSim[r]]; }
            mesh.SetVertices(rPos);
            mesh.SetNormals(rNrm);
            if (tangents) { Tangents(); mesh.SetTangents(rTan); }
            mesh.RecalculateBounds();
        }

        /// <summary>Per-vertex tangent along the fabric's u direction, with handedness.</summary>
        void Tangents()
        {
            System.Array.Clear(tAcc, 0, tAcc.Length);
            System.Array.Clear(bAcc, 0, bAcc.Length);
            var rt = data.renderTris; var uv = data.uv;
            for (int t = 0; t < rt.Length; t += 3)
            {
                int i0 = rt[t], i1 = rt[t + 1], i2 = rt[t + 2];
                Vector3 e1 = rPos[i1] - rPos[i0], e2 = rPos[i2] - rPos[i0];
                Vector2 d1 = uv[i1] - uv[i0], d2 = uv[i2] - uv[i0];
                float det = d1.x * d2.y - d2.x * d1.y;
                if (Mathf.Abs(det) < 1e-12f) continue;
                float inv = 1f / det;
                Vector3 T = (e1 * d2.y - e2 * d1.y) * inv, B = (e2 * d1.x - e1 * d2.x) * inv;
                tAcc[i0] += T; tAcc[i1] += T; tAcc[i2] += T;
                bAcc[i0] += B; bAcc[i1] += B; bAcc[i2] += B;
            }
            for (int r = 0; r < rTan.Length; r++)
            {
                Vector3 n = rNrm[r], T = tAcc[r] - n * Vector3.Dot(n, tAcc[r]);
                if (T.sqrMagnitude < 1e-12f) T = Vector3.Cross(n, Mathf.Abs(n.y) < 0.9f ? Vector3.up : Vector3.right);
                T.Normalize();
                float w = Vector3.Dot(Vector3.Cross(n, T), bAcc[r]) < 0f ? -1f : 1f;
                rTan[r] = new Vector4(T.x, T.y, T.z, w);
            }
        }
    }
}
