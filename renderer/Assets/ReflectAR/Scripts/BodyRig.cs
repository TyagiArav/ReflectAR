using UnityEngine;

namespace ReflectAR
{
    /// <summary>BlazePose landmark indices used by the renderer.</summary>
    public static class LM
    {
        public const int Nose = 0, LEye = 2, REye = 5, LEar = 7, REar = 8,
            LShoulder = 11, RShoulder = 12, LElbow = 13, RElbow = 14, LWrist = 15, RWrist = 16,
            LPinky = 17, RPinky = 18, LIndex = 19, RIndex = 20,
            LHip = 23, RHip = 24, LKnee = 25, RKnee = 26, LAnkle = 27, RAnkle = 28;
    }

    public struct Capsule
    {
        public Vector3 a, b;
        public float r;
        public Capsule(Vector3 a, Vector3 b, float r) { this.a = a; this.b = b; this.r = r; }

        public Vector3 Closest(Vector3 p)
        {
            Vector3 ab = b - a;
            float t = Mathf.Clamp01(Vector3.Dot(p - a, ab) / Mathf.Max(ab.sqrMagnitude, 1e-6f));
            return a + ab * t;
        }
    }

    /// <summary>
    /// Smoothed skeleton plus derived body frames and measurements, in Unity coordinates of the
    /// real (unreflected) body. Garments are built from this; reflection is a parent transform.
    /// </summary>
    public class BodyRig
    {
        public readonly Vector3[] J = new Vector3[33];
        public readonly float[] Vis = new float[33];
        public bool Valid;

        public Vector3 HipC, ShC, NeckBase, HeadC;
        public Vector3 Right, Up, Fwd;             // chest frame (Right = user's right)
        public Vector3 PRight, PFwd;               // pelvis frame (shares Up)
        public float ShoulderW = 0.36f, HipW = 0.22f, TorsoLen = 0.5f, HeadR = 0.1f;
        public float ArmLenL = 0.55f, ArmLenR = 0.55f;
        public float UpperL = 0.29f, UpperR = 0.29f, ForeL = 0.26f, ForeR = 0.26f;
        public Capsule[] Colliders = new Capsule[0];

        /// <summary>Body-relative unit: 1 for a 0.36 m shoulder-joint span. Garment and proxy
        /// dimensions scale with it, so the overlay fits even if absolute distance is off.</summary>
        public float Unit => ShoulderW / 0.36f;

        public Quaternion ChestRot => Quaternion.LookRotation(Fwd, Up);
        public Quaternion PelvisRot => Quaternion.LookRotation(PFwd, Up);

        public float smoothing = 30f;               // 1/s, extra interpolation between 30 Hz frames
        bool measured;

        public void Update(Vector3[] target, float[] vis, float dt, bool snap)
        {
            float a = snap ? 1f : 1f - Mathf.Exp(-smoothing * dt);
            for (int i = 0; i < 33; i++)
            {
                J[i] = Vector3.Lerp(J[i], target[i], a);
                Vis[i] = vis[i];
            }
            Derive(snap || !measured);
            Valid = true;
        }

        void Derive(bool resetMeasures)
        {
            HipC = (J[LM.LHip] + J[LM.RHip]) * 0.5f;
            ShC = (J[LM.LShoulder] + J[LM.RShoulder]) * 0.5f;
            Up = (ShC - HipC).normalized;
            Right = Vector3.ProjectOnPlane(J[LM.RShoulder] - J[LM.LShoulder], Up).normalized;
            Fwd = Vector3.Cross(Right, Up);
            PRight = Vector3.ProjectOnPlane(J[LM.RHip] - J[LM.LHip], Up).normalized;
            if (PRight.sqrMagnitude < 0.5f) PRight = Right;
            PFwd = Vector3.Cross(PRight, Up);

            // Body measurements change slowly; smooth hard so garments don't breathe.
            float k = resetMeasures ? 1f : 0.03f;
            ShoulderW = Mathf.Lerp(ShoulderW, Mathf.Clamp(Vector3.Distance(J[LM.LShoulder], J[LM.RShoulder]), 0.25f, 0.55f), k);
            HipW = Mathf.Lerp(HipW, Mathf.Clamp(Vector3.Distance(J[LM.LHip], J[LM.RHip]), 0.15f, 0.40f), k);
            TorsoLen = Mathf.Lerp(TorsoLen, Mathf.Clamp(Vector3.Distance(HipC, ShC), 0.35f, 0.75f), k);
            float earW = Vector3.Distance(J[LM.LEar], J[LM.REar]);
            HeadR = Mathf.Lerp(HeadR, Mathf.Clamp(earW * 0.62f, 0.075f, 0.13f), k);
            ArmLenL = Mathf.Lerp(ArmLenL, Seg(LM.LShoulder, LM.LElbow) + Seg(LM.LElbow, LM.LWrist), k);
            ArmLenR = Mathf.Lerp(ArmLenR, Seg(LM.RShoulder, LM.RElbow) + Seg(LM.RElbow, LM.RWrist), k);
            UpperL = Mathf.Lerp(UpperL, Mathf.Clamp(Seg(LM.LShoulder, LM.LElbow), 0.18f, 0.40f), k);
            UpperR = Mathf.Lerp(UpperR, Mathf.Clamp(Seg(LM.RShoulder, LM.RElbow), 0.18f, 0.40f), k);
            ForeL = Mathf.Lerp(ForeL, Mathf.Clamp(Seg(LM.LElbow, LM.LWrist), 0.16f, 0.36f), k);
            ForeR = Mathf.Lerp(ForeR, Mathf.Clamp(Seg(LM.RElbow, LM.RWrist), 0.16f, 0.36f), k);
            measured = true;

            NeckBase = ShC + (Up * 0.03f - Fwd * 0.01f) * Unit;
            HeadC = (J[LM.LEar] + J[LM.REar]) * 0.5f;

            Colliders = new[]
            {
                new Capsule(HipC + Up * 0.05f, ShC - Up * 0.05f, 0.12f * Unit),               // torso
                new Capsule(J[LM.LHip], J[LM.LKnee], 0.085f * Unit),
                new Capsule(J[LM.RHip], J[LM.RKnee], 0.085f * Unit),
                new Capsule(J[LM.LKnee], J[LM.LAnkle], 0.065f * Unit),
                new Capsule(J[LM.RKnee], J[LM.RAnkle], 0.065f * Unit),
                new Capsule(J[LM.LShoulder], J[LM.LElbow], 0.05f * Unit),
                new Capsule(J[LM.RShoulder], J[LM.RElbow], 0.05f * Unit),
                new Capsule(J[LM.LElbow], J[LM.LWrist], 0.045f * Unit),
                new Capsule(J[LM.RElbow], J[LM.RWrist], 0.045f * Unit),
            };
        }

        float Seg(int a, int b) => Vector3.Distance(J[a], J[b]);
    }
}
