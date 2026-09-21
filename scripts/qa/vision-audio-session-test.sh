#!/usr/bin/env bash
# Builds visionos/Audio/GXXRAudioSession.mm + scripts/qa/vision-audio-session-test.mm for xrsimulator and runs them in a simulator
# device. With GX_OPENAL_LIB (a libopenal.a that has the alsem fix, see vision-audio-build-openal.sh) an OpenAL Soft device is also
# opened on the configured session for every spatial mode.
#   scripts/qa/vision-audio-session-test.sh --udid <simulator udid>
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_dir"
udid=""
while [ $# -gt 0 ]; do case "$1" in --udid) udid="$2"; shift 2;; *) echo "unknown arg $1" >&2; exit 2;; esac; done
[ -n "$udid" ] || { echo "Pass --udid <simulator device>" >&2; exit 2; }
out="${GX_AUDIO_TEST_OUT:-${TMPDIR:-/tmp}/gx-vision-audio}"
mkdir -p "$out"
sdk="$(xcrun --sdk xrsimulator --show-sdk-path)"
extra=(); libs=()
if [ -n "${GX_OPENAL_LIB:-}" ]; then
  extra=(-DGX_TEST_WITH_OPENAL=1 -I"${GX_OPENAL_INCLUDE:?set GX_OPENAL_INCLUDE with GX_OPENAL_LIB}")
  libs=("$GX_OPENAL_LIB" -framework AudioToolbox -framework CoreAudio -framework CoreFoundation)
fi
xcrun --sdk xrsimulator clang++ -std=c++17 -O0 -g -Wall -Wextra -Wno-unused-parameter -fobjc-arc -ObjC++ \
  -target arm64-apple-xros2.0-simulator -isysroot "$sdk" ${extra[@]+"${extra[@]}"} -Ivisionos/Audio \
  scripts/qa/vision-audio-session-test.mm visionos/Audio/GXXRAudioSession.mm ${libs[@]+"${libs[@]}"} \
  -framework Foundation -framework AVFAudio -framework UIKit \
  -o "$out/vision-audio-session-test"
xcrun simctl bootstatus "$udid" >/dev/null 2>&1 || xcrun simctl boot "$udid" || true
xcrun simctl spawn "$udid" "$out/vision-audio-session-test" 2>&1 | tee "$out/vision-audio-session-test.log"
