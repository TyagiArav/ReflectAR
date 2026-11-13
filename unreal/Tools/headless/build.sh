#!/bin/bash
# Builds the headless test bench against the renderer's sources (g++ or clang++, no Unreal needed).
cd "$(dirname "$0")"
S=../../Source/ReflectAR
${CXX:-g++} -std=c++17 -O2 -w -I shim -I $S/Public harness.cpp $S/Private/BodyRig.cpp $S/Private/GarmentMesher.cpp \
  $S/Private/GarmentSpec.cpp $S/Private/GarmentTrims.cpp $S/Private/ClothGarment.cpp $S/Private/BodyProxy.cpp -o harness
