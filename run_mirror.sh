#!/bin/bash
# Run ReflectAR the efficient way: perception + the packaged app (no Unreal Editor).
# Quit the app with Esc; perception is stopped with it.
# Package the app first from the editor (Platforms > Mac > Package Project) into unreal/Build/.
cd "$(dirname "$0")"
APP=unreal/Build/Mac/ReflectAR.app
[ -d "$APP" ] || { echo "No $APP yet: package it from the Unreal editor first."; exit 1; }
./run_perception.sh "$@" &
PERCEPTION=$!
trap 'kill $PERCEPTION 2>/dev/null' EXIT
sleep 2
open -W "$APP"
