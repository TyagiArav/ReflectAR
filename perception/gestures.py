"""Swipe (browse) and raised-fist hold (select) gestures.

Swipes are detected in the raw 2D image, in body-relative units: hand position relative to the
shoulder centre, divided by the shoulder width in pixels. That makes the thresholds independent
of how far away the user stands and of depth-estimation noise, and avoids the lag of the 3D
smoothing filters. The raw camera image is not mirrored, so the user's right is -u.

A swipe is one fast, mostly sideways stroke: it starts when the hand's sideways speed passes
swipe_start_speed_sw, and fires mid-stroke once it has covered swipe_min_dx_sw with a peak
speed of swipe_peak_speed_sw. The way back is not a swipe: after one, a stroke the other way
only counts once the hand has settled, and (within swipe_return_window_s) only if it is nearly
as fast as the swipe was. swipe_direction "natural" works like scrolling (the carousel follows
the hand: moving it to the right brings in the item on the left); "pointer" is the opposite.

The fist hold uses the Mirror-frame joints (hand raised above the shoulder) plus the
hand-landmark fist classifier.
"""
from collections import deque
import numpy as np

L_SHOULDER, R_SHOULDER, L_WRIST, R_WRIST, L_HIP, R_HIP = 11, 12, 15, 16, 23, 24
SIDES = {"left": (L_WRIST, L_SHOULDER), "right": (R_WRIST, R_SHOULDER)}
HAND = {"left": (15, 17, 19), "right": (16, 18, 20)}        # wrist, pinky, index


class _Stroke:
    def __init__(self, t, x, y, d):
        self.t0, self.x0, self.d, self.fired, self.suppressed = t, x, d, False, False
        self.ylo = self.yhi = y
        self.peak, self.last_fast = 0.0, t


class Gestures:
    def __init__(self, cfg):
        self.c = cfg
        self.hist = {s: deque() for s in SIDES}       # (t, x, y) in shoulder widths
        self.offs = {s: np.zeros((3, 2)) for s in SIDES}   # hand landmark -> wrist offsets (sw)
        self.offs_seen = {s: [True, False, False] for s in SIDES}
        self.stroke = {s: None for s in SIDES}
        self.last_swipe_t, self.last_swipe_dir, self.last_swipe_peak = -1e9, 0, 0.0
        self.settled = True                           # hands held still (or lowered) since the last swipe
        self.debug = ""
        self._reset_fist()
        self.need_release = False

    def reset(self):
        for s in SIDES:
            self.hist[s].clear()
            self.stroke[s] = None
        self._reset_fist()

    def _reset_fist(self):
        self.fist_side = self.fist_start = self.fist_last_true = self.fist_anchor = None
        self.progress = 0.0

    # ------------------------------------------------------------------ swipe
    def _hand(self, side, uv, vis, sh_c, sw):
        """Hand position (sw, + = user's right / up) from whichever hand landmarks are visible.
        Index and pinky are mapped back to the wrist with learned offsets, so the point doesn't
        jump when blur hides one of them."""
        pts = [np.array([-(uv[i, 0] - sh_c[0]) / sw, -(uv[i, 1] - sh_c[1]) / sw]) for i in HAND[side]]
        ok = [vis[i] > self.c["swipe_min_vis"] for i in HAND[side]]
        off, seen = self.offs[side], self.offs_seen[side]
        if ok[0]:
            for k in (1, 2):
                d = pts[k] - pts[0]
                if ok[k] and np.hypot(*d) > 0.6:              # not on this hand: ignore it
                    ok[k] = False
                elif ok[k]:
                    off[k] = d if not seen[k] else off[k] + 0.1 * (d - off[k])
                    seen[k] = True
        est = [pts[k] - off[k] for k in range(3) if ok[k] and (k == 0 or seen[k])]
        return np.mean(est, axis=0) if est else None

    def _swipe(self, t, uv, vis):
        c = self.c
        sh_c = (uv[L_SHOULDER] + uv[R_SHOULDER]) / 2
        sw = max(float(np.linalg.norm(uv[L_SHOULDER] - uv[R_SHOULDER])), 20.0)
        hip_y = -(((uv[L_HIP] + uv[R_HIP]) / 2)[1] - sh_c[1]) / sw if min(vis[L_HIP], vis[R_HIP]) > 0.3 else -2.6
        fired, still = None, True
        for side in SIDES:
            h, st = self.hist[side], self.stroke[side]
            p = self._hand(side, uv, vis, sh_c, sw)
            if p is not None and p[1] > hip_y + c["swipe_min_height_sw"]:
                # blurred landmarks that pass the visibility bar can be far off: drop impossible jumps
                if not h or t - h[-1][0] > 0.25 or np.hypot(p[0] - h[-1][1], p[1] - h[-1][2]) / (t - h[-1][0]) < 10.0:
                    h.append((t, p[0], p[1]))
            while h and t - h[0][0] > 0.5:
                h.popleft()
            if not h or t - h[-1][0] > 0.25:                     # hand lost / lowered: end any stroke
                if st is not None:
                    self._end(side, st, "hand lost")
                continue
            # held still: stayed within swipe_rest_sw over the last swipe_settle_s (jitter-proof)
            win = [q for q in h if t - q[0] <= c["swipe_settle_s"]]
            if (len(win) < 4 or win[-1][0] - win[0][0] < 0.7 * c["swipe_settle_s"]
                    or np.ptp([q[1] for q in win]) > c["swipe_rest_sw"] or np.ptp([q[2] for q in win]) > c["swipe_rest_sw"]):
                still = False
            recent = [q for q in h if t - q[0] <= 0.15]
            if len(recent) < 2 or recent[-1][0] - recent[0][0] < 0.05 or recent[-1][0] < t:
                continue                                          # no new sample this frame (blur): keep state
            ts = np.array([q[0] for q in recent]); xs = np.array([q[1] for q in recent]); ys = np.array([q[2] for q in recent])
            vx = float(np.polyfit(ts - ts[-1], xs, 1)[0])
            speed = abs(vx)
            x, y = xs[-1], ys[-1]
            if st is None and speed > c["swipe_start_speed_sw"]:
                d = 1 if vx > 0 else -1
                back = [q for q in h if t - q[0] <= 0.3]          # start where this motion began
                x0 = min(q[1] for q in back) if d > 0 else max(q[1] for q in back)
                st = self.stroke[side] = _Stroke(t, x0, y, d)
                # the way back after a swipe: ignore it unless the hand settled first
                if d == -self.last_swipe_dir and not self.settled:
                    st.suppressed = True
            if st is None:
                continue
            if np.sign(vx) == st.d and speed > c["swipe_end_speed_sw"]:
                st.last_fast = t
            elif t - st.last_fast > 0.15:                        # slowed or turned (with grace for blur)
                self._end(side, st, "stroke ended")
                continue
            st.peak = max(st.peak, speed if np.sign(vx) == st.d else 0.0)
            st.ylo, st.yhi = min(st.ylo, y), max(st.yhi, y)
            dx = st.d * (x - st.x0)
            if st.fired or st.suppressed or fired:
                continue
            dur = t - st.t0
            ok = (dx >= c["swipe_min_dx_sw"] and st.peak >= c["swipe_peak_speed_sw"]
                  and dx >= c["swipe_min_dx_over_dy"] * (st.yhi - st.ylo) and dur <= c["swipe_max_dur_s"])
            if not ok:
                continue
            since = t - self.last_swipe_t
            if since < c["swipe_cooldown_s"]:
                continue
            if (st.d == -self.last_swipe_dir and since < c["swipe_return_window_s"]
                    and st.peak < c["swipe_return_speed_ratio"] * self.last_swipe_peak):
                # too slow for a swipe back (yet): a drift home. Re-checked while the stroke lasts
                self.debug = f"{side} dx={st.d * dx:+.2f}sw peak={st.peak:.1f}sw/s -> wait (slower than the last swipe)"
                continue
            st.fired = True
            fired = st.d
            self.last_swipe_t, self.last_swipe_dir, self.last_swipe_peak = t, st.d, st.peak
            self.settled = False
            self.debug = f"{side} dx={st.d * dx:+.2f}sw dur={dur:.2f}s peak={st.peak:.1f}sw/s -> FIRE"
        if still and fired is None and not any(self.stroke.values()):
            self.settled = True
        if fired is None:
            return None
        to_right = fired > 0                                      # hand moved toward the user's right
        if self.c.get("swipe_direction", "natural") == "natural":
            return "prev" if to_right else "next"                 # carousel follows the hand
        return "next" if to_right else "prev"

    def _end(self, side, st, why):
        if st.fired and st.d == self.last_swipe_dir:              # the swipe's full peak, for the return check
            self.last_swipe_peak = max(self.last_swipe_peak, st.peak)
        if not st.fired and not st.suppressed and st.peak > self.c["swipe_start_speed_sw"]:
            self.debug = f"{side} peak={st.peak:.1f}sw/s -> reject ({why} before {self.c['swipe_min_dx_sw']}sw)"
        self.stroke[side] = None

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
