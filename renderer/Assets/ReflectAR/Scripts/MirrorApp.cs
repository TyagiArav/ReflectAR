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
    ///
    /// Runs under URP or HDRP (ReflectAR > Render Pipeline menu). HDRP adds physically based
    /// fabric (HDRP/Lit with weave normal maps), soft shadows, SSAO and contact shadows (HdrpLook).
    /// </summary>
    public class MirrorApp : MonoBehaviour
    {
        public ViewMode mode = ViewMode.CameraFeed;
        public bool showProxy;
        public bool showDebug = true;
        public bool cubeTest;
        [Tooltip("Extrapolate the pose to when each frame reaches the screen (P toggles, for A/B).")]
        public bool predict = true;
        [Tooltip("Extra prediction lead (ms) on top of config.json's latencies, Mirror mode. Tune with [ and ].")]
        public float leadMs;
        [Tooltip("Trim the hands / bare arms / head occluders to perception's person mask (O toggles, for A/B).")]
        public bool trimToMask = true;

        TrackingClient client;
        Camera cam;
        Transform mirrorWorld;
        readonly BodyRig body = new BodyRig();
        readonly BodyShape shape = new BodyShape();
        ClothGarment[] garments;
        GarmentSpec[] specs;
        BodyProxy proxy;
        readonly Vector3[] maskJoints = new Vector3[33];
        MirrorUI ui;
        GameObject feedQuad, cube;
        Light keyLight, fillLight;
        bool hdrp;
        Mesh feedMesh;
        Material feedMat;

        int current, selected = -1;
        float visible;           // 0..1 garment fade
        bool hadBody;
        float lastTracked = -10f;

        void Start()
        {
            // Frame pacing: vsync would render at the panel rate (120 Hz on ProMotion) for 30 Hz
            // tracking; cap at 60 fps while someone is tracked and 30 fps when idle (Update).
            QualitySettings.vSyncCount = 0;
            Application.targetFrameRate = 60;
            Screen.sleepTimeout = SleepTimeout.NeverSleep;

            client = gameObject.AddComponent<TrackingClient>();
            cam = Camera.main;
            cam.clearFlags = CameraClearFlags.SolidColor;
            cam.backgroundColor = Color.black;
            cam.nearClipPlane = 0.05f;
            cam.farClipPlane = 20f;

            mirrorWorld = new GameObject("MirrorWorld").transform;
            hdrp = RenderLook.HDRP;
            if (hdrp)
            {
                HdrpLook.SetupCamera(cam);
                HdrpLook.CreateVolume();
            }

            specs = GarmentCatalog.All;
            garments = new ClothGarment[specs.Length];
            for (int i = 0; i < specs.Length; i++)
            {
                var go = new GameObject("Garment_" + specs[i].name, typeof(MeshFilter), typeof(MeshRenderer));
                go.transform.SetParent(mirrorWorld, false);
                var g = go.AddComponent<ClothGarment>();
                g.Init(specs[i], body);
                go.SetActive(false);
                garments[i] = g;
            }
            garments[current].gameObject.SetActive(true);

            proxy = new BodyProxy(mirrorWorld, new Material(Shader.Find("ReflectAR/DepthOnly")), hdrp);

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
            cube.GetComponent<MeshRenderer>().sharedMaterial = GarmentMaterials.Plain(new Color(0.8f, 0.8f, 0.8f));
            cube.SetActive(false);

            keyLight = FindAnyObjectByType<Light>();
            if (keyLight == null) keyLight = new GameObject("KeyLight").AddComponent<Light>();
            if (hdrp)
            {
                // key with soft shadows, plus a dim fill from the other side so shadowed folds aren't black
                HdrpLook.SetupLight(keyLight, HdrpLook.KeyLux, true, 6f);
                fillLight = new GameObject("FillLight").AddComponent<Light>();
                HdrpLook.SetupLight(fillLight, HdrpLook.FillLux, false, 0f);
            }
            else
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
                if (kb.pKey.wasPressedThisFrame) { predict = !predict; ui.Toast(predict ? "Prediction on" : "Prediction off"); }
                if (kb.oKey.wasPressedThisFrame) { trimToMask = !trimToMask; ui.Toast(trimToMask ? "Occlusion: person mask" : "Occlusion: capsules only"); }
                if (kb.leftBracketKey.wasPressedThisFrame) leadMs -= 5f;
                if (kb.rightBracketKey.wasPressedThisFrame) leadMs += 5f;
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
            var cg = garments[current];
            statCloth = Mathf.Max(statCloth, cg.LastStepMs);
            if (Time.realtimeSinceStartup - statT < 3f) return;
            int rb = 0; foreach (var gg in garments) rb += gg.RebuildCount;
            Debug.Log($"[ReflectAR stats] avg {statN / statSum:0} fps, worst frame {statWorst * 1000:0} ms, " +
                      $"cloth max {statCloth:0.0} ms, rebuilds +{rb - statRebuilds} (last {cg.LastBuildMs:0} ms), verts {cg.VertexCount}");
            statT = Time.realtimeSinceStartup; statWorst = statSum = statCloth = 0; statN = 0; statRebuilds = rb;
        }

        void Update()
        {
            LogStats();
            client.Poll();
            HandleInput();
            var f = client.Latest;
            bool fresh = client.Connected && f != null && Time.time - client.LastFrameTime < 0.5f;
            bool tracked = fresh && f.tracked;
            Application.targetFrameRate = tracked || visible > 0f ? 60 : 30;
            if (tracked)
            {
                // Mirror: the real reflection has no delay, so aim for the moment this frame is on
                // screen. CameraFeed: match the (equally delayed) image under the overlay instead.
                double now = TrackingClient.Now;
                bool matchFeed = mode == ViewMode.CameraFeed && now - client.FeedCaptureTime < 0.5;
                double target = matchFeed ? client.FeedCaptureTime : now + client.DisplayLatency + leadMs * 0.001;
                client.Predict(target, predict, predict && !matchFeed);
                if (!hadBody) shape.Reset(body);                 // a new person: measure them from scratch
                body.Update(client.Joints, client.Visibility, Time.deltaTime, !hadBody || Time.time - lastTracked > 0.5f);
                hadBody = true;
                lastTracked = Time.time;
            }
            // hold the last pose 0.5 s, then fade over 0.3 s; dim while out of range
            float fadeTo = !hadBody || Time.time - lastTracked > 0.5f ? 0f : (f != null && f.range == "ok" ? 1f : 0.35f);
            visible = Mathf.MoveTowards(visible, fadeTo, Time.deltaTime / 0.3f);
            if (visible <= 0f && Time.time - lastTracked > 0.8f) hadBody = false;

            mirrorWorld.localScale = mode == ViewMode.Mirror ? new Vector3(1, 1, -1) : Vector3.one;
            // key light from above, beside the screen, falling on the (real or reflected) front of the body
            float zs = mode == ViewMode.Mirror ? 1f : -1f;
            keyLight.transform.rotation = Quaternion.LookRotation(new Vector3(0.55f, -0.6f, zs));
            if (fillLight != null) fillLight.transform.rotation = Quaternion.LookRotation(new Vector3(-0.7f, -0.2f, zs));

            if (body.Valid) foreach (var gg in garments) gg.Prepare();   // background builds, so swiping never waits
            var g = garments[current];
            g.Alpha = visible;
            g.gameObject.SetActive(visible > 0.001f);
            if (body.Valid && visible > 0f) g.Step(Time.deltaTime);
            bool maskFresh = tracked && client.Mask != null && TrackingClient.Now - client.MaskCaptureTime < 0.3;
            bool maskOn = trimToMask && maskFresh;
            if (maskFresh)
            {
                client.PoseAt(client.MaskCaptureTime, maskJoints);
                // measure the user's build from the silhouette, a few times a second
                if (f.range == "ok" && Time.frameCount % 4 == 0)
                {
                    Vector3 cp = CamPosMirror(f.cam);
                    shape.Measure(client.MaskBytes, client.MaskWidth, client.MaskHeight, MaskMatrix(f.cam), maskJoints, body,
                        new Vector3(cp.x, cp.y, -cp.z), Time.deltaTime * 4f);
                }
            }
            if (maskOn)
            {
                Shader.SetGlobalTexture("_ReflectARMask", client.Mask);
                Shader.SetGlobalFloat("_ReflectARMaskThreshold", 0.5f);
            }
            if (f != null && f.cam != null && f.cam.fx > 0)
            {
                Shader.SetGlobalMatrix("_ReflectARMaskMatrix", MaskMatrix(f.cam));
                Shader.SetGlobalMatrix("_ReflectARWorldToBody", mirrorWorld.worldToLocalMatrix);
            }
            // HDRP: the proxies show the feed where they cover it (see ReflectAR_DepthOnly.shader)
            bool feedOn = mode == ViewMode.CameraFeed && f != null && f.cam != null && f.cam.fx > 0;
            Shader.SetGlobalTexture("_ReflectARFeed", client.Feed);
            Shader.SetGlobalFloat("_ReflectARFeedOn", feedOn ? (f.range == "ok" || !f.tracked ? 1f : 0.6f) : 0f);
            proxy.Update(body, body.Valid && visible > 0f, showProxy, specs[current].tank ? 0f : specs[current].sleeve, maskOn ? maskJoints : null, g);
            UpdateFeedQuad(f);
            cube.SetActive(cubeTest && mode == ViewMode.Mirror);
            if (cube.activeSelf && f != null && f.screen != null)
                cube.transform.SetPositionAndRotation(new Vector3(f.screen.w_m / 2, f.screen.h_m / 2, 0.75f), Quaternion.Euler(20, 30, 0));
            string lat = $"latency {client.LatencyMs:0} ms  ·  prediction {(predict ? $"+{client.HorizonMs:0} ms" : "off")}  lead {leadMs:+0;-0} ms  [P] [ [ ] ]  ·  occlusion {(maskOn ? "person mask" : "capsules")} [O]";
            ui.Update(f, client.Connected, tracked, current, selected, mode, showDebug, cam, body, lat);
        }

        void LateUpdate()
        {
            var f = client.Latest;
            if (f == null || f.cam == null || f.cam.fx <= 0) return;
            if (mode == ViewMode.CameraFeed) ApplyFeedCamera(f.cam);
            else ApplyOffAxis(f.screen, hadBody ? client.Eye : DefaultEye(f.screen));
            // the viewpoint in body coords, for the occlusion prepass (ReflectAR_DepthOnly.shader)
            Shader.SetGlobalVector("_ReflectARCamBody", mirrorWorld.InverseTransformPoint(cam.transform.position));
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

        /// <summary>Body coords (Unity, unreflected) -> camera image (u / w, v / h) * depth, for the mask lookup.</summary>
        static Matrix4x4 MaskMatrix(CamInfo ci)
        {
            var K = Matrix4x4.identity;
            K.m00 = ci.fx / ci.w; K.m02 = ci.cx / ci.w;
            K.m11 = ci.fy / ci.h; K.m12 = ci.cy / ci.h;
            // Unity -> Mirror (z flip), Mirror -> camera (inverse of CamToMirror + position), intrinsics
            return K * CamToMirror(ci).transpose * Matrix4x4.Translate(-CamPosMirror(ci)) * Matrix4x4.Scale(new Vector3(1, 1, -1));
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
}
