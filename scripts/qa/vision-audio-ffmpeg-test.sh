#!/usr/bin/env bash
# Builds scripts/qa/vision-audio-ffmpeg-test.mm for xrsimulator against the engine build's vcpkg FFmpeg and runs it in a
# simulator device (xcrun simctl spawn).
#
#   scripts/qa/vision-audio-ffmpeg-test.sh --udid <simulator udid>
#
# Environment: GX_BUILD_DIR (engine build dir; default <repo>/build/visionos-simulator, must contain
#   vcpkg_installed/arm64-xrsimulator), GX_AUDIO_TEST_OUT (default $TMPDIR/gx-vision-audio).
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_dir"
udid=""
while [ $# -gt 0 ]; do case "$1" in --udid) udid="$2"; shift 2;; *) echo "unknown arg $1" >&2; exit 2;; esac; done
bd="${GX_BUILD_DIR:-$repo_dir/build/visionos-simulator}"
vp="$bd/vcpkg_installed/arm64-xrsimulator"
out="${GX_AUDIO_TEST_OUT:-${TMPDIR:-/tmp}/gx-vision-audio}"
mkdir -p "$out"
[ -f "$vp/lib/libavcodec.a" ] || { echo "vcpkg ffmpeg not found at $vp (run scripts/build/visionos/build-engine.sh --simulator or set GX_BUILD_DIR)" >&2; exit 2; }
[ -n "$udid" ] || { echo "Pass --udid <simulator device>" >&2; exit 2; }
sdk="$(xcrun --sdk xrsimulator --show-sdk-path)"
xcrun --sdk xrsimulator clang++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -fobjc-arc \
  -target arm64-apple-xros2.0-simulator -isysroot "$sdk" -I"$vp/include" \
  scripts/qa/vision-audio-ffmpeg-test.mm \
  "$vp/lib/libavformat.a" "$vp/lib/libavcodec.a" "$vp/lib/libavutil.a" "$vp/lib/libswresample.a" "$vp/lib/libz.a" "$vp/lib/libbz2.a" \
  -framework Foundation -framework CoreFoundation -framework CoreVideo -framework CoreMedia -liconv \
  -o "$out/vision-audio-ffmpeg-test"
xcrun simctl bootstatus "$udid" >/dev/null 2>&1 || xcrun simctl boot "$udid" || true
xcrun simctl spawn "$udid" "$out/vision-audio-ffmpeg-test" | tee "$out/vision-audio-ffmpeg-test.log"
