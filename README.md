# ReflectAR

A virtual try-on mirror. ReflectAR tracks your body through a single webcam and draws simulated garments onto your reflection, rendered from your own eye position so they line up the way a real mirror image would.

v1 is a computer prototype: it runs on the computer's own camera and screen. It runs on Macs (Apple Silicon recommended) and Windows PCs (NVIDIA GPU recommended), and on Linux. The same pipeline is meant to drive a 43" two-way-mirror build later.

The system has two programs that talk over a local TCP socket:

```
 webcam ──► perception/ (Python)  ──TCP 127.0.0.1:5566──►  renderer/ (Unity)  ──► screen
            pose, hands, depth,                             off-axis camera, cloth
            gestures, filtering                             garments, occlusion, UI
                                                            (HDRP or URP)
```

---

## Technologies used

### Perception service (`perception/`)

| Technology | What it's used for |
|---|---|
| **Python 3.11+** | The perception service and its tools |
| **ONNX Runtime** | Runs the neural networks on the best device available: **Core ML** (Apple Neural Engine or GPU) on Macs, **TensorRT / CUDA** on NVIDIA GPUs, **DirectML** on other Windows GPUs (`backend.py`) |
| **OpenVINO** | CPU inference. It's used whenever a model runs on the CPU, because it's about 1.6× faster there than ONNX Runtime's CPU path |
| **OpenCV** (`opencv-python`) | Webcam capture, image warping and resizing, checkerboard calibration |
| **NumPy** | All geometry, filtering and tensor pre/post-processing |
| **MediaPipe BlazePose (full)** | Body detection, 33-joint landmarks and the person segmentation mask. MediaPipe's ROI tracking is re-implemented in `blazepose.py`, so the MediaPipe package itself is **not** needed |
| **MediaPipe Hand Landmarker** | Fist detection on a crop around the raised wrist |
| **Depth Anything V2 Metric-Indoor-Small** | Metric depth from a single camera |
| **One-Euro filter + constant-velocity Kalman filter** | Smoothing joints and predicting pose ahead in time (`filters.py`) |
| **TensorFlow + tf2onnx, ONNX** | Only used by `tools/` to convert the original models to `.onnx`. Not needed to run ReflectAR |

All model files are already committed in `perception/models/`, in two forms: `.onnx` for ONNX Runtime, and the originals (`.tflite`, plus the OpenVINO IR `.xml`/`.bin` for depth) for OpenVINO. The pose and hand models also have a `_fp16.onnx` copy for the Apple Neural Engine. Nothing has to be downloaded.

### Renderer (`renderer/`)

| Technology | What it's used for |
|---|---|
| **Unity 6 (6000.6.6f1)** | The real-time renderer and app |
| **High Definition Render Pipeline (HDRP) 17.6** | Default look: `HDRP/Lit` fabrics, soft shadows, SSAO and contact shadows (`HdrpLook.cs`, `GarmentMaterials.cs`) |
| **Universal Render Pipeline (URP) 17.6** | Lighter alternative, switchable from the *ReflectAR > Render Pipeline* menu; custom shaders `ReflectAR_Garment`, `ReflectAR_Feed`, `ReflectAR_DepthOnly` |
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
├── run_mirror.sh                # macOS: perception + the standalone app (lowest CPU / battery)
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
2. Open it with Unity 6000.6.6f1. The first import takes a few minutes while Unity regenerates `Library/`. The packages in `Packages/manifest.json` (HDRP, URP, Input System, uGUI) download automatically.
3. In the Project window, open `Assets/ReflectAR/ReflectAR.unity`.
4. The first time the project opens with HDRP installed, `Editor/PipelineSetup.cs` creates `Assets/Settings/HDRP/ReflectAR_HDRP.asset`, makes HDRP the active pipeline and writes the garment material templates to `Resources/HDRP/`. **ReflectAR > Render Pipeline > URP / HDRP** switches between the two (the URP assets are kept). If HDRP complains about missing settings, run **Window > Rendering > HDRP Wizard > Fix All** once.

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

**Lowest CPU / battery (macOS):** build the app once (Unity: *File > Build Profiles > macOS*, output `renderer/Builds/ReflectAR.app`), then run `./run_mirror.sh`. It starts perception and the app together, and stops perception when you quit the app with **Esc**. The Unity Editor redraws its own windows every frame: in Play mode it measured 85–220 % CPU against ~55 % for the app.

Stand **1–3 m** from the screen. Outside that range a prompt shows ("Please step back" / "Come a little closer"), garments dim, and gestures are disabled.

### Useful `server.py` flags

| Flag | Effect |
|---|---|
| `--log` | Print tracking state every second, plus a `[swipe?] … reject` line explaining every rejected swipe |
| `--snapshot dbg.jpg` | Write an annotated debug frame every second |
| `--video file.mp4` | Replay a recording instead of the webcam |
| `--no-image` | Don't stream the camera image to Unity |
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

- **Inference backends (`backend.py`):** each model loads through ONNX Runtime on the best device it finds, or through OpenVINO on the CPU. On a Mac, the Neural Engine is a good fit because Unity doesn't use it, so inference neither loads the CPU nor competes with rendering. Given the same input, both backends agree to 0.01 px in float32. The Neural Engine only runs float16, so on `ane` the pose and hand models load from their `_fp16.onnx` copies; Core ML would otherwise run a float32 model on its CPU path. Those copies move pose landmarks by ~0.7–1 px on 1280×720 frames (visible joints) and hand landmarks by ~0.1 px. Set `pose_device` to `gpu` for float32 on the Apple GPU instead. The `.onnx` files were converted from the originals with `tools/convert_models.py` (the MediaPipe `.tflite` models) and `tools/ir_to_onnx.py` (the depth IR); you only need to re-run them if a model changes.
- **Pose:** the stock MediaPipe BlazePose *full* detector and landmark models, with MediaPipe's ROI tracking re-implemented (`blazepose.py`). Each joint's 3D position comes from back-projecting its 2D pixel at a fused distance, plus BlazePose's relative per-joint depth, so the skeleton always lands exactly on the person in the image (`geometry.py`, `server.py`).
- **Depth from one camera:** Depth Anything V2 *Metric-Indoor-Small*, converted to OpenVINO IR, runs on its own thread, capped at 2 Hz (`compute.depth_max_hz`; 0.5 Hz when nobody is in view) with its own CPU-thread budget so it doesn't slow pose inference. Its torso depth is fused with a skeleton-size cue (70 % mono depth). When the two disagree, mono depth wins, because the skeleton cue is the one that glitches when limbs leave the frame.
- **Gestures:** `gestures.py`. A swipe is the wrist crossing ≥ 0.9 shoulder-widths horizontally within 0.9 s, measured on the raw 2D image relative to the shoulders, so it's independent of distance and has no filter lag. Arcing strokes are fine as long as horizontal motion dominates, and blurry frames are tolerated. With `--log`, every candidate stroke prints a `[swipe?] … reject` line with the reason. Fist = MediaPipe hand-landmark model on a crop around the raised wrist (`handpose.py`), with finger-curl ratios. A fist held still blocks swipes.
- **Latency / pose prediction:** perception sends each frame's joints *with velocities* at the frame's capture time (`filters.PosePredictor`: One-Euro at rest, a constant-velocity Kalman filter once a joint moves). Every render frame, `TrackingClient.Predict` extrapolates them to the moment that frame reaches the screen, so the garment sits on where the body is now, not where it was ~80 ms ago, and moves at the render rate rather than in 30 Hz steps. In *CameraFeed* mode the overlay is instead matched to the capture time of the image shown under it. Clocks are synced from each frame's send time. Measured end to end with a known trajectory and 60 ms latency: fast-motion error halves (110 → 55 mm), slow motion −20 %, rest jitter < 1 mm/frame. The rest of the pipeline was cut too: JSON goes out before the camera image, the fist classifier runs on its own thread, and pose and depth get separate OpenVINO thread budgets. Together these took capture→Unity from 141 ms to 19 ms on a 4-core test box. Tuning lives in `predict`, `filters` and `camera.latency_s` in `calib/config.json`. Debug line (D) shows latency and prediction horizon.
- **Occlusion (`BodyProxy.cs`, `ReflectAR_DepthOnly.shader`):** depth-only stand-ins for the body hide whatever of the garment is behind it. Parts under the garment (torso, neck, sleeved arm) are fitted capsules. Exposed parts (bare forearms, hands with fingers, head with hair) are drawn oversized, then trimmed per pixel to the person mask that BlazePose's landmark model produces (sent with each frame, `net.mask_width`): each fragment is projected into the camera image and dropped where the mask says there is no person, so the cut-out follows the real silhouette. The lookup uses where that body part was when the mask was captured, so prediction doesn't tear it, and it works in both modes. Oversized parts are pushed back by their oversize so they can't poke through sleeves or the collar. On the test clip it covers 99 % of the real hands, forearms and head (capsules alone: 87 %), and occluder pixels off the body drop from 35 % to 7 %. B shows the proxies (trimmed parts tinted warm).
- **Wire format:** TCP, with Python as the server. Each message is 1 type byte, a 4-byte big-endian length, then the payload: `J` = JSON tracking frame; `R` = float64 capture time + uint16 width + uint16 height + RGB24 camera image (rows bottom first); `M` = float64 capture time + uint16 width + uint16 height + the person mask (uint8, 255 = person). Unity still accepts the older `I` (float64 capture time + JPEG). The image goes raw because on one machine that's far cheaper than a JPEG encode plus a decode on Unity's main thread.
- **Render pipelines (`HdrpLook.cs`, `GarmentMaterials.cs`):** the scene is set up at runtime for whichever pipeline is active. Under **HDRP**, garments are `HDRP/Lit` (opaque, double-sided) with per-fabric base colour, weave normal map and mask map (AO in the weave's valleys, smoothness per fibre: fleece 0.1, denim 0.2, nylon 0.5–0.6). They get a 3000 lux key light with soft shadows (6° angular diameter), a 700 lux fill, ambient from a soft gradient "room", SSAO and contact shadows. The body proxies cast shadows and are in the depth buffer, so hands, arms and the head shade the garment. Fixed exposure (EV100 10) and neutral tonemapping; the feed and proxy shaders divide the exposure back out, so the camera image keeps its brightness. SMAA instead of TAA (cloth has no motion vectors, so TAA would smear it), and no bloom or dithering, so black stays black on the mirror. While a garment fades in or out it switches to a transparent copy of its material. Under **URP** everything renders as before with the `ReflectAR/Garment` triplanar shader.
  - In HDRP the camera feed is drawn after the depth prepass, so it can't show through the proxies' depth; the proxies' colour pass draws the feed pixel they cover instead (`ReflectAR_DepthOnly.shader`).
- **Renderer:** `MirrorApp.cs` has two modes. *CameraFeed* places a virtual camera at the webcam pose with its intrinsics, mirrored, over the live camera feed. *Mirror* renders the reflected `MirrorWorld` with a Kooima off-axis frustum from the tracked eye; this is the mode the two-way-mirror build will use.
- **Garments (upper body only):** 10 tops in `GarmentCatalog.cs`: tee, V-neck, Breton long-sleeve, polo, flannel, hoodie, bomber, denim jacket, puffer, tank.
  - *Body shape* (`BodyShape.cs`): the torso is measured from the person mask at six heights between the hips and the shoulders. It walks the mask outward from the torso's centre line to the silhouette edge, then averages the ratio to the generic body over ~1.5 s into `BodyRig.Profile`. A side is skipped while its arm touches the torso (that edge would be the arm), so it measures whenever an arm is away from the body, for example while swiping. Nothing is measured unless the user faces the camera (±25°). Wider also means deeper (60 % of the width change), for chest and belly. Garments rebuild in the background when a level changes by more than 8 % for 1.5 s.
  - *Mesh* (`GarmentMesher.cs`): each garment is generated per user as one seamless surface. A signed-distance field smoothly blends the torso, shoulders, sleeves and collar, so armpits and shoulders have no gaps. It is meshed with surface nets and cut open at the neck, hem and cuffs. The torso cross-section follows the measured body plus ease. Below the chest it hangs from whatever is widest above it (chest or belly) by `fit` (tank 0.15, tee 0.75, hoodie 0.95), the way fabric falls instead of tucking into the waist. Ribbed hem bands (hoodie, bomber, puffer) pull back in to the hips, and gathered cuffs sit at the wrist with the sleeve bloused over them; both hold to the body in the cloth sim.
  - *UV layout*: the surface is laid out like sewn pattern pieces: front and back panels unrolled around the torso (side seams), a yoke over the shoulder tops, and each sleeve unrolled around the arm (seam underneath). Texture stretch on the tee, flannel and hoodie: median 1.0, 90th percentile ≤ 1.45, area scale within ±8 % for 90 % of the surface. Vertices are split along the seams only for drawing; the solver keeps the welded mesh, and normals come from it, so seams don't show in the shading.
  - *Closures* (`GarmentTrims.cs`): real geometry that rides on the cloth. A raised placket with sewn buttons (polo 2, flannel 8, denim with metal shanks), or a zipper with tape, teeth, slider and pull (bomber, puffer). It hangs off a chain of points found by ray down the centre front of the rest mesh; each point follows its triangle on the simulated cloth and carries a local frame, so the closure bends with the fabric.
  - *Physics* (`ClothGarment.cs`): an XPBD cloth solver, with near-inextensible stretch, soft compression so the fabric buckles into wrinkles, and bending, gravity, inertia and damping. It collides with capsules fitted to the body. The cloth is tethered to a version of the mesh skinned to six tracked bones; the tether is ~0 at the collar and shoulders and grows toward the hem and cuffs. Fabric parameters (denim stiff, jersey soft, puffer thick) are per garment.
  - *Shading*: HDRP as above. URP (`ReflectAR_Garment.shader`): seamless triplanar fabric anchored to the rest pose, so patterns stick to the cloth, with sheen and weave shading. The solver costs ~6 ms per frame in the Editor (Release code optimization).

---

## Performance and battery

Measured on an M5 Pro while tracking one person (CPU, 100 % = one core). The "before" figures are from the OpenVINO-only build:

| | Before | After |
|---|---|---|
| Perception | ~970 % | ~120–170 % with pose on the CPU; about half that with pose on the Neural Engine |
| Renderer | 85–220 % (Editor, Play mode) | ~55 % (standalone app) |
| Worst frame | up to 533 ms (garment rebuild on the main thread) | ~22 ms |

What made the difference:
- **Depth model:** runs at 2 Hz (`compute.depth_max_hz`), and 0.5 Hz with nobody in view (`depth_idle_hz`). It only feeds the user distance, which is smoothed at < 1 Hz.
- **Pose on the Neural Engine:** the pose and hand models run as float16 on the Neural Engine (pose 9.7 → ~3 ms per frame, hands 6.4 → ~2 ms). Replaying a 1280×720 clip at 30 fps, perception went from 109–114 % to 53–54 % CPU.
- **CPU thread budgets:** 2 threads each for pose and depth when they run on the CPU (`pose_threads`, `depth_threads`); big pools spin and burn power for ~1 ms of latency.
- **Idle mode:** with nobody in view for 3 s, person detection drops to 6 fps (`idle_pose_fps`).
- **Camera image:** sent raw over localhost instead of as JPEG.
- **Garment meshes:** built on a worker thread and pre-built for all 10 garments once someone is tracked. A rebuild needs a sustained change in the user's measurements (3 s cooldown while the profile settles, then 10 s), because each rebuild resets the cloth.
- **Cloth:** mesh detail scales with body size and is capped at 4,000 vertices. The cloth inherits 90 % of the body's motion each frame, so 2 substeps are enough and it doesn't trail behind the user.
- **Frame rate:** capped at 60 fps while tracking and 30 fps when idle, with vsync off (a 120 Hz panel would otherwise render 120 fps).
- **Diagnostics:** Unity logs `[ReflectAR stats]` every 3 s with fps, worst frame, cloth cost and rebuild count.

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
