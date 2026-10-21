using UnityEngine;

namespace ReflectAR
{
    /// <summary>
    /// Depth-only stand-ins for the user's body (occlusion prepass), built from the skeleton.
    ///
    /// Parts under the garment (torso, neck, sleeved arm) are fitted capsules that hide the far side
    /// of the garment. Exposed parts (bare forearms, hands with fingers, head with hair) are drawn
    /// oversized and trimmed in the shader to perception's person mask, so their outline follows
    /// the real silhouette instead of a capsule's. They are pushed back in depth by however much
    /// they were enlarged, so their front surface stays at the real body surface and can't poke
    /// through sleeves or the collar.
    /// </summary>
    public class BodyProxy
    {
        // index layout
        const int Torso = 0, Neck = 1, Legs = 2,            // 4 leg capsules
            Covered = 6,                                    // 2 per arm: sleeved segments
            Exposed = 10,                                   // 2 per arm: bare segments
            Hands = 14, Thumbs = 16, Head = 18, Count = 19;

        // extra radius of exposed parts (m, for a 0.36 m shoulder span); the mask trims it back
        const float ArmMargin = 0.03f, HandMargin = 0.035f, HeadMargin = 0.05f;

        readonly Transform[] parts = new Transform[Count];
        readonly MeshRenderer[] renderers = new MeshRenderer[Count];
        readonly MaterialPropertyBlock mpb = new MaterialPropertyBlock();
        readonly Material mat;
        static readonly int UseMaskId = Shader.PropertyToID("_UseMask"), PushId = Shader.PropertyToID("_DepthPush"),
            OffsetId = Shader.PropertyToID("_MaskOffset");
        static readonly (int a, int b, float r)[] LegSegs =
        {
            (LM.LHip, LM.LKnee, 0.07f), (LM.RHip, LM.RKnee, 0.07f), (LM.LKnee, LM.LAnkle, 0.055f), (LM.RKnee, LM.RAnkle, 0.055f),
        };

        /// <param name="castShadows">HDRP: hands, arms and head shadow the garment</param>
        public BodyProxy(Transform parent, Material m, bool castShadows)
        {
            mat = m;
            var root = new GameObject("BodyProxy").transform;
            root.SetParent(parent, false);
            for (int i = 0; i < Count; i++)
            {
                var go = GameObject.CreatePrimitive(i == Head ? PrimitiveType.Sphere : PrimitiveType.Capsule);
                Object.Destroy(go.GetComponent<Collider>());
                renderers[i] = go.GetComponent<MeshRenderer>();
                renderers[i].sharedMaterial = mat;
                renderers[i].shadowCastingMode = castShadows ? UnityEngine.Rendering.ShadowCastingMode.On : UnityEngine.Rendering.ShadowCastingMode.Off;
                go.transform.SetParent(root, false);
                parts[i] = go.transform;
            }
        }

        static void Place(Transform t, Vector3 a, Vector3 b, float rx, float rz, Vector3 fwdHint)
        {
            Vector3 d = b - a;
            float len = d.magnitude;
            if (len < 1e-4f || rx <= 0f) { t.localScale = Vector3.zero; return; }
            Vector3 up = d / len;
            Vector3 f = Vector3.ProjectOnPlane(fwdHint, up);
            if (f.sqrMagnitude < 1e-4f) f = Vector3.ProjectOnPlane(Vector3.forward, up);
            t.localPosition = (a + b) * 0.5f;
            t.localRotation = Quaternion.LookRotation(f.normalized, up);
            t.localScale = new Vector3(rx * 2f, (len + 2f * Mathf.Min(rx, rz)) * 0.5f, rz * 2f);
        }

        static void Hide(Transform t) => t.localScale = Vector3.zero;

        /// <param name="sleeve">fraction of the arm the current garment covers (GarmentSpec.sleeve)</param>
        /// <param name="maskJoints">the pose when the person mask was captured, or null if there is no
        /// current mask; each trimmed part looks the mask up where it was at that moment</param>
        public void Update(BodyRig b, bool on, bool debugVisible, float sleeve, Vector3[] maskJoints)
        {
            parts[0].parent.gameObject.SetActive(on);
            if (!on) return;
            mat.SetFloat("_ColorMask", debugVisible ? 15f : 0f);
            float u = b.Unit;
            var J = b.J;
            bool masked = maskJoints != null;
            float g = masked ? u : 0f;              // without a mask, oversizing would cut holes around the body

            // ---- under the garment: fitted, never trimmed
            Place(parts[Torso], b.HipC, b.ShC, b.ShoulderW * 0.42f, 0.095f * u, b.Fwd);
            Place(parts[Neck], b.ShC, b.HeadC - b.Up * 0.05f * u, 0.05f * u, 0.05f * u, b.Fwd);
            for (int i = 0; i < LegSegs.Length; i++)
                Place(parts[Legs + i], J[LegSegs[i].a], J[LegSegs[i].b], LegSegs[i].r * u, LegSegs[i].r * u, b.Fwd);

            for (int s = 0; s < 2; s++)
            {
                bool left = s == 0;
                int si = left ? LM.LShoulder : LM.RShoulder, ei = left ? LM.LElbow : LM.RElbow, wi = left ? LM.LWrist : LM.RWrist;
                int ii = left ? LM.LIndex : LM.RIndex, pi = left ? LM.LPinky : LM.RPinky, ti = left ? 21 : 22;
                Vector3 S = J[si], E = J[ei], W = J[wi];
                float L1 = Vector3.Distance(S, E), L2 = Vector3.Distance(E, W);
                float rUpper = 0.045f * u, rFore = 0.04f * u, m = ArmMargin * g;

                // arm: covered from the shoulder to the cuff, bare (oversized + trimmed) after it
                float cuff = Mathf.Clamp01(sleeve) * (L1 + L2);
                float bare = cuff + (cuff > 0f ? 0.02f * u + rFore + m : 0.06f * u);   // clear of the cuff / armhole
                Vector3 P(float x) => x <= L1 ? Vector3.Lerp(S, E, x / Mathf.Max(L1, 1e-4f)) : Vector3.Lerp(E, W, (x - L1) / Mathf.Max(L2, 1e-4f));

                if (cuff > 0f) Place(parts[Covered + 2 * s], S, P(Mathf.Min(cuff, L1)), rUpper, rUpper, b.Fwd); else Hide(parts[Covered + 2 * s]);
                if (cuff > L1) Place(parts[Covered + 2 * s + 1], E, P(cuff), rFore, rFore, b.Fwd); else Hide(parts[Covered + 2 * s + 1]);

                int x0 = Exposed + 2 * s, x1 = x0 + 1;
                if (bare < L1)
                {
                    Place(parts[x0], P(bare), E, rUpper + m, rUpper + m, b.Fwd);
                    Place(parts[x1], E, W, rFore + m, rFore + m, b.Fwd);
                }
                else if (bare < L1 + L2)
                {
                    Place(parts[x0], P(bare), W, rFore + m, rFore + m, b.Fwd);
                    Hide(parts[x1]);
                }
                else { Hide(parts[x0]); Hide(parts[x1]); }
                Props(x0, masked, m, Offset(maskJoints, J, ei, wi));
                Props(x1, masked, m, Offset(maskJoints, J, ei, wi));

                // hand: wrist -> past the knuckles to cover the fingers; thumb separately
                Vector3 K = (J[ii] + J[pi]) * 0.5f;
                Vector3 dir = K - W;
                float dl = dir.magnitude;
                dir = dl > 1e-4f ? dir / dl : b.Up;
                float rHand = Mathf.Clamp(Vector3.Distance(J[ii], J[pi]) * 0.6f, 0.03f * u, 0.05f * u), mh = HandMargin * g;
                Vector3 h0 = W + dir * (sleeve > 0.9f ? rHand + mh : 0f);          // long sleeves: clear of the cuff
                Place(parts[Hands + s], h0, K + dir * 0.07f * u, rHand + mh, 0.6f * rHand + mh, Vector3.Cross(dir, b.Right));
                Props(Hands + s, masked, mh, Offset(maskJoints, J, wi, ii));
                Vector3 T = J[ti];
                Vector3 td = (T - W).sqrMagnitude > 1e-6f ? (T - W).normalized : dir;
                Place(parts[Thumbs + s], W + td * 0.02f * u, T + td * 0.035f * u, 0.018f * u + mh * 0.6f, 0.018f * u + mh * 0.6f, b.Fwd);
                Props(Thumbs + s, masked, mh * 0.6f, Offset(maskJoints, J, wi, ti));
            }

            // ---- head, with room for hair
            float mhd = HeadMargin * g;
            parts[Head].localPosition = b.HeadC + b.Up * 0.02f * u;
            parts[Head].localRotation = b.ChestRot;
            parts[Head].localScale = new Vector3(b.HeadR * 1.9f + 2f * mhd, b.HeadR * 2.3f + 2f * mhd, b.HeadR * 2.1f + 2f * mhd);
            Props(Head, masked, mhd, Offset(maskJoints, J, LM.LEar, LM.REar));
            for (int i = 0; i < Exposed; i++) Props(i, false, 0f, Vector3.zero);
        }

        /// <summary>How far a part has moved since the mask was captured (mean of two joints).</summary>
        static Vector3 Offset(Vector3[] maskJ, Vector3[] J, int a, int b) =>
            maskJ == null ? Vector3.zero : ((J[a] - maskJ[a]) + (J[b] - maskJ[b])) * 0.5f;

        void Props(int i, bool trim, float push, Vector3 offset)
        {
            mpb.SetFloat(UseMaskId, trim ? 1f : 0f);
            mpb.SetFloat(PushId, push);
            mpb.SetVector(OffsetId, offset);
            renderers[i].SetPropertyBlock(mpb);
        }
    }
}
