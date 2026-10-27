#!/bin/bash
# Start the ReflectAR perception service (camera -> pose/depth on Core ML, CUDA or CPU -> Unity on :5566).
# macOS only lets root open an Intel RealSense, so with one plugged in this asks for your password.
cd "$(dirname "$0")/perception" || exit 1
if [ "$(uname)" = Darwin ] && [ "$(id -u)" != 0 ] && ioreg -p IOUSB -w0 | grep -q RealSense; then
    echo "[run] RealSense connected: starting with sudo"
    exec sudo PYTHONDONTWRITEBYTECODE=1 .venv/bin/python -u server.py --log "$@"   # no root-owned .pyc files
fi
exec .venv/bin/python -u server.py --log "$@"
