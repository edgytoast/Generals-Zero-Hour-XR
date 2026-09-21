#!/usr/bin/env bash
# Host unit test for the visionOS spatial audio math and state (scripts/qa/vision-audio-listener-test.cpp). No simulator needed.
# Also cross-checks the board mapping against the real xrWorldToBoard() in GeneralsMD/Code/Main/XrWorld.h.
#   scripts/qa/vision-audio-listener-test.sh
# Environment: CXX (default clang++), TMPDIR.
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_dir"
CXX="${CXX:-clang++}"
out_dir="$(mktemp -d "${TMPDIR:-/tmp}/vision-audio-listener-test.XXXXXX")"
trap 'rm -rf "$out_dir"' EXIT
main=GeneralsMD/Code/Main
"$CXX" -std=c++17 -Wall -Wextra -Werror -Wno-missing-field-initializers -fsanitize=undefined,address -fno-sanitize-recover=undefined \
  -DGX_TEST_XRWORLD=1 -I"$main" -I"$main/visionos/xr_shim" -ICore/Libraries/Source/d3d8gles/include \
  scripts/qa/vision-audio-listener-test.cpp -o "$out_dir/vision-audio-listener-test"
"$out_dir/vision-audio-listener-test"
