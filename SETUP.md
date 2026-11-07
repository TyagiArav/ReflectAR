# Setting up ReflectAR on your computer

ReflectAR runs on **macOS** (Apple Silicon) and **Windows 10/11**. Setting it up takes about an hour the first time, mostly downloads: the Unreal Engine install is 40–60 GB, so start it first. You don't need a depth camera: without the RealSense D415, ReflectAR uses your laptop's webcam.

The setup script for your system checks everything below, fixes what it can, and tells you exactly what's still missing. You can run it as often as you like.

| | macOS | Windows |
|---|---|---|
| Setup script | `./setup.sh` | `setup.bat` (double-click it) |
| Start tracking | `./run_perception.sh` | `run_perception.bat` |
| C++ toolchain | Xcode (App Store) | Visual Studio 2022 17.14+ or 2026 |
| Tracking runs on | Neural Engine (Core ML) | NVIDIA GPU (CUDA) or any DirectX 12 GPU (DirectML) |

---

## 1. Install the big things first

Start these in parallel, since they take the longest.

**Both systems**
- **Git**, then clone the repository:
  ```
  git clone https://github.com/tyagiarav/reflectar.git ReflectAR
  ```
- **Python 3.11 or newer, 64-bit.**
  - macOS: `brew install python@3.12`, or the installer from python.org. The `python3` that comes with macOS is too old.
  - Windows: `winget install Python.Python.3.12`, or python.org. Tick **Add python.exe to PATH**.
- **Epic Games Launcher**, then **Unreal Engine 5.8.** Sign in (create a free Epic account if you need one), go to **Unreal Engine → Library**, click **+** next to *Engine Versions*, choose **5.8**, and click **Install**.
  - macOS: `brew install --cask epic-games`, or download it from epicgames.com.
  - Windows: `winget install EpicGames.EpicGamesLauncher`, or epicgames.com.

**macOS only**
- **Xcode** from the App Store (about 10 GB). The Command Line Tools alone are not enough. The setup script then switches to it, accepts its licence, and downloads the Metal Toolchain. Each of these asks for your password or a confirmation.

**Windows only**
- **Visual Studio 2022 (17.14 or newer) or Visual Studio 2026.** The free Community edition is fine. In the installer, tick the workload **Game development with C++**. It includes the Windows SDK. If you already have Visual Studio, open the *Visual Studio Installer*, click **Modify**, and add that workload.
- An up-to-date **GPU driver**. NVIDIA: driver 580 or newer.

## 2. Run the setup script

From the `ReflectAR` folder:

```bash
./setup.sh          # macOS
```
```
setup.bat           :: Windows, or double-click it in Explorer
```

It does the following:

1. Creates the Python environment in `perception/.venv` and installs the packages. On Windows it picks CUDA if you have an NVIDIA GPU, and DirectML otherwise.
2. Checks that the tracking models load, and shows which chip they run on.
3. Checks Xcode (macOS) or Visual Studio and the Windows SDK (Windows).
4. Finds Unreal Engine 5.8 and compiles the ReflectAR C++ module. Build output goes to `unreal/build.log`.

Every line is marked **✓ / [ok]** or **✗ / [x]**. If anything is marked ✗, fix it as the message says and run the script again.

If Unreal is installed somewhere unusual, tell the script where:
- macOS: `UE_ROOT="/path/to/UE_5.8" ./setup.sh`
- Windows (PowerShell): `$env:UE_ROOT='D:\Epic\UE_5.8'; .\setup_windows.ps1`

## 3. Run it

1. **Start tracking:** `./run_perception.sh` (macOS) or `run_perception.bat` (Windows). The first time, allow camera access when the system asks. Wait for `[net] listening on 127.0.0.1:5566`.
2. **Open** `unreal/ReflectAR.uproject` by double-clicking it.
   - The first start takes 5–15 minutes while Unreal compiles shaders and ReflectAR generates its materials. Later starts take seconds.
   - If it asks to rebuild the `ReflectAR` module, choose **Yes**.
3. Press **Play** (▶). *New Editor Window* (in the ▶ menu) gives a window you can resize.
4. **Stand 1–3 m from the screen.** Closer than that, it shows "Please step back".

**Controls:** swipe a hand sideways to browse garments, and hold up a fist for 5 s to put one on. In Play you can also use **← / →** to browse, **Space** to select, **M** to switch between camera feed and mirror, **D** for debug, and **Esc** to stop. The README lists the rest.

**No camera?** Instead of step 1, run a made-up skeleton:
```bash
cd perception
.venv/bin/python fake_server.py          # macOS
.venv\Scripts\python fake_server.py      # Windows
```

---

## Troubleshooting

| Problem | Fix |
|---|---|
| **Epic Launcher: "Install failed … Failed to save application manifest file", error IS-0007** (macOS) | The launcher forgot to create a folder. `./setup.sh` creates it; or run `mkdir -p ~/Library/Application\ Support/Epic/EpicGamesLauncher/Data/Manifests/Pending`, then click **Retry** in the launcher |
| **"Xcode Metal Compiler error: … missing Metal Toolchain"** when Unreal opens (macOS) | `xcodebuild -downloadComponent MetalToolchain` (or `./setup.sh`), then reopen the project |
| **"You have not agreed to the Xcode license"** (macOS) | `sudo xcodebuild -license`: press **q**, then type `agree` |
| `xcode-select` points at CommandLineTools (macOS) | `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer` |
| Build fails with a Visual Studio / MSVC version error (Windows) | Update Visual Studio to 17.14+ (or install 2026) in the Visual Studio Installer, with *Game development with C++* |
| "Unreal Engine 5.8 not found" | Install 5.8 in the Epic launcher (other versions won't work), or set `UE_ROOT` (see step 2) |
| **The mirror runs at ~3 fps** | The editor slows itself down when its window isn't focused. The project turns this off by default; if you changed it, open *Edit → Editor Preferences → General → Performance* and untick **Use Less CPU when in Background** |
| **Laggy, "Memory Pressure Warning"** | Unreal needs a lot of memory. Quit Chrome and other heavy apps, keep only one Unreal window open, and restart the computer if it has been running a long time. 16 GB of RAM is the practical minimum |
| Garments look grey/checkered (default material) | The materials weren't generated. In *Window → Output Log*, switch the input box from **Cmd** to **Python**, and run `import importlib, reflectar_setup; importlib.reload(reflectar_setup); reflectar_setup.build()` |
| "Please step back" / "Come a little closer" | Stand 1–3 m from the camera. With a webcam, the distance is estimated from your body and can jump around for the first few seconds |
| Stuck on "Waiting for the perception service" | Start `run_perception.sh` / `run_perception.bat` and check it prints `[net] listening on 127.0.0.1:5566` |
| `Could not open camera` | macOS: *System Settings → Privacy & Security → Camera*, allow your terminal app. Windows: *Settings → Privacy & security → Camera*, allow desktop apps, and close other apps using the camera (Teams, Zoom, browser tabs) |
| `[backend]` line says models run on the CPU (Windows) | NVIDIA: update the driver to 580+. Other GPUs: rerun `setup.bat`, which installs DirectML |
| "Load Errors: Asset Manager settings do not include an entry for … GameFeatureData" | Harmless: it comes from an editor plugin, not ReflectAR. Close the Message Log |
| `pip install` fails on `pyrealsense2` | The script retries without it. That package is only needed for the D415; the webcam works without it |

## Optional: let Claude Code control the editor

The project enables Unreal's built-in MCP server, and `.mcp.json` points Claude Code at it (`http://127.0.0.1:8000/mcp`). The server starts with the editor and only accepts connections from your own machine. With the editor open, start Claude Code in the `ReflectAR` folder and approve the `unreal-mcp` server. If it stops responding, type `ModelContextProtocol.StartServer` in the editor's **Cmd** box.
