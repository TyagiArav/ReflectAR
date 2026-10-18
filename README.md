# ReflectAR — v1 (MacBook prototype)

Garments drawn onto the user's reflection, from the viewer's eye position. v1 runs on the MacBook's own camera and screen.

```
perception/  Python 3.11 + OpenVINO     webcam → BlazePose + hand + metric mono depth → TCP :5566
renderer/    Unity 6000.6 (URP)         off-axis / webcam-intrinsics camera, procedural garments, gestures UI
```

## Run

1. Grant camera access to the app you launch from (System Settings → Privacy & Security → Camera). Turn **Center Stage off**: it crops the image and changes the focal length.
2. `./run_perception.sh`, which adds `--log` by default. Add `--snapshot /tmp/dbg.jpg` to write an annotated debug frame every second.
3. Open `renderer/` in Unity, open `Assets/ReflectAR/ReflectAR.unity`, and press Play. The mirror UI waits for perception and connects on its own.

Without a camera, use `perception/.venv/bin/python perception/fake_server.py [--script swipe|fist] [--dist 0.8]`, which streams a synthetic skeleton. `server.py --video file.mp4` replays a recording.

## Using it

| Action | Gesture | Key |
|---|---|---|
| Next / previous garment | Swipe a hand left→right / right→left, above hip height | → / ← |
| Select garment | Raise a hand above the shoulder, make a fist, hold 5 s (the ring fills) | Space |
| Camera-feed ↔ mirror mode | – | M |
| Show body proxy / cube test / debug | – | B / C / D |

The user must stand **1–3 m** from the screen. Outside that range a prompt shows ("Please step back" / "Come a little closer"), garments dim, and gestures are disabled.

## How it works

- **Pose:** the stock MediaPipe BlazePose *full* detector and landmark `.tflite` models, run by OpenVINO, with MediaPipe's ROI tracking re-implemented (`blazepose.py`). Each joint's 3D position comes from back-projecting its 2D pixel at a fused distance, plus BlazePose's relative per-joint depth, so the skeleton always lands exactly on the person in the image (`geometry.py`, `server.py`).
- **Depth from one camera:** Depth Anything V2 *Metric-Indoor-Small*, converted to OpenVINO IR, runs on its own thread (~19 Hz on the M5 Pro CPU). Its torso depth is fused with a skeleton-size cue (70 % mono depth). When the two disagree, mono depth wins, because the skeleton cue is the one that glitches when limbs leave the frame.
- **Gestures:** `gestures.py`. A swipe is the wrist crossing ≥ 0.9 shoulder-widths horizontally within 0.9 s, measured on the raw 2D image relative to the shoulders, so it's independent of distance and has no filter lag. Arcing strokes are fine as long as horizontal motion dominates, and blurry frames are tolerated. With `--log`, every candidate stroke prints a `[swipe?] … reject` line with the reason. Fist = MediaPipe hand-landmark model on a crop around the raised wrist (`handpose.py`), with finger-curl ratios. A fist held still blocks swipes.
- **Renderer:** `MirrorApp.cs` has two modes. *CameraFeed* places a virtual camera at the webcam pose with its intrinsics, mirrored, over the live JPEG feed. *Mirror* renders the reflected `MirrorWorld` with a Kooima off-axis frustum from the tracked eye; this is the mode the two-way-mirror build will use.
- **Garments (upper body only):** 10 tops in `GarmentCatalog.cs`: tee, V-neck, Breton long-sleeve, polo, flannel, hoodie, bomber, denim jacket, puffer, tank.
  - *Mesh* (`GarmentMesher.cs`): each garment is generated per user as one seamless surface. A signed-distance field smoothly blends the torso, shoulders, sleeves and collar, so armpits and shoulders have no gaps. It is meshed with surface nets and cut open at the neck, hem and cuffs.
  - *Physics* (`ClothGarment.cs`): an XPBD cloth solver, with near-inextensible stretch, soft compression so the fabric buckles into wrinkles, and bending, gravity, inertia and damping. It collides with capsules fitted to the body. The cloth is tethered to a version of the mesh skinned to six tracked bones; the tether is ~0 at the collar and shoulders and grows toward the hem and cuffs. Fabric parameters (denim stiff, jersey soft, puffer thick) are per garment.
  - *Shading* (`ReflectAR_Garment.shader`): seamless triplanar fabric anchored to the rest pose, so patterns stick to the cloth. It adds sheen, weave shading, and procedural zippers and button plackets. The solver costs ~6 ms per frame in the Editor (Release code optimization).

## Calibration

- **Distance (required, done 2026-10-16 at 1.0 m and 2.0 m):** stand exactly at a measured distance and run `./run_perception.sh --calib-dist <metres>`. Each run adds a point to `perception/calib/depth_calib.json`; two points fit scale + offset for each cue. Without this file the 1–3 m prompts are wrong by up to 40 %.

## Calibration TODO

- `perception/calibrate_intrinsics.py` (9×6 checkerboard) → `calib/intrinsics.json`. Until then, the FOV comes from `hfov_deg` in `calib/config.json`.
- `camera_pose` and `screen` in `calib/config.json` assume a 14" MacBook Pro. Change them for the 43" mirror build.
- Absolute distance with one camera is approximate (±10–20 %). Fusion helps, but a real stereo/ToF camera is still the target for the mirror build.
