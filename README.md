# ReflectAR

A virtual try-on mirror. ReflectAR tracks your body through an Intel RealSense depth camera (D415) or a plain webcam and draws simulated garments onto your reflection, rendered from your own eye position so they line up the way a real mirror image would.

v1 is a computer prototype: it runs on the computer's own camera and screen. It runs on Macs (Apple Silicon recommended) and Windows PCs (NVIDIA GPU recommended), and on Linux. The same pipeline is meant to drive a 43" two-way-mirror build later.

The system has two programs that talk over a local TCP socket:

```
 camera ──► perception/ (Python)  ──TCP 127.0.0.1:5566──►  unreal/ (Unreal Engine 5)  ──► screen
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
| **OpenCV** (`opencv-python`) | Webcam capture, image warping and resizing, checkerboard calibration |
| **NumPy** | All geometry, filtering and tensor pre/post-processing |
| **MediaPipe BlazePose (full)** | Body detection, 33-joint landmarks and the person segmentation mask. MediaPipe's ROI tracking is re-implemented in `blazepose.py`, so the MediaPipe package itself is **not** needed |
| **MediaPipe Hand Landmarker** | Fist detection on a crop around the raised wrist |
| **Intel RealSense SDK** (`pyrealsense2`; `pyrealsense2-macosx` on macOS) | Colour + measured depth from a D415, aligned to the colour image (`realsense.py`) |
| **Depth Anything V2 Metric-Indoor-Small** | Metric depth from a single camera, when there's no RealSense |
| **One-Euro filter + constant-velocity Kalman filter** | Smoothing joints and predicting pose ahead in time (`filters.py`) |
| **TensorFlow + tf2onnx, ONNX** | Only used by `tools/` to convert the original models to `.onnx`. Not needed to run ReflectAR |

All model files are already committed in `perception/models/`, in two forms: `.onnx` for ONNX Runtime, and the originals (`.tflite`, plus the OpenVINO IR `.xml`/`.bin` for depth) for OpenVINO. The pose and hand models also have a `_fp16.onnx` copy for the Apple Neural Engine. Nothing has to be downloaded.

### Renderer (`unreal/`)

| Technology | What it's used for |
|---|---|
| **Unreal Engine 5.6+** | The real-time renderer and app (C++ project, no Blueprints) |
| **C++** | Everything in `Source/ReflectAR/` |
| **Procedural Mesh Component** | The simulated garments, their closures, the body occluders and the camera feed, rebuilt every frame |
| **Cloth shading model, virtual shadow maps, contact shadows** | Fabric with sheen and weave normal maps, soft shadows from a 3000 lux key light |
| **Python Editor Script Plugin** | `Content/Python/reflectar_setup.py` builds the materials on the first editor start |
| **Canvas HUD** | On-screen prompts and the garment carousel |
| **Kooima off-axis projection** | Renders the scene from the tracked eye position, as a mirror would show it (`UMirrorLocalPlayer`) |
| **Signed-distance fields + surface nets** | Generates each garment as one seamless mesh fitted to the user (`GarmentMesher.cpp`) |
| **XPBD cloth simulation** | Custom position-based cloth solver with body-capsule collisions (`ClothGarment.cpp`) |
| **Mask-trimmed occluders** | Hide the garment behind your hands, arms and head, trimmed per pixel to the person mask (`BodyProxy.cpp`, `M_Proxy`) |

The renderer was a Unity 6 project (HDRP / URP) until this port; it is in the git history (`main` before the Unreal branch) if you need it.

---

## Repository layout

```
ReflectAR/
├── run_perception.sh            # starts the perception service (macOS / Linux)
├── run_perception.bat           # starts the perception service (Windows)
├── run_mirror.sh                # macOS: perception + the packaged app (lowest CPU / battery)
├── perception/
│   ├── requirements.txt         # Python dependencies, with the right runtime per platform
│   ├── server.py                # main service: camera → tracking → TCP stream
│   ├── realsense.py             # Intel RealSense capture + aligned depth
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
└── unreal/                      # Unreal Engine 5 project
    ├── ReflectAR.uproject
    ├── Config/                  # game mode, local player, renderer settings
    ├── Content/Python/          # reflectar_setup.py: builds the materials (Content/ReflectAR/)
    └── Source/ReflectAR/
        ├── Public/, Private/    # ReflectARMirror (the app), TrackingClient, BodyRig, BodyProxy,
        │                        # GarmentMesher, ClothGarment, GarmentTrims, GarmentSpec, HUD
        └── ReflectAR.Build.cs
```

---

## Setup

### 1. Requirements

- **Git**
- **Python 3.11 or newer**, 64-bit (tested with 3.11 and 3.13)
- **Unreal Engine 5.6 or newer** (Epic Games Launcher → *Unreal Engine* → *Library*), and a C++ toolchain: **Xcode** on macOS, **Visual Studio 2022** with the *Game development with C++* workload on Windows. The project is C++, so the editor compiles it on first open. A newer 5.x engine should open it after a one-time conversion prompt.
- A camera, optional: an **Intel RealSense D415** (preferred: it measures depth) on a **USB 3** port and cable, or any webcam. You can test everything without one (see [Running without a camera](#running-without-a-camera)).
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

### 4. Open the Unreal project

1. Open `unreal/ReflectAR.uproject` (double-click, or *Browse* in the launcher). Unreal asks to build the `ReflectAR` module: choose **Yes**. On macOS you can instead run *Generate Xcode Project* from the `.uproject`'s context menu and build the `ReflectAREditor` target.
2. On the first editor start, `Content/Python/init_unreal.py` runs `reflectar_setup.py`, which writes the materials and the parameter collection to `Content/ReflectAR/` (the Output Log shows `ReflectAR setup: materials written to /Game/ReflectAR`). They are generated, not committed; to rebuild them, run `import importlib, reflectar_setup; importlib.reload(reflectar_setup); reflectar_setup.build()` in the Output Log's Python console.
3. Nothing else needs setting up: the project opens an empty map, and `AReflectARGameMode` spawns the mirror (`AReflectARMirror`), its lights and its HUD when you press Play.

### 5. Camera permissions

- **Intel RealSense on macOS:** macOS only lets root open a RealSense, so with one plugged in `run_perception.sh` and `run_mirror.sh` start perception with `sudo` and ask for your password. Nothing else needs setting up. Use a USB 3 cable plugged straight into the Mac: on a USB 2 link (the startup line says `USB 2.1`) the D415 only runs 640×480 at 15 fps; on USB 3 it runs colour at 960×540 @ 60 fps and depth at 848×480 @ 90 fps (the D415 can't do those rates at 1280×720, which tops out at 30). Quick test: `sudo .venv/bin/python tools/realsense_check.py` from `perception/`.

- **macOS:** grant camera access to the app you start perception from, such as Terminal, iTerm or VS Code: *System Settings → Privacy & Security → Camera*. Turn **Center Stage off** (*Control Centre → Video Effects*). It crops the image and changes the focal length, which breaks the geometry.
- **Windows:** *Settings → Privacy & security → Camera*, and allow desktop apps to use the camera. Close other apps that are using it.

The camera API is chosen automatically (AVFoundation on macOS, Media Foundation on Windows, OpenCV's default on Linux). To force one, set `camera.api` in `perception/calib/config.json` to `avfoundation`, `msmf`, `dshow`, `v4l2` or `any`.

---

## Running

Start the two halves in either order. The Unreal app waits for perception and connects on its own.

**1. Start perception**

```bash
./run_perception.sh          # macOS / Linux
run_perception.bat           # Windows
```

Both run `server.py --log` from inside `perception/`, and pass any extra flags through. To run it by hand, `cd perception` first, because paths are relative.

Startup prints a `[backend] …` line saying which device each model runs on, a `[cam] …` line, and `[net] listening on 127.0.0.1:5566`. The first start also compiles the models (Core ML: seconds; TensorRT: minutes). Later starts use the cache in `perception/models/cache/`.

**2. Start the renderer:** in the Unreal editor press **Play** (*Selected Viewport*, or *New Editor Window* for a window you can resize to the screen).

**Lowest CPU / battery (macOS):** package the app once (*Platforms > Mac > Package Project*, into `unreal/Build/`, which gives `unreal/Build/Mac/ReflectAR.app`), then run `./run_mirror.sh`. It starts perception and the app together, and stops perception when you quit the app with **Esc**. The editor redraws its own windows every frame, so the packaged app uses much less CPU.

Stand **1–3 m** from the screen. Outside that range a prompt shows ("Please step back" / "Come a little closer"), garments dim, and gestures are disabled.

### Useful `server.py` flags

| Flag | Effect |
|---|---|
| `--log` | Print tracking state every second, plus a `[swipe?] … reject` line explaining every rejected swipe |
| `--snapshot dbg.jpg` | Write an annotated debug frame every second |
| `--video file.mp4` | Replay a recording instead of the webcam |
| `--no-image` | Don't stream the camera image to the renderer |
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

Then press Play in Unreal.

---

## Controls

| Action | Gesture | Key |
|---|---|---|
| Previous / next garment | Swipe a hand to the right / left, above hip height. Like scrolling: the carousel follows your hand (`swipe_direction`) | ← / → |
| Select garment | Raise a hand above the shoulder, make a fist, hold 5 s (the ring fills) | Space |
| Camera-feed ↔ mirror mode | – | M |
| Show body proxy / cube test / debug overlay | – | B / C / D |
| Pose prediction on / off (A/B test) | – | P |
| Occlusion trimmed to the person mask on / off (A/B test) | – | O |
| Prediction lead −5 / +5 ms (Mirror mode) | – | [ / ] |
| Quit | – | Esc |

There are 10 upper-body garments (`GarmentSpec.cpp`): tee, V-neck, Breton long-sleeve, polo, flannel, hoodie, bomber, denim jacket, puffer and tank. The same toggles (and the light levels `KeyLux`, `FillLux`, `Ambient`) are properties of `AReflectARMirror`, editable in the *Details* panel while playing.

---

## Configuration and calibration

All tuning lives in `perception/calib/config.json`. Each section has a `_note` explaining its fields.

- **Inference device (`compute`):** `backend` = `auto` | `onnx` | `openvino`, and `pose_device` / `depth_device` = `auto` | `ane` | `gpu` | `cpu`. `auto` means the Neural Engine on Macs, the GPU where an NVIDIA or DirectML GPU is available, and otherwise the CPU. `pose_threads` / `depth_threads` set CPU thread budgets (0 = automatic).
- **RealSense (`camera.source`, `camera.realsense`):** `source` is `auto` (the RealSense when one is connected and can be opened, else the webcam), `realsense` or `webcam`. With a RealSense, intrinsics come from its factory calibration and distance is measured, so the intrinsics and distance calibrations below are webcam-only. Set `camera_pose` to the position of its **colour** lens. The `usb3` / `usb2` entries pick the stream mode per USB link.
- **Distance (webcam only; required on a new machine or camera):** stand exactly at a measured distance and run `./run_perception.sh --calib-dist <metres>`. Do this at two distances, such as 1.0 m and 2.0 m. Each run adds a point to `perception/calib/depth_calib.json`, and two points fit a scale and offset for each depth cue. Without it, the 1–3 m prompts can be off by up to 40 %. The committed file was calibrated on the original MacBook.
- **Camera intrinsics (recommended):** print a 9×6 inner-corner checkerboard, measure one square, then run `.venv/bin/python calibrate_intrinsics.py --square <metres>` from `perception/`. Show the board at 15–25 angles and distances, pressing SPACE for each one, then press Q to solve. This writes `calib/intrinsics.json`. Until you do, the field of view comes from `camera.hfov_deg`.
- **Screen and camera position:** `screen` (physical size in metres and resolution) and `camera_pose` (the camera's position relative to the screen's lower-left corner) default to a 14" MacBook Pro. Measure and change them for any other display, including the 43" mirror build.
- **Camera:** `camera.index` picks the camera if the wrong one opens, and `camera.api` the capture API (see [Camera permissions](#5-camera-permissions)).
- **Occlusion mask:** `net.mask_width` is the resolution of the person mask sent to the renderer (0 turns it off).
- **Prediction lead (optional, Mirror mode):** wave a hand and press [ / ] until the garment stops trailing or leading the reflection. Then add the shown lead to `camera.latency_s` (or `predict.display_latency_s`).

With a webcam, absolute distance is approximate (±10–20 %); fusion helps. The RealSense measures it (D415: about ±2 % at 2 m).

---

## How it works

- **Inference backends (`backend.py`):** each model loads through ONNX Runtime on the best device it finds, or through OpenVINO on the CPU. On a Mac, the Neural Engine is a good fit because the renderer doesn't use it, so inference neither loads the CPU nor competes with rendering. Given the same input, both backends agree to 0.01 px in float32. The Neural Engine only runs float16, so on `ane` the pose and hand models load from their `_fp16.onnx` copies; Core ML would otherwise run a float32 model on its CPU path. Those copies move pose landmarks by ~0.7–1 px on 1280×720 frames (visible joints) and hand landmarks by ~0.1 px. Set `pose_device` to `gpu` for float32 on the Apple GPU instead. The `.onnx` files were converted from the originals with `tools/convert_models.py` (the MediaPipe `.tflite` models) and `tools/ir_to_onnx.py` (the depth IR); you only need to re-run them if a model changes.
- **Pose:** the stock MediaPipe BlazePose *full* detector and landmark models, with MediaPipe's ROI tracking re-implemented (`blazepose.py`). Each joint's 3D position comes from back-projecting its 2D pixel at a fused distance, plus BlazePose's relative per-joint depth, so the skeleton always lands exactly on the person in the image (`geometry.py`, `server.py`).
- **Depth from a RealSense (`realsense.py`):** colour and depth stream together and depth is aligned to the colour pixels on the capture thread. The torso depth sets the distance directly, and each visible joint takes the depth measured at its pixel (the nearest surface within 0.35 m of where BlazePose puts it, so limb edges don't read the wall), pushed back by a surface-to-joint offset (`depth.*_surface_offset_m`). Hidden joints keep BlazePose's relative depth. Frames carry their exposure time on the host clock. On start the camera is reset and the start retried until frames arrive, because on macOS a stopped or refused D415 often streams nothing until it's power-cycled.
- **Depth from one camera (webcam):** Depth Anything V2 *Metric-Indoor-Small*, converted to OpenVINO IR, runs on its own thread, capped at 2 Hz (`compute.depth_max_hz`; 0.5 Hz when nobody is in view) with its own CPU-thread budget so it doesn't slow pose inference. Its torso depth is fused with a skeleton-size cue (70 % mono depth). When the two disagree, mono depth wins, because the skeleton cue is the one that glitches when limbs leave the frame.
- **Gestures:** `gestures.py`. A swipe is one fast, mostly sideways stroke of the hand (wrist, index and pinky combined, so one blurred landmark doesn't break it), measured on the raw 2D image relative to the shoulders, so it's independent of distance and has no filter lag. It fires mid-stroke once the hand has covered 0.8 shoulder widths with a peak speed of 1.8 shoulder widths/s. Moving the hand back is not a swipe: a stroke the other way only counts once the hand has been still for 0.6 s, and within 3 s of a swipe only if it's at least 80 % as fast. `swipe_direction` is `natural` (scrolling: move the hand right to bring in the item on the left) or `pointer`. With `--log`, rejected strokes print a `[swipe?] … reject` line with the reason. On a synthetic benchmark of realistic swipes, returns, double swipes and non-swipe hand motion, 96 % of swipe cases and 98 % of non-swipe cases come out right (previous detector: 86 % / 94 %; its main failure was the way back firing as an opposite swipe). Fist = MediaPipe hand-landmark model on a crop around the raised wrist (`handpose.py`), with finger-curl ratios. A fist held still blocks swipes.
- **Latency / pose prediction:** perception sends each frame's joints *with velocities* at the frame's capture time (`filters.PosePredictor`: One-Euro at rest, a constant-velocity Kalman filter once a joint moves). Every render frame, `FTrackingClient::Predict` extrapolates them to the moment that frame reaches the screen, so the garment sits on where the body is now, not where it was ~80 ms ago, and moves at the render rate rather than in 30 Hz steps. In *CameraFeed* mode the overlay is instead matched to the capture time of the image shown under it. Clocks are synced from each frame's send time. Measured end to end with a known trajectory and 60 ms latency: fast-motion error halves (110 → 55 mm), slow motion −20 %, rest jitter < 1 mm/frame. The rest of the pipeline was cut too: JSON goes out before the camera image, the fist classifier runs on its own thread, and pose and depth get separate OpenVINO thread budgets. Together these took capture→renderer from 141 ms to 19 ms on a 4-core test box (measured with the Unity renderer). Tuning lives in `predict`, `filters` and `camera.latency_s` in `calib/config.json`. Debug line (D) shows latency and prediction horizon.
- **Occlusion (`BodyProxy.cpp`, `M_Proxy`):** stand-ins for the body hide whatever of the garment is behind it. Parts under the garment (torso, neck, sleeved arm) are fitted capsules. Exposed parts (bare forearms, hands with fingers, head with hair) are drawn oversized, then trimmed per pixel to the person mask that BlazePose's landmark model produces (sent with each frame, `net.mask_width`): each fragment is projected into the camera image and dropped where the mask says there is no person, so the cut-out follows the real silhouette. The lookup uses where that body part was when the mask was captured, so prediction doesn't tear it, and it works in both modes. Oversized parts are pushed back by their oversize so they can't poke through sleeves or the collar, and are kept off the garment's outline on screen, where the mask can't tell a hand from the torso behind it (a fitted core hides the cloth there). The occluders are one procedural mesh rebuilt each frame; each part's trim flag, depth push and motion since the mask go in its vertices' UV1–UV3. They draw as the camera-feed pixel they cover (CameraFeed mode) or black (Mirror), so they read as the real body, and they cast shadows, so hands, arms and the head shade the garment. B shows them (trimmed parts tinted warm). With the Unity renderer, on the test clip, they covered 99 % of the real hands, forearms and head (capsules alone: 87 %), and occluder pixels off the body dropped from 35 % to 7 %.
- **Wire format:** TCP, with Python as the server. Each message is 1 type byte, a 4-byte big-endian length, then the payload: `J` = JSON tracking frame; `R` = float64 capture time + uint16 width + uint16 height + RGB24 camera image (rows bottom first); `M` = float64 capture time + uint16 width + uint16 height + the person mask (uint8, 255 = person). The renderer also accepts the older `I` (float64 capture time + JPEG), decoded on its network thread. The image goes raw because on one machine that's far cheaper than a JPEG encode plus a decode.
- **Coordinates:** all body, cloth and garment code works in "body coordinates": metres, x to the right as seen facing the screen, y up, z into the screen (perception's Mirror frame with z flipped; `BodyMath.h`). The `MirrorWorld` component maps them to Unreal space (cm, X forward, Y right, Z up) with a proper rotation and ×100, so geometry keeps its winding; in Mirror mode it also mirrors body z, which is the reflection.
- **Renderer (`ReflectARMirror.cpp`, `MirrorLocalPlayer.cpp`):** two modes. *CameraFeed* places the view at the webcam pose with its intrinsics over the live camera feed, mirrored by flipping the view's x axis (Unreal sees a reflected view and reverses culling itself). *Mirror* renders the reflected `MirrorWorld` with a Kooima off-axis frustum from the tracked eye; this is the mode the two-way-mirror build will use. `UMirrorLocalPlayer::GetProjectionData` hands both to Unreal as a view matrix and a reversed-Z, infinite-far projection.
- **Look:** physical units: a 3000 lux key light with soft shadows (6° light source angle) and contact shadows, a 700 lux fill, and the room's bounce light as an emissive term (`Ambient`), since there is no environment to light the cloth indirectly. The camera exposure is fixed (ISO 100, 1/60 s, f/4, EV100 ~10), with no tone curve, bloom, vignette, motion blur or film grain, so the camera feed passes through unchanged (the feed and occluder materials divide the exposure back out) and black stays black on the mirror. FXAA instead of TSR, because the cloth has no motion vectors and TSR would smear it. Lumen and reflections are off (`Config/DefaultEngine.ini`).
- **Garments (upper body only):** 10 tops in `GarmentSpec.cpp`: tee, V-neck, Breton long-sleeve, polo, flannel, hoodie, bomber, denim jacket, puffer, tank.
  - *Body shape* (`BodyRig.cpp`, `FBodyShape`): the torso is measured from the person mask at six heights between the hips and the shoulders. It walks the mask outward from the torso's centre line to the silhouette edge, then averages the ratio to the generic body over ~1.5 s into `FBodyRig::Profile`. A side is skipped while its arm touches the torso (that edge would be the arm), so it measures whenever an arm is away from the body, for example while swiping. Nothing is measured unless the user faces the camera (±25°). Wider also means deeper (60 % of the width change), for chest and belly. Garments rebuild in the background when a level changes by more than 8 % for 1.5 s.
  - *Mesh* (`GarmentMesher.cpp`): each garment is generated per user as one seamless surface. A signed-distance field smoothly blends the torso, shoulders, sleeves and collar, so armpits and shoulders have no gaps. It is meshed with surface nets and cut open at the neck, hem and cuffs. The torso cross-section follows the measured body plus ease. Below the chest it hangs from whatever is widest above it (chest or belly) by `Fit` (tank 0.15, tee 0.75, hoodie 0.95), the way fabric falls instead of tucking into the waist. Ribbed hem bands (hoodie, bomber, puffer) pull back in to the hips, and gathered cuffs sit at the wrist with the sleeve bloused over them; both hold to the body in the cloth sim. Built on the thread pool.
  - *UV layout*: the surface is laid out like sewn pattern pieces: front and back panels unrolled around the torso (side seams), a yoke over the shoulder tops, and each sleeve unrolled around the arm (seam underneath). Texture stretch on the tee, flannel and hoodie: median 1.0, 90th percentile ≤ 1.45, area scale within ±8 % for 90 % of the surface. Vertices are split along the seams only for drawing; the solver keeps the welded mesh, and normals come from it, so seams don't show in the shading.
  - *Closures* (`GarmentTrims.cpp`): real geometry that rides on the cloth. A raised placket with sewn buttons (polo 2, flannel 8, denim with metal shanks), or a zipper with tape, teeth, slider and pull (bomber, puffer). It hangs off a chain of points found by ray down the centre front of the rest mesh; each point follows its triangle on the simulated cloth and carries a local frame, so the closure bends with the fabric.
  - *Physics* (`ClothGarment.cpp`): an XPBD cloth solver, with near-inextensible stretch, soft compression so the fabric buckles into wrinkles, and bending, gravity, inertia and damping. It collides with capsules fitted to the body. The cloth is tethered to a version of the mesh skinned to six tracked bones; the tether is ~0 at the collar and shoulders and grows toward the hem and cuffs. Fabric parameters (denim stiff, jersey soft, puffer thick) are per garment.
  - *Shading* (`M_Fabric`): Unreal's cloth shading model, with per-fabric base colour, a weave normal map and a mask (AO in the weave's valleys, smoothness per fibre: fleece 0.1, denim 0.2, nylon 0.5–0.6), all generated at startup (`FFabricMaps`, 512² with mips), and sheen (fuzz) per garment. Two-sided, lit by the outward normal on both sides, so shading doesn't depend on triangle winding. While a garment fades in or out it switches to a translucent copy of its material (`M_FabricFade`).

---

## Performance and battery

Measured on an M5 Pro while tracking one person (CPU, 100 % = one core), with the Unity renderer; the Unreal renderer hasn't been measured yet. The "before" figures are from the OpenVINO-only build:

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
- **Diagnostics:** the renderer logs `[ReflectAR stats]` every 3 s (Output Log) with fps, worst frame, cloth cost and rebuild count.

## Troubleshooting

| Problem | Fix |
|---|---|
| `Could not open camera` | macOS: grant camera permission to your terminal app. Windows: check camera privacy settings, close other apps using the camera, or set `"api": "dshow"` in `camera` in `config.json`. Also check `camera.index` |
| `FileNotFoundError` for `calib/config.json` or `models/...` | Run `server.py` from inside `perception/`, or use the launch script |
| `.venv/bin/python: No such file or directory` | The virtual environment must be at `perception/.venv`; redo step 3 |
| Models run on the CPU although you have a GPU | Check the `[backend]` startup line. NVIDIA: update the driver to 580+. Windows without NVIDIA: install `onnxruntime-directml` (step 3). Run with `--ep-log` for details |
| `[backend] … TensorrtExecutionProvider failed … trying without it` | TensorRT isn't fully installed. This is harmless: it falls back to CUDA |
| The app stays on "Starting… Waiting for the perception service" | Check that perception printed `[net] listening on 127.0.0.1:5566` and nothing else uses port 5566. Host and port are set in `net` in `config.json` and in `FTrackingClient`'s constructor |
| Garments or occluders render with the grey default material | The materials are missing: check the Output Log for `ReflectAR setup` lines, or run `reflectar_setup.build()` (setup step 4) |
| Distance prompts are wrong | Webcam: redo the distance calibration on this machine |
| `a RealSense is connected but macOS only lets root open it` | Start perception through `run_perception.sh`, or with `sudo` |
| `RealSense … started but sent no frames` / `failed to set power state` | Unplug and replug the D415, and quit any app using it as a webcam (FaceTime, Photo Booth, browsers) |
| `[cam] WARNING: RealSense is on a USB 2 link` | Use a USB 3 cable (many USB-C cables are USB 2 only) in a port on the Mac itself, not a hub. The `[state]` line's `cam=` and `depth=` show the rates actually arriving |
| Garments are offset from your body | Set `screen` and `camera_pose` for your display, and run the intrinsics calibration |
| First start is slow | Models are compiled on first run and cached in `perception/models/cache/`. TensorRT can take minutes; later starts are faster |
