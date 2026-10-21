using System.Collections.Generic;
using UnityEngine;

namespace ReflectAR
{
    /// <summary>
    /// A garment simulated as cloth. The seamless mesh from GarmentMesher is skinned to six bones
    /// driven by the tracked skeleton; that skinned shape is the "body-fitted" target. An XPBD
    /// solver then simulates the fabric: inextensible stretch, soft compression (so it buckles into
    /// wrinkles), bending, gravity, inertia, damping and collisions against capsules fitted to the
    /// user's body. Each vertex is tethered to its skinned target by a max distance that is ~0 at the
    /// collar and shoulders and grows toward the hem and cuffs, which is how real garments hang.
    /// </summary>
    [RequireComponent(typeof(MeshFilter), typeof(MeshRenderer))]
    public class ClothGarment : MonoBehaviour
    {
        public GarmentSpec spec;
        public BodyRig body;
        public int substeps = 2;                     // cloth inherits body motion (follow), so 2 is enough
        const int MaxVerts = 4000;
        [Range(0, 1)] public float follow = 0.9f;  // share of body motion the cloth inherits directly

        public float Alpha { get; set; } = 1f;
        public int VertexCount => x?.Length ?? 0;
        public float LastBuildMs { get; private set; }
        public float LastStepMs { get; private set; }
        public int RebuildCount { get; private set; }

        Mesh mesh;
        Material mat;
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

        public void Init(GarmentSpec s, BodyRig b, Material baseMat)
        {
            spec = s; body = b;
            mesh = new Mesh { name = s.name, indexFormat = UnityEngine.Rendering.IndexFormat.UInt32 };
            mesh.MarkDynamic();
            GetComponent<MeshFilter>().sharedMesh = mesh;
            mat = new Material(baseMat) { name = s.name };
            mat.SetTexture("_BaseMap", PatternTex.Make(s));
            mat.SetFloat("_Tile", 1f / Mathf.Max(s.tile, 0.01f));
            mat.SetFloat("_Sheen", s.sheen);
            mat.SetFloat("_Placket", s.placket);
            GetComponent<MeshRenderer>().sharedMaterial = mat;
            GetComponent<MeshRenderer>().shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
        }

        public void ResetCloth() => simInit = false;

        // ---------------------------------------------------------------- mesh (re)build
        // Generating a garment takes 100-250 ms, so it always runs on a worker thread and the
        // finished result is swapped in on the main thread; the render loop never waits for it.

        class Built
        {
            public RestSkeleton skel;
            public GarmentMeshData data;
            public float[] maxDist, skinK, eRest, bRest;
            public int[] eA, eB, bA, bB;
            public float ms;
        }

        System.Threading.Tasks.Task<Built> pending;
        float driftSince = -1f, lastApplied = -100f;

        /// <summary>Start generating this garment for the current body if needed (cheap to call every frame).</summary>
        public void Prepare()
        {
            if (body == null || !body.Valid || pending != null) return;
            bool rebuild = !hasMesh;
            if (hasMesh)
            {
                float d(float a, float b) => Mathf.Abs(a - b) / Mathf.Max(b, 1e-3f);
                bool drift = d(body.ShoulderW, built.sw) > 0.25f || d(body.TorsoLen, built.L) > 0.25f
                    || d(body.UpperL, built.upperL) > 0.3f || d(body.UpperR, built.upperR) > 0.3f
                    || d(body.ForeL, built.foreL) > 0.3f || d(body.ForeR, built.foreR) > 0.3f;
                // only a sustained change in the user's proportions (new person, or measurements
                // settling) is worth a rebuild; tracking jitter is not
                // the measured silhouette arrives (or changes noticeably): refit soon
                float bc = body.Shape[BodyRig.ShapeChest];
                bool shapeNew = bc > 0f && (built.mChest <= 0f || d(bc, built.mChest) > 0.12f
                    || d(body.Shape[BodyRig.ShapeArm], Mathf.Max(built.mArm, 1e-3f)) > 0.2f && body.Shape[BodyRig.ShapeArm] > 0f);
                if (!drift) driftSince = -1f;
                else if (driftSince < 0f) driftSince = Time.time;
                rebuild = (drift && Time.time - driftSince > 2f && Time.time - lastApplied > 10f)
                       || (shapeNew && Time.time - lastApplied > (built.mChest <= 0f ? 0.5f : 8f));
            }
            if (!rebuild) return;
            var skel = RestSkeleton.From(body);
            var g = spec;
            pending = System.Threading.Tasks.Task.Run(() => Generate(skel, g));
        }

        static Built Generate(RestSkeleton skel, GarmentSpec spec)
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();
            // Mesh detail is relative to the body (same look for every user) and capped, because
            // solver cost grows with vertex count: ~4k vertices keeps it to a few ms per frame.
            float cell = GarmentMesher.DefaultCell * Mathf.Max(skel.u, 0.8f);
            var data = GarmentMesher.Build(skel, spec, cell);
            if (data.rest.Length > MaxVerts)
                data = GarmentMesher.Build(skel, spec, cell * Mathf.Sqrt(data.rest.Length / (MaxVerts * 0.9f)));
            var r = new Built { skel = skel, data = data };
            int n = r.data.rest.Length;
            r.maxDist = new float[n]; r.skinK = new float[n];
            for (int i = 0; i < n; i++)
            {
                float l = r.data.looseness[i];
                r.maxDist[i] = Mathf.Lerp(0.002f, (0.03f + spec.ease) * spec.drape * skel.u, Mathf.Pow(l, 1.4f));
                r.skinK[i] = Mathf.Lerp(0.6f, 0.015f, Mathf.Pow(l, 0.7f));
            }
            var tris = r.data.tris;
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
            for (int i = 0; i < r.eA.Length; i++) r.eRest[i] = Vector3.Distance(r.data.rest[r.eA[i]], r.data.rest[r.eB[i]]);
            for (int i = 0; i < r.bA.Length; i++) r.bRest[i] = Vector3.Distance(r.data.rest[r.bA[i]], r.data.rest[r.bB[i]]);
            r.ms = (float)sw.Elapsed.TotalMilliseconds;
            return r;
        }

        /// <summary>Swap in a finished background build (main thread; ~1 ms).</summary>
        void ApplyPending()
        {
            if (pending == null || !pending.IsCompleted) return;
            var t = pending; pending = null;
            if (t.IsFaulted) { Debug.LogException(t.Exception); return; }
            var r = t.Result;
            built = r.skel; data = r.data;
            maxDist = r.maxDist; skinK = r.skinK;
            eA = r.eA; eB = r.eB; bA = r.bA; bB = r.bB; eRest = r.eRest; bRest = r.bRest;
            int n = data.rest.Length;
            x = new Vector3[n]; prev = new Vector3[n]; target = new Vector3[n]; lastTarget = new Vector3[n];

            mesh.Clear();
            mesh.vertices = data.rest;
            mesh.SetUVs(0, new List<Vector3>(data.rest));      // rest position -> seamless triplanar fabric
            mesh.SetUVs(1, new List<Vector3>(data.restN));
            mesh.triangles = data.tris;
            mesh.RecalculateNormals();

            float u = built.u;
            mat.SetVector("_Front", new Vector4(built.L + 0.06f * u, spec.hem * built.L,
                Mathf.Lerp(spec.hem * built.L, built.L + 0.06f * u, 1f - spec.placketLen), u));
            var rf = RestFrames(built);
            for (int b = 0; b < 6; b++) restInv[b] = rf[b].inverse;
            hasMesh = true;
            simInit = false;
            lastApplied = Time.time;
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

        // ---------------------------------------------------------------- simulation
        public void Step(float dt)
        {
            if (body == null || !body.Valid) return;
            ApplyPending();
            Prepare();
            if (!hasMesh) { GetComponent<MeshRenderer>().enabled = false; return; }
            GetComponent<MeshRenderer>().enabled = true;
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
            mesh.vertices = x;
            mesh.RecalculateNormals();
            mesh.RecalculateBounds();
            mat.SetFloat("_Alpha", Alpha);
            LastStepMs = (float)sw.Elapsed.TotalMilliseconds;
        }
    }
}
