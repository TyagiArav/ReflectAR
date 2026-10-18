"""ReflectAR perception service.

Webcam -> BlazePose (OpenVINO) + metric mono depth -> 3D joints/eye in the
Mirror frame -> gestures + range check -> TCP stream to Unity.

Wire format (TCP, Python is the server, Unity connects):
    1 byte type ('J' = JSON tracking frame, 'I' = camera image)
    4 byte big-endian payload length
    payload: 'J' = UTF-8 JSON; 'I' = 8-byte big-endian float64 capture time + JPEG;
             'M' = float64 capture time + uint16 width + uint16 height + person mask
                   (uint8, rows top first, 255 = person) used to trim the occlusion proxy

Latency: each 'J' frame carries joint positions *and velocities* at the frame's capture time
't' plus its send time 't_send'; Unity extrapolates them to the moment each rendered frame
reaches the screen (see filters.PosePredictor, TrackingClient.Predict).

Run:  .venv/bin/python server.py [--snapshot debug.jpg] [--no-image]
"""
import argparse, json, os, socket, struct, sys, threading, time
if sys.platform == "win32":                          # MSMF otherwise takes seconds to open a webcam
    os.environ.setdefault("OPENCV_VIDEOIO_MSMF_ENABLE_HW_TRANSFORMS", "0")
import numpy as np
import cv2

from backend import Backend, warmup
from blazepose import BlazePose, DET_SIZE, LM_SIZE, frame_mask
from handpose import HandPose, HAND_SIZE, is_fist
from depth import MonoDepth, IN_H, IN_W
from filters import OneEuro, PosePredictor
from geometry import Intrinsics, skeleton_distance, CameraToMirror
from gestures import Gestures, SIDES

L_EYE, R_EYE = 2, 5
TORSO = (11, 12, 23, 24)


# --------------------------------------------------------------------------- IO
class Camera:
    """Grabs frames on a thread so inference never waits on the driver."""

    APIS = {"avfoundation": cv2.CAP_AVFOUNDATION, "msmf": cv2.CAP_MSMF, "dshow": cv2.CAP_DSHOW,
            "v4l2": cv2.CAP_V4L2, "any": cv2.CAP_ANY}

    def __init__(self, index, w, h, video=None, api="auto"):
        self.video = video
        if api == "auto":
            api = {"darwin": "avfoundation", "win32": "msmf"}.get(sys.platform, "any")
        self.cap = cv2.VideoCapture(video) if video else cv2.VideoCapture(index, self.APIS[api])
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, w)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, h)
        self.cap.set(cv2.CAP_PROP_FPS, 30)
        if not self.cap.isOpened():
            raise RuntimeError("Could not open camera. macOS: grant camera access to your terminal app "
                               "(System Settings > Privacy & Security > Camera). Windows: Settings > Privacy > "
                               "Camera, and close other apps using it; or try \"api\": \"dshow\" in config.json.")
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
    """Single-client TCP server. Keeps only the newest image; JSON events are merged.

    Tracking JSON goes out first; the image is JPEG-encoded afterwards on this thread, so
    neither the encode nor the larger write delays the pose.
    """

    def __init__(self, host, port, jpeg_width=960, jpeg_quality=75):
        self.jpeg_w, self.jpeg_q = jpeg_width, jpeg_quality
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind((host, port))
        self.srv.listen(1)
        self.conn = None
        self.lock = threading.Condition()
        self.json_msg = None
        self.image = None                            # (capture time, BGR frame or JPEG bytes)
        self.mask = None                             # (capture time, uint8 mask)
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

    def push(self, msg, image=None, mask=None):
        with self.lock:
            if self.conn is None:
                msg["events"] = []                   # nobody to act on them: don't replay later
            elif self.json_msg is not None:          # unsent: keep its events
                msg["events"] = self.json_msg["events"] + msg["events"]
            self.json_msg = msg
            if image is not None:
                self.image = image
            if mask is not None:
                self.mask = mask
            self.lock.notify()

    def _send(self):
        while True:
            with self.lock:
                self.lock.wait_for(lambda: self.json_msg is not None and self.conn is not None)
                msg, img, mask, conn = self.json_msg, self.image, self.mask, self.conn
                self.json_msg, self.image, self.mask = None, None, None
            try:
                msg["t_send"] = time.time()
                body = json.dumps(msg, separators=(",", ":")).encode()
                conn.sendall(b"J" + struct.pack(">I", len(body)) + body)
                if mask is not None:
                    t_m, m = mask
                    conn.sendall(b"M" + struct.pack(">IdHH", m.size + 12, t_m, m.shape[1], m.shape[0]) + m.tobytes())
                if img is not None:
                    t_img, jpg = img
                    if not isinstance(jpg, bytes):
                        h, w = jpg.shape[:2]
                        small = cv2.resize(jpg, (self.jpeg_w, int(h * self.jpeg_w / w)), interpolation=cv2.INTER_AREA)
                        jpg = cv2.imencode(".jpg", small, [cv2.IMWRITE_JPEG_QUALITY, self.jpeg_q])[1].tobytes()
                    conn.sendall(b"I" + struct.pack(">Id", len(jpg) + 8, t_img) + jpg)
            except OSError:
                print("[net] renderer disconnected")
                with self.lock:
                    if self.conn is conn:
                        self.conn = None


class HandWorker:
    """Fist classifier off the critical path. Only the 5 s select-hold uses it, so the main
    loop takes the newest result (one frame old) instead of waiting ~5 ms per raised hand."""

    def __init__(self, hands):
        self.hands = hands
        self.cond = threading.Condition()
        self.job = None
        self.fists, self.ms = {}, 0.0
        threading.Thread(target=self._loop, daemon=True).start()

    def submit(self, bgr, rois):
        """rois: {side: roi} for the raised hands; returns the newest fist state for those sides."""
        with self.cond:
            if rois:
                self.job = (bgr, rois)
                self.cond.notify()
            else:
                self.job, self.fists = None, {}
            return {s: v for s, v in self.fists.items() if s in rois}

    def _loop(self):
        while True:
            with self.cond:
                self.cond.wait_for(lambda: self.job is not None)
                (bgr, rois), self.job = self.job, None
            t0 = time.perf_counter()
            rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
            fists = {side: is_fist(self.hands(rgb, roi)) for side, roi in rois.items()}
            with self.cond:
                self.fists = fists
            self.ms = 0.9 * self.ms + 0.1 * (time.perf_counter() - t0) * 1000


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
    ap.add_argument("--ep-log", action="store_true", help="ONNX Runtime info logging: shows how many nodes of "
                    "each model Core ML / TensorRT took and what fell back to the CPU")
    args = ap.parse_args()
    cfg = json.load(open(args.config))

    comp = cfg.get("compute", {})
    backend = Backend({**comp, "verbose": args.ep_log})
    pose = BlazePose(backend)
    hand_model = HandPose(backend)
    depth_model_hz = comp.get("depth_max_hz", 12.0)
    t_w = time.time()                                # compile Core ML programs / TensorRT engines now
    warmup(pose.det, (1, DET_SIZE, DET_SIZE, 3)); warmup(pose.lm, (1, LM_SIZE, LM_SIZE, 3))
    warmup(hand_model.model, (1, HAND_SIZE, HAND_SIZE, 3))
    hands = HandWorker(hand_model)
    depth = MonoDepth(backend, max_hz=depth_model_hz)
    warmup(depth.model, (1, 3, IN_H, IN_W))
    print(backend.summary())
    print(f"[backend] devices {backend.devices}, CPU threads {backend.threads}, depth capped at "
          f"{depth_model_hz} Hz, warm-up {time.time() - t_w:.1f} s")
    cc = cfg["camera"]
    cam = Camera(cc["index"], cc["width"], cc["height"], args.video, cc.get("api", "auto"))
    frame, _, seq = cam.read(0)
    H, W = frame.shape[:2]
    K = Intrinsics.load(cc, W, H)
    to_mirror = CameraToMirror(cfg["camera_pose"])
    net = Streamer(cfg["net"]["host"], cfg["net"]["port"], cfg["net"]["jpeg_width"], cfg["net"]["jpeg_quality"])
    gest = Gestures(cfg["gestures"])
    rng = RangeCheck(*cfg["range_m"])
    fj, fe = cfg["filters"]["joints"], cfg["filters"]["eye"]
    f_joints, f_eye, f_dist = PosePredictor(**fj), PosePredictor(**fe), OneEuro(0.5, 0.3)
    pred_cfg = cfg.get("predict", {})
    cam_lat = cc.get("latency_s", 0.0)               # exposure -> frame delivered to us
    wfuse = cfg["depth"]["fusion_weight"]
    cal = load_depth_calib()
    calib = DistanceCalibrator(args.calib_dist, cal) if args.calib_dist else None
    src = "intrinsics.json" if os.path.exists(cc.get("intrinsics_file", "")) else f"hfov {cc['hfov_deg']} deg guess"
    print(f"[cam] {W}x{H}  fx={K.fx:.0f} ({src})  depth calib: skel={cal['skel']} da={cal['da']} "
          f"from {[p['d'] for p in cal['points']]} m")

    fps, pose_ms, t_log, t_snap, last_seen, last_dbg = 0.0, 0.0, 0.0, 0.0, 0.0, ""
    last_da = (None, -1e9)
    while True:
        frame, stamp, seq = cam.read(seq)
        t0 = time.time()
        t_cap = stamp - cam_lat
        depth.submit(frame)
        res = pose(frame)
        pose_ms = 0.9 * pose_ms + 0.1 * (time.time() - t0) * 1000
        msg = {"t": t_cap, "seq": seq, "tracked": False, "range": "none", "dist": 0.0,
               "events": [], "fist_progress": 0.0, "fist_hand": "",
               "cam": {**K.as_dict(), "pos": cfg["camera_pose"]["pos_m"],
                       "tilt": cfg["camera_pose"].get("tilt_down_deg", 0.0)},
               "screen": cfg["screen"], "predict": pred_cfg}
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
            if z_da:
                last_da = (z_da, stamp)
            elif stamp - last_da[1] < 0.5:            # brief dropout (stale map during fast motion,
                z_da = last_da[0]                     # torso samples hit background): hold, don't switch cue
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
            P_m, V_m = f_joints(to_mirror(P_c), stamp, vis)
            eye, eye_v = f_eye(to_mirror((P_c[L_EYE] + P_c[R_EYE]) / 2), stamp)
            dist = z_root
            z_skel_log, z_da_log, scale = z_skel, z_da, z_fused / max(z_skel, 1e-3)

            # --- hands for the fist gesture (only for raised hands), async: result is 1 frame old
            rois = {}
            for side, (wi, si) in SIDES.items():
                if vis[wi] > 0.5 and P_m[wi, 1] > P_m[si, 1] - 0.05:
                    roi = HandPose.roi_from_pose(uv, side)
                    if roi is not None:
                        rois[side] = roi
            fists = hands.submit(frame, rois)
            msg.update(tracked=True, range=rng(dist), dist=dist,
                       joints=np.round(P_m, 4).ravel().tolist(),
                       vel=np.round(V_m, 3).ravel().tolist(),
                       vis=np.round(vis, 3).tolist(),
                       eye=np.round(eye, 4).tolist(),
                       eye_vel=np.round(eye_v, 3).tolist(),
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

        dt = time.time() - t0
        fps = 0.9 * fps + 0.1 / max(dt, 1e-3)
        msg["proc_fps"], msg["depth_hz"] = round(fps, 1), round(depth.hz, 1)
        msg["proc_ms"] = round(dt * 1000, 1)
        mask_w = cfg["net"].get("mask_width", 256)
        mask = (t_cap, frame_mask(res, W, H, mask_w)) if res and msg["tracked"] and mask_w else None
        net.push(msg, None if args.no_image else (t_cap, frame), mask)

        if msg["events"]:
            print(f"[gesture] {msg['events']}   {gest.debug}")
        elif args.log and gest.debug and gest.debug != last_dbg:
            print(f"[swipe?] {gest.debug}")
        last_dbg = gest.debug
        if args.log and stamp - t_log > 1.0:
            t_log = stamp
            print(f"[state] tracked={msg['tracked']} range={msg['range']} dist={msg['dist']:.2f} "
                  f"zskel={msg.get('z_skel')} zda={msg.get('z_da')} scale={msg.get('scale')} "
                  f"fists={fists} prog={msg['fist_progress']} pose={pose_ms:.1f}ms hands={hands.ms:.1f}ms "
                  f"proc={fps:.0f}fps depth={depth.hz:.0f}Hz "
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
