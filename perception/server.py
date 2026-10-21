"""ReflectAR perception service.

Webcam -> BlazePose (OpenVINO) + metric mono depth -> 3D joints/eye in the
Mirror frame -> gestures + range check -> TCP stream to Unity.

Wire format (TCP, Python is the server, Unity connects):
    1 byte type ('J' = JSON tracking frame, 'R' = raw RGB camera frame: u16 width, u16 height, pixels)
    4 byte big-endian payload length
    payload

Run:  .venv/bin/python server.py [--snapshot debug.jpg] [--no-image]
"""
import argparse, json, os, socket, struct, threading, time
import numpy as np
import cv2
import openvino as ov

from blazepose import BlazePose
from handpose import HandPose, is_fist
from depth import MonoDepth
from filters import OneEuro
from geometry import Intrinsics, skeleton_distance, CameraToMirror
from gestures import Gestures, SIDES
from silhouette import measure as measure_silhouette

L_EYE, R_EYE = 2, 5
TORSO = (11, 12, 23, 24)


# --------------------------------------------------------------------------- IO
class Camera:
    """Grabs frames on a thread so inference never waits on the driver."""

    def __init__(self, index, w, h, video=None, fps=30):
        self.video = video
        self.cap = cv2.VideoCapture(video) if video else cv2.VideoCapture(index, cv2.CAP_AVFOUNDATION)
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, w)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, h)
        self.cap.set(cv2.CAP_PROP_FPS, fps)
        self.cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)            # never hand us a stale queued frame
        if not self.cap.isOpened():
            raise RuntimeError("Could not open camera. Grant camera access to your terminal app "
                               "(System Settings > Privacy & Security > Camera).")
        self.frame, self.stamp, self.seq = None, 0.0, 0
        self.cond = threading.Condition()
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while True:
            ok, f = self.cap.read()
            if not ok:
                if self.video:                       # loop recordings
                    self.cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                time.sleep(0.01)
                continue
            if self.video:
                time.sleep(1 / 30)
            with self.cond:
                self.frame, self.stamp, self.seq = f, time.time(), self.seq + 1
                self.cond.notify_all()

    def read(self, last_seq):
        with self.cond:
            self.cond.wait_for(lambda: self.seq != last_seq, timeout=1.0)
            return self.frame, self.stamp, self.seq


class Streamer:
    """Single-client TCP server. Keeps only the newest image; JSON events are merged."""

    def __init__(self, host, port):
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind((host, port))
        self.srv.listen(1)
        self.conn = None
        self.lock = threading.Condition()
        self.json_msg = None
        self.jpeg = None
        threading.Thread(target=self._accept, daemon=True).start()
        threading.Thread(target=self._send, daemon=True).start()
        print(f"[net] listening on {host}:{port}")

    @property
    def connected(self):
        return self.conn is not None

    def _accept(self):
        while True:
            c, addr = self.srv.accept()
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print(f"[net] renderer connected from {addr}")
            with self.lock:
                if self.conn:
                    self.conn.close()
                self.conn = c

    def push(self, msg, jpeg=None):
        with self.lock:
            if self.conn is None:
                msg["events"] = []                   # nobody to act on them: don't replay later
            elif self.json_msg is not None:          # unsent: keep its events
                msg["events"] = self.json_msg["events"] + msg["events"]
            self.json_msg = msg
            if jpeg is not None:
                self.jpeg = jpeg
            self.lock.notify()

    def _send(self):
        while True:
            with self.lock:
                self.lock.wait_for(lambda: self.json_msg is not None and self.conn is not None)
                msg, jpg, conn = self.json_msg, self.jpeg, self.conn
                self.json_msg, self.jpeg = None, None
            try:
                if jpg is not None:
                    conn.sendall(b"R" + struct.pack(">I", len(jpg)) + jpg)
                body = json.dumps(msg, separators=(",", ":")).encode()
                conn.sendall(b"J" + struct.pack(">I", len(body)) + body)
            except OSError:
                print("[net] renderer disconnected")
                with self.lock:
                    if self.conn is conn:
                        self.conn = None


# ------------------------------------------------------------------ calibration
DEPTH_CALIB = "calib/depth_calib.json"


def _fit(points, key):
    """z_true = a * z_raw + b: scale only from one point, scale + offset from two or more."""
    raw = np.array([p[key] for p in points]); true = np.array([p["d"] for p in points])
    if len(points) == 1 or np.ptp(raw) < 0.2:
        return float(np.sum(true * raw) / np.sum(raw * raw)), 0.0
    a, b = np.polyfit(raw, true, 1)
    return float(a), float(b)


def load_depth_calib():
    if os.path.exists(DEPTH_CALIB):
        d = json.load(open(DEPTH_CALIB))
        pts = d.get("points")
        if pts is None:                               # v1 file: a single 2-scale measurement
            m = d["measured_at_m"]
            pts = [{"d": m, "skel": m / d["skel_scale"], "da": m / d["da_scale"]}]
        sa, sb = _fit(pts, "skel")
        da, db = _fit(pts, "da")
        return {"skel": (sa, sb), "da": (da, db), "points": pts, "calibrated": True}
    return {"skel": (1.0, 0.0), "da": (1.0, 0.0), "points": [], "calibrated": False}


class DistanceCalibrator:
    """User stands at a tape-measured distance; each run adds one point (re-measuring a distance
    replaces it). Two points at different distances fit scale + offset for each cue."""

    def __init__(self, dist, cal, n=120):
        self.dist, self.n, self.cal, self.skel, self.da = dist, n, cal, [], []
        print(f"[calib] stand {dist:.2f} m from the camera, facing it, arms relaxed. Collecting {n} frames...")

    def add(self, z_skel, z_da):
        if z_da:
            self.skel.append(z_skel); self.da.append(z_da)
        if len(self.skel) >= self.n:
            pts = [p for p in self.cal["points"] if abs(p["d"] - self.dist) > 0.05]
            pts.append({"d": self.dist, "skel": float(np.median(self.skel)), "da": float(np.median(self.da)),
                        "date": time.strftime("%Y-%m-%d %H:%M")})
            json.dump({"points": sorted(pts, key=lambda p: p["d"])}, open(DEPTH_CALIB, "w"), indent=2)
            print(f"[calib] wrote {DEPTH_CALIB}: {pts}")
            print(f"[calib] fit skel={_fit(pts, 'skel')} da={_fit(pts, 'da')}")
            raise SystemExit(0)


# ------------------------------------------------------------------------- main
class RangeCheck:
    def __init__(self, lo, hi, hyst=0.05):
        self.lo, self.hi, self.h, self.state = lo, hi, hyst, "none"

    def __call__(self, d):
        if d is None:
            self.state = "none"
        elif d < self.lo - (self.h if self.state != "too_close" else -self.h):
            self.state = "too_close"
        elif d > self.hi + (self.h if self.state != "too_far" else -self.h):
            self.state = "too_far"
        else:
            self.state = "ok"
        return self.state


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="calib/config.json")
    ap.add_argument("--snapshot", help="write an annotated debug frame here every second")
    ap.add_argument("--no-image", action="store_true", help="don't stream camera JPEGs")
    ap.add_argument("--video", help="replay a recorded video file instead of the webcam")
    ap.add_argument("--calib-dist", type=float, help="stand exactly this many metres from the camera; "
                    "calibrates the distance estimate and exits")
    ap.add_argument("--log", action="store_true", help="print tracking state every second")
    args = ap.parse_args()
    cfg = json.load(open(args.config))

    core = ov.Core()
    core.set_property("CPU", {"CACHE_DIR": "models/cache"})
    perf = cfg.get("perf", {})
    pose = BlazePose(core, threads=perf.get("pose_threads", 2))
    hands = HandPose(core, threads=perf.get("hand_threads", 2))
    depth = MonoDepth(core, hz=perf.get("depth_hz", 3.0), idle_hz=perf.get("depth_idle_hz", 0.5),
                      threads=perf.get("depth_threads", 2))
    idle_fps = perf.get("idle_pose_fps", 6.0)
    cc = cfg["camera"]
    cam = Camera(cc["index"], cc["width"], cc["height"], args.video, cc.get("fps", 30))
    frame, _, seq = cam.read(0)
    H, W = frame.shape[:2]
    K = Intrinsics.load(cc, W, H)
    to_mirror = CameraToMirror(cfg["camera_pose"])
    net = Streamer(cfg["net"]["host"], cfg["net"]["port"])
    gest = Gestures(cfg["gestures"])
    rng = RangeCheck(*cfg["range_m"])
    fj, fe = cfg["filters"]["joints"], cfg["filters"]["eye"]
    f_joints, f_eye, f_dist = OneEuro(**fj), OneEuro(**fe), OneEuro(1.0, 1.5)
    wfuse = cfg["depth"]["fusion_weight"]
    cal = load_depth_calib()
    calib = DistanceCalibrator(args.calib_dist, cal) if args.calib_dist else None
    src = "intrinsics.json" if os.path.exists(cc.get("intrinsics_file", "")) else f"hfov {cc['hfov_deg']} deg guess"
    print(f"[cam] {W}x{H}  fx={K.fx:.0f} ({src})  depth calib: skel={cal['skel']} da={cal['da']} "
          f"from {[p['d'] for p in cal['points']]} m")

    jpeg_w, jpeg_q = cfg["net"]["jpeg_width"], cfg["net"]["jpeg_quality"]
    fps, t_log, t_snap, last_seen, last_dbg = 0.0, 0.0, 0.0, 0.0, ""
    cam_fps, last_stamp = 30.0, time.time()
    last_idle_run, depth_runs, depth_t = 0.0, 0, time.time()
    while True:
        frame, stamp, seq = cam.read(seq)
        # nobody in view for a while: only look for a person a few times a second
        if stamp - last_seen > 3.0 and stamp - last_idle_run < 1.0 / idle_fps:
            continue
        last_idle_run = stamp
        t0 = time.time()
        depth.submit(frame)
        res = pose(frame)
        msg = {"t": stamp, "seq": seq, "tracked": False, "range": "none", "dist": 0.0,
               "events": [], "fist_progress": 0.0, "fist_hand": "",
               "cam": {**K.as_dict(), "pos": cfg["camera_pose"]["pos_m"],
                       "tilt": cfg["camera_pose"].get("tilt_down_deg", 0.0)},
               "screen": cfg["screen"]}
        fists = {}
        cue = skeleton_distance(res["uv"], res["world"], res["vis"], K) if res else None
        if cue is not None:
            uv, world, vis = res["uv"], res["world"], res["vis"]
            # --- distance: skeleton-size cue fused with metric mono depth (both calibrated)
            z_skel_raw = cue
            z_skel = cal["skel"][0] * z_skel_raw + cal["skel"][1]
            if z_skel < 0.3:                      # outside the calibrated range (very close): unusable
                z_skel = None
            samples = [depth.sample(*uv[i], W, H) for i in TORSO if vis[i] > 0.5]
            samples = [s for s in samples if s]
            z_da_raw = float(np.median(samples)) + cfg["depth"]["torso_surface_offset_m"] if samples else None
            z_da = cal["da"][0] * z_da_raw + cal["da"][1] if z_da_raw else None
            if z_skel is None:
                if not z_da:
                    z_skel = max(z_skel_raw, 0.3)
                else:
                    z_skel = z_da
            z_fused = z_skel
            if cal["calibrated"] and z_da:
                # cues agree: blend; disagree: the skeleton cue is the one that glitches
                # (limbs out of frame, turned torso), so trust mono depth alone
                agree = 0.75 * z_da < z_skel < 1.33 * z_da
                z_fused = wfuse * z_da + (1 - wfuse) * z_skel if agree else z_da
            z_root = float(f_dist(np.array([z_fused]), stamp)[0])
            if calib is not None:
                calib.add(z_skel_raw, z_da_raw)
            # --- per-joint depth from BlazePose's relative z; x/y back-projected from pixels,
            #     so the 3D skeleton always lands exactly on the person in the image
            Z = np.maximum(z_root + (world[:, 2] - world[list(TORSO), 2].mean()), 0.2)
            P_c = np.stack([(uv[:, 0] - K.cx) * Z / K.fx, (uv[:, 1] - K.cy) * Z / K.fy, Z], axis=1)
            P_m = f_joints(to_mirror(P_c), stamp)
            eye = f_eye(to_mirror((P_c[L_EYE] + P_c[R_EYE]) / 2), stamp)
            dist = z_root
            z_skel_log, z_da_log, scale = z_skel, z_da, z_fused / max(z_skel, 1e-3)

            # --- hands for the fist gesture (only for raised hands)
            rgb = None
            for side, (wi, si) in SIDES.items():
                if vis[wi] > 0.5 and P_m[wi, 1] > P_m[si, 1] - 0.05:
                    roi = HandPose.roi_from_pose(uv, side)
                    if roi is not None:
                        rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB) if rgb is None else rgb
                        fists[side] = is_fist(hands(rgb, roi))
            # actual body widths from the segmentation mask (cheap; a few times a second is plenty)
            if seq % 5 == 0:
                sil = measure_silhouette(res["seg"], res["seg_affine"], uv, vis)
                to_m = z_root / K.fx
                msg["shape"] = [round(sil.get(k, 0.0) * to_m, 4) for k in ("hip", "waist", "chest", "shoulder", "arm")]
            msg.update(tracked=True, range=rng(dist), dist=dist,
                       joints=np.round(P_m, 4).ravel().tolist(),
                       vis=np.round(vis, 3).tolist(),
                       eye=np.round(eye, 4).tolist(),
                       uv=np.round(uv, 1).ravel().tolist(),
                       z_skel=round(z_skel_log, 3), z_da=round(z_da_log or 0.0, 3), scale=round(scale, 3))
            if rng.state == "ok":
                msg["events"] = gest.update(stamp, P_m, vis, fists, uv)
                msg["fist_progress"] = round(gest.progress, 3)
                msg["fist_hand"] = gest.fist_side or ""
            else:
                gest.reset()
            last_seen = stamp
        else:
            rng(None)
            gest.reset()
            if stamp - last_seen > 1.0:
                f_joints.reset(); f_eye.reset(); f_dist.reset()
        depth.active = stamp - last_seen < 3.0

        jpg = None
        if not args.no_image and net.connected:          # nobody watching: skip the JPEG encode
            # Raw RGB over localhost (~1.5 MB/frame) is far cheaper than JPEG encode here + decode in Unity.
            fw_, fh_ = jpeg_w, int(H * jpeg_w / W)
            small = cv2.resize(frame, (fw_, fh_), interpolation=cv2.INTER_AREA)
            jpg = struct.pack(">HH", fw_, fh_) + cv2.cvtColor(small, cv2.COLOR_BGR2RGB).tobytes()
        dt = time.time() - t0
        fps = 0.9 * fps + 0.1 / max(dt, 1e-3)
        msg["proc_fps"], msg["depth_hz"] = round(fps, 1), round(depth.hz, 1)
        cam_fps = 0.9 * cam_fps + 0.1 / max(stamp - last_stamp, 1e-3); last_stamp = stamp
        msg["lat_ms"] = round((time.time() - stamp) * 1000, 1)
        msg["cam_fps"] = round(cam_fps, 1)
        net.push(msg, jpg)

        if msg["events"]:
            print(f"[gesture] {msg['events']}   {gest.debug}")
        elif args.log and gest.debug and gest.debug != last_dbg:
            print(f"[swipe?] {gest.debug}")
        last_dbg = gest.debug
        if args.log and stamp - t_log > 1.0:
            depth.hz = (depth.runs - depth_runs) / max(time.time() - depth_t, 1e-3)
            depth_runs, depth_t = depth.runs, time.time()
            t_log = stamp
            print(f"[state] tracked={msg['tracked']} range={msg['range']} dist={msg['dist']:.2f} "
                  f"zskel={msg.get('z_skel')} zda={msg.get('z_da')} scale={msg.get('scale')} "
                  f"fists={fists} prog={msg['fist_progress']} proc={fps:.0f}fps cam={cam_fps:.0f}fps lat={msg['lat_ms']}ms depth={depth.hz:.0f}Hz "
                  f"net={'up' if net.connected else 'down'}")
        if args.snapshot and stamp - t_snap > 1.0:
            t_snap = stamp
            dbg = frame.copy()
            if res:
                for (u, v), c in zip(res["uv"], res["vis"]):
                    cv2.circle(dbg, (int(u), int(v)), 4, (0, 255, 0) if c > 0.5 else (0, 0, 255), -1)
            cv2.putText(dbg, f"{msg['range']} {msg['dist']:.2f}m {fists}", (20, 40),
                        cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 255, 255), 2)
            cv2.imwrite(args.snapshot, dbg)


if __name__ == "__main__":
    main()
