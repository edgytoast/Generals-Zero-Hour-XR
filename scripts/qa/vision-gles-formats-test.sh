#!/bin/bash
# GeneralsX @test visionOS port - host-side (no GPU, no simulator) test of the backend's CPU texture
# conversion: compiles the production UploadDesc / DXT decoders / prepareLevelUpload verbatim out of
# gles_pipeline.cpp and checks the RGBA (or packed 16-bit) result for every D3D format the engine
# requests, with and without S3TC. ANGLE-Metal has no S3TC, so the software decode is the shipping path.
# Usage: bash scripts/qa/vision-gles-formats-test.sh
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_dir"
deps="${GX_DEPS_ROOT:-$HOME/CandC/deps}"
angle_inc="${GX_ANGLE_ROOT:-$deps/angle/install}/xrsimulator/include"
dxvk="${GX_DXVK_DIR:-$deps/src/fbraz3-dxvk}"
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/vision-gles-formats.XXXXXX")"
src=Core/Libraries/Source/d3d8gles/src/gles_pipeline.cpp
{
  sed -n '/^struct UploadDesc {/,/^};/p' "$src"
  sed -n '/^static inline void UnpackRGB565(/,/^}/p' "$src"
  sed -n '/^static void DecodeBC1Block(/,/^}/p' "$src"
  sed -n '/^static void DecodeBC3AlphaBlock(/,/^}/p' "$src"
  sed -n '/^static bool DecodeDXTLevel(/,/^}/p' "$src"
  sed -n '/^static bool prepareLevelUpload(/,/^}/p' "$src"
} > "$test_dir/production.inc"
test -s "$test_dir/production.inc"
"${CXX:-clang++}" -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-macro-redefined -fsanitize=undefined \
  -I"$test_dir" -I"$angle_inc" -I"$dxvk/include/native" -I"$dxvk/include/native/windows" -I"$dxvk/include/native/directx" \
  -IGeneralsMD/Code/CompatLib/Include -IGenerals/Code/CompatLib/Include -IDependencies/Utility -ICore/Libraries/Include \
  -ICore/Libraries/Source/WWVegas/WWLib -D_UNIX -DWIN32_LEAN_AND_MEAN \
  scripts/qa/vision-gles-formats-test.cpp -o "$test_dir/formats-test"
"$test_dir/formats-test"
