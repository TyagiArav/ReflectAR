"""MediaPipe hand-landmark model on OpenVINO, cropped from BlazePose wrist/index/pinky.

Used only to tell a closed fist from an open hand for the 'select' gesture.
"""
import math
import numpy as np
import cv2
import openvino as ov
from blazepose import roi_affine, sigmoid

HAND_SIZE = 224
# (mcp, tip) per finger: index, middle, ring, pinky
FINGERS = [(5, 8), (9, 12), (13, 16), (17, 20)]

# BlazePose landmark ids per side: wrist, pinky, index
POSE_HAND = {"left": (15, 17, 19), "right": (16, 18, 20)}


class HandPose:
    def __init__(self, core: ov.Core, model_dir="models", device="CPU", threads=2):
        cfg = {"INFERENCE_NUM_THREADS": threads, "NUM_STREAMS": 1, "PERFORMANCE_HINT": "LATENCY"}
        self.model = core.compile_model(f"{model_dir}/hand_landmark_full.tflite", device, cfg)
        self.req = self.model.create_infer_request()

    @staticmethod
    def roi_from_pose(uv, side):
        w, p, i = (uv[k] for k in POSE_HAND[side])
        mid = (2 * i + p) / 3.0
        v = mid - w
        n = float(np.hypot(*v))
        if n < 2:
            return None
        rot = math.pi / 2 - math.atan2(-v[1], v[0])
        center = w + 0.7 * v
        return center, 3.2 * n, rot

    def __call__(self, rgb, roi):
        center, side, rot = roi
        M = roi_affine(center, side, rot, HAND_SIZE)
        crop = cv2.warpAffine(rgb, M, (HAND_SIZE, HAND_SIZE),
                              flags=cv2.INTER_LINEAR | cv2.WARP_INVERSE_MAP)
        res = self.req.infer({0: (crop.astype(np.float32) / 255.0)[None]})
        flag = float(res[self.model.output(1)].ravel()[0])
        if flag > 1.0 or flag < 0.0:
            flag = float(sigmoid(flag))
        lm = res[self.model.output(0)].reshape(21, 3)
        uv = lm[:, :2] @ M[:, :2].T + M[:, 2]
        return {"uv": uv, "score": flag, "curl": finger_curl(lm[:, :2])}


def finger_curl(pts):
    """Per finger: |tip-wrist| / |mcp-wrist|. ~2 when extended, <1.3 when curled."""
    w = pts[0]
    return np.array([np.linalg.norm(pts[t] - w) / max(np.linalg.norm(pts[m] - w), 1e-3)
                     for m, t in FINGERS])


def is_fist(hand, min_score=0.6):
    if hand is None or hand["score"] < min_score:
        return None                       # unknown
    curled = int(np.sum(hand["curl"] < 1.3))
    extended = int(np.sum(hand["curl"] > 1.6))
    if curled >= 3:
        return True
    if extended >= 3:
        return False
    return None
