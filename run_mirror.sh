#!/bin/bash
# Run ReflectAR the efficient way: perception + the packaged app (no Unreal Editor).
# Quit the app with Esc; perception is stopped with it.
# Package the app first from the editor (Platforms > Mac > Package Project) into unreal/Build/.
cd "$(dirname "$0")"
APP=unreal/Build/Mac/ReflectAR.app
[ -d "$APP" ] || { echo "No $APP yet: package it from the Unreal editor first."; exit 1; }
if [ "$(id -u)" != 0 ] && ioreg -p IOUSB -w0 | grep -q RealSense; then
    # RealSense: perception runs as root (see run_perception.sh); ask for the password now,
    # because the background job can't, and stop it with sudo since we can't signal it
    sudo -v || exit 1
    ./run_perception.sh "$@" &
    PERCEPTION=$!
    trap 'sudo kill -INT $PERCEPTION 2>/dev/null' EXIT
else
    ./run_perception.sh "$@" &
    PERCEPTION=$!
    trap 'kill $PERCEPTION 2>/dev/null' EXIT
fi
sleep 2
open -W "$APP"
