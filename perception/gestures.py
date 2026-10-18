"""Swipe (browse) and raised-fist hold (select) gestures.

Swipes are detected in the raw 2D image, in body-relative units: wrist position relative
to the shoulder centre, divided by the shoulder width in pixels. That makes the threshold
independent of how far away the user stands and of depth-estimation noise, and avoids the
lag of the 3D smoothing filters. A hand moving toward the user's right is "next", toward
the user's left is "prev" (the raw camera image is not mirrored, so that is -u).

The fist hold uses the Mirror-frame joints (hand raised above the shoulder) plus the
hand-landmark fist classifier.
"""
from collections import deque
import numpy as np

L_SHOULDER, R_SHOULDER, L_WRIST, R_WRIST, L_HIP, R_HIP = 11, 12, 15, 16, 23, 24
SIDES = {"left": (L_WRIST, L_SHOULDER), "right": (R_WRIST, R_SHOULDER)}


class Gestures:
    def __init__(self, cfg):
        self.c = cfg
        self.hist = {s: deque() for s in SIDES}       # (t, x, y) in shoulder widths
        self.last_seen = {s: -1e9 for s in SIDES}
        self.last_swipe_t = -1e9
        self.last_swipe_dir = 0
        self.debug = ""
        self._reset_fist()
        self.need_release = False

    def reset(self):
        for h in self.hist.values():
            h.clear()
        self._reset_fist()

    def _reset_fist(self):
        self.fist_side = self.fist_start = self.fist_last_true = self.fist_anchor = None
        self.progress = 0.0

    # ------------------------------------------------------------------ swipe
    def _swipe(self, t, uv, vis):
        c = self.c
        sh_c = (uv[L_SHOULDER] + uv[R_SHOULDER]) / 2
        sw = max(float(np.linalg.norm(uv[L_SHOULDER] - uv[R_SHOULDER])), 20.0)
        hip_y = -(((uv[L_HIP] + uv[R_HIP]) / 2)[1] - sh_c[1]) / sw if min(vis[L_HIP], vis[R_HIP]) > 0.3 else -2.6
        for side, (wi, _) in SIDES.items():
            h = self.hist[side]
            x = -(uv[wi, 0] - sh_c[0]) / sw                 # + toward the user's right
            y = -(uv[wi, 1] - sh_c[1]) / sw                 # + up
            active = vis[wi] > c["swipe_min_vis"] and y > hip_y + c["swipe_min_height_sw"]
            if active:
                h.append((t, x, y))
                self.last_seen[side] = t
            elif t - self.last_seen[side] > c["swipe_gap_s"]:
                h.clear()                                   # only forget after a real gap
            while h and t - h[0][0] > c["swipe_window_s"]:
                h.popleft()
            if len(h) < 3:
                continue
            ts = np.array([p[0] for p in h]); xs = np.array([p[1] for p in h]); ys = np.array([p[2] for p in h])
            # best stroke ending now: the earliest sample far enough away horizontally
            dx_all = xs[-1] - xs
            for d in (1, -1):
                idx = np.where(d * dx_all >= c["swipe_min_dx_sw"])[0]
                if idx.size == 0:
                    continue
                i = idx[-1]                                 # shortest qualifying stroke
                seg_x, seg_y = xs[i:], ys[i:]
                dur = ts[-1] - ts[i]
                path = np.sum(np.abs(np.diff(seg_x)))
                straight = abs(dx_all[i]) / max(path, 1e-6)
                dy = np.ptp(seg_y)
                speed = abs(dx_all[i]) / max(dur, 1e-3)
                # natural swipes arc; only require the stroke to be mostly horizontal
                ok = (dur > 0.05 and straight > 0.6 and dy < c["swipe_max_dy_sw"]
                      and abs(dx_all[i]) > c["swipe_min_dx_over_dy"] * dy and speed > c["swipe_min_speed_sw"])
                self.debug = (f"{side} dx={dx_all[i]:+.2f}sw dur={dur:.2f}s straight={straight:.2f} "
                              f"dy={dy:.2f} speed={speed:.1f}sw/s -> {'FIRE' if ok else 'reject'}")
                if not ok:
                    continue
                since = t - self.last_swipe_t
                if since < c["swipe_cooldown_s"] or (d == -self.last_swipe_dir and since < c["swipe_reverse_block_s"]):
                    # a return stroke: consume it, so it can't fire later once the block expires
                    self.debug += " (cooldown, consumed)"
                    h.clear()
                    break
                self.last_swipe_t, self.last_swipe_dir = t, d
                for hh in self.hist.values():
                    hh.clear()
                return "next" if d > 0 else "prev"
        return None

    # ------------------------------------------------------------------ frame
    def update(self, t, joints, vis, fists, uv):
        """joints: (33,3) Mirror frame (filtered); uv: (33,2) raw pixels; fists per side."""
        events = []
        # a fist that has been held still for a moment owns the hands; otherwise swipes win
        if self.progress < self.c["fist_blocks_swipe_after"]:
            e = self._swipe(t, uv, vis)
            if e:
                events.append(e)
                self._reset_fist()
                return events

        raised = {side: vis[wi] > 0.5 and joints[wi, 1] > joints[si, 1] - 0.05 for side, (wi, si) in SIDES.items()}
        if self.need_release:
            if not any(raised[s] and fists.get(s) for s in SIDES):
                self.need_release = False
            self.progress = 0.0
            return events

        if self.fist_side is None:
            for side in SIDES:
                if raised[side] and fists.get(side) is True:
                    self.fist_side, self.fist_start, self.fist_last_true = side, t, t
                    self.fist_anchor = joints[SIDES[side][0]].copy()
                    break
        else:
            side = self.fist_side
            w = joints[SIDES[side][0]]
            ok = raised[side] and fists.get(side) is not False
            if ok and fists.get(side) is True:
                self.fist_last_true = t
            drift = np.linalg.norm(w[:2] - self.fist_anchor[:2])
            if not ok or t - self.fist_last_true > self.c["fist_gap_tolerance_s"] \
                    or drift > self.c["fist_max_drift_m"]:
                self._reset_fist()
            else:
                self.progress = min((t - self.fist_start) / self.c["fist_hold_s"], 1.0)
                if self.progress >= 1.0:
                    events.append("select")
                    self._reset_fist()
                    self.need_release = True
        return events
