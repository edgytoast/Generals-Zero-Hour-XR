#!/usr/bin/env bash
# Builds scripts/qa/vision-audio-openal-test.mm for xrsimulator against the engine build's static libopenal.a and
# runs it inside a simulator device (xcrun simctl spawn).
#
#   scripts/qa/vision-audio-openal-test.sh [--udid <simulator udid>] [--host]
#
# Environment:
#   GX_OPENAL_LIB      path to libopenal.a for xrsimulator (default: <build>/_deps/openal_soft-build/libopenal.a; NEEDS the
#                      alsem fix, see scripts/qa/vision-audio-build-openal.sh)
#   GX_OPENAL_INCLUDE  openal-soft include dir (default: <repo>/build/visionos-simulator/_deps/openal_soft-src/include)
#   GX_AUDIO_TEST_OUT  output directory (default: $TMPDIR/gx-vision-audio)
# --host runs the same test as a macOS binary (no simulator) against a macOS libopenal, for quick iteration only.
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_dir"
udid=""; host=0
while [ $# -gt 0 ]; do case "$1" in --udid) udid="$2"; shift 2;; --host) host=1; shift;; *) echo "unknown arg $1" >&2; exit 2;; esac; done
bd="${GX_BUILD_DIR:-$repo_dir/build/visionos-simulator}"
lib="${GX_OPENAL_LIB:-$bd/_deps/openal_soft-build/libopenal.a}"
inc="${GX_OPENAL_INCLUDE:-$bd/_deps/openal_soft-src/include}"
out="${GX_AUDIO_TEST_OUT:-${TMPDIR:-/tmp}/gx-vision-audio}"
mkdir -p "$out"
[ -f "$lib" ] || { echo "libopenal.a not found at $lib (run scripts/build/visionos/build-engine.sh --simulator or set GX_OPENAL_LIB)" >&2; exit 2; }
[ -f "$inc/AL/alext.h" ] || { echo "openal headers not found at $inc (set GX_OPENAL_INCLUDE)" >&2; exit 2; }
if nm "$lib" 2>/dev/null | grep -q "U _sem_init"; then
  echo "WARNING: $lib uses POSIX sem_init (unpatched openal-soft): alcCreateContext will abort on visionOS." >&2
  echo "         Build a fixed one with scripts/qa/vision-audio-build-openal.sh and pass it via GX_OPENAL_LIB." >&2
fi
sdk="$(xcrun --sdk xrsimulator --show-sdk-path)"
xcrun --sdk xrsimulator clang++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -fobjc-arc \
  -target arm64-apple-xros2.0-simulator -isysroot "$sdk" -I"$inc" \
  scripts/qa/vision-audio-openal-test.mm "$lib" \
  -framework Foundation -framework AudioToolbox -framework CoreAudio -framework CoreFoundation \
  -o "$out/vision-audio-openal-test"
if [ -z "$udid" ]; then udid="$(cat "${GX_SIM_UDID_FILE:-/nonexistent}" 2>/dev/null || true)"; fi
[ -n "$udid" ] || { echo "Pass --udid <simulator device> (create one: xcrun simctl create GXR-audio com.apple.CoreSimulator.SimDeviceType.Apple-Vision-Pro-4K com.apple.CoreSimulator.SimRuntime.xrOS-26-5)" >&2; exit 2; }
xcrun simctl bootstatus "$udid" >/dev/null 2>&1 || xcrun simctl boot "$udid" || true
xcrun simctl spawn "$udid" "$out/vision-audio-openal-test" | tee "$out/vision-audio-openal-test.log"
