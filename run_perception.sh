#!/bin/bash
# Start the ReflectAR perception service (webcam -> BlazePose/OpenVINO -> Unity on :5566).
cd "$(dirname "$0")/perception" && exec .venv/bin/python -u server.py --log "$@"
