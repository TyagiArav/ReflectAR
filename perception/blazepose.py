"""BlazePose (full) on OpenVINO: detector + landmark model with ROI tracking.

Re-implements MediaPipe's pose pipeline around the stock .tflite models, which
OpenVINO reads directly. Outputs 33 landmarks in image pixels, 33 metric world
landmarks (hip-centred, camera-aligned) and a person segmentation mask.
"""
import math
import numpy as np
import cv2
import openvino as ov

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
    def __init__(self, core: ov.Core, model_dir="models", device="CPU", config=None):
        self.det = core.compile_model(f"{model_dir}/pose_detection.tflite", device, config or {})
        self.lm = core.compile_model(f"{model_dir}/pose_landmark_full.tflite", device, config or {})
        self.det_req = self.det.create_infer_request()
        self.lm_req = self.lm.create_infer_request()
        self.anchors = _gen_anchors()
        self.roi = None            # (center, side, rot) carried between frames

    # ---------------------------------------------------------------- detector
    def _detect(self, rgb):
        h, w = rgb.shape[:2]
        s = max(h, w)
        padx, pady = (s - w) // 2, (s - h) // 2
        sq = cv2.copyMakeBorder(rgb, pady, s - h - pady, padx, s - w - padx, cv2.BORDER_CONSTANT)
        inp = cv2.resize(sq, (DET_SIZE, DET_SIZE)).astype(np.float32) / 127.5 - 1.0
        res = self.det_req.infer({0: inp[None]})
        raw = res[self.det.output(0)][0]
        scores = sigmoid(res[self.det.output(1)][0, :, 0])
        best = int(np.argmax(scores))
        if scores[best] < 0.5:
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
        res = self.lm_req.infer({0: (crop.astype(np.float32) / 255.0)[None]})
        flag = float(res[self.lm.output(1)].ravel()[0])
        if flag > 1.0 or flag < 0.0:
            flag = float(sigmoid(flag))
        if flag < 0.5:
            return None
        raw = res[self.lm.output(0)].reshape(39, 5)
        uv = raw[:, :2] @ M[:, :2].T + M[:, 2]
        z = raw[:, 2] * side / LM_SIZE
        vis = sigmoid(raw[:, 3])
        pres = sigmoid(raw[:, 4])

        world = res[self.lm.output(4)].reshape(39, 3).copy()
        c, s = math.cos(rot), math.sin(rot)
        wx, wy = world[:, 0].copy(), world[:, 1].copy()
        world[:, 0] = c * wx - s * wy
        world[:, 1] = s * wx + c * wy

        seg = sigmoid(res[self.lm.output(2)][0, :, :, 0])

        # next-frame ROI from auxiliary keypoints 33 (hip centre) / 34 (above head)
        self.roi = roi_from_points(uv[33], uv[34])
        return {
            "uv": uv[:33], "z_px": z[:33], "vis": np.minimum(vis, pres)[:33],
            "world": world[:33], "score": flag, "seg": seg, "seg_affine": M,
        }
