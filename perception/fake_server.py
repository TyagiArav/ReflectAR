"""Synthetic perception stream for renderer testing without a camera.

Sends a walking/waving skeleton standing ~1.8 m from the screen in the same wire format
as server.py. Optional: --script swipe|fist exercises the gesture events; belly|shoulders hold still with the
hands resting on the belly / on the shoulders (occlusion tests, with a synthetic person mask).
"""
import argparse, json, math, time
import numpy as np
import cv2
from server import Streamer
from geometry import Intrinsics, CameraToMirror

# rest skeleton in a body frame: x = user's right, y up (hips at 0), z toward the screen
REST = {
    0: (0, 0.70, 0.10), 2: (-0.032, 0.735, 0.08), 5: (0.032, 0.735, 0.08), 7: (-0.075, 0.70, 0.0), 8: (0.075, 0.70, 0.0),
    11: (-0.18, 0.50, 0), 12: (0.18, 0.50, 0), 13: (-0.22, 0.22, 0), 14: (0.22, 0.22, 0),
    15: (-0.24, -0.03, 0.03), 16: (0.24, -0.03, 0.03), 17: (-0.25, -0.10, 0.03), 18: (0.25, -0.10, 0.03),
    19: (-0.23, -0.11, 0.05), 20: (0.23, -0.11, 0.05), 21: (-0.22, -0.08, 0.06), 22: (0.22, -0.08, 0.06),
    23: (-0.10, 0, 0), 24: (0.10, 0, 0), 25: (-0.11, -0.45, 0.02), 26: (0.11, -0.45, 0.02),
    27: (-0.11, -0.88, 0), 28: (0.11, -0.88, 0),
}

def skeleton(t, script):
    J = np.zeros((33, 3))
    for k, v in REST.items():
        J[k] = v
    for k in (1, 3, 4, 6, 9, 10):
        J[k] = J[0]
    J[29:31] = J[27:29]; J[31:33] = J[27:29]
    still = script in ("belly", "shoulders")
    sway = 0.0 if still else 0.04 * math.sin(t * 1.3)
    turn = 0.0 if still else 0.35 * math.sin(t * 0.5)
    # arm swing / wave
    for side, (e, w, hs) in {"l": (13, 15, (17, 19, 21)), "r": (14, 16, (18, 20, 22))}.items():
        sgn = -1 if side == "l" else 1
        ang = 0.4 * math.sin(t * 2 + (0 if side == "l" else math.pi))
        sh = J[11 if side == "l" else 12]
        def rot(p, a, piv):
            d = p - piv
            return piv + np.array([d[0], d[1] * math.cos(a) - d[2] * math.sin(a), d[1] * math.sin(a) + d[2] * math.cos(a)])
        for idx in (e, w) + hs:
            J[idx] = rot(J[idx], -ang, sh)
    if script == "fist" and (t % 12) < 7:          # right hand raised above shoulder
        J[14] = J[12] + (0.10, 0.10, 0.05); J[16] = J[12] + (0.12, 0.38, 0.05)
        for i in (18, 20, 22): J[i] = J[16] + (0, 0.06, 0)
    if script in ("belly", "shoulders"):            # held poses, hands resting on the body (-z: in front of it)
        for sgn, (e, w, p, i, th) in ((-1, (13, 15, 17, 19, 21)), (1, (14, 16, 18, 20, 22))):
            if script == "belly":
                pts = {e: (0.22, 0.22, 0.03), w: (0.06, 0.16, 0.15), p: (0.02, 0.10, 0.15), i: (0.0, 0.15, 0.16), th: (0.04, 0.21, 0.17)}
            else:
                pts = {e: (0.21, 0.25, 0.15), w: (0.15, 0.45, 0.13), p: (0.20, 0.51, 0.07), i: (0.15, 0.55, 0.07), th: (0.10, 0.49, 0.13)}
            for k, (x, y, z) in pts.items():
                J[k] = (sgn * x, y, -z)
    if script == "swipe":
        ph = (t % 3) / 3
        x = -0.35 + 0.9 * min(ph * 4, 1) if ph < 0.5 else 0.55
        J[16] = (x, 0.35, 0.25); J[14] = (0.22, 0.25, 0.1)
        for i in (18, 20, 22): J[i] = J[16] + (0.03, 0.03, 0)
    # body yaw + sway, place 1.8 m in front of the screen centre, hips at 0.95 m height
    c, s = math.cos(turn), math.sin(turn)
    Rz = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    J = J @ Rz.T + np.array([0.151 + sway, -0.05, 1.8])
    return J

def person_mask(J, K, cam, out_w=256):
    """Rough person segmentation from the skeleton (filled limbs, torso and head), like frame_mask's."""
    s = out_w / K.w
    out = np.zeros((int(round(K.h * s)), out_w), np.uint8)
    def px(p):                                      # Mirror -> camera (x, y flipped, tilt 0) -> mask pixels
        c = np.array([-(p[0] - cam.pos[0]), -(p[1] - cam.pos[1]), p[2] - cam.pos[2]])
        return np.array([K.fx * c[0] / c[2] + K.cx, K.fy * c[1] / c[2] + K.cy]) * s, K.fx * s / c[2]
    def limb(a, b, r):
        (pa, f), (pb, _) = px(a), px(b)
        cv2.line(out, tuple(np.round(pa).astype(int)), tuple(np.round(pb).astype(int)), 255, max(1, int(2 * r * f)))
        for q in (pa, pb):
            cv2.circle(out, tuple(np.round(q).astype(int)), max(1, int(r * f)), 255, -1)
    torso = [J[11] + (J[11] - J[12]) * 0.12, J[12] + (J[12] - J[11]) * 0.12, J[24] + (J[24] - J[23]) * 0.3, J[23] + (J[23] - J[24]) * 0.3]
    cv2.fillPoly(out, [np.round(np.array([px(p)[0] for p in torso])).astype(np.int32)], 255)
    for a, b, r in ((11, 13, 0.05), (12, 14, 0.05), (13, 15, 0.04), (14, 16, 0.04), (23, 25, 0.08), (24, 26, 0.08),
                    (25, 27, 0.06), (26, 28, 0.06), (15, 19, 0.035), (15, 17, 0.035), (16, 20, 0.035), (16, 18, 0.035),
                    (15, 21, 0.015), (16, 22, 0.015), (11, 12, 0.06)):
        limb(J[a], J[b], r)
    head = (J[7] + J[8]) / 2
    limb(head + (0, 0.03, 0), head - (0, 0.06, 0), 0.095)
    limb((J[11] + J[12]) / 2, head, 0.06)
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--script", choices=["none", "swipe", "fist", "belly", "shoulders"], default="none")
    ap.add_argument("--dist", type=float, default=1.8)
    ap.add_argument("--port", type=int, default=0, help="override config.json's port")
    ap.add_argument("--mask", action="store_true", help="also send a person mask drawn from the skeleton")
    args = ap.parse_args()
    cfg = json.load(open("calib/config.json"))
    net = Streamer(cfg["net"]["host"], args.port or cfg["net"]["port"])
    K = Intrinsics.load(cfg["camera"], 1280, 720)
    img = np.zeros((540, 960, 3), np.uint8)
    img[:] = (60, 50, 45)
    cv2.putText(img, "SYNTHETIC FEED", (300, 280), cv2.FONT_HERSHEY_SIMPLEX, 1.6, (200, 200, 200), 3)
    cam = CameraToMirror(cfg["camera_pose"])
    t0, seq, prog = time.time(), 0, 0.0
    while True:
        now = time.time()
        t = now - t0
        J = skeleton(t, args.script)
        J[:, 2] += args.dist - 1.8
        V = (skeleton(t + 0.01, args.script) - skeleton(t - 0.01, args.script)) / 0.02
        eye = (J[2] + J[5]) / 2
        events = []
        if args.script == "fist":
            if (t % 12) < 7 and (t % 12) > 1:
                prog = min(((t % 12) - 1) / 5, 1.0)
                if prog >= 1.0 and seq % 1 == 0 and not getattr(main, "fired", False):
                    events.append("select"); main.fired = True
            else:
                prog = 0.0; main.fired = False
        if args.script == "swipe" and abs((t % 3) - 0.4) < 1 / 60:
            events.append("next")
        d = float(J[23:25, 2].mean())
        rng = "too_close" if d < 1.0 else "too_far" if d > 3.0 else "ok"
        msg = {"t": now, "seq": seq, "tracked": True, "range": rng, "dist": d, "events": events,
               "fist_progress": prog if prog < 1 else 0.0, "fist_hand": "right" if prog > 0 else "",
               "joints": J.ravel().round(4).tolist(), "vel": V.ravel().round(3).tolist(), "vis": [0.99] * 33,
               "eye": eye.round(4).tolist(), "eye_vel": ((V[2] + V[5]) / 2).round(3).tolist(),
               "cam": {**K.as_dict(), "pos": cfg["camera_pose"]["pos_m"], "tilt": 0.0}, "screen": cfg["screen"],
               "predict": cfg.get("predict", {}),
               "proc_fps": 30.0, "depth_hz": 0.0, "scale": 1.0, "z_skel": d, "z_da": 0.0}
        # Streamer resizes + sends the image raw
        net.push(msg, (now, img) if seq % 30 == 0 else None, (now, person_mask(J, K, cam)) if args.mask else None)
        seq += 1
        time.sleep(1 / 30)

if __name__ == "__main__":
    main()
