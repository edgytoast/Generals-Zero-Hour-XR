#!/usr/bin/env bash
# Host test for the forwarding engine bridge (GeneralsMD/Code/Main/visionos/VisionEngineBridgeXr.cpp).
#
#   scripts/qa/vision-bridge-forward-test.sh
#
# Compiles VisionEngineBridgeXr.cpp with GX_XR_HOST against a copy of the REAL XrGameBoot.h (the Android/JNI wrapper
# removed by sed, so the declarations are exactly the engine host's) and links it with recording XrGameBoot_* stand-ins.
# Once package C makes XrGameBoot.h host-neutral the sed becomes a no-op and the same test keeps working.
#
# Environment: CXX (default clang++), TMPDIR.
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_dir"
CXX="${CXX:-clang++}"
out_dir="$(mktemp -d "${TMPDIR:-/tmp}/vision-bridge-forward-test.XXXXXX")"
trap 'rm -rf "$out_dir"' EXIT
main=GeneralsMD/Code/Main
mkdir -p "$out_dir/inc"
sed -e 's|^#ifdef __ANDROID__|#if 1|' \
    -e '/^#include <jni.h>/d' \
    -e '/JNIEnv/d' \
    -e 's|^#include "Lib/BaseType.h".*|typedef int Bool;|' \
    "$main/XrGameBoot.h" > "$out_dir/inc/XrGameBoot.h"
"$CXX" -std=c++17 -Wall -Wextra -Wno-missing-field-initializers -fsanitize=undefined,address -fno-sanitize-recover=undefined \
  -DGX_XR_HOST=1 -DGX_VISION_BRIDGE_NO_ENGINE_QUERIES=1 \
  -I"$out_dir/inc" -I"$main/visionos" -I"$main" -I"$main/visionos/xr_shim" \
  -ICore/Libraries/Source/WWVegas/WWLib -ICore/Libraries/Source/d3d8gles/include \
  scripts/qa/vision-bridge-forward-test.cpp "$main/visionos/VisionEngineBridgeXr.cpp" -o "$out_dir/vision-bridge-forward-test"
"$out_dir/vision-bridge-forward-test"
