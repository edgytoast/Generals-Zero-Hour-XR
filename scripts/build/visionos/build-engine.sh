#!/bin/bash
# Build the Zero Hour engine as a STATIC library for visionOS (target z_generals ->
# libGeneralsZHEngine.a) with the CMake presets visionos-simulator / visionos-device.
#
#   build-engine.sh [--simulator | --device | --both] [--clean] [-j N] [--no-warning-summary]
#
#   --simulator  xrsimulator SDK, arm64 (default)
#   --device     xros SDK, arm64
#   --both       simulator, then device
#   --clean      delete the slice's build tree first (the vcpkg binary cache is kept, so
#                third-party packages are restored, not recompiled)
#   -j N         parallel jobs (default $GX_JOBS, 4: the machine is often shared)
#
# Only target z_generals is built, never 'all': z_wwaudio (the Miles wrapper) does not
# compile off Windows. The build tree is <repo>/build/<preset> (override the root with
# GX_ENGINE_BUILD_ROOT). The vcpkg dependencies are installed by the CMake configure step
# (manifest mode, overlay triplets and ports); see docs/BUILD/VISIONOS.md.
#
# Afterwards run make-xcframework.sh (merge + xcframework) and verify-engine.sh.
set -euo pipefail

SLICES=()
CLEAN=0
WARN_SUMMARY=1
JOBS_ARG=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --simulator) SLICES+=(simulator) ;;
    --device)    SLICES+=(device) ;;
    --both)      SLICES=(simulator device) ;;
    --clean)     CLEAN=1 ;;
    --no-warning-summary) WARN_SUMMARY=0 ;;
    -j)          JOBS_ARG="${2:?-j needs a number}"; shift ;;
    -j*)         JOBS_ARG="${1#-j}" ;;
    -h|--help)   sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "build-engine.sh: unknown argument '$1' (see --help)" >&2; exit 2 ;;
  esac
  shift
done
[[ ${#SLICES[@]} -gt 0 ]] || SLICES=(simulator)

# shellcheck source=env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/env.sh"
JOBS="${JOBS_ARG:-$GX_JOBS}"
export VCPKG_MAX_CONCURRENCY="$JOBS"
LOG_DIR="$GX_ENGINE_BUILD_ROOT/logs"
mkdir -p "$LOG_DIR"

fmt_elapsed() { printf '%dm%02ds' $(( $1 / 60 )) $(( $1 % 60 )); }

build_slice() {
  local slice="$1" preset="visionos-$1"
  local bdir="$GX_ENGINE_BUILD_ROOT/$preset"
  local log="$LOG_DIR/build-engine-$slice.log"
  local t0 t1 t2

  echo "==> [$slice] preset $preset, build tree $bdir, -j $JOBS"
  if (( CLEAN )); then
    echo "==> [$slice] --clean: removing $bdir"
    rm -rf "$bdir"
  fi

  t0=$SECONDS
  echo "==> [$slice] configure (installs vcpkg dependencies on first use; log: $log)"
  ( cd "$GX_REPO_ROOT" && cmake --preset "$preset" -B "$bdir" ) 2>&1 | tee "$log"
  t1=$SECONDS
  echo "==> [$slice] configure done in $(fmt_elapsed $((t1 - t0)))"

  echo "==> [$slice] build target z_generals"
  cmake --build "$bdir" --target z_generals -j "$JOBS" 2>&1 | tee -a "$log"
  t2=$SECONDS
  echo "==> [$slice] build done in $(fmt_elapsed $((t2 - t1))) (total $(fmt_elapsed $((t2 - t0))))"

  local lib="$bdir/GeneralsMD/Code/Main/libGeneralsZHEngine.a"
  [[ -f "$lib" ]] || { echo "build-engine.sh: expected archive missing: $lib" >&2; exit 1; }
  echo "==> [$slice] archive: $lib ($(du -h "$lib" | cut -f1))"

  if (( WARN_SUMMARY )); then
    local total
    total="$(grep -c 'warning:' "$log" || true)"
    echo "==> [$slice] compiler warnings: $total (top categories):"
    grep -o '\[-W[A-Za-z0-9+_=-]*\]' "$log" | sort | uniq -c | sort -rn | head -8 | sed 's/^/      /' || true
  fi
}

for s in "${SLICES[@]}"; do build_slice "$s"; done
echo "==> done. Next: scripts/build/visionos/make-xcframework.sh && scripts/build/visionos/verify-engine.sh"
