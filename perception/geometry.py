"""Camera intrinsics, monocular metric 3D, and Camera -> Mirror frame transform.

Mirror frame (M): origin lower-left of the display, +X right as the user sees it,
+Y up, +Z out of the screen toward the user. Camera frame (C): OpenCV (x right
in the raw image, y down, z forward). A camera on the screen looking at the user
has x_C = -X_M, y_C = -Y_M, z_C = +Z_M (before tilt).
"""
import json, math, os
import numpy as np


class Intrinsics:
    def __init__(self, fx, fy, cx, cy, w, h):
        self.fx, self.fy, self.cx, self.cy, self.w, self.h = fx, fy, cx, cy, w, h

    @classmethod
    def load(cls, cam_cfg, w, h):
        path = cam_cfg.get("intrinsics_file")
        if path and os.path.exists(path):
            d = json.load(open(path))
            sx, sy = w / d["w"], h / d["h"]
            return cls(d["fx"] * sx, d["fy"] * sy, d["cx"] * sx, d["cy"] * sy, w, h)
        fx = (w / 2) / math.tan(math.radians(cam_cfg["hfov_deg"]) / 2)
        return cls(fx, fx, w / 2, h / 2, w, h)

    def as_dict(self):
        return dict(fx=self.fx, fy=self.fy, cx=self.cx, cy=self.cy, w=self.w, h=self.h)


def solve_translation(uv, world, vis, K: Intrinsics, min_vis=0.5):
    """Find T so that world + T projects onto uv (linear least squares).

    BlazePose world landmarks are metric, hip-centred and camera-aligned, so the
    only unknown is where the hips are in camera space.
    """
    idx = np.where(vis > min_vis)[0]
    idx = idx[idx >= 11] if np.sum(idx >= 11) >= 6 else idx   # prefer body over face
    if idx.size < 4:
        return None
    A, b = [], []
    for i in idx:
        X, Y, Z = world[i]
        du, dv = uv[i, 0] - K.cx, uv[i, 1] - K.cy
        A.append([K.fx, 0, -du]); b.append(du * Z - K.fx * X)
        A.append([0, K.fy, -dv]); b.append(dv * Z - K.fy * Y)
    T, *_ = np.linalg.lstsq(np.array(A), np.array(b), rcond=None)
    return T if T[2] > 0.2 else None


def skeleton_distance(uv, world, vis, K: Intrinsics, min_vis=0.5):
    """Camera distance of the torso from body size in pixels vs BlazePose's metric skeleton.

    Uses torso length (shoulder centre -> hip centre) when the hips are visible, which is
    stable under turning; falls back to shoulder width when only the upper body is in view.
    """
    if vis[11] < min_vis or vis[12] < min_vis:
        return None
    sh_px, sh_w = (uv[11] + uv[12]) / 2, (world[11] + world[12]) / 2
    if vis[23] > min_vis and vis[24] > min_vis:
        hip_px, hip_w = (uv[23] + uv[24]) / 2, (world[23] + world[24]) / 2
        px = float(np.hypot(*(sh_px - hip_px)))
        L = float(np.linalg.norm((sh_w - hip_w)[:2]))
        f = K.fy
    else:
        px = float(np.hypot(*(uv[11] - uv[12])))
        L = 0.36                                   # typical biacromial joint distance
        f = K.fx
    return f * L / px if px > 5 else None


class CameraToMirror:
    def __init__(self, pose_cfg):
        self.pos = np.array(pose_cfg["pos_m"], np.float64)
        th = math.radians(pose_cfg.get("tilt_down_deg", 0.0))
        tilt = np.array([[1, 0, 0],
                         [0, math.cos(th), math.sin(th)],
                         [0, -math.sin(th), math.cos(th)]])
        flip = np.diag([-1.0, -1.0, 1.0])
        self.R = flip @ tilt

    def __call__(self, p_c):
        return (np.asarray(p_c) @ self.R.T) + self.pos
