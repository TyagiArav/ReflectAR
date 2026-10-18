"""Metric monocular depth (Depth Anything V2 Metric-Indoor-Small, OpenVINO IR).

Runs on its own thread at whatever rate the CPU allows (~15-20 Hz on an M5 Pro);
the main loop always reads the latest map. This is the 'single camera as a
depth camera' half of the depth estimate; see fusion in server.py.
"""
import threading, time
import numpy as np
import cv2
import openvino as ov

IN_W, IN_H = 476, 266                      # multiples of 14 (ViT patch)
MEAN = np.array([0.485, 0.456, 0.406], np.float32)
STD = np.array([0.229, 0.224, 0.225], np.float32)


class MonoDepth:
    def __init__(self, core: ov.Core, path="models/da2_metric_indoor_small.xml", device="CPU"):
        self.model = core.compile_model(path, device, {"PERFORMANCE_HINT": "LATENCY"})
        self._frame = None
        self._lock = threading.Lock()
        self.depth = None                  # (IN_H, IN_W) metres from camera
        self.stamp = 0.0
        self.hz = 0.0
        self._run = True
        threading.Thread(target=self._loop, daemon=True).start()

    def submit(self, bgr):
        with self._lock:
            self._frame = bgr

    def _loop(self):
        req = self.model.create_infer_request()
        while self._run:
            with self._lock:
                f, self._frame = self._frame, None
            if f is None:
                time.sleep(0.003)
                continue
            t0 = time.time()
            rgb = cv2.cvtColor(cv2.resize(f, (IN_W, IN_H), interpolation=cv2.INTER_AREA),
                               cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
            x = ((rgb - MEAN) / STD).transpose(2, 0, 1)[None]
            self.depth = req.infer({0: x})[self.model.output(0)][0]
            self.stamp = time.time()
            self.hz = 0.9 * self.hz + 0.1 / max(self.stamp - t0, 1e-3)

    def sample(self, u, v, img_w, img_h, r=3):
        """Median depth around image pixel (u, v) of the full-res frame."""
        d = self.depth
        if d is None:
            return None
        x = int(np.clip(u / img_w * IN_W, 0, IN_W - 1))
        y = int(np.clip(v / img_h * IN_H, 0, IN_H - 1))
        win = d[max(y - r, 0):y + r + 1, max(x - r, 0):x + r + 1]
        win = win[(win > 0.2) & (win < 8.0)]
        return float(np.median(win)) if win.size else None

    def stop(self):
        self._run = False
