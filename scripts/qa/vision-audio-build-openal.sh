#!/usr/bin/env bash
# Builds a libopenal.a for the visionOS simulator (or device) WITH the semaphore fix from
# scripts/qa/vision-audio-openal-alsem.patch, for the audio tests, from the openal-soft source tree the engine
# build already fetched (<build>/_deps/openal_soft-src, already carrying cmake/patches/openal-soft-1.24.2-visionos.patch).
#
# Why: openal-soft 1.24.2's common/alsem.h only uses libdispatch semaphores for TARGET_OS_IOS/TV; on visionOS it falls back
# to POSIX sem_init, which Darwin does not implement, so alcCreateContext throws std::system_error(EAGAIN) and the app
# aborts. Once the alsem hunk is part of cmake/patches/openal-soft-1.24.2-visionos.patch this script is unnecessary.
#
#   scripts/qa/vision-audio-build-openal.sh [--device]
# Environment: GX_BUILD_DIR (engine build dir, default <repo>/build/visionos-simulator), GX_DEPS_ROOT (default
# $HOME/CandC/deps), GX_AUDIO_OPENAL_OUT (default $GX_DEPS_ROOT/au-audio-build). Prints the library path.
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
sysroot=xrsimulator; slice=sim
if [ "${1:-}" = "--device" ]; then sysroot=xros; slice=dev; fi
bd="${GX_BUILD_DIR:-$repo_dir/build/visionos-$([ $slice = sim ] && echo simulator || echo device)}"
deps="${GX_DEPS_ROOT:-$HOME/CandC/deps}"
out="${GX_AUDIO_OPENAL_OUT:-$deps/au-audio-build}"
src_in="$bd/_deps/openal_soft-src"
[ -f "$src_in/common/alsem.h" ] || { echo "openal-soft source not found at $src_in (run scripts/build/visionos/build-engine.sh first or set GX_BUILD_DIR)" >&2; exit 2; }
mkdir -p "$out"
if [ ! -f "$out/openal-src-$slice/common/alsem.h" ]; then
  rm -rf "$out/openal-src-$slice"; cp -R "$src_in" "$out/openal-src-$slice"; rm -rf "$out/openal-src-$slice/.git"
fi
if ! grep -q "TARGET_OS_VISION" "$out/openal-src-$slice/common/alsem.h"; then
  (cd "$out/openal-src-$slice" && patch -p1 < "$repo_dir/scripts/qa/vision-audio-openal-alsem.patch")
fi
cmake -S "$out/openal-src-$slice" -B "$out/openal-$slice" -G Ninja -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT=$sysroot \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=2.0 -DLIBTYPE=STATIC -DALSOFT_RTKIT=OFF -DALSOFT_EXAMPLES=OFF \
  -DALSOFT_UTILS=OFF -DALSOFT_TESTS=OFF -DALSOFT_NO_CONFIG_UTIL=ON -DALSOFT_INSTALL=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo >"$out/cfg-$slice.log"
cmake --build "$out/openal-$slice" -j "${GX_JOBS:-4}" >"$out/build-$slice.log"
nm "$out/openal-$slice/libopenal.a" 2>/dev/null | grep -q "U _sem_init" && { echo "still references sem_init: patch not applied" >&2; exit 1; }
echo "$out/openal-$slice/libopenal.a"
