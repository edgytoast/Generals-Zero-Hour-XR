#!/bin/bash
# GeneralsX @test visionOS port - build and run the d3d8gles-on-ANGLE-Metal device test.
#
#   scripts/qa/vision-gles-device-test.sh [--udid <simulator UDID>] [--out <dir>] [--build-only]
#   scripts/qa/vision-gles-device-test.sh --macos [--out <dir>]      (debug aid, see below)
#
# Builds, for the xrsimulator SDK (arm64):
#   * the d3d8gles backend sources (d3d8gles.cpp, gles_dispatch.cpp) -- compiled straight from the
#     repo with the same defines the CMake target uses (GX_PLATFORM_VISIONOS, GX_D3D8GLES_BACKEND);
#   * the test (vision-gles-device-cases.cpp: D3D8 API + d3d8gles_* calls, vision-gles-device-test.mm:
#     ANGLE EGL/Metal host);
#   * SDL stubs (the backend only needs SDL_GetTicks/SDL_GetError; the windowed GL path is never
#     entered in XR mode), so no SDL build is required;
# and links them against ANGLE's libANGLE-shared.dylib. The binary is a plain command-line
# simulator-platform executable: `xcrun simctl spawn <udid>` runs it inside the visionOS simulator.
#
# Environment (all optional):
#   GX_DEPS_ROOT     root of the out-of-repo dependencies (default /Users/jvadala/CandC/deps)
#   GX_ANGLE_ROOT    ANGLE install root produced by build-angle.sh (default $GX_DEPS_ROOT/angle/install)
#   GX_DXVK_DIR      checkout providing the D3D8 headers   (default $GX_DEPS_ROOT/src/fbraz3-dxvk)
#   GX_SDL_INCLUDE   SDL3 headers                          (default $GX_DEPS_ROOT/src/SDL-release-3.4.2/include)
#   GX_ANGLE_MACOS_LIB  directory with a macOS libANGLE-shared.dylib (only for --macos)
#
# --macos builds the same test for the host Mac against a macOS ANGLE-Metal and runs it directly. It
# exists to debug the harness while the visionOS simulator is unavailable: a macOS GPU exposes S3TC/BC,
# a different depth format mapping and other limits, so it is NOT evidence about visionOS; the "no S3TC"
# and format checks are expected to differ there.
#
# Never use the shared default simulator: create your own, e.g.
#   xcrun simctl create GXR-gles com.apple.CoreSimulator.SimDeviceType.Apple-Vision-Pro com.apple.CoreSimulator.SimRuntime.xrOS-26-5
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEPS="${GX_DEPS_ROOT:-/Users/jvadala/CandC/deps}"
ANGLE="${GX_ANGLE_ROOT:-$DEPS/angle/install}/xrsimulator"
DXVK="${GX_DXVK_DIR:-$DEPS/src/fbraz3-dxvk}"
SDLINC="${GX_SDL_INCLUDE:-$DEPS/src/SDL-release-3.4.2/include}"
OUT="$REPO/build/vision-gles-test"
UDID=""
BUILD_ONLY=0
MACOS=0
while [ $# -gt 0 ]; do
  case "$1" in
    --udid) UDID="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --build-only) BUILD_ONLY=1; shift ;;
    --macos) MACOS=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
SDKNAME=xrsimulator; TARGET=arm64-apple-xros2.0-simulator; ANGLELIB="$ANGLE/lib"
if [ "$MACOS" = 1 ]; then
  SDKNAME=macosx; TARGET=arm64-apple-macos14.0; ANGLELIB="${GX_ANGLE_MACOS_LIB:?set GX_ANGLE_MACOS_LIB for --macos}"
fi
for p in "$ANGLELIB/libANGLE-shared.dylib" "$ANGLE/include/GLES3/gl3.h" "$DXVK/include/native/directx/d3d8.h" "$SDLINC/SDL3/SDL.h"; do
  [ -e "$p" ] || { echo "missing $p (set GX_DEPS_ROOT / GX_ANGLE_ROOT / GX_DXVK_DIR / GX_SDL_INCLUDE)" >&2; exit 2; }
done
mkdir -p "$OUT"

D3D8GLES="$REPO/Core/Libraries/Source/d3d8gles"
COMMON=(-arch arm64 --target=$TARGET -O1 -g -Wall -Wno-unused -Wno-unknown-pragmas -Wno-macro-redefined
        -DGX_PLATFORM_VISIONOS=1 -DGX_D3D8GLES_BACKEND=1 -D_UNIX -DWIN32_LEAN_AND_MEAN)
CXXI=(-I"$REPO/GeneralsMD/Code/CompatLib/Include" -I"$REPO/Generals/Code/CompatLib/Include"
      -I"$DXVK/include/native" -I"$DXVK/include/native/windows" -I"$DXVK/include/native/directx"
      -I"$REPO/Dependencies/Utility" -I"$REPO/Core/Libraries/Include"
      -I"$REPO/Core/Libraries/Source/WWVegas/WWLib" -I"$REPO/Core/Libraries/Source/WWVegas"
      -I"$REPO/Core/Libraries/Source/WWVegas/WWDebug" -I"$REPO/Core/Libraries/Source/WWVegas/WWMath"
      -I"$D3D8GLES/include" -I"$D3D8GLES/src" -I"$ANGLE/include" -I"$SDLINC")
CXX=(xcrun --sdk $SDKNAME clang++ -std=c++20 "${COMMON[@]}")

echo "== compile d3d8gles backend"
"${CXX[@]}" "${CXXI[@]}" -c "$D3D8GLES/src/d3d8gles.cpp" -o "$OUT/d3d8gles.o"
"${CXX[@]}" "${CXXI[@]}" -c "$D3D8GLES/src/gles_dispatch.cpp" -o "$OUT/gles_dispatch.o"
echo "== compile test"
"${CXX[@]}" "${CXXI[@]}" -c "$REPO/scripts/qa/vision-gles-device-cases.cpp" -o "$OUT/cases.o"
"${CXX[@]}" "${CXXI[@]}" -fobjc-arc -ObjC++ -c "$REPO/scripts/qa/vision-gles-device-test.mm" -o "$OUT/host.o"
"${CXX[@]}" "${CXXI[@]}" -c "$REPO/scripts/qa/vision-gles-sdl-stubs.cpp" -o "$OUT/sdlstubs.o"
echo "== link"
xcrun --sdk $SDKNAME clang++ -arch arm64 --target=$TARGET \
  "$OUT/cases.o" "$OUT/host.o" "$OUT/d3d8gles.o" "$OUT/gles_dispatch.o" "$OUT/sdlstubs.o" \
  -L"$ANGLELIB" -lANGLE-shared -Wl,-rpath,"$ANGLELIB" \
  -framework Metal -framework Foundation -o "$OUT/vision-gles-device-test"
echo "built $OUT/vision-gles-device-test"
[ "$BUILD_ONLY" = 1 ] && exit 0

if [ "$MACOS" = 1 ]; then
  set +e
  "$OUT/vision-gles-device-test" 2>&1 | tee "$OUT/run-macos.log"
  exit "${PIPESTATUS[0]}"
fi

if [ -z "$UDID" ]; then
  echo "no --udid given: not running (create a private simulator and pass --udid)" >&2
  exit 0
fi
echo "== run in simulator $UDID"
xcrun simctl bootstatus "$UDID" -b >/dev/null 2>&1 || xcrun simctl boot "$UDID" || true
set +e
xcrun simctl spawn "$UDID" "$OUT/vision-gles-device-test" 2>&1 | tee "$OUT/run.log"
rc=${PIPESTATUS[0]}
set -e
echo "exit code: $rc (log: $OUT/run.log)"
exit "$rc"
