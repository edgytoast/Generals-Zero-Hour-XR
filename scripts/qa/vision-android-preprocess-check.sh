#!/bin/bash
# Proves, as far as possible without the Android NDK, that the visionOS work on XrGameBoot.{h,cpp}
# left the Android build logically unchanged: preprocess the file BEFORE (git revision, default
# visionos-port) and AFTER (working tree) with -D__ANDROID__ (and without GX_PLATFORM_VISIONOS), with
# stub <jni.h> / <android/log.h>, strip line markers and blank lines, and diff. Every difference must be
# explained (see docs/visionos-engine-host.md "Android equivalence check").
#
#   scripts/qa/vision-android-preprocess-check.sh [--before-rev REV] [--build-dir DIR] [--out DIR]
#
# The include paths and definitions come from the visionOS simulator build tree (the engine headers are
# platform independent); it must have been configured (scripts/build/visionos/build-engine.sh --simulator).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REV=visionos-port
BUILD="$ROOT/build/visionos-simulator"
OUT="${TMPDIR:-/tmp}/gx-android-pp"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --before-rev) REV="$2"; shift 2 ;;
    --build-dir) BUILD="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
mkdir -p "$OUT/stubs/android" "$OUT/before" "$OUT/after"
: > "$OUT/stubs/jni.h"; : > "$OUT/stubs/android/log.h"
git -C "$ROOT" show "$REV:GeneralsMD/Code/Main/XrGameBoot.cpp" > "$OUT/before/XrGameBoot.cpp"
git -C "$ROOT" show "$REV:GeneralsMD/Code/Main/XrGameBoot.h" > "$OUT/before/XrGameBoot.h"
cp "$ROOT/GeneralsMD/Code/Main/XrGameBoot.cpp" "$ROOT/GeneralsMD/Code/Main/XrGameBoot.h" "$OUT/after/"

# Flags of the real compile command (includes and -D), minus output/dependency options and the visionOS macro.
CMD="$(ninja -C "$BUILD" -t commands GeneralsMD/Code/Main/CMakeFiles/z_generals.dir/XrGameBoot.cpp.o | tail -1)"
FLAGS=()
read -r -a TOK <<< "$CMD"
skip=0
for ((i=0;i<${#TOK[@]};i++)); do
  t="${TOK[$i]}"
  if (( skip )); then
    # arguments following -isystem / -isysroot / -arch / -target / -MF / -MT / -o / -c
    FLAGS+=("$t"); skip=0; continue
  fi
  case "$t" in
    -I*|-D*|-std=*) [[ "$t" == "-DGX_PLATFORM_VISIONOS=1" ]] || FLAGS+=("$t") ;;
    -isystem|-isysroot) FLAGS+=("$t"); skip=1 ;;
  esac
done
run_pp() {  # $1 = dir with XrGameBoot.{cpp,h}, $2 = input file name, $3 = output
  xcrun clang++ -x c++ -E -D__ANDROID__ -UGX_PLATFORM_VISIONOS -I"$OUT/stubs" "${FLAGS[@]}" "$1/$2" 2> "$3.err" \
    | grep -v '^# [0-9]' | grep -v '^[[:space:]]*$' > "$3"
}
for f in XrGameBoot.cpp XrGameBoot.h; do
  run_pp "$OUT/before" "$f" "$OUT/before.$f.i"
  run_pp "$OUT/after"  "$f" "$OUT/after.$f.i"
  echo "== $f: before $(wc -l < "$OUT/before.$f.i") lines, after $(wc -l < "$OUT/after.$f.i") lines"
  diff -u "$OUT/before.$f.i" "$OUT/after.$f.i" > "$OUT/$f.diff" || true
  echo "   diff: $(grep -c '^[-+][^-+]' "$OUT/$f.diff" || true) changed lines -> $OUT/$f.diff"
  # Moving the boot tail into a function only reorders lines; the multiset diff shows what really changed.
  diff <(sort "$OUT/before.$f.i") <(sort "$OUT/after.$f.i") > "$OUT/$f.sorted.diff" || true
  echo "   order-insensitive diff: $(grep -c '^[<>]' "$OUT/$f.sorted.diff" || true) lines -> $OUT/$f.sorted.diff"
done
echo "preprocessor errors (before / after):"; grep -c "error:" "$OUT"/before.*.err "$OUT"/after.*.err || true
