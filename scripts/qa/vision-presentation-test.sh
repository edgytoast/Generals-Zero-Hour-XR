#!/usr/bin/env bash
# visionOS presentation layer host tests (package C2): graphics settings, eye sizing, world frame, presentation state machine
# (scenarios), panel layout + hit-testing through the real interaction layer, readability arithmetic, feedback, workspace
# persistence and the update / step / finish pipeline. No device, no simulator, no game data.
#
#   scripts/qa/vision-presentation-test.sh
#
# Environment: CXX (default clang++), TMPDIR.
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_dir"
CXX="${CXX:-clang++}"
out_dir="$(mktemp -d "${TMPDIR:-/tmp}/vision-presentation-test.XXXXXX")"
trap 'rm -rf "$out_dir"' EXIT
main=GeneralsMD/Code/Main
shim=$main/visionos/xr_shim
d3d=Core/Libraries/Source/d3d8gles/include
flags=(-std=c++17 -Wall -Wextra -Wno-missing-field-initializers -fsanitize=undefined,address -fno-sanitize-recover=undefined
  -I"$main" -I"$main/visionos" -I"$shim" -I"$d3d")
"$CXX" "${flags[@]}" scripts/qa/vision-presentation-test.cpp "$main/visionos/VisionPresentation.cpp" "$main/visionos/VisionInteraction.cpp" \
  "$main/visionos/GXGraphicsSettings.cpp" -o "$out_dir/vision-presentation-test"
"$out_dir/vision-presentation-test"
