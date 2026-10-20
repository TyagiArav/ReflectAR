using System;
using System.Collections.Concurrent;
using System.Net.Sockets;
using System.Threading;
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

    /// <summary>One perception frame. Coordinates are in the Mirror frame (metres, right-handed).</summary>
    [Serializable]
    public class TrackingFrame
    {
        public double t;
        public int seq;
        public bool tracked;
        public string range = "none";
        public float dist;
        public string[] events;
        public float fist_progress;
        public string fist_hand;
        public float[] joints;   // 33 * 3
        public float[] vis;      // 33
        public float[] eye;      // 3
        public CamInfo cam;
        public ScreenInfo screen;
        public float proc_fps, depth_hz, scale, z_skel, z_da, lat_ms, cam_fps;
    }

    /// <summary>
    /// Connects to perception/server.py over TCP and exposes the newest frame.
    /// Mirror -> Unity conversion (Unity z = -Mirror z) happens here and only here.
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

        public readonly Vector3[] Joints = new Vector3[33];   // Unity coords, real (unreflected) body
        public readonly float[] Visibility = new float[33];
        public Vector3 Eye;

        volatile bool connected, running = true;
        Thread thread;
        readonly object gate = new object();
        TrackingFrame pendingFrame;
        byte[] pendingJpeg;

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
                            if (header[0] == (byte)'J')
                            {
                                var f = JsonUtility.FromJson<TrackingFrame>(System.Text.Encoding.UTF8.GetString(body));
                                if (f.events != null) foreach (var e in f.events) Events.Enqueue(e);
                                lock (gate) pendingFrame = f;
                            }
                            else if (header[0] == (byte)'I')
                            {
                                lock (gate) pendingJpeg = body;
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

        void Update()
        {
            TrackingFrame f;
            byte[] jpg;
            lock (gate)
            {
                f = pendingFrame; pendingFrame = null;
                jpg = pendingJpeg; pendingJpeg = null;
            }
            if (jpg != null) Feed.LoadImage(jpg, false);
            if (f == null) return;
            Latest = f;
            LastFrameTime = Time.time;
            if (f.tracked && f.joints != null && f.joints.Length >= 99)
            {
                for (int i = 0; i < 33; i++)
                {
                    Joints[i] = MirrorToUnity(f.joints[3 * i], f.joints[3 * i + 1], f.joints[3 * i + 2]);
                    Visibility[i] = f.vis[i];
                }
                Eye = MirrorToUnity(f.eye[0], f.eye[1], f.eye[2]);
            }
        }
    }
}
