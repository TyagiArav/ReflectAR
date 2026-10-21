"""BlazePose (full): detector + landmark model with ROI tracking.

Re-implements MediaPipe's pose pipeline around the stock models (backend.py runs them:
the .tflite on OpenVINO, or the converted .onnx on ONNX Runtime / Core ML / CUDA). Outputs 33 landmarks in image pixels, 33 metric world
landmarks (hip-centred, camera-aligned) and a person segmentation mask.
"""
import math
import numpy as np
import cv2

DET_SIZE = 224
LM_SIZE = 256


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-np.clip(x, -50, 50)))


def _gen_anchors():
    strides = [8, 16, 32, 32, 32]
    anchors, i = [], 0
    while i < len(strides):
        j, per_cell = i, 0
        while j < len(strides) and strides[j] == strides[i]:
            per_cell += 2          # aspect 1.0 + interpolated scale
            j += 1
        fm = int(math.ceil(DET_SIZE / strides[i]))
        for y in range(fm):
            for x in range(fm):
                anchors.extend([((x + 0.5) / fm, (y + 0.5) / fm)] * per_cell)
        i = j
    return np.array(anchors, np.float32)


def pick_closest(raw, anchors, scores, thresh=0.5, iou_max=0.3):
    """Index of the person to track: the largest one in the image (= closest to the mirror),
    not the most confident one, which in a busy room is often someone in the background.
    Overlapping detections of the same person are merged first (non-max suppression)."""
    idx = np.where(scores >= thresh)[0]
    if idx.size == 0:
        return None
    idx = idx[np.argsort(-scores[idx])]
    c = raw[idx, 0:2] / DET_SIZE + anchors[idx]
    wh = raw[idx, 2:4] / DET_SIZE
    lo, hi = c - wh / 2, c + wh / 2
    keep = []
    for i in range(idx.size):
        ok = True
        for j in keep:
            inter = np.prod(np.clip(np.minimum(hi[i], hi[j]) - np.maximum(lo[i], lo[j]), 0, None))
            union = np.prod(wh[i]) + np.prod(wh[j]) - inter
            if inter / max(union, 1e-9) > iou_max:
                ok = False
                break
        if ok:
            keep.append(i)
    # body scale: hip centre -> point above the head (keypoints 0 and 1)
    size = [np.linalg.norm(raw[idx[i], 6:8] - raw[idx[i], 4:6]) for i in keep]
    return int(idx[keep[int(np.argmax(size))]])


def roi_affine(center, side, rot, out):
    """2x3 matrix mapping output-crop pixels -> image pixels."""
    c, s = math.cos(rot), math.sin(rot)
    k = side / out
    R = np.array([[c * k, -s * k], [s * k, c * k]], np.float32)
    t = np.asarray(center, np.float32) - R @ np.array([out / 2, out / 2], np.float32)
    return np.hstack([R, t[:, None]])


def roi_from_points(p_center, p_top, scale=1.25):
    d = np.asarray(p_top) - np.asarray(p_center)
    side = 2.0 * float(np.hypot(*d)) * scale
    rot = math.pi / 2 - math.atan2(-d[1], d[0])
    rot = (rot + math.pi) % (2 * math.pi) - math.pi
    return np.asarray(p_center, np.float32), side, rot


class BlazePose:
    def __init__(self, backend):
        self.det = backend.load("pose_detection", "pose")
        self.lm = backend.load("pose_landmark_full", "pose")
        self.anchors = _gen_anchors()
        self.roi = None            # (center, side, rot) carried between frames

    # ---------------------------------------------------------------- detector
    def _detect(self, rgb):
        h, w = rgb.shape[:2]
        s = max(h, w)
        padx, pady = (s - w) // 2, (s - h) // 2
        sq = cv2.copyMakeBorder(rgb, pady, s - h - pady, padx, s - w - padx, cv2.BORDER_CONSTANT)
        inp = cv2.resize(sq, (DET_SIZE, DET_SIZE)).astype(np.float32) / 127.5 - 1.0
        res = self.det(inp[None])
        raw = res[0][0]
        scores = sigmoid(res[1][0, :, 0])
        best = pick_closest(raw, self.anchors, scores)
        if best is None:
            return None
        r, a = raw[best], self.anchors[best]
        kp0 = (r[4:6] / DET_SIZE + a) * s - (padx, pady)
        kp1 = (r[6:8] / DET_SIZE + a) * s - (padx, pady)
        return roi_from_points(kp0, kp1)

    # ---------------------------------------------------------------- landmarks
    def __call__(self, bgr):
        """Returns dict or None. Pixel coords refer to the input (un-mirrored) image."""
        rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
        for attempt in range(2):
            if self.roi is None:
                self.roi = self._detect(rgb)
                if self.roi is None:
                    return None
            out = self._landmarks(rgb, *self.roi)
            if out is not None:
                return out
            self.roi = None        # lost track: re-detect once this frame
        return None

    def _landmarks(self, rgb, center, side, rot):
        M = roi_affine(center, side, rot, LM_SIZE)
        crop = cv2.warpAffine(rgb, M, (LM_SIZE, LM_SIZE),
                              flags=cv2.INTER_LINEAR | cv2.WARP_INVERSE_MAP,
                              borderMode=cv2.BORDER_CONSTANT)
        res = self.lm((crop.astype(np.float32) / 255.0)[None])
        flag = float(res[1].ravel()[0])
        if flag > 1.0 or flag < 0.0:
            flag = float(sigmoid(flag))
        if flag < 0.5:
            return None
        raw = res[0].reshape(39, 5)
        uv = raw[:, :2] @ M[:, :2].T + M[:, 2]
        z = raw[:, 2] * side / LM_SIZE
        vis = sigmoid(raw[:, 3])
        pres = sigmoid(raw[:, 4])

        world = res[4].reshape(39, 3).copy()
        c, s = math.cos(rot), math.sin(rot)
        wx, wy = world[:, 0].copy(), world[:, 1].copy()
        world[:, 0] = c * wx - s * wy
        world[:, 1] = s * wx + c * wy

        seg = sigmoid(res[2][0, :, :, 0])

        # next-frame ROI from auxiliary keypoints 33 (hip centre) / 34 (above head)
        self.roi = roi_from_points(uv[33], uv[34])
        return {
            "uv": uv[:33], "z_px": z[:33], "vis": np.minimum(vis, pres)[:33],
            "world": world[:33], "score": flag, "seg": seg, "seg_affine": M,
        }


def frame_mask(res, img_w, img_h, out_w):
    """Person segmentation in full-frame coordinates, (out_h, out_w) uint8, 255 = person.

    The landmark model segments its 256x256 crop; seg_affine maps crop pixels to image
    pixels, so a scaled copy of it warps the mask into a small frame-aligned image."""
    out_h = int(round(img_h * out_w / img_w))
    M = res["seg_affine"] * (out_w / img_w)             # crop -> image -> mask pixels
    m = cv2.warpAffine(res["seg"].astype(np.float32), M, (out_w, out_h), flags=cv2.INTER_LINEAR,
                       borderMode=cv2.BORDER_CONSTANT, borderValue=0)
    return (np.clip(m, 0, 1) * 255).astype(np.uint8)
