using System;
using System.Diagnostics;
using System.Net.Sockets;
using System.Threading;
using System.Collections.Concurrent;
using UnityEngine;

namespace ReflectAR
{
    [Serializable]
    public class CamInfo
    {
        public float fx, fy, cx, cy, w, h, tilt;
        public float[] pos;
    }

    [Serializable]
    public class ScreenInfo
    {
        public float w_m = 0.302f, h_m = 0.196f;
    }

    /// <summary>Prediction tuning, from the "predict" block of perception/calib/config.json.</summary>
    [Serializable]
    public class PredictInfo
    {
        public float display_latency_s = 0.025f, horizon_tau_s = 0.35f, max_horizon_s = 0.25f, blend_tc_s = 0.015f;
    }

    /// <summary>One perception frame. Coordinates are in the Mirror frame (metres, right-handed).</summary>
    [Serializable]
    public class TrackingFrame
    {
        public double t, t_send;  // capture / send time, perception clock (s)
        public int seq;
        public bool tracked;
        public string range = "none";
        public float dist;
        public string[] events;
        public float fist_progress;
        public string fist_hand;
        public float[] joints;   // 33 * 3, filtered, at time t
        public float[] vel;      // 33 * 3, m/s
        public float[] vis;      // 33
        public float[] eye;      // 3
        public float[] eye_vel;  // 3
        public CamInfo cam;
        public ScreenInfo screen;
        public PredictInfo predict;
        public float proc_fps, proc_ms, depth_hz, scale, z_skel, z_da;
        [NonSerialized] public double arrival;   // local clock
    }

    /// <summary>
    /// Connects to perception/server.py over TCP and predicts the pose to any render time.
    /// Mirror -> Unity conversion (Unity z = -Mirror z) happens here and only here.
    ///
    /// Perception sends each frame's joints with velocities at the frame's capture time. Predict()
    /// extrapolates them to when the rendered frame will be on screen, so the overlay lands on
    /// the user's current pose rather than one from ~80 ms ago, and moves every render frame
    /// instead of in 30 Hz steps.
    /// </summary>
    public class TrackingClient : MonoBehaviour
    {
        public string host = "127.0.0.1";
        public int port = 5566;

        public TrackingFrame Latest { get; private set; }
        public float LastFrameTime { get; private set; } = -100f;
        public bool Connected => connected;
        public Texture2D Feed { get; private set; }
        public readonly ConcurrentQueue<string> Events = new ConcurrentQueue<string>();

        public readonly Vector3[] Joints = new Vector3[33];   // Unity coords, real (unreflected) body, predicted
        public readonly float[] Visibility = new float[33];
        public Vector3 Eye;

        /// <summary>Capture -> arrival of the latest frame (ms), and the last prediction horizon (ms).</summary>
        public float LatencyMs { get; private set; }
        public float HorizonMs { get; private set; }
        /// <summary>Capture time of the camera image now in <see cref="Feed"/>, local clock.</summary>
        public double FeedCaptureTime { get; private set; } = double.NegativeInfinity;
        /// <summary>Person segmentation in camera-image coordinates (R8, v = 0 at the image top).</summary>
        public Texture2D Mask { get; private set; }
        /// <summary>The same mask on the CPU (row 0 = image top), for measuring the body's silhouette.</summary>
        public byte[] MaskBytes { get; private set; }
        public int MaskWidth => Mask != null ? Mask.width : 0;
        public int MaskHeight => Mask != null ? Mask.height : 0;
        public double MaskCaptureTime { get; private set; } = double.NegativeInfinity;

        /// <summary>Monotonic local clock (s), usable from any thread.</summary>
        public static double Now => Stopwatch.GetTimestamp() / (double)Stopwatch.Frequency;

        volatile bool connected, running = true;
        Thread thread;
        readonly object gate = new object();
        TrackingFrame pendingFrame;
        byte[] pendingJpeg, pendingMask;
        double pendingJpegT, pendingMaskT;
        int pendingMaskW, pendingMaskH;

        // perception clock -> local clock: offset = min over recent frames of (arrival - t_send)
        readonly double[] offsets = new double[64];
        int offsetCount, offsetHead;
        double clockOffset;

        // prediction state, Unity coords
        readonly Vector3[] basePos = new Vector3[33], baseVel = new Vector3[33], corr = new Vector3[33];
        Vector3 eyePos, eyeVel, eyeCorr;
        double captureTime, corrStart, lastTarget;
        bool hasState;
        PredictInfo tuning = new PredictInfo();

        public static Vector3 MirrorToUnity(float x, float y, float z) => new Vector3(x, y, -z);

        void OnEnable()
        {
            Feed = new Texture2D(2, 2, TextureFormat.RGB24, false) { wrapMode = TextureWrapMode.Clamp };
            thread = new Thread(Run) { IsBackground = true, Name = "ReflectAR.Tracking" };
            thread.Start();
        }

        void OnDisable()
        {
            running = false;
            thread?.Join(500);
        }

        void Run()
        {
            var header = new byte[5];
            while (running)
            {
                try
                {
                    using (var client = new TcpClient())
                    {
                        client.NoDelay = true;
                        client.Connect(host, port);
                        client.ReceiveTimeout = 3000;
                        connected = true;
                        var s = client.GetStream();
                        while (running)
                        {
                            ReadExactly(s, header, 5);
                            int len = (header[1] << 24) | (header[2] << 16) | (header[3] << 8) | header[4];
                            var body = new byte[len];
                            ReadExactly(s, body, len);
                            double arrival = Now;
                            if (header[0] == (byte)'J')
                            {
                                var f = JsonUtility.FromJson<TrackingFrame>(System.Text.Encoding.UTF8.GetString(body));
                                f.arrival = arrival;
                                if (f.events != null) foreach (var e in f.events) Events.Enqueue(e);
                                lock (gate) pendingFrame = f;
                            }
                            else if (header[0] == (byte)'I' && len > 8)
                            {
                                // 8-byte big-endian capture time, then the JPEG
                                var tb = new byte[8];
                                Buffer.BlockCopy(body, 0, tb, 0, 8);
                                if (BitConverter.IsLittleEndian) Array.Reverse(tb);
                                var jpg = new byte[len - 8];
                                Buffer.BlockCopy(body, 8, jpg, 0, len - 8);
                                lock (gate) { pendingJpeg = jpg; pendingJpegT = BitConverter.ToDouble(tb, 0); }
                            }
                            else if (header[0] == (byte)'M' && len > 12)
                            {
                                // capture time (float64), width, height (uint16), then uint8 rows, top first
                                var tb = new byte[8];
                                Buffer.BlockCopy(body, 0, tb, 0, 8);
                                if (BitConverter.IsLittleEndian) Array.Reverse(tb);
                                int w = (body[8] << 8) | body[9], h = (body[10] << 8) | body[11];
                                if (w * h != len - 12) continue;
                                var m = new byte[w * h];
                                Buffer.BlockCopy(body, 12, m, 0, w * h);
                                lock (gate) { pendingMask = m; pendingMaskT = BitConverter.ToDouble(tb, 0); pendingMaskW = w; pendingMaskH = h; }
                            }
                        }
                    }
                }
                catch (Exception)
                {
                    connected = false;
                    Thread.Sleep(500);
                }
            }
        }

        static void ReadExactly(NetworkStream s, byte[] buf, int n)
        {
            int off = 0;
            while (off < n)
            {
                int r = s.Read(buf, off, n - off);
                if (r <= 0) throw new Exception("socket closed");
                off += r;
            }
        }

        /// <summary>Takes in whatever arrived since the last call. Call once per frame, before Predict.</summary>
        public void Poll()
        {
            TrackingFrame f;
            byte[] jpg, mask;
            double jpgT, maskT;
            int maskW, maskH;
            lock (gate)
            {
                f = pendingFrame; pendingFrame = null;
                jpg = pendingJpeg; pendingJpeg = null;
                jpgT = pendingJpegT;
                mask = pendingMask; pendingMask = null;
                maskT = pendingMaskT; maskW = pendingMaskW; maskH = pendingMaskH;
            }
            if (f != null)
            {
                // Same machine: transit is ~0.1 ms, so the smallest (arrival - send) is the clock offset.
                offsets[offsetHead] = f.arrival - f.t_send;
                offsetHead = (offsetHead + 1) % offsets.Length;
                offsetCount = Math.Min(offsetCount + 1, offsets.Length);
                clockOffset = double.MaxValue;
                for (int i = 0; i < offsetCount; i++) clockOffset = Math.Min(clockOffset, offsets[i]);

                Latest = f;
                LastFrameTime = Time.time;
                if (f.predict != null) tuning = f.predict;
                LatencyMs = (float)((f.arrival - (f.t + clockOffset)) * 1000.0);
                if (f.tracked && f.joints != null && f.joints.Length >= 99) Ingest(f);
            }
            if (jpg != null && offsetCount > 0)
            {
                Feed.LoadImage(jpg, false);
                FeedCaptureTime = jpgT + clockOffset;
            }
            if (mask != null && offsetCount > 0)
            {
                if (Mask == null || Mask.width != maskW || Mask.height != maskH)
                    Mask = new Texture2D(maskW, maskH, TextureFormat.R8, false)
                        { wrapMode = TextureWrapMode.Clamp, filterMode = FilterMode.Bilinear };
                Mask.LoadRawTextureData(mask);
                Mask.Apply(false);
                MaskBytes = mask;
                MaskCaptureTime = maskT + clockOffset;
            }
        }

        void Ingest(TrackingFrame f)
        {
            double now = Now;
            double at = lastTarget > 0 ? lastTarget : now;
            bool blendable = hasState && now - captureTime < 0.5;
            // what was being shown, so the hand-over to the new frame can be eased (see Predict)
            for (int i = 0; i < 33 && blendable; i++) corr[i] = Eval(basePos[i], baseVel[i], corr[i], at, now);
            if (blendable) eyeCorr = Eval(eyePos, eyeVel, eyeCorr, at, now);

            bool hasVel = f.vel != null && f.vel.Length >= 99;
            for (int i = 0; i < 33; i++)
            {
                basePos[i] = MirrorToUnity(f.joints[3 * i], f.joints[3 * i + 1], f.joints[3 * i + 2]);
                baseVel[i] = hasVel ? MirrorToUnity(f.vel[3 * i], f.vel[3 * i + 1], f.vel[3 * i + 2]) : Vector3.zero;
                Visibility[i] = f.vis[i];
            }
            eyePos = MirrorToUnity(f.eye[0], f.eye[1], f.eye[2]);
            eyeVel = f.eye_vel != null && f.eye_vel.Length == 3 ? MirrorToUnity(f.eye_vel[0], f.eye_vel[1], f.eye_vel[2]) : Vector3.zero;
            captureTime = f.t + clockOffset;
            hasState = true;

            float h = Horizon(at);
            for (int i = 0; i < 33; i++)
            {
                corr[i] = blendable ? corr[i] - Extrapolate(basePos[i], baseVel[i], h) : Vector3.zero;
                if (corr[i].sqrMagnitude > 0.3f * 0.3f) corr[i] = Vector3.zero;    // re-detection: snap
            }
            eyeCorr = blendable ? eyeCorr - Extrapolate(eyePos, eyeVel, h) : Vector3.zero;
            if (eyeCorr.sqrMagnitude > 0.3f * 0.3f) eyeCorr = Vector3.zero;
            corrStart = now;
        }

        float Horizon(double target) => Mathf.Clamp((float)(target - captureTime), -0.1f, tuning.max_horizon_s);

        Vector3 Extrapolate(Vector3 p, Vector3 v, float h)
        {
            // damped: long horizons extrapolate less than linearly, so stops overshoot less
            float tau = tuning.horizon_tau_s;
            float k = tau > 0f && h > 0f ? tau * (1f - Mathf.Exp(-h / tau)) : h;
            return p + v * k;
        }

        Vector3 Eval(Vector3 p, Vector3 v, Vector3 c, double target, double now)
        {
            float decay = tuning.blend_tc_s > 0f ? Mathf.Exp(-(float)(now - corrStart) / tuning.blend_tc_s) : 0f;
            return Extrapolate(p, v, Horizon(target)) + c * decay;
        }

        /// <summary>
        /// Fills <see cref="Joints"/> and <see cref="Eye"/> with the pose at local time
        /// <paramref name="target"/>. extrapolate = false shows the filtered pose at capture time
        /// (for A/B comparison); blend eases each new frame in over blend_tc_s instead of jumping.
        /// </summary>
        public void Predict(double target, bool extrapolate, bool blend)
        {
            if (!hasState) return;
            if (!extrapolate) target = captureTime;
            lastTarget = target;
            double now = Now;
            HorizonMs = Horizon(target) * 1000f;
            for (int i = 0; i < 33; i++)
                Joints[i] = blend ? Eval(basePos[i], baseVel[i], corr[i], target, now) : Extrapolate(basePos[i], baseVel[i], Horizon(target));
            Eye = blend ? Eval(eyePos, eyeVel, eyeCorr, target, now) : Extrapolate(eyePos, eyeVel, Horizon(target));
        }

        /// <summary>The pose at local time <paramref name="t"/> without the hand-over easing, e.g. at the
        /// mask's capture time, to see how far each joint has moved since then.</summary>
        public void PoseAt(double t, Vector3[] joints)
        {
            if (!hasState) return;
            float h = Horizon(t);
            for (int i = 0; i < 33; i++) joints[i] = Extrapolate(basePos[i], baseVel[i], h);
        }

        public float DisplayLatency => tuning.display_latency_s;
    }
}
