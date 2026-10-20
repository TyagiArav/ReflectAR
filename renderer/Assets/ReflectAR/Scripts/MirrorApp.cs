using UnityEngine;
using UnityEngine.InputSystem;

namespace ReflectAR
{
    public enum ViewMode { CameraFeed, Mirror }

    /// <summary>
    /// Top-level controller: camera modes, body proxy, garment catalogue, gestures and UI.
    ///
    /// CameraFeed: a "digital mirror" for the laptop. A virtual camera with the webcam's
    ///   intrinsics sits at the webcam's pose; the image is flipped horizontally and garments are
    ///   drawn over the live feed.
    /// Mirror: what the two-way-mirror build needs. Black background, garments placed on the
    ///   reflected body (MirrorWorld scale z = -1), rendered with an off-axis frustum from the
    ///   tracked eye through the physical screen rectangle.
    /// </summary>
    public class MirrorApp : MonoBehaviour
    {
        public ViewMode mode = ViewMode.CameraFeed;
        public bool showProxy;
        public bool showDebug = true;
        public bool cubeTest;

        TrackingClient client;
        Camera cam;
        Transform mirrorWorld;
        readonly BodyRig body = new BodyRig();
        ClothGarment[] garments;
        GarmentSpec[] specs;
        BodyProxy proxy;
        MirrorUI ui;
        GameObject feedQuad, cube;
        Light keyLight;
        Mesh feedMesh;
        Material feedMat;

        int current, selected = -1;
        float visible;           // 0..1 garment fade
        bool hadBody;
        float lastTracked = -10f;

        void Start()
        {
            Application.targetFrameRate = 60;
            QualitySettings.vSyncCount = 1;
            Screen.sleepTimeout = SleepTimeout.NeverSleep;

            client = gameObject.AddComponent<TrackingClient>();
            cam = Camera.main;
            cam.clearFlags = CameraClearFlags.SolidColor;
            cam.backgroundColor = Color.black;
            cam.nearClipPlane = 0.05f;
            cam.farClipPlane = 20f;

            mirrorWorld = new GameObject("MirrorWorld").transform;

            var garmentMat = new Material(Shader.Find("ReflectAR/Garment"));
            specs = GarmentCatalog.All;
            garments = new ClothGarment[specs.Length];
            for (int i = 0; i < specs.Length; i++)
            {
                var go = new GameObject("Garment_" + specs[i].name, typeof(MeshFilter), typeof(MeshRenderer));
                go.transform.SetParent(mirrorWorld, false);
                var g = go.AddComponent<ClothGarment>();
                g.Init(specs[i], body, garmentMat);
                go.SetActive(false);
                garments[i] = g;
            }
            garments[current].gameObject.SetActive(true);

            proxy = new BodyProxy(mirrorWorld, new Material(Shader.Find("ReflectAR/DepthOnly")));

            feedMat = new Material(Shader.Find("ReflectAR/Feed"));
            feedQuad = new GameObject("CameraFeed", typeof(MeshFilter), typeof(MeshRenderer));
            feedMesh = new Mesh { name = "FeedQuad" };
            feedMesh.SetVertices(new Vector3[4]);
            feedMesh.SetUVs(0, new[] { new Vector2(0, 1), new Vector2(1, 1), new Vector2(1, 0), new Vector2(0, 0) });
            feedMesh.SetTriangles(new[] { 0, 1, 2, 0, 2, 3 }, 0);
            feedQuad.GetComponent<MeshFilter>().sharedMesh = feedMesh;
            feedQuad.GetComponent<MeshRenderer>().sharedMaterial = feedMat;

            cube = GameObject.CreatePrimitive(PrimitiveType.Cube);
            Destroy(cube.GetComponent<Collider>());
            cube.name = "CubeTest";
            cube.transform.localScale = Vector3.one * 0.2f;
            cube.GetComponent<MeshRenderer>().sharedMaterial = new Material(garmentMat);
            cube.SetActive(false);

            keyLight = FindAnyObjectByType<Light>();
            if (keyLight != null)
            {
                keyLight.type = LightType.Directional;
                keyLight.intensity = 1.1f;
                keyLight.shadows = LightShadows.None;
            }

            ui = new MirrorUI(specs);
        }

        // ------------------------------------------------------------------ input
        void HandleInput()
        {
            var kb = Keyboard.current;
            if (kb != null)
            {
                if (kb.rightArrowKey.wasPressedThisFrame) Browse(+1);
                if (kb.leftArrowKey.wasPressedThisFrame) Browse(-1);
                if (kb.spaceKey.wasPressedThisFrame) Select();
                if (kb.mKey.wasPressedThisFrame) mode = mode == ViewMode.CameraFeed ? ViewMode.Mirror : ViewMode.CameraFeed;
                if (kb.bKey.wasPressedThisFrame) showProxy = !showProxy;
                if (kb.dKey.wasPressedThisFrame) showDebug = !showDebug;
                if (kb.cKey.wasPressedThisFrame) cubeTest = !cubeTest;
                if (kb.escapeKey.wasPressedThisFrame) Application.Quit();
            }
            while (client.Events.TryDequeue(out var e))
            {
                if (e == "next") Browse(+1);
                else if (e == "prev") Browse(-1);
                else if (e == "select") Select();
            }
        }

        void Browse(int d)
        {
            garments[current].gameObject.SetActive(false);
            current = (current + d + garments.Length) % garments.Length;
            garments[current].ResetCloth();
            garments[current].gameObject.SetActive(true);
            ui.OnBrowse(d);
        }

        void Select()
        {
            selected = current;
            ui.Toast($"Selected: {specs[current].name}");
        }

        // ------------------------------------------------------------------ frame
        // frame-time stats, logged every 3 s (worst frame, cloth cost, mesh rebuilds)
        float statT, statWorst, statSum, statCloth; int statN, statRebuilds;

        void LogStats()
        {
            float dt = Time.unscaledDeltaTime;
            statWorst = Mathf.Max(statWorst, dt); statSum += dt; statN++;
            var g = garments[current];
            statCloth = Mathf.Max(statCloth, g.LastStepMs);
            if (Time.realtimeSinceStartup - statT < 3f) return;
            int rb = 0; foreach (var gg in garments) rb += gg.RebuildCount;
            Debug.Log($"[ReflectAR stats] avg {statN / statSum:0} fps, worst frame {statWorst * 1000:0} ms, " +
                      $"cloth max {statCloth:0.0} ms, rebuilds +{rb - statRebuilds} (last {g.LastBuildMs:0} ms), verts {g.VertexCount}");
            statT = Time.realtimeSinceStartup; statWorst = statSum = statCloth = 0; statN = 0; statRebuilds = rb;
        }

        void Update()
        {
            LogStats();
            HandleInput();
            var f = client.Latest;
            bool fresh = client.Connected && f != null && Time.time - client.LastFrameTime < 0.5f;
            bool tracked = fresh && f.tracked;
            if (tracked)
            {
                body.Update(client.Joints, client.Visibility, Time.deltaTime, !hadBody || Time.time - lastTracked > 0.5f);
                hadBody = true;
                lastTracked = Time.time;
            }
            // hold the last pose 0.5 s, then fade over 0.3 s; dim while out of range
            float target = !hadBody || Time.time - lastTracked > 0.5f ? 0f : (f != null && f.range == "ok" ? 1f : 0.35f);
            visible = Mathf.MoveTowards(visible, target, Time.deltaTime / 0.3f);
            if (visible <= 0f && Time.time - lastTracked > 0.8f) hadBody = false;

            mirrorWorld.localScale = mode == ViewMode.Mirror ? new Vector3(1, 1, -1) : Vector3.one;
            // key light from above, beside the screen, falling on the (real or reflected) front of the body
            if (keyLight != null)
                keyLight.transform.rotation = Quaternion.LookRotation(new Vector3(0.55f, -0.6f, mode == ViewMode.Mirror ? 1f : -1f));

            if (body.Valid) foreach (var gg in garments) gg.Prepare();   // background, so swiping never waits
            var g = garments[current];
            g.Alpha = visible;
            g.gameObject.SetActive(visible > 0.001f);
            if (body.Valid && visible > 0f) g.Step(Time.deltaTime);
            proxy.Update(body, body.Valid && visible > 0f, showProxy);
            UpdateFeedQuad(f);
            cube.SetActive(cubeTest && mode == ViewMode.Mirror);
            if (cube.activeSelf && f != null && f.screen != null)
                cube.transform.SetPositionAndRotation(new Vector3(f.screen.w_m / 2, f.screen.h_m / 2, 0.75f), Quaternion.Euler(20, 30, 0));
            ui.Update(f, client.Connected, tracked, current, selected, mode, showDebug, cam, body);
        }

        void LateUpdate()
        {
            var f = client.Latest;
            if (f == null || f.cam == null || f.cam.fx <= 0) return;
            if (mode == ViewMode.CameraFeed) ApplyFeedCamera(f.cam);
            else ApplyOffAxis(f.screen, hadBody ? client.Eye : DefaultEye(f.screen));
        }

        static Vector3 DefaultEye(ScreenInfo s) => new Vector3(s.w_m / 2, s.h_m / 2 + 0.25f, -1.5f);

        // Camera -> Mirror rotation, identical to perception/geometry.py
        static Matrix4x4 CamToMirror(CamInfo ci)
        {
            float th = ci.tilt * Mathf.Deg2Rad;
            var tilt = Matrix4x4.identity;
            tilt.SetRow(1, new Vector4(0, Mathf.Cos(th), Mathf.Sin(th), 0));
            tilt.SetRow(2, new Vector4(0, -Mathf.Sin(th), Mathf.Cos(th), 0));
            var flip = Matrix4x4.Scale(new Vector3(-1, -1, 1));
            return flip * tilt;
        }

        static Vector3 CamPosMirror(CamInfo ci) => ci.pos != null && ci.pos.Length == 3 ? new Vector3(ci.pos[0], ci.pos[1], ci.pos[2]) : Vector3.zero;

        void ApplyFeedCamera(CamInfo ci)
        {
            cam.clearFlags = CameraClearFlags.SolidColor;
            Matrix4x4 R = CamToMirror(ci);
            Vector3 pos = CamPosMirror(ci);
            // world(Unity) -> mirror -> camera(OpenCV) -> GL view
            var zflip = Matrix4x4.Scale(new Vector3(1, 1, -1));
            var gl = Matrix4x4.Scale(new Vector3(1, -1, -1));
            Matrix4x4 view = gl * R.transpose * Matrix4x4.Translate(-pos) * zflip;

            float n = cam.nearClipPlane, fa = cam.farClipPlane;
            var P = Matrix4x4.zero;
            P[0, 0] = -2f * ci.fx / ci.w;                 // negative: horizontal mirror flip
            P[0, 2] = -(1f - 2f * ci.cx / ci.w);
            P[1, 1] = 2f * ci.fy / ci.h;
            P[1, 2] = 2f * ci.cy / ci.h - 1f;
            P[2, 2] = -(fa + n) / (fa - n);
            P[2, 3] = -2f * fa * n / (fa - n);
            P[3, 2] = -1f;

            Vector3 fwdM = R.MultiplyVector(Vector3.forward), upM = R.MultiplyVector(Vector3.down);
            transform.position = new Vector3(pos.x, pos.y, -pos.z);
            cam.transform.SetPositionAndRotation(transform.position,
                Quaternion.LookRotation(new Vector3(fwdM.x, fwdM.y, -fwdM.z), new Vector3(upM.x, upM.y, -upM.z)));
            cam.worldToCameraMatrix = view;
            cam.projectionMatrix = P;
        }

        void ApplyOffAxis(ScreenInfo s, Vector3 eye)
        {
            cam.clearFlags = CameraClearFlags.SolidColor;
            Vector3 pa = Vector3.zero, pb = new Vector3(s.w_m, 0, 0), pc = new Vector3(0, s.h_m, 0);
            Vector3 vr = (pb - pa).normalized, vu = (pc - pa).normalized;
            Vector3 vn = Vector3.Cross(vu, vr).normalized;          // toward the viewer (Unity z < 0)
            Vector3 va = pa - eye, vb = pb - eye, vc = pc - eye;
            float d = -Vector3.Dot(va, vn);
            if (d < 0.05f) return;
            float n = cam.nearClipPlane, fa = cam.farClipPlane;
            float l = Vector3.Dot(vr, va) * n / d, r = Vector3.Dot(vr, vb) * n / d;
            float b = Vector3.Dot(vu, va) * n / d, t = Vector3.Dot(vu, vc) * n / d;
            var M = Matrix4x4.identity;
            M.SetRow(0, new Vector4(vr.x, vr.y, vr.z, 0));
            M.SetRow(1, new Vector4(vu.x, vu.y, vu.z, 0));
            M.SetRow(2, new Vector4(vn.x, vn.y, vn.z, 0));          // GL view looks down -z = into the mirror
            cam.transform.SetPositionAndRotation(eye, Quaternion.LookRotation(-vn, vu));
            cam.worldToCameraMatrix = M * Matrix4x4.Translate(-eye);
            cam.projectionMatrix = Matrix4x4.Frustum(l, r, b, t, n, fa);
        }

        void UpdateFeedQuad(TrackingFrame f)
        {
            bool on = mode == ViewMode.CameraFeed && f != null && f.cam != null && f.cam.fx > 0;
            feedQuad.SetActive(on);
            if (!on) return;
            feedMat.SetTexture("_BaseMap", client.Feed);
            feedMat.SetFloat("_Dim", f.range == "ok" || !f.tracked ? 1f : 0.6f);
            var ci = f.cam;
            Matrix4x4 R = CamToMirror(ci);
            Vector3 pos = CamPosMirror(ci);
            const float D = 15f;
            var px = new[] { new Vector2(0, 0), new Vector2(ci.w, 0), new Vector2(ci.w, ci.h), new Vector2(0, ci.h) };
            var v = new Vector3[4];
            for (int i = 0; i < 4; i++)
            {
                var pc = new Vector3((px[i].x - ci.cx) * D / ci.fx, (px[i].y - ci.cy) * D / ci.fy, D);
                Vector3 pm = R.MultiplyVector(pc) + pos;
                v[i] = new Vector3(pm.x, pm.y, -pm.z);
            }
            feedMesh.SetVertices(v);
            feedMesh.RecalculateBounds();
        }
    }

    /// <summary>Depth-only capsules standing in for the user's body (occlusion prepass).</summary>
    public class BodyProxy
    {
        readonly Transform[] parts;
        readonly Material mat;
        static readonly (int a, int b, float r)[] Limbs =
        {
            (LM.LShoulder, LM.LElbow, 0.045f), (LM.RShoulder, LM.RElbow, 0.045f),
            (LM.LElbow, LM.LWrist, 0.04f), (LM.RElbow, LM.RWrist, 0.04f),
            (LM.LHip, LM.LKnee, 0.07f), (LM.RHip, LM.RKnee, 0.07f),
            (LM.LKnee, LM.LAnkle, 0.055f), (LM.RKnee, LM.RAnkle, 0.055f),
        };
        // parts: limbs..., torso, neck, head, lhand, rhand

        public BodyProxy(Transform parent, Material m)
        {
            mat = m;
            var root = new GameObject("BodyProxy").transform;
            root.SetParent(parent, false);
            parts = new Transform[Limbs.Length + 5];
            for (int i = 0; i < parts.Length; i++)
            {
                bool sphere = i >= Limbs.Length + 2;
                var go = GameObject.CreatePrimitive(sphere ? PrimitiveType.Sphere : PrimitiveType.Capsule);
                Object.Destroy(go.GetComponent<Collider>());
                go.GetComponent<MeshRenderer>().sharedMaterial = mat;
                go.GetComponent<MeshRenderer>().shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
                go.transform.SetParent(root, false);
                parts[i] = go.transform;
            }
        }

        static void Place(Transform t, Vector3 a, Vector3 b, float rx, float rz, Vector3 fwdHint)
        {
            Vector3 d = b - a;
            float len = d.magnitude;
            if (len < 1e-4f) { t.localScale = Vector3.zero; return; }
            Vector3 up = d / len;
            Vector3 f = Vector3.ProjectOnPlane(fwdHint, up);
            if (f.sqrMagnitude < 1e-4f) f = Vector3.ProjectOnPlane(Vector3.forward, up);
            t.localPosition = (a + b) * 0.5f;
            t.localRotation = Quaternion.LookRotation(f.normalized, up);
            t.localScale = new Vector3(rx * 2f, (len + 2f * Mathf.Min(rx, rz)) * 0.5f, rz * 2f);
        }

        public void Update(BodyRig b, bool on, bool debugVisible)
        {
            parts[0].parent.gameObject.SetActive(on);
            if (!on) return;
            mat.SetFloat("_ColorMask", debugVisible ? 15f : 0f);
            for (int i = 0; i < Limbs.Length; i++)
                Place(parts[i], b.J[Limbs[i].a], b.J[Limbs[i].b], Limbs[i].r * b.Unit, Limbs[i].r * b.Unit, b.Fwd);
            int k = Limbs.Length;
            Place(parts[k], b.HipC, b.ShC, b.ShoulderW * 0.42f, 0.095f * b.Unit, b.Fwd);                     // torso
            Place(parts[k + 1], b.ShC, b.HeadC - b.Up * 0.05f * b.Unit, 0.05f * b.Unit, 0.05f * b.Unit, b.Fwd);                  // neck
            parts[k + 2].localPosition = b.HeadC; parts[k + 2].localRotation = b.ChestRot;
            parts[k + 2].localScale = new Vector3(b.HeadR * 1.9f, b.HeadR * 2.3f, b.HeadR * 2.1f);    // head
            for (int s = 0; s < 2; s++)
            {
                int w = s == 0 ? LM.LWrist : LM.RWrist, ix = s == 0 ? LM.LIndex : LM.RIndex, pk = s == 0 ? LM.LPinky : LM.RPinky;
                var t = parts[k + 3 + s];
                t.localPosition = (b.J[w] + b.J[ix] + b.J[pk]) / 3f;
                t.localScale = Vector3.one * 0.09f * b.Unit;
            }
        }
    }
}
