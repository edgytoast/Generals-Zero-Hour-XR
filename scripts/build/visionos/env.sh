#!/bin/bash
# Shared environment for the visionOS engine build scripts. Source it, do not run it:
#
#   source scripts/build/visionos/env.sh
#
# Every default below can be overridden by exporting the variable first.
#
#   GX_DEPS_ROOT          Directory that holds the big out-of-repo dependencies
#                         (vcpkg checkout, ANGLE build, download and binary caches).
#                         Default: /Users/jvadala/CandC/deps
#   VCPKG_ROOT            vcpkg checkout used by the CMake presets (toolchain file).
#                         Default: $GX_DEPS_ROOT/vcpkg. Must be a FULL clone at a commit
#                         that already supports visionOS (vcpkg PR #45464, June 2025);
#                         the manifest baseline in vcpkg.json only pins port versions.
#   VCPKG_DOWNLOADS       Source-tarball cache shared by all triplets.
#   VCPKG_BINARY_SOURCES  Compiled-package cache. Default: a read/write file cache under
#                         $GX_DEPS_ROOT so third-party packages compile once per triplet.
#   GX_ANGLE_INSTALL      ANGLE install prefix (scripts/build/visionos/build-angle.sh).
#   GX_ENGINE_BUILD_ROOT  Where the engine build trees and merged archives go.
#                         Default: <repo>/build (git-ignored).
#   GX_XROS_DEPLOYMENT_TARGET  visionOS deployment target of the engine and its vcpkg
#                         libraries (default 2.0). It is hard-coded in cmake/triplets/*.cmake
#                         and the presets; changing it here alone is a configure error.
#   GX_JOBS               Parallel build jobs (default 4: the machine is often shared).
#
# The script exits non-zero when it is executed instead of sourced, and returns non-zero
# from `source` when a prerequisite is missing.

if [[ "${BASH_SOURCE[0]:-$0}" == "$0" ]]; then
  echo "env.sh must be sourced: source ${0}" >&2
  exit 2
fi

_gx_env_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GX_REPO_ROOT="$(cd "$_gx_env_dir/../../.." && pwd)"
export GX_REPO_ROOT

export GX_DEPS_ROOT="${GX_DEPS_ROOT:-/Users/jvadala/CandC/deps}"
export VCPKG_ROOT="${VCPKG_ROOT:-$GX_DEPS_ROOT/vcpkg}"
export VCPKG_DOWNLOADS="${VCPKG_DOWNLOADS:-$GX_DEPS_ROOT/vcpkg-downloads}"
export GX_VCPKG_BINARY_CACHE="${GX_VCPKG_BINARY_CACHE:-$GX_DEPS_ROOT/vcpkg-binary-cache}"
export VCPKG_BINARY_SOURCES="${VCPKG_BINARY_SOURCES:-clear;files,$GX_VCPKG_BINARY_CACHE,readwrite}"
export VCPKG_DISABLE_METRICS=1
export GX_ANGLE_INSTALL="${GX_ANGLE_INSTALL:-$GX_DEPS_ROOT/angle/install}"
export GX_ENGINE_BUILD_ROOT="${GX_ENGINE_BUILD_ROOT:-$GX_REPO_ROOT/build}"
export GX_XROS_DEPLOYMENT_TARGET="${GX_XROS_DEPLOYMENT_TARGET:-2.0}"
export GX_JOBS="${GX_JOBS:-4}"
# vcpkg builds one package at a time by default per port but each port fans out; keep it
# in step with the build jobs so a shared machine is not overloaded.
export VCPKG_MAX_CONCURRENCY="${VCPKG_MAX_CONCURRENCY:-$GX_JOBS}"

mkdir -p "$GX_VCPKG_BINARY_CACHE" "$VCPKG_DOWNLOADS" 2>/dev/null || true

_gx_fail=0
_gx_need() {  # _gx_need <command> <hint>
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "env.sh: '$1' not found. $2" >&2
    _gx_fail=1
  fi
}

_gx_need cmake "Install CMake >= 3.28 (brew install cmake)."
_gx_need ninja "Install Ninja (brew install ninja)."
_gx_need git "Install git."
_gx_need xcrun "Install Xcode with the visionOS SDK and select it (xcode-select -s)."
_gx_need libtool "Install the Xcode command line tools."
_gx_need pkg-config "Install pkg-config (brew install pkg-config); FFmpeg is found through it."

# CMake >= 3.28 is required for CMAKE_SYSTEM_NAME=visionOS.
if command -v cmake >/dev/null 2>&1; then
  _gx_cmake_ver="$(cmake --version | head -n1 | sed -E 's/[^0-9]*([0-9]+\.[0-9]+(\.[0-9]+)?).*/\1/')"
  _gx_major="${_gx_cmake_ver%%.*}"; _gx_rest="${_gx_cmake_ver#*.}"; _gx_minor="${_gx_rest%%.*}"
  if (( _gx_major < 3 || (_gx_major == 3 && _gx_minor < 28) )); then
    echo "env.sh: CMake $_gx_cmake_ver is too old; visionOS needs CMake >= 3.28." >&2
    _gx_fail=1
  fi
fi

for _gx_sdk in xros xrsimulator; do
  if ! xcrun --sdk "$_gx_sdk" --show-sdk-path >/dev/null 2>&1; then
    echo "env.sh: the '$_gx_sdk' SDK is missing. Install the visionOS platform in Xcode (Settings > Components)." >&2
    _gx_fail=1
  fi
done

if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
  echo "env.sh: no bootstrapped vcpkg at VCPKG_ROOT=$VCPKG_ROOT." >&2
  echo "        git clone https://github.com/microsoft/vcpkg.git \"$VCPKG_ROOT\" && \"$VCPKG_ROOT/bootstrap-vcpkg.sh\" -disableMetrics" >&2
  _gx_fail=1
elif [[ ! -f "$VCPKG_ROOT/scripts/cmake/vcpkg_common_definitions.cmake" ]] \
     || ! grep -q "VCPKG_TARGET_IS_VISIONOS" "$VCPKG_ROOT/scripts/cmake/vcpkg_common_definitions.cmake"; then
  echo "env.sh: the vcpkg checkout at $VCPKG_ROOT predates visionOS support; update it (git pull; bootstrap-vcpkg.sh)." >&2
  _gx_fail=1
fi

# Not fatal here: only the app link and the render backend need ANGLE, the engine archive does not.
if [[ ! -d "$GX_ANGLE_INSTALL" ]]; then
  echo "env.sh: note: ANGLE is not built at $GX_ANGLE_INSTALL (scripts/build/visionos/build-angle.sh); the engine archive does not need it." >&2
fi

unset -f _gx_need
unset _gx_env_dir _gx_sdk _gx_cmake_ver _gx_major _gx_rest _gx_minor
if (( _gx_fail )); then unset _gx_fail; return 1; fi
unset _gx_fail
return 0
