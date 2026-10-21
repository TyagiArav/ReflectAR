"""Measure the user's actual body from BlazePose's person-segmentation mask.

Garments used to be sized from shoulder-joint spacing and average-person proportions, which
made them read too large. Here we scan the mask across the torso at four heights and across
the upper arm, giving real silhouette half-widths. Wherever an arm hangs alongside the torso,
the scan stops at the arm's edge so the arm isn't counted as torso.

Everything is computed in image pixels; the caller converts to metres at the torso depth
(the silhouette's side edges lie at roughly the depth of the joint centres).
"""
import numpy as np

# rows measured, as a fraction of the way from the hip centre (0) to the shoulder centre (1)
ROWS = {"hip": 0.05, "waist": 0.4, "chest": 0.75, "shoulder": 1.0}
ARM = {+1: (12, 14, 16), -1: (11, 13, 15)}         # side -> shoulder, elbow, wrist (+1 = person's right)


def _seg_dist(p, a, b):
    ab = b - a
    t = np.clip(((p - a) @ ab) / max(float(ab @ ab), 1e-6), 0.0, 1.0)
    return np.linalg.norm(p - (a + t[:, None] * ab), axis=1)


class Silhouette:
    def __init__(self, seg, M, thresh=0.5):
        self.seg = seg
        A = M[:, :2]
        self.t = M[:, 2]
        self.Ainv = np.linalg.inv(A)
        self.th = thresh

    def inside(self, pts):
        c = (pts - self.t) @ self.Ainv.T                  # image px -> 256x256 crop px
        ok = (c[:, 0] >= 0) & (c[:, 0] < 255) & (c[:, 1] >= 0) & (c[:, 1] < 255)
        ix = np.clip(c[:, 0].astype(int), 0, 255); iy = np.clip(c[:, 1].astype(int), 0, 255)
        return ok & (self.seg[iy, ix] > self.th)

    def edge(self, origin, direction, max_px, step=1.5):
        """Distance (px) from origin along direction to the first pixel outside the mask."""
        d = np.arange(0.0, max_px, step)
        m = self.inside(origin + direction * d[:, None])
        if not m[:3].all():
            return None, d
        out = np.where(~m)[0]
        return (float(d[out[0]]) if out.size else None), d


def measure(seg, M, uv, vis, min_vis=0.5):
    """Returns dict of half-widths in px: hip/waist/chest/shoulder, plus 'arm' (upper-arm radius)."""
    if min(vis[11], vis[12]) < min_vis:
        return {}
    sil = Silhouette(seg, M)
    ls, rs = uv[11], uv[12]
    sh_c = (ls + rs) / 2
    span = float(np.linalg.norm(rs - ls))
    if span < 20:
        return {}
    across = (rs - ls) / span                                 # toward the person's right shoulder
    hips_ok = min(vis[23], vis[24]) > min_vis
    hip_c = (uv[23] + uv[24]) / 2 if hips_ok else sh_c + np.array([-across[1], across[0]]) * span * 1.35
    arm_r = 0.11 * span                                       # rough arm radius in px, for the arm corridor
    out = {}
    # a raised / sideways upper arm continues the silhouette past the shoulder: no shoulder edge
    def arm_out(side):
        si, ei, _ = ARM[side]
        v = uv[ei] - uv[si]
        return abs(float(v @ across)) > 0.55 * max(float(np.linalg.norm(v)), 1e-3)
    for name, f in ROWS.items():
        if name in ("hip", "waist") and not hips_ok:
            continue
        if name == "shoulder" and (arm_out(+1) and arm_out(-1)):
            continue
        c = hip_c + (sh_c - hip_c) * f
        sides = []
        for side in (+1, -1):
            if name == "shoulder" and arm_out(side):
                continue
            e, d = sil.edge(c, across * side, 1.3 * span)
            if e is None:
                continue
            if name != "shoulder":
                # stop at an arm hanging alongside the torso
                si, ei, wi = ARM[side]
                pts = c + across * side * d[:, None]
                near = np.minimum(_seg_dist(pts, uv[si], uv[ei]), _seg_dist(pts, uv[ei], uv[wi]))
                hit = np.where(near < arm_r)[0]
                if hit.size and vis[ei] > min_vis:
                    e = min(e, float(d[hit[0]]))
            sides.append(e)
        if sides:
            hw = float(np.mean(sides)) if len(sides) == 2 and abs(sides[0] - sides[1]) < 0.25 * span else min(sides)
            # sanity: a torso half-width is between 0.3 and 0.75 of the shoulder-joint span
            if 0.3 * span < hw < 0.75 * span:
                out[name] = hw
    # upper-arm radius: scan perpendicular to the arm at its midpoint, both ways
    radii = []
    for side in (+1, -1):
        si, ei, _ = ARM[side]
        if min(vis[si], vis[ei]) < min_vis:
            continue
        a, b = uv[si], uv[ei]
        L = float(np.linalg.norm(b - a))
        if L < 10:
            continue
        mid = a + (b - a) * 0.55
        n = np.array([-(b - a)[1], (b - a)[0]]) / L
        e1, _ = sil.edge(mid, n, 0.4 * span)
        e2, _ = sil.edge(mid, -n, 0.4 * span)
        cand = [e for e in (e1, e2) if e is not None]
        if cand:
            radii.append(min(cand))                            # the other side may merge into the torso
    if radii:
        r = float(np.median(radii))
        if 0.05 * span < r < 0.25 * span:
            out["arm"] = r
    return out
