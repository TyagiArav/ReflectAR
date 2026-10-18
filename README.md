# ReflectAR — v1 (MacBook prototype)

Garments drawn onto the user's reflection, from the viewer's eye position. v1 runs on the MacBook's own camera and screen, and also on Windows PCs (NVIDIA GPU recommended).

```
perception/  Python 3.11+, ONNX Runtime   webcam → BlazePose + hand + metric mono depth → TCP :5566
             (Core ML / CUDA) or OpenVINO
renderer/    Unity 6000.6 (URP)           off-axis / webcam-intrinsics camera, procedural garments, gestures UI
```

## Setup

```
cd perception
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt          # macOS
py -3.11 -m venv .venv && .venv\Scripts\pip install -r requirements.txt     # Windows
```

`requirements.txt` picks the inference runtime per platform: Core ML (Neural Engine / GPU) on macOS 14+, CUDA on Windows/Linux with an NVIDIA GPU (driver 580+; CUDA and cuDNN come as pip packages). Windows PCs without NVIDIA: swap `onnxruntime-gpu[...]` for `onnxruntime-directml`. The startup log prints what each model runs on, e.g. `pose_landmark_full=CoreML` or `=CUDA`; the first start compiles the models (Core ML: seconds; TensorRT, if installed: minutes), later starts use the cache in `models/cache/`.

## Run

1. Grant camera access to the app you launch from (macOS: System Settings → Privacy & Security → Camera; Windows: Settings → Privacy & security → Camera). On a Mac, turn **Center Stage off**: it crops the image and changes the focal length.
2. `./run_perception.sh` (Windows: `run_perception.bat`), which adds `--log` by default. Add `--snapshot dbg.jpg` to write an annotated debug frame every second.
3. Open `renderer/` in Unity, open `Assets/ReflectAR/ReflectAR.unity`, and press Play. The mirror UI waits for perception and connects on its own.

Without a camera, use `perception/.venv/bin/python perception/fake_server.py [--script swipe|fist] [--dist 0.8]`, which streams a synthetic skeleton. `server.py --video file.mp4` replays a recording.

## Using it

| Action | Gesture | Key |
|---|---|---|
| Next / previous garment | Swipe a hand left→right / right→left, above hip height | → / ← |
| Select garment | Raise a hand above the shoulder, make a fist, hold 5 s (the ring fills) | Space |
| Camera-feed ↔ mirror mode | – | M |
| Show body proxy / cube test / debug | – | B / C / D |
| Pose prediction on / off (A/B) | – | P |
| Prediction lead −5 / +5 ms (Mirror mode) | – | [ / ] |

The user must stand **1–3 m** from the screen. Outside that range a prompt shows ("Please step back" / "Come a little closer"), garments dim, and gestures are disabled.

## How it works

- **Inference backends (`backend.py`):** each model runs through ONNX Runtime on the best device it finds (Core ML on the Apple Neural Engine, which Unity doesn't use, so it neither loads the CPU nor competes with rendering; TensorRT/CUDA on NVIDIA; DirectML), or OpenVINO on the CPU. Pick per model group in `compute` in `calib/config.json`: `pose_device` / `depth_device` = `auto` | `ane` | `gpu` | `cpu`, `backend` = `auto` | `onnx` | `openvino`. CPU inference uses OpenVINO when it's installed (about 1.6× faster than ONNX Runtime's CPU path here). The `.onnx` files are converted from the originals (`tools/convert_models.py` for the MediaPipe `.tflite`, `tools/ir_to_onnx.py` for the depth IR); given the same input, both backends agree to 0.01 px. `--ep-log` shows how much of each model Core ML / TensorRT took and what fell back to the CPU.
- **Pose:** the stock MediaPipe BlazePose *full* detector and landmark models, with MediaPipe's ROI tracking re-implemented (`blazepose.py`). Each joint's 3D position comes from back-projecting its 2D pixel at a fused distance, plus BlazePose's relative per-joint depth, so the skeleton always lands exactly on the person in the image (`geometry.py`, `server.py`).
- **Depth from one camera:** Depth Anything V2 *Metric-Indoor-Small*, converted to OpenVINO IR, runs on its own thread, capped at 12 Hz (`compute.depth_max_hz`) with its own CPU-thread budget so it doesn't slow pose inference. Its torso depth is fused with a skeleton-size cue (70 % mono depth). When the two disagree, mono depth wins, because the skeleton cue is the one that glitches when limbs leave the frame.
- **Gestures:** `gestures.py`. A swipe is the wrist crossing ≥ 0.9 shoulder-widths horizontally within 0.9 s, measured on the raw 2D image relative to the shoulders, so it's independent of distance and has no filter lag. Arcing strokes are fine as long as horizontal motion dominates, and blurry frames are tolerated. With `--log`, every candidate stroke prints a `[swipe?] … reject` line with the reason. Fist = MediaPipe hand-landmark model on a crop around the raised wrist (`handpose.py`), with finger-curl ratios. A fist held still blocks swipes.
- **Latency / pose prediction:** perception sends each frame's joints *with velocities* at the frame's capture time (`filters.PosePredictor`: One-Euro at rest, a constant-velocity Kalman filter once a joint moves). Every render frame, `TrackingClient.Predict` extrapolates them to the moment that frame reaches the screen, so the garment sits on where the body is now, not where it was ~80 ms ago, and moves at the render rate rather than in 30 Hz steps. In *CameraFeed* mode the overlay is instead matched to the capture time of the image shown under it. Clocks are synced from each frame's send time. Measured end to end with a known trajectory and 60 ms latency: fast-motion error halves (110 → 55 mm), slow motion −20 %, rest jitter < 1 mm/frame. The rest of the pipeline was cut too: JSON goes out before the JPEG (now encoded on the network thread), the fist classifier runs on its own thread, and pose and depth get separate OpenVINO thread budgets. Together these took capture→Unity from 141 ms to 19 ms on a 4-core test box. Tuning lives in `predict`, `filters` and `camera.latency_s` in `calib/config.json`. Debug line (D) shows latency and prediction horizon.
- **Renderer:** `MirrorApp.cs` has two modes. *CameraFeed* places a virtual camera at the webcam pose with its intrinsics, mirrored, over the live JPEG feed. *Mirror* renders the reflected `MirrorWorld` with a Kooima off-axis frustum from the tracked eye; this is the mode the two-way-mirror build will use.
- **Garments (upper body only):** 10 tops in `GarmentCatalog.cs`: tee, V-neck, Breton long-sleeve, polo, flannel, hoodie, bomber, denim jacket, puffer, tank.
  - *Mesh* (`GarmentMesher.cs`): each garment is generated per user as one seamless surface. A signed-distance field smoothly blends the torso, shoulders, sleeves and collar, so armpits and shoulders have no gaps. It is meshed with surface nets and cut open at the neck, hem and cuffs.
  - *Physics* (`ClothGarment.cs`): an XPBD cloth solver, with near-inextensible stretch, soft compression so the fabric buckles into wrinkles, and bending, gravity, inertia and damping. It collides with capsules fitted to the body. The cloth is tethered to a version of the mesh skinned to six tracked bones; the tether is ~0 at the collar and shoulders and grows toward the hem and cuffs. Fabric parameters (denim stiff, jersey soft, puffer thick) are per garment.
  - *Shading* (`ReflectAR_Garment.shader`): seamless triplanar fabric anchored to the rest pose, so patterns stick to the cloth. It adds sheen, weave shading, and procedural zippers and button plackets. The solver costs ~6 ms per frame in the Editor (Release code optimization).

## Calibration

- **Distance (required, done 2026-10-16 at 1.0 m and 2.0 m):** stand exactly at a measured distance and run `./run_perception.sh --calib-dist <metres>`. Each run adds a point to `perception/calib/depth_calib.json`; two points fit scale + offset for each cue. Without this file the 1–3 m prompts are wrong by up to 40 %.

- **Prediction lead (optional, Mirror mode):** wave a hand and press [ / ] until the garment stops trailing or leading the reflection, then add the shown lead to `camera.latency_s` (or `predict.display_latency_s`) in `calib/config.json`.

## Calibration TODO

- `perception/calibrate_intrinsics.py` (9×6 checkerboard) → `calib/intrinsics.json`. Until then, the FOV comes from `hfov_deg` in `calib/config.json`.
- `camera_pose` and `screen` in `calib/config.json` assume a 14" MacBook Pro. Change them for the 43" mirror build.
- Absolute distance with one camera is approximate (±10–20 %). Fusion helps, but a real stereo/ToF camera is still the target for the mirror build.
