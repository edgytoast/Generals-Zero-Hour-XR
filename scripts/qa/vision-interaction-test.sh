#!/usr/bin/env bash
# visionOS interaction layer host tests (no device, no simulator, no game data).
#
#   scripts/qa/vision-interaction-test.sh [--existing]
#
# Builds VisionInteraction.cpp + scripts/qa/vision-interaction-test.cpp with the stand-in <openxr/openxr.h>
# (GeneralsMD/Code/Main/visionos/xr_shim) and runs the scenario tests against the recording fake engine bridge.
#
#   --existing   additionally build and run every scripts/qa/xr-*-test.cpp that compiles against the stand-in
#                (proves the shared Quest headers are untouched: same counts as before). Tests that need the OpenXR
#                SDK (xr-interaction, xr-menu-routing) are run only when OPENXR_INCLUDE points at the SDK include dir.
#
# Also runs scripts/qa/vision-bridge-forward-test.sh (the forwarding engine bridge against recording XrGameBoot_* stand-ins).
#
# Environment: CXX (default clang++), TMPDIR, OPENXR_INCLUDE (optional real OpenXR headers for the two SDK tests).
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_dir"
CXX="${CXX:-clang++}"
out_dir="$(mktemp -d "${TMPDIR:-/tmp}/vision-interaction-test.XXXXXX")"
trap 'rm -rf "$out_dir"' EXIT
main=GeneralsMD/Code/Main
shim=$main/visionos/xr_shim
d3d=Core/Libraries/Source/d3d8gles/include
flags=(-std=c++17 -Wall -Wextra -Wno-missing-field-initializers -fsanitize=undefined,address -fno-sanitize-recover=undefined
  -I"$main" -I"$main/visionos" -I"$shim" -I"$d3d")

# The Obj-C++ input glue (visionos/Input/GXXRInput.mm) is exercised too. It is compiled on its own with ONLY the
# shell include paths: on a case-insensitive filesystem "XRInteraction.h" (visionos/Platform) would otherwise resolve to
# the Quest header XrInteraction.h when GeneralsMD/Code/Main is on the include path.
"$CXX" -x objective-c++ -fobjc-arc -std=c++17 -Wall -Wextra -fsanitize=undefined,address \
  -Ivisionos/Platform -Ivisionos/Input -c visionos/Input/GXXRInput.mm -o "$out_dir/GXXRInput.o"
"$CXX" "${flags[@]}" scripts/qa/vision-interaction-test.cpp "$main/visionos/VisionInteraction.cpp" \
  "$out_dir/GXXRInput.o" -framework Foundation -o "$out_dir/vision-interaction-test"
"$out_dir/vision-interaction-test"
"$repo_dir/scripts/qa/vision-bridge-forward-test.sh"

if [[ "${1:-}" == "--existing" ]]; then
  total=0; passed=0; failed=0
  run_one() {
    local name="$1"; shift
    local extra=("$@")
    total=$((total+1))
    if "$CXX" -std=c++17 -Wall -Wextra -Wno-missing-field-initializers -fsanitize=undefined ${extra[@]+"${extra[@]}"} \
         -I"$main" -I"$d3d" "scripts/qa/$name.cpp" -o "$out_dir/$name" 2>"$out_dir/$name.log"; then
      local args=()
      case "$name" in xr-camera-test|xr-comfort-test|xr-interaction-test|xr-placement-test) args=("$out_dir/$name.tmp");; esac
      if "$out_dir/$name" ${args[@]+"${args[@]}"} >"$out_dir/$name.out" 2>&1; then
        passed=$((passed+1)); printf '  ok   %-28s %s\n' "$name" "$(tail -1 "$out_dir/$name.out")"
      else
        failed=$((failed+1)); printf '  FAIL %-28s\n' "$name"; tail -5 "$out_dir/$name.out"
      fi
    else
      printf '  SKIP %-28s (does not build against the stand-in header)\n' "$name"
      total=$((total-1))
    fi
  }
  for t in board camera comfort diorama endgame height input layers loading math menu performance placement presentation tactics world; do
    run_one "xr-$t-test" -I"$shim"
  done
  if [[ -n "${OPENXR_INCLUDE:-}" ]]; then
    for t in interaction menu-routing; do run_one "xr-$t-test" -I"$OPENXR_INCLUDE"; done
  fi
  # The bridge tests (scripts/qa/xr-*-test.sh) extract production functions from the engine sources and compile them with
  # -I <build>/vcpkg_installed/arm64-android/include (where Android gets <openxr/openxr.h>). Point that argument at a
  # directory whose include/ is the stand-in header. Tests that need the real SDK types (XrSpace, XrSession, ...), the
  # Android SDK/NDK or a GL device are skipped, not failed.
  mkdir -p "$out_dir/fakeandroid/vcpkg_installed/arm64-android"
  ln -sfn "$repo_dir/$shim" "$out_dir/fakeandroid/vcpkg_installed/arm64-android/include"
  for t in build-controls console-bridge height-bridge hover-info pick-bridge shadow-scope tactical-bridge trigger-bridge \
           panel-text workspace scene loading-presenter; do
    total=$((total+1))
    if bash "scripts/qa/xr-$t-test.sh" "$out_dir/fakeandroid" >"$out_dir/xr-$t.sh.out" 2>&1; then
      passed=$((passed+1)); printf '  ok   %-28s %s\n' "xr-$t-test.sh" "$(tail -1 "$out_dir/xr-$t.sh.out")"
    elif grep -Eq "unknown type name '(Xr|PFN_xr)" "$out_dir/xr-$t.sh.out"; then
      total=$((total-1)); printf '  SKIP %-28s (needs the real OpenXR SDK types)\n' "xr-$t-test.sh"
    else
      failed=$((failed+1)); printf '  FAIL %-28s\n' "xr-$t-test.sh"; tail -5 "$out_dir/xr-$t.sh.out"
    fi
  done
  # ground-observer reads source text
  total=$((total+1))
  if "$CXX" -std=c++17 -Wno-missing-field-initializers -I"$main" -I"$d3d" -I"$shim" scripts/qa/xr-ground-observer-test.cpp \
      -o "$out_dir/xr-ground-observer-test" 2>/dev/null &&
     "$out_dir/xr-ground-observer-test" "$main/XrHello.cpp" "$main/XrGameBoot.cpp" >"$out_dir/go.out" 2>&1; then
    passed=$((passed+1)); printf '  ok   %-28s %s\n' xr-ground-observer-test "$(tail -1 "$out_dir/go.out")"
  else
    failed=$((failed+1)); echo "  FAIL xr-ground-observer-test"
  fi
  echo "existing Quest host tests: $passed passed, $failed failed (of $total run)"
  [[ $failed -eq 0 ]]
fi
