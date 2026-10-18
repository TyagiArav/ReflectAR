# ReflectAR

A virtual try-on mirror. ReflectAR tracks your body through a single webcam and draws simulated garments onto your reflection, rendered from your own eye position so they line up the way a real mirror image would.

v1 is a computer prototype: it runs on the computer's own camera and screen. It runs on Macs (Apple Silicon recommended) and Windows PCs (NVIDIA GPU recommended), and on Linux. The same pipeline is meant to drive a 43" two-way-mirror build later.

The system has two programs that talk over a local TCP socket:

```
 webcam ──► perception/ (Python)  ──TCP 127.0.0.1:5566──►  renderer/ (Unity)  ──► screen
            pose, hands, depth,                             off-axis camera, cloth
            gestures, filtering                             garments, occlusion, UI
```

---

## Technologies used

### Perception service (`perception/`)

| Technology | What it's used for |
|---|---|
| **Python 3.11+** | The perception service and its tools |
| **ONNX Runtime** | Runs the neural networks on the best device available: **Core ML** (Apple Neural Engine or GPU) on Macs, **TensorRT / CUDA** on NVIDIA GPUs, **DirectML** on other Windows GPUs (`backend.py`) |
| **OpenVINO** | CPU inference. It's used whenever a model runs on the CPU, because it's about 1.6× faster there than ONNX Runtime's CPU path |
| **OpenCV** (`opencv-python`) | Webcam capture, image warping, JPEG encoding, checkerboard calibration |
| **NumPy** | All geometry, filtering and tensor pre/post-processing |
| **MediaPipe BlazePose (full)** | Body detection, 33-joint landmarks and the person segmentation mask. MediaPipe's ROI tracking is re-implemented in `blazepose.py`, so the MediaPipe package itself is **not** needed |
| **MediaPipe Hand Landmarker** | Fist detection on a crop around the raised wrist |
| **Depth Anything V2 Metric-Indoor-Small** | Metric depth from a single camera |
| **One-Euro filter + constant-velocity Kalman filter** | Smoothing joints and predicting pose ahead in time (`filters.py`) |
| **TensorFlow + tf2onnx, ONNX** | Only used by `tools/` to convert the original models to `.onnx`. Not needed to run ReflectAR |

All model files are already committed in `perception/models/`, in two forms: `.onnx` for ONNX Runtime, and the originals (`.tflite`, plus the OpenVINO IR `.xml`/`.bin` for depth) for OpenVINO. Nothing has to be downloaded.

### Renderer (`renderer/`)

| Technology | What it's used for |
|---|---|
| **Unity 6 (6000.6.6f1)** | The real-time renderer and app |
| **Universal Render Pipeline (URP) 17.6** | Rendering, plus three custom shaders (`ReflectAR_Garment`, `ReflectAR_Feed`, `ReflectAR_DepthOnly`) |
| **Unity Input System 1.20** | Keyboard controls |
| **uGUI** | On-screen prompts and the garment carousel |
| **C#** | Everything in `Assets/ReflectAR/Scripts/` |
| **Kooima off-axis projection** | Renders the scene from the tracked eye position, as a mirror would show it |
| **Signed-distance fields + surface nets** | Generates each garment as one seamless mesh fitted to the user (`GarmentMesher.cs`) |
| **XPBD cloth simulation** | Custom position-based cloth solver with body-capsule collisions (`ClothGarment.cs`) |
| **Mask-trimmed depth occluders** | Hide the garment behind your hands, arms and head, trimmed per pixel to the person mask (`BodyProxy.cs`) |

---

## Repository layout

```
ReflectAR/
├── run_perception.sh            # starts the perception service (macOS / Linux)
├── run_perception.bat           # starts the perception service (Windows)
├── perception/
│   ├── requirements.txt         # Python dependencies, with the right runtime per platform
│   ├── server.py                # main service: webcam → tracking → TCP stream
│   ├── backend.py               # ONNX Runtime / OpenVINO model loading and device choice
│   ├── fake_server.py           # synthetic skeleton stream, for testing without a camera
│   ├── calibrate_intrinsics.py  # checkerboard camera calibration
│   ├── blazepose.py             # BlazePose detector + landmarks + person mask + ROI tracking
│   ├── handpose.py              # hand landmarks + fist classifier
│   ├── depth.py                 # Depth Anything V2 metric depth (own thread)
│   ├── geometry.py              # back-projection, camera → mirror transform
│   ├── filters.py               # One-Euro / Kalman pose predictor
│   ├── gestures.py              # swipe + fist-hold detection
│   ├── calib/                   # config.json (all tuning) + depth_calib.json
│   ├── models/                  # .onnx, .tflite and OpenVINO IR model files
│   └── tools/                   # model converters (only needed if a model changes)
└── renderer/                    # Unity project
    └── Assets/ReflectAR/
        ├── ReflectAR.unity      # the main scene
        ├── Scripts/             # MirrorApp, TrackingClient, BodyRig, BodyProxy, garments, UI
        └── Resources/           # shaders
```

---

## Setup

### 1. Requirements

- **Git**
- **Python 3.11 or newer**, 64-bit (tested with 3.11 and 3.13)
- **Unity Hub** and **Unity 6000.6.6f1**. Install it from Unity Hub → *Installs* → *Install Editor* (use the *Archive* tab if that exact version isn't listed). A newer 6000.x editor will usually upgrade the project fine.
- A webcam, optional: you can test everything without one (see [Running without a camera](#running-without-a-camera)).
- For accelerated inference, one of:
  - a Mac with **macOS 14+** (Apple Silicon) for Core ML,
  - an **NVIDIA GPU with driver 580+**, on Windows or Linux. CUDA and cuDNN are installed as pip packages, so there's no separate CUDA install,
  - any **DirectX 12 GPU** on Windows, via DirectML.

  Without any of these, everything runs on the CPU through OpenVINO. A 4+ core CPU is recommended.

### 2. Clone the repository

```bash
git clone https://github.com/tyagiarav/reflectar.git ReflectAR
cd ReflectAR
```

### 3. Set up the Python environment

The virtual environment must live at `perception/.venv`, because the launch scripts look for it there.

**macOS / Linux**

```bash
cd perception
python3 -m venv .venv
.venv/bin/pip install --upgrade pip
.venv/bin/pip install -r requirements.txt
cd ..
```

**Windows (PowerShell)**

```powershell
cd perception
py -3.11 -m venv .venv
.venv\Scripts\pip install --upgrade pip
.venv\Scripts\pip install -r requirements.txt
cd ..
```

`requirements.txt` picks the inference runtime for your platform:

| Platform | Installs | Runs on |
|---|---|---|
| macOS | `onnxruntime` | Core ML (Neural Engine / GPU) |
| Windows / Linux | `onnxruntime-gpu[cuda,cudnn]` | NVIDIA GPU via CUDA (falls back to the CPU without one) |
| All | `openvino`, `opencv-python`, `numpy` | CPU inference, camera, maths |

- **Windows without an NVIDIA GPU:** in `requirements.txt`, replace the `onnxruntime-gpu[cuda,cudnn]` line with `onnxruntime-directml` before installing.
- **Fastest NVIDIA path (optional):** also install TensorRT 10. The first start then builds TensorRT engines, which takes minutes; they're cached after that.
- **CPU-only Linux:** the `onnxruntime-gpu` CUDA wheels are large. To skip them, remove that line; everything then runs on OpenVINO.

Quick check that the models load (run from `perception/`; on Windows use `.venv\Scripts\python`):

```bash
.venv/bin/python -c "import json; from backend import Backend; from blazepose import BlazePose; from handpose import HandPose; from depth import MonoDepth; b = Backend(json.load(open('calib/config.json'))['compute']); BlazePose(b); HandPose(b); MonoDepth(b); print(b.summary())"
```

It prints which device each model ended up on, for example `pose_landmark_full=CoreML`, `=CUDA` or `=OpenVINO CPU`.

### 4. Open the Unity project

1. Open Unity Hub → *Projects* → *Add* → *Add project from disk*, and choose the `renderer/` folder (not the repository root).
2. Open it with Unity 6000.6.6f1. The first import takes a few minutes while Unity regenerates `Library/`. The packages in `Packages/manifest.json` (URP, Input System, uGUI) download automatically.
3. In the Project window, open `Assets/ReflectAR/ReflectAR.unity`.

### 5. Camera permissions

- **macOS:** grant camera access to the app you start perception from, such as Terminal, iTerm or VS Code: *System Settings → Privacy & Security → Camera*. Turn **Center Stage off** (*Control Centre → Video Effects*). It crops the image and changes the focal length, which breaks the geometry.
- **Windows:** *Settings → Privacy & security → Camera*, and allow desktop apps to use the camera. Close other apps that are using it.

The camera API is chosen automatically (AVFoundation on macOS, Media Foundation on Windows, OpenCV's default on Linux). To force one, set `camera.api` in `perception/calib/config.json` to `avfoundation`, `msmf`, `dshow`, `v4l2` or `any`.

---

## Running

Start the two halves in either order. The Unity app waits for perception and connects on its own.

**1. Start perception**

```bash
./run_perception.sh          # macOS / Linux
run_perception.bat           # Windows
```

Both run `server.py --log` from inside `perception/`, and pass any extra flags through. To run it by hand, `cd perception` first, because paths are relative.

Startup prints a `[backend] …` line saying which device each model runs on, a `[cam] …` line, and `[net] listening on 127.0.0.1:5566`. The first start also compiles the models (Core ML: seconds; TensorRT: minutes). Later starts use the cache in `perception/models/cache/`.

**2. Start the renderer:** in Unity, with `ReflectAR.unity` open, press **Play**.

Stand **1–3 m** from the screen. Outside that range a prompt shows ("Please step back" / "Come a little closer"), garments dim, and gestures are disabled.

### Useful `server.py` flags

| Flag | Effect |
|---|---|
| `--log` | Print tracking state every second, plus a `[swipe?] … reject` line explaining every rejected swipe |
| `--snapshot dbg.jpg` | Write an annotated debug frame every second |
| `--video file.mp4` | Replay a recording instead of the webcam |
| `--no-image` | Don't stream camera JPEGs to Unity |
| `--calib-dist <m>` | Distance calibration (see below); exits when done |
| `--ep-log` | Show how much of each model Core ML / TensorRT took and what fell back to the CPU |
| `--config <path>` | Use a different config file (default `calib/config.json`) |

### Running without a camera

`fake_server.py` streams a synthetic walking and waving skeleton in the same wire format, so you can work on the renderer anywhere (on Windows use `.venv\Scripts\python`):

```bash
cd perception
.venv/bin/python fake_server.py                   # idle skeleton at 1.8 m
.venv/bin/python fake_server.py --script swipe    # repeatedly triggers swipes
.venv/bin/python fake_server.py --script fist     # triggers the fist-hold select
.venv/bin/python fake_server.py --dist 0.8        # stand too close, to test range prompts
```

Then press Play in Unity.

---

## Controls

| Action | Gesture | Key |
|---|---|---|
| Next / previous garment | Swipe a hand left→right / right→left, above hip height | → / ← |
| Select garment | Raise a hand above the shoulder, make a fist, hold 5 s (the ring fills) | Space |
| Camera-feed ↔ mirror mode | – | M |
| Show body proxy / cube test / debug overlay | – | B / C / D |
| Pose prediction on / off (A/B test) | – | P |
| Occlusion trimmed to the person mask on / off (A/B test) | – | O |
| Prediction lead −5 / +5 ms (Mirror mode) | – | [ / ] |
| Quit (in a build) | – | Esc |

There are 10 upper-body garments (`GarmentCatalog.cs`): tee, V-neck, Breton long-sleeve, polo, flannel, hoodie, bomber, denim jacket, puffer and tank.

---

## Configuration and calibration

All tuning lives in `perception/calib/config.json`. Each section has a `_note` explaining its fields.

- **Inference device (`compute`):** `backend` = `auto` | `onnx` | `openvino`, and `pose_device` / `depth_device` = `auto` | `ane` | `gpu` | `cpu`. `auto` means the Neural Engine on Macs, the GPU where an NVIDIA or DirectML GPU is available, and otherwise the CPU. `pose_threads` / `depth_threads` set CPU thread budgets (0 = automatic).
- **Distance (required on a new machine or camera):** stand exactly at a measured distance and run `./run_perception.sh --calib-dist <metres>`. Do this at two distances, such as 1.0 m and 2.0 m. Each run adds a point to `perception/calib/depth_calib.json`, and two points fit a scale and offset for each depth cue. Without it, the 1–3 m prompts can be off by up to 40 %. The committed file was calibrated on the original MacBook.
- **Camera intrinsics (recommended):** print a 9×6 inner-corner checkerboard, measure one square, then run `.venv/bin/python calibrate_intrinsics.py --square <metres>` from `perception/`. Show the board at 15–25 angles and distances, pressing SPACE for each one, then press Q to solve. This writes `calib/intrinsics.json`. Until you do, the field of view comes from `camera.hfov_deg`.
- **Screen and camera position:** `screen` (physical size in metres and resolution) and `camera_pose` (the camera's position relative to the screen's lower-left corner) default to a 14" MacBook Pro. Measure and change them for any other display, including the 43" mirror build.
- **Camera:** `camera.index` picks the camera if the wrong one opens, and `camera.api` the capture API (see [Camera permissions](#5-camera-permissions)).
- **Occlusion mask:** `net.mask_width` is the resolution of the person mask sent to Unity (0 turns it off).
- **Prediction lead (optional, Mirror mode):** wave a hand and press [ / ] until the garment stops trailing or leading the reflection. Then add the shown lead to `camera.latency_s` (or `predict.display_latency_s`).

Absolute distance from one camera is approximate (±10–20 %). Fusion helps, but a real stereo or ToF camera is the target for the mirror build.

---

## How it works

- **Inference backends (`backend.py`):** each model loads through ONNX Runtime on the best device it finds, or through OpenVINO on the CPU. On a Mac, the Neural Engine is a good fit because Unity doesn't use it, so inference neither loads the CPU nor competes with rendering. Given the same input, both backends agree to 0.01 px. The `.onnx` files were converted from the originals with `tools/convert_models.py` (the MediaPipe `.tflite` models) and `tools/ir_to_onnx.py` (the depth IR); you only need to re-run them if a model changes.
- **Pose:** the stock MediaPipe BlazePose *full* detector and landmark models, with MediaPipe's ROI tracking re-implemented (`blazepose.py`). Each joint's 3D position comes from back-projecting its 2D pixel at a fused distance, plus BlazePose's relative per-joint depth, so the skeleton always lands exactly on the person in the image (`geometry.py`, `server.py`).
- **Depth from one camera:** Depth Anything V2 runs on its own thread, capped at 12 Hz (`compute.depth_max_hz`), so it doesn't slow pose inference. Its torso depth is fused with a skeleton-size cue (70 % mono depth). When the two disagree, mono depth wins, because the skeleton cue is the one that glitches when limbs leave the frame.
- **Gestures (`gestures.py`):** a swipe is the wrist crossing ≥ 0.9 shoulder-widths horizontally within 0.9 s, measured in the 2D image relative to the shoulders, so it doesn't depend on distance and has no filter lag. A fist is detected by the hand-landmark model on a crop around the raised wrist, using finger-curl ratios (`handpose.py`). A fist held still blocks swipes.
- **Latency and pose prediction:** each frame's joints are sent *with velocities* at the frame's capture time (`filters.PosePredictor`: One-Euro at rest, a constant-velocity Kalman filter once a joint moves). Every render frame, `TrackingClient.Predict` extrapolates them to the moment that frame reaches the screen, so the garment sits where the body is now and moves at the render rate rather than in 30 Hz steps. Capture-to-Unity latency was about 19 ms on a 4-core machine with CPU inference.
- **Occlusion (`BodyProxy.cs`, `ReflectAR_DepthOnly.shader`):** depth-only stand-ins for the body hide whatever part of the garment is behind it. Parts under the garment (torso, neck, sleeved arms) are fitted capsules. Exposed parts (bare forearms, hands, head with hair) are drawn oversized, then trimmed per pixel to BlazePose's person mask, so the cut-out follows your real silhouette. On the test clip this covers 99 % of the real hands, forearms and head (capsules alone: 87 %). B shows the proxies, with the trimmed parts tinted warm.
- **Wire format:** TCP, with Python as the server. Each message is 1 type byte, a 4-byte big-endian length, then the payload: `J` = JSON tracking frame; `I` = float64 capture time + JPEG; `M` = float64 capture time + uint16 width + uint16 height + the person mask (uint8, 255 = person).
- **Renderer modes (`MirrorApp.cs`):** *CameraFeed* places a virtual camera at the webcam pose with its intrinsics, mirrored, over the live feed. *Mirror* renders the reflected world with a Kooima off-axis frustum from the tracked eye; this is the mode the two-way-mirror build will use.
- **Garments:** each one is generated per user as a single seamless surface. A signed-distance field blends the torso, shoulders, sleeves and collar, which is then meshed with surface nets and cut open at the neck, hem and cuffs (`GarmentMesher.cs`). An XPBD cloth solver adds stretch, buckling, bending, gravity and damping, collides with capsules fitted to the body, and is tethered to a skinned copy of the mesh (`ClothGarment.cs`). Fabric parameters are set per garment. The shader adds triplanar fabric that sticks to the cloth, sheen, weave, and procedural zippers and plackets (`ReflectAR_Garment.shader`).

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `Could not open camera` | macOS: grant camera permission to your terminal app. Windows: check camera privacy settings, close other apps using the camera, or set `"api": "dshow"` in `camera` in `config.json`. Also check `camera.index` |
| `FileNotFoundError` for `calib/config.json` or `models/...` | Run `server.py` from inside `perception/`, or use the launch script |
| `.venv/bin/python: No such file or directory` | The virtual environment must be at `perception/.venv`; redo step 3 |
| Models run on the CPU although you have a GPU | Check the `[backend]` startup line. NVIDIA: update the driver to 580+. Windows without NVIDIA: install `onnxruntime-directml` (step 3). Run with `--ep-log` for details |
| `[backend] … TensorrtExecutionProvider failed … trying without it` | TensorRT isn't fully installed. This is harmless: it falls back to CUDA |
| Unity stays on "Starting… Waiting for the perception service" | Check that perception printed `[net] listening on 127.0.0.1:5566` and nothing else uses port 5566. Host and port are set in `net` in `config.json` and on the `TrackingClient` component |
| Distance prompts are wrong | Redo the distance calibration on this machine |
| Garments are offset from your body | Set `screen` and `camera_pose` for your display, and run the intrinsics calibration |
| First start is slow | Models are compiled on first run and cached in `perception/models/cache/`. TensorRT can take minutes; later starts are faster |
