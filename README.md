# ReflectAR — v1 (MacBook prototype)

Garments drawn onto the user's reflection, from the viewer's eye position. v1 runs on the MacBook's own camera and screen, and also on Windows PCs (NVIDIA GPU recommended).

```
perception/  Python 3.11+, ONNX Runtime   webcam → BlazePose + hand + metric mono depth → TCP :5566
             (Core ML / CUDA) or OpenVINO
renderer/    Unity 6000.6 (HDRP or URP)   off-axis / webcam-intrinsics camera, procedural garments, gestures UI
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
   The first time the project opens with HDRP installed, `Editor/PipelineSetup.cs` creates `Assets/Settings/HDRP/ReflectAR_HDRP.asset`, makes HDRP the active pipeline and writes the garment material templates to `Resources/HDRP/`. **ReflectAR > Render Pipeline > URP / HDRP** switches between the two (the URP assets are kept). If HDRP complains about missing settings, run **Window > Rendering > HDRP Wizard > Fix All** once.

Without a camera, use `perception/.venv/bin/python perception/fake_server.py [--script swipe|fist] [--dist 0.8]`, which streams a synthetic skeleton. `server.py --video file.mp4` replays a recording.

## Using it

| Action | Gesture | Key |
|---|---|---|
| Next / previous garment | Swipe a hand left→right / right→left, above hip height | → / ← |
| Select garment | Raise a hand above the shoulder, make a fist, hold 5 s (the ring fills) | Space |
| Camera-feed ↔ mirror mode | – | M |
| Show body proxy / cube test / debug | – | B / C / D |
| Pose prediction on / off (A/B) | – | P |
| Occlusion trimmed to the person mask on / off (A/B) | – | O |
| Prediction lead −5 / +5 ms (Mirror mode) | – | [ / ] |

The user must stand **1–3 m** from the screen. Outside that range a prompt shows ("Please step back" / "Come a little closer"), garments dim, and gestures are disabled.

## How it works

- **Inference backends (`backend.py`):** each model runs through ONNX Runtime on the best device it finds (Core ML on the Apple Neural Engine, which Unity doesn't use, so it neither loads the CPU nor competes with rendering; TensorRT/CUDA on NVIDIA; DirectML), or OpenVINO on the CPU. Pick per model group in `compute` in `calib/config.json`: `pose_device` / `depth_device` = `auto` | `ane` | `gpu` | `cpu`, `backend` = `auto` | `onnx` | `openvino`. CPU inference uses OpenVINO when it's installed (about 1.6× faster than ONNX Runtime's CPU path here). The `.onnx` files are converted from the originals (`tools/convert_models.py` for the MediaPipe `.tflite`, `tools/ir_to_onnx.py` for the depth IR); given the same input, both backends agree to 0.01 px. `--ep-log` shows how much of each model Core ML / TensorRT took and what fell back to the CPU.
- **Pose:** the stock MediaPipe BlazePose *full* detector and landmark models, with MediaPipe's ROI tracking re-implemented (`blazepose.py`). Each joint's 3D position comes from back-projecting its 2D pixel at a fused distance, plus BlazePose's relative per-joint depth, so the skeleton always lands exactly on the person in the image (`geometry.py`, `server.py`).
- **Depth from one camera:** Depth Anything V2 *Metric-Indoor-Small*, converted to OpenVINO IR, runs on its own thread, capped at 12 Hz (`compute.depth_max_hz`) with its own CPU-thread budget so it doesn't slow pose inference. Its torso depth is fused with a skeleton-size cue (70 % mono depth). When the two disagree, mono depth wins, because the skeleton cue is the one that glitches when limbs leave the frame.
- **Gestures:** `gestures.py`. A swipe is the wrist crossing ≥ 0.9 shoulder-widths horizontally within 0.9 s, measured on the raw 2D image relative to the shoulders, so it's independent of distance and has no filter lag. Arcing strokes are fine as long as horizontal motion dominates, and blurry frames are tolerated. With `--log`, every candidate stroke prints a `[swipe?] … reject` line with the reason. Fist = MediaPipe hand-landmark model on a crop around the raised wrist (`handpose.py`), with finger-curl ratios. A fist held still blocks swipes.
- **Latency / pose prediction:** perception sends each frame's joints *with velocities* at the frame's capture time (`filters.PosePredictor`: One-Euro at rest, a constant-velocity Kalman filter once a joint moves). Every render frame, `TrackingClient.Predict` extrapolates them to the moment that frame reaches the screen, so the garment sits on where the body is now, not where it was ~80 ms ago, and moves at the render rate rather than in 30 Hz steps. In *CameraFeed* mode the overlay is instead matched to the capture time of the image shown under it. Clocks are synced from each frame's send time. Measured end to end with a known trajectory and 60 ms latency: fast-motion error halves (110 → 55 mm), slow motion −20 %, rest jitter < 1 mm/frame. The rest of the pipeline was cut too: JSON goes out before the JPEG (now encoded on the network thread), the fist classifier runs on its own thread, and pose and depth get separate OpenVINO thread budgets. Together these took capture→Unity from 141 ms to 19 ms on a 4-core test box. Tuning lives in `predict`, `filters` and `camera.latency_s` in `calib/config.json`. Debug line (D) shows latency and prediction horizon.
- **Occlusion (`BodyProxy.cs`, `ReflectAR_DepthOnly.shader`):** depth-only stand-ins for the body hide whatever of the garment is behind it. Parts under the garment (torso, neck, sleeved arm) are fitted capsules. Exposed parts (bare forearms, hands with fingers, head with hair) are drawn oversized, then trimmed per pixel to the person mask that BlazePose's landmark model produces (sent with each frame, `net.mask_width`): each fragment is projected into the camera image and dropped where the mask says there is no person, so the cut-out follows the real silhouette. The lookup uses where that body part was when the mask was captured, so prediction doesn't tear it, and it works in both modes. Oversized parts are pushed back by their oversize so they can't poke through sleeves or the collar. On the test clip it covers 99 % of the real hands, forearms and head (capsules alone: 87 %), and occluder pixels off the body drop from 35 % to 7 %. B shows the proxies (trimmed parts tinted warm).
- **Render pipelines (`HdrpLook.cs`, `GarmentMaterials.cs`):** the scene is set up at runtime for whichever pipeline is active. Under **HDRP**, garments are `HDRP/Lit` (opaque, double-sided) with per-fabric base colour, weave normal map and mask map (AO in the weave's valleys, smoothness per fibre: fleece 0.1, denim 0.2, nylon 0.5–0.6). They get a 3000 lux key light with soft shadows (6° angular diameter), a 700 lux fill, ambient from a soft gradient "room", SSAO and contact shadows. The body proxies cast shadows and are in the depth buffer, so hands, arms and the head shade the garment. Fixed exposure (EV100 10) and neutral tonemapping; the feed and proxy shaders divide the exposure back out, so the camera image keeps its brightness. SMAA instead of TAA (cloth has no motion vectors, so TAA would smear it), and no bloom or dithering, so black stays black on the mirror. While a garment fades in or out it switches to a transparent copy of its material. Under **URP** everything renders as before with the `ReflectAR/Garment` triplanar shader.
  - In HDRP the camera feed is drawn after the depth prepass, so it can't show through the proxies' depth; the proxies' colour pass draws the feed pixel they cover instead (`ReflectAR_DepthOnly.shader`).
- **Renderer:** `MirrorApp.cs` has two modes. *CameraFeed* places a virtual camera at the webcam pose with its intrinsics, mirrored, over the live JPEG feed. *Mirror* renders the reflected `MirrorWorld` with a Kooima off-axis frustum from the tracked eye; this is the mode the two-way-mirror build will use.
- **Garments (upper body only):** 10 tops in `GarmentCatalog.cs`: tee, V-neck, Breton long-sleeve, polo, flannel, hoodie, bomber, denim jacket, puffer, tank.
  - *Body shape* (`BodyShape.cs`): the torso is measured from the person mask at six heights between the hips and the shoulders. It walks the mask outward from the torso's centre line to the silhouette edge, then averages the ratio to the generic body over ~1.5 s into `BodyRig.Profile`. A side is skipped while its arm touches the torso (that edge would be the arm), so it measures whenever an arm is away from the body, for example while swiping. Nothing is measured unless the user faces the camera (±25°). Wider also means deeper (60 % of the width change), for chest and belly. Garments rebuild when a level changes by more than 5 %.
  - *Mesh* (`GarmentMesher.cs`): each garment is generated per user as one seamless surface. A signed-distance field smoothly blends the torso, shoulders, sleeves and collar, so armpits and shoulders have no gaps. It is meshed with surface nets and cut open at the neck, hem and cuffs. The torso cross-section follows the measured body plus ease. Below the chest it hangs from whatever is widest above it (chest or belly) by `fit` (tank 0.15, tee 0.75, hoodie 0.95), the way fabric falls instead of tucking into the waist. Ribbed hem bands (hoodie, bomber, puffer) pull back in to the hips, and gathered cuffs sit at the wrist with the sleeve bloused over them; both hold to the body in the cloth sim.
  - *UV layout*: the surface is laid out like sewn pattern pieces: front and back panels unrolled around the torso (side seams), a yoke over the shoulder tops, and each sleeve unrolled around the arm (seam underneath). Texture stretch on the tee, flannel and hoodie: median 1.0, 90th percentile ≤ 1.45, area scale within ±8 % for 90 % of the surface. Vertices are split along the seams only for drawing; the solver keeps the welded mesh, and normals come from it, so seams don't show in the shading.
  - *Closures* (`GarmentTrims.cs`): real geometry that rides on the cloth. A raised placket with sewn buttons (polo 2, flannel 8, denim with metal shanks), or a zipper with tape, teeth, slider and pull (bomber, puffer). It hangs off a chain of points found by ray down the centre front of the rest mesh; each point follows its triangle on the simulated cloth and carries a local frame, so the closure bends with the fabric.
  - *Physics* (`ClothGarment.cs`): an XPBD cloth solver, with near-inextensible stretch, soft compression so the fabric buckles into wrinkles, and bending, gravity, inertia and damping. It collides with capsules fitted to the body. The cloth is tethered to a version of the mesh skinned to six tracked bones; the tether is ~0 at the collar and shoulders and grows toward the hem and cuffs. Fabric parameters (denim stiff, jersey soft, puffer thick) are per garment.
  - *Shading*: HDRP as above. URP (`ReflectAR_Garment.shader`): seamless triplanar fabric anchored to the rest pose, so patterns stick to the cloth, with sheen and weave shading. The solver costs ~6 ms per frame in the Editor (Release code optimization).

## Calibration

- **Distance (required, done 2026-10-16 at 1.0 m and 2.0 m):** stand exactly at a measured distance and run `./run_perception.sh --calib-dist <metres>`. Each run adds a point to `perception/calib/depth_calib.json`; two points fit scale + offset for each cue. Without this file the 1–3 m prompts are wrong by up to 40 %.

- **Prediction lead (optional, Mirror mode):** wave a hand and press [ / ] until the garment stops trailing or leading the reflection, then add the shown lead to `camera.latency_s` (or `predict.display_latency_s`) in `calib/config.json`.

## Calibration TODO

- `perception/calibrate_intrinsics.py` (9×6 checkerboard) → `calib/intrinsics.json`. Until then, the FOV comes from `hfov_deg` in `calib/config.json`.
- `camera_pose` and `screen` in `calib/config.json` assume a 14" MacBook Pro. Change them for the 43" mirror build.
- Absolute distance with one camera is approximate (±10–20 %). Fusion helps, but a real stereo/ToF camera is still the target for the mirror build.
