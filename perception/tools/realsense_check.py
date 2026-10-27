"""RealSense sanity check: opens the camera the way server.py does, prints the mode it got,
the factory intrinsics and the depth at the image centre for a few seconds, and writes a
colour + depth snapshot.

Run from perception/ (macOS needs sudo):  sudo .venv/bin/python tools/realsense_check.py
Try another mode:  ... realsense_check.py 640x480 480x270 30   (colour, depth, fps)
"""
import json, os, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import numpy as np
import cv2
import realsense

here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rc = json.load(open(os.path.join(here, "calib", "config.json")))["camera"].get("realsense", {})
if len(sys.argv) == 4:
    c, d = ([int(x) for x in a.split("x")] for a in sys.argv[1:3])
    rc = {**rc, "usb2": {"color": c, "depth": d, "fps": int(sys.argv[3])},
          "usb3": {"color": c, "depth": d, "fps": int(sys.argv[3])}}
cam = realsense.RealSenseCamera(rc)
dep = realsense.SensorDepth(cam)
print(cam.desc)
print("intrinsics", {k: round(v, 1) for k, v in cam.intrinsics.items()})
seq, t0, n, lags = 0, time.time(), 0, []
while time.time() - t0 < 5:
    frame, stamp, seq = cam.read(seq)
    n += 1
    lags.append(time.time() - stamp)
    H, W = frame.shape[:2]
    valid = np.count_nonzero(cam.depth) / cam.depth.size
    if n % 15 == 0:
        print(f"centre depth {dep.sample(W / 2, H / 2, W, H)} m, {valid:.0%} of pixels have depth")
print(f"{n / (time.time() - t0):.1f} fps, exposure -> here {1000 * np.median(lags):.0f} ms")
vis = cv2.applyColorMap(cv2.convertScaleAbs(cam.depth, alpha=255 / (4.0 / cam.depth_scale)), cv2.COLORMAP_TURBO)
out = os.path.join(tempfile.gettempdir(), "realsense_check.jpg")   # not the repo: written as root
cv2.imwrite(out, np.hstack([frame, cv2.resize(vis, (W, H))]))
print("wrote", out)
