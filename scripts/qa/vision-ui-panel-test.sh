#!/usr/bin/env bash
# Host test for the visionOS spatial-UI panel model and actions.
#
#   scripts/qa/vision-ui-panel-test.sh [--existing]
#
# Compiles GeneralsMD/Code/Main/visionos/VisionPanelModel.cpp and VisionCommandActions.cpp (pure C++, macOS clang++,
# ASan + UBSan, no SDK, no engine) against a recording fake of the engine seams and runs scripts/qa/vision-ui-panel-test.cpp.
# With --existing the unchanged interaction / bridge host tests are run as well.
#
# Environment: CXX (default clang++), TMPDIR.
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_dir"
CXX="${CXX:-clang++}"
out_dir="$(mktemp -d "${TMPDIR:-/tmp}/vision-ui-panel-test.XXXXXX")"
trap 'rm -rf "$out_dir"' EXIT
main=GeneralsMD/Code/Main
"$CXX" -std=c++17 -Wall -Wextra -Wno-missing-field-initializers -fsanitize=undefined,address -fno-sanitize-recover=undefined \
  -I"$main/visionos" -I"$main" -I"$main/visionos/xr_shim" -ICore/Libraries/Source/WWVegas/WWLib -ICore/Libraries/Source/d3d8gles/include \
  scripts/qa/vision-ui-panel-test.cpp "$main/visionos/VisionPanelModel.cpp" "$main/visionos/VisionCommandActions.cpp" \
  -o "$out_dir/vision-ui-panel-test"
"$out_dir/vision-ui-panel-test"
if [[ "${1:-}" == "--existing" ]]; then
  scripts/qa/vision-interaction-test.sh --existing
  scripts/qa/vision-bridge-forward-test.sh
fi
