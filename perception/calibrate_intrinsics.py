"""Webcam intrinsics from a printed checkerboard (OpenCV).

Print a 9x6 inner-corner checkerboard, measure the square size, hold it in front of the
camera at 15-25 different angles/distances. Press SPACE to grab a view, Q to solve.
Writes calib/intrinsics.json, which server.py loads instead of the hfov guess.

Turn OFF Center Stage (Control Centre > Video Effects) first: it crops/zooms the image and
silently changes the focal length.
"""
import argparse, json, sys
import numpy as np
import cv2

ap = argparse.ArgumentParser()
ap.add_argument("--cols", type=int, default=9)
ap.add_argument("--rows", type=int, default=6)
ap.add_argument("--square", type=float, default=0.025, help="square size in metres")
ap.add_argument("--width", type=int, default=1280)
ap.add_argument("--height", type=int, default=720)
a = ap.parse_args()

obj = np.zeros((a.rows * a.cols, 3), np.float32)
obj[:, :2] = np.mgrid[0:a.cols, 0:a.rows].T.reshape(-1, 2) * a.square
cap = cv2.VideoCapture(0, {"darwin": cv2.CAP_AVFOUNDATION, "win32": cv2.CAP_MSMF}.get(sys.platform, cv2.CAP_ANY))
cap.set(cv2.CAP_PROP_FRAME_WIDTH, a.width); cap.set(cv2.CAP_PROP_FRAME_HEIGHT, a.height)
objs, imgs, size = [], [], None
crit = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 1e-3)
while True:
    ok, f = cap.read()
    if not ok:
        continue
    g = cv2.cvtColor(f, cv2.COLOR_BGR2GRAY); size = g.shape[::-1]
    found, corners = cv2.findChessboardCorners(g, (a.cols, a.rows), cv2.CALIB_CB_FAST_CHECK)
    vis = f.copy()
    if found:
        cv2.drawChessboardCorners(vis, (a.cols, a.rows), corners, found)
    cv2.putText(vis, f"views: {len(objs)}  SPACE=grab  Q=solve", (20, 40), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 255), 2)
    cv2.imshow("calibrate", vis)
    k = cv2.waitKey(1) & 0xFF
    if k == ord(" ") and found:
        objs.append(obj); imgs.append(cv2.cornerSubPix(g, corners, (11, 11), (-1, -1), crit))
    elif k == ord("q"):
        break
cv2.destroyAllWindows()
if len(objs) < 8:
    raise SystemExit("need at least 8 views")
rms, K, dist, _, _ = cv2.calibrateCamera(objs, imgs, size, None, None)
out = {"w": size[0], "h": size[1], "fx": K[0, 0], "fy": K[1, 1], "cx": K[0, 2], "cy": K[1, 2],
       "dist": dist.ravel().tolist(), "rms_px": rms,
       "hfov_deg": float(np.degrees(2 * np.arctan(size[0] / (2 * K[0, 0]))))}
json.dump(out, open("calib/intrinsics.json", "w"), indent=2)
print(json.dumps(out, indent=2))
