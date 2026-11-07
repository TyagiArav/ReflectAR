#!/bin/bash
# One-time ReflectAR setup for macOS: Python environment, model check, Unreal + Xcode checks, C++ build.
# Safe to run again: every step skips what is already done. See SETUP.md for the full guide.
#   ./setup.sh            set up everything
#   ./setup.sh --no-build skip compiling the Unreal module
set -u
cd "$(dirname "$0")" || exit 1
ROOT="$PWD"
UE_VERSION=5.8
UE_ROOT="${UE_ROOT:-/Users/Shared/Epic Games/UE_$UE_VERSION}"
BUILD=1
[ "${1:-}" = "--no-build" ] && BUILD=0

ok()   { printf '  \033[32m✓\033[0m %s\n' "$1"; }
warn() { printf '  \033[33m!\033[0m %s\n' "$1"; }
fail() { printf '  \033[31m✗\033[0m %s\n' "$1"; }
step() { printf '\n\033[1m%s\033[0m\n' "$1"; }
ask()  { read -r -p "    $1 [Y/n] " a; [ -z "$a" ] || [ "$a" = y ] || [ "$a" = Y ]; }
PROBLEMS=0

[ "$(uname)" = Darwin ] || { echo "This script is for macOS. On Windows run setup.bat."; exit 1; }

# ---------------------------------------------------------------- Python
step "1. Python environment (perception/.venv)"
PY=""
for c in python3.13 python3.12 python3.11 python3; do
    p=$(command -v "$c" 2>/dev/null) || continue
    "$p" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)' 2>/dev/null && { PY="$p"; break; }
done
if [ -z "$PY" ]; then
    fail "Python 3.11 or newer not found (macOS's built-in python3 is too old)."
    echo "    Install it with Homebrew:  brew install python@3.12   (or from python.org), then run ./setup.sh again."
    exit 1
fi
ok "Found $("$PY" --version) at $PY"
VENV=perception/.venv
if [ ! -x "$VENV/bin/python" ]; then
    "$PY" -m venv "$VENV" || { fail "Could not create $VENV"; exit 1; }
    ok "Created $VENV"
fi
ok "perception/.venv runs $("$VENV/bin/python" --version)"
# a venv made by uv has no pip: add it
"$VENV/bin/python" -m pip --version >/dev/null 2>&1 || "$VENV/bin/python" -m ensurepip --upgrade >/dev/null
"$VENV/bin/python" -m pip install -q --upgrade pip
if "$VENV/bin/python" -m pip install -q -r perception/requirements.txt; then
    ok "Installed perception/requirements.txt"
else
    warn "Full install failed; retrying without the RealSense package (only needed for a D415)"
    grep -v pyrealsense2 perception/requirements.txt > "$VENV/requirements-no-realsense.txt"
    "$VENV/bin/python" -m pip install -q -r "$VENV/requirements-no-realsense.txt" || { fail "pip install failed (see above)"; exit 1; }
    ok "Installed requirements (without RealSense: the webcam path works)"
fi

step "2. Model check"
if (cd perception && .venv/bin/python -c "import json; from backend import Backend; from blazepose import BlazePose; from handpose import HandPose; from depth import MonoDepth; b = Backend(json.load(open('calib/config.json'))['compute']); BlazePose(b); HandPose(b); MonoDepth(b); print('    ' + b.summary())"); then
    ok "All models load"
else
    fail "The models did not load (see the error above)"; PROBLEMS=1
fi

# ---------------------------------------------------------------- Xcode
step "3. Xcode (needed to compile the Unreal project's C++)"
if [ ! -d /Applications/Xcode.app ]; then
    fail "Xcode is not installed. Get it from the App Store (the Command Line Tools alone are not enough), open it once, then run ./setup.sh again."
    PROBLEMS=1
else
    if [ "$(xcode-select -p 2>/dev/null)" != /Applications/Xcode.app/Contents/Developer ]; then
        warn "The active developer directory is $(xcode-select -p 2>/dev/null), not Xcode.app"
        if ask "Switch to Xcode.app now? (asks for your password)"; then
            sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
        fi
    fi
    if ! xcodebuild -license check >/dev/null 2>&1; then
        warn "The Xcode licence has not been accepted"
        if ask "Show and accept it now? (asks for your password; press q, then type agree)"; then
            sudo xcodebuild -license
        fi
    fi
    if ! xcodebuild -checkFirstLaunchStatus >/dev/null 2>&1; then
        warn "Xcode's first-launch components are not installed"
        if ask "Install them now? (asks for your password)"; then
            sudo xcodebuild -runFirstLaunch
        fi
    fi
    if ! xcrun -f metal >/dev/null 2>&1; then
        warn "The Metal Toolchain is missing (Xcode 26+ no longer bundles it; Unreal needs it for shaders)"
        if ask "Download it now? (about 800 MB)"; then
            xcodebuild -downloadComponent MetalToolchain
        fi
    fi
    if xcodebuild -license check >/dev/null 2>&1 && xcrun -f metal >/dev/null 2>&1 \
        && [ "$(xcode-select -p)" = /Applications/Xcode.app/Contents/Developer ]; then
        ok "$(xcodebuild -version | head -1) is ready, with the Metal Toolchain"
    else
        fail "Xcode is not fully set up yet (see the warnings above)"; PROBLEMS=1
    fi
fi

# ---------------------------------------------------------------- Unreal
step "4. Unreal Engine $UE_VERSION"
# The Epic Games Launcher sometimes fails an install with IS-0007 because it never creates this folder
LAUNCHER_DATA="$HOME/Library/Application Support/Epic/EpicGamesLauncher/Data"
[ -d "$LAUNCHER_DATA" ] && mkdir -p "$LAUNCHER_DATA/Manifests/Pending"
if [ ! -x "$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" ]; then
    fail "Unreal Engine $UE_VERSION not found at $UE_ROOT"
    echo "    Install the Epic Games Launcher (brew install --cask epic-games, or epicgames.com), sign in,"
    echo "    then Unreal Engine > Library > + > $UE_VERSION. Installed somewhere else? Run: UE_ROOT=/path/to/UE_$UE_VERSION ./setup.sh"
    PROBLEMS=1
else
    ok "Found $(grep -E '"(Major|Minor|Patch)Version"' "$UE_ROOT/Engine/Build/Build.version" | tr -dc '0-9\n' | paste -sd. -) at $UE_ROOT"
    if [ $BUILD = 1 ]; then
        step "5. Compiling the ReflectAR editor module (a few minutes the first time)"
        if "$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" ReflectAREditor Mac Development \
            -Project="$ROOT/unreal/ReflectAR.uproject" -WaitMutex > "$ROOT/unreal/build.log" 2>&1; then
            ok "Build succeeded"
        else
            grep -E "error|Error" "$ROOT/unreal/build.log" | head -15
            fail "Build failed: full log in unreal/build.log"; PROBLEMS=1
        fi
    fi
fi

# ---------------------------------------------------------------- done
step "Summary"
if [ $PROBLEMS = 0 ]; then
    ok "Everything is set up."
    cat <<'EOF'

    Next:
      1. ./run_perception.sh          (allow camera access for your terminal when macOS asks)
      2. open unreal/ReflectAR.uproject, wait for shaders to compile, press Play
      3. stand 1-3 m from the screen
    No camera handy?  cd perception && .venv/bin/python fake_server.py   instead of step 1.
EOF
else
    fail "Some steps need attention (marked ✗ above). Fix them and run ./setup.sh again; see SETUP.md > Troubleshooting."
    exit 1
fi
