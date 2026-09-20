#!/usr/bin/env bash
# Host test for the visionOS game-data validator + import transaction. Builds the two portable
# C++ sources in visionos/GameData with the test driver and runs them. All fixtures are
# synthetic (tiny fabricated .big files); no retail data is used.
#
# Usage:
#   scripts/qa/vision-gamedata-test.sh                      run the suite
#   scripts/qa/vision-gamedata-test.sh ZERO_HOUR [GENERALS] additionally validate a real/fake
#                                                           folder read-only and print the report
#   CXX=clang++ OUT_DIR=/some/dir scripts/qa/vision-gamedata-test.sh
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out_dir="${OUT_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/gxgd-test.XXXXXX")}"
cxx="${CXX:-c++}"
mkdir -p "$out_dir"
"$cxx" -std=c++17 -O1 -g -Wall -Wextra \
    -o "$out_dir/vision-gamedata-test" \
    "$repo_dir/scripts/qa/vision-gamedata-test.cpp" \
    "$repo_dir/visionos/GameData/GXGameDataValidator.cpp" \
    "$repo_dir/visionos/GameData/GXGameDataInstall.cpp"
"$out_dir/vision-gamedata-test"
if [[ $# -gt 0 ]]; then
    "$out_dir/vision-gamedata-test" --validate "$@"
fi
echo "Test binary: $out_dir/vision-gamedata-test"
