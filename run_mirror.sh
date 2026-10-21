#!/bin/bash
# Run ReflectAR the efficient way: perception + the standalone app (no Unity Editor).
# Quit the app with Esc; perception is stopped with it.
# Build the app first from the Editor (File > Build Profiles > macOS) into renderer/Builds/ReflectAR.app.
cd "$(dirname "$0")"
APP=renderer/Builds/ReflectAR.app
[ -d "$APP" ] || { echo "No $APP yet: build it from Unity first."; exit 1; }
./run_perception.sh "$@" &
PERCEPTION=$!
trap 'kill $PERCEPTION 2>/dev/null' EXIT
sleep 2
open -W "$APP"
