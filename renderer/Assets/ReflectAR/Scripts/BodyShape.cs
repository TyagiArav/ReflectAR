using UnityEngine;

namespace ReflectAR
{
    /// <summary>
    /// Measures the user's torso from perception's person mask, so garments are cut to their
    /// actual build (broad chest, belly, hips) instead of a generic body scaled by shoulder width.
    ///
    /// At a few heights between the hips and the shoulders it walks the mask outward from the
    /// torso's centre line, along the body's left-right axis, until it leaves the person: that is
    /// the silhouette half-width there. A side is skipped while the arm touches that edge (the
    /// silhouette would include the arm), and the whole sample while the user isn't facing the
    /// camera. The ratio to the generic model (RestSkeleton.BodyModel) is averaged over ~1.5 s
    /// into BodyRig.Profile. Whatever the user is wearing counts as body, which is what a
    /// garment drawn over it has to cover.
    /// </summary>
    public class BodyShape
    {
        /// <summary>Measurement heights, as fractions of hip-joint -> shoulder-joint length.</summary>
        public static readonly float[] Levels = { -0.05f, 0.12f, 0.28f, 0.44f, 0.58f, 0.72f };

        public float timeConstant = 1.5f;        // s
        public float maxYawDeg = 25f;
        readonly int[] samples = new int[6];

        /// <summary>Accepted level measurements since the last Reset.</summary>
        public int Samples { get; private set; }

        public void Reset(BodyRig b)
        {
            b.ResetShape();
            System.Array.Clear(samples, 0, samples.Length);
            Samples = 0;
        }

        /// <summary>Profile value at t = height / torso length (clamped at the end levels).</summary>
        public static float ProfileAt(float[] profile, float t)
        {
            var L = Levels;
            if (t <= L[0]) return profile[0];
            for (int i = 1; i < L.Length; i++)
                if (t <= L[i]) return Mathf.Lerp(profile[i - 1], profile[i], (t - L[i - 1]) / (L[i] - L[i - 1]));
            return profile[L.Length - 1];
        }

        /// <param name="mask">person mask, row 0 = image top, 0..255</param>
        /// <param name="bodyToImage">body coords -> (u, v, 1) * depth, (u, v) in [0, 1] of the image, v down</param>
        /// <param name="J">the pose when the mask was captured (body coords)</param>
        /// <param name="camPos">camera position (body coords)</param>
        /// <param name="dt">time since the previous call (s)</param>
        public void Measure(byte[] mask, int mw, int mh, Matrix4x4 bodyToImage, Vector3[] J, BodyRig b, Vector3 camPos, float dt)
        {
            if (mask == null || mw <= 0 || mh <= 0 || mask.Length < mw * mh) return;
            if (b.Vis[LM.LShoulder] < 0.6f || b.Vis[LM.RShoulder] < 0.6f || b.Vis[LM.LHip] < 0.5f || b.Vis[LM.RHip] < 0.5f) return;
            Vector3 hipC = (J[LM.LHip] + J[LM.RHip]) * 0.5f, shC = (J[LM.LShoulder] + J[LM.RShoulder]) * 0.5f;
            float L = Vector3.Distance(hipC, shC);
            if (L < 0.2f) return;
            Vector3 up = (shC - hipC) / L;
            Vector3 chestR = Vector3.ProjectOnPlane(J[LM.RShoulder] - J[LM.LShoulder], up).normalized;
            Vector3 pelvisR = Vector3.ProjectOnPlane(J[LM.RHip] - J[LM.LHip], up).normalized;
            if (pelvisR.sqrMagnitude < 0.5f) pelvisR = chestR;
            Vector3 fwd = Vector3.Cross(chestR, up);
            if (Mathf.Abs(Vector3.Dot(fwd, (camPos - shC).normalized)) < Mathf.Cos(maxYawDeg * Mathf.Deg2Rad)) return;

            float u = b.Unit, rate = 1f - Mathf.Exp(-dt / timeConstant);
            for (int k = 0; k < Levels.Length; k++)
            {
                float t = Levels[k];
                // same left-right axis as the cloth colliders: pelvis at the hips, chest above
                Vector3 right = Vector3.Slerp(pelvisR, chestR, Mathf.SmoothStep(0f, 1f, (t - 0.1f) / 0.7f)).normalized;
                Vector3 c = hipC + up * (t * L);
                float sum = 0f;
                int n = 0;
                for (int side = -1; side <= 1; side += 2)
                {
                    Vector3 dir = right * side;
                    float edge = ScanEdge(mask, mw, mh, bodyToImage, c, dir, 0.45f * u);
                    if (edge <= 0f || ArmTouches(J, side > 0, hipC, up, c, dir, t * L, edge, u)) continue;
                    sum += edge;
                    n++;
                }
                if (n == 0) continue;
                RestSkeleton.BodyModel(t, b.HipW, b.ShoulderW, u, out float a, out _);
                float r = sum / n / a;
                if (r < 0.7f || r > 1.6f) continue;
                if (samples[k] > 15 && Mathf.Abs(r - b.Profile[k]) > 0.2f) continue;     // a bag, a loose sleeve, ...
                b.Profile[k] = samples[k] == 0 ? r : Mathf.Lerp(b.Profile[k], r, rate);
                samples[k]++;
                Samples++;
            }
        }

        /// <summary>Distance (m) from c along dir to where the mask stops being the person, or -1
        /// if it never does or the line leaves the image.</summary>
        static float ScanEdge(byte[] mask, int mw, int mh, Matrix4x4 M, Vector3 c, Vector3 dir, float maxLen)
        {
            if (!Project(M, c, out Vector2 p0) || !Project(M, c + dir * maxLen, out Vector2 p1)) return -1f;
            p0.x *= mw; p0.y *= mh; p1.x *= mw; p1.y *= mh;
            int steps = Mathf.CeilToInt(Vector2.Distance(p0, p1) * 2f);         // half-pixel steps
            if (steps < 4 || Sample(mask, mw, mh, p0) < 128) return -1f;
            for (int i = 1; i <= steps; i++)
            {
                int m = Sample(mask, mw, mh, Vector2.Lerp(p0, p1, i / (float)steps));
                if (m < 0) return -1f;
                if (m < 128) return maxLen * (i - 0.5f) / steps;
            }
            return -1f;
        }

        /// <summary>Whether this side's arm (or hand) is at this height and within a few cm of the
        /// silhouette edge, so the edge may be the arm's.</summary>
        static bool ArmTouches(Vector3[] J, bool right, Vector3 hipC, Vector3 up, Vector3 c, Vector3 dir, float h, float edge, float u)
        {
            int s = right ? LM.RShoulder : LM.LShoulder, e = right ? LM.RElbow : LM.LElbow;
            int w = right ? LM.RWrist : LM.LWrist, k = right ? LM.RIndex : LM.LIndex;
            return Near(J[s], J[e]) || Near(J[e], J[w]) || Near(J[w], J[k]);

            bool Near(Vector3 a, Vector3 b)
            {
                float ha = Vector3.Dot(a - hipC, up) - h, hb = Vector3.Dot(b - hipC, up) - h, margin = 0.06f * u;
                if (Mathf.Min(ha, hb) > margin || Mathf.Max(ha, hb) < -margin) return false;
                float t = Mathf.Abs(ha - hb) > 1e-4f ? Mathf.Clamp01(ha / (ha - hb)) : 0.5f;
                float lat = Vector3.Dot(Vector3.Lerp(a, b, t) - c, dir), ra = 0.055f * u;
                return lat + ra > edge - 0.015f && lat - ra < edge + 0.03f;
            }
        }

        static bool Project(Matrix4x4 M, Vector3 p, out Vector2 uv)
        {
            Vector3 q = M.MultiplyPoint3x4(p);
            uv = q.z > 1e-3f ? new Vector2(q.x / q.z, q.y / q.z) : Vector2.zero;
            return q.z > 1e-3f;
        }

        static int Sample(byte[] mask, int mw, int mh, Vector2 p)
        {
            int x = Mathf.FloorToInt(p.x), y = Mathf.FloorToInt(p.y);
            return x < 0 || y < 0 || x >= mw || y >= mh ? -1 : mask[y * mw + x];
        }
    }
}
