#!/bin/bash
# Start the ReflectAR perception service (webcam -> pose/depth on Core ML, CUDA or CPU -> Unity on :5566).
cd "$(dirname "$0")/perception" && exec .venv/bin/python -u server.py --log "$@"
