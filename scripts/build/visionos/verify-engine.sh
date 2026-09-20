#!/bin/bash
# Verify the visionOS engine archives.
#
#   verify-engine.sh [--simulator] [--device] [--allow-unexpected] [--keep]
#
# For each slice (default: every slice that has a merged archive from make-xcframework.sh,
# build/xcframework/<slice>/libGeneralsZHEngine_all.a, else the plain z_generals archive):
#   1. architecture: exactly arm64 (lipo -info);
#   2. platform:     every object carries LC_BUILD_VERSION with platform VISIONOSSIMULATOR
#                    (simulator) or VISIONOS (device) and the deployment target
#                    GX_XROS_DEPLOYMENT_TARGET (default 2.0), read with `vtool -show-build`;
#   3. undefined symbols: the archive is partially linked (`ld -r -all_load`, which also
#      reports duplicate definitions) and then test-linked against the system frameworks
#      recorded in link-flags.txt. What stays undefined is classified into
#        EXPECTED    supplied by other packages or the host app: the D3D8 -> GLES backend
#                    (Direct3DCreate8_GLES, d3d8gles_*), the Quest XR hooks the shared engine
#                    code calls behind GX_XR_HOST (XrGameBoot_*, GX_XR_*), the host boundary
#                    (GXEngine*), and ANGLE's EGL/GLES entry points (egl*, gl*);
#        UNEXPECTED  anything else: a real unresolved dependency.
#      Extra "expected" patterns can be added with GX_EXPECTED_UNDEFINED_EXTRA (an extended
#      regex over the demangled-free C symbol names, e.g. '^_MyHostSymbol').
#
# Exit status: 1 on an architecture/platform/deployment-target mismatch or a duplicate
# definition; 2 when UNEXPECTED undefined symbols remain (unless --allow-unexpected).
set -uo pipefail

SLICES=()
ALLOW_UNEXPECTED=0
KEEP=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --simulator) SLICES+=(simulator) ;;
    --device)    SLICES+=(device) ;;
    --allow-unexpected) ALLOW_UNEXPECTED=1 ;;
    --keep)      KEEP=1 ;;
    -h|--help)   sed -n '2,26p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "verify-engine.sh: unknown argument '$1' (see --help)" >&2; exit 2 ;;
  esac
  shift
done

# shellcheck source=env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/env.sh" || exit 1
OUT="$GX_ENGINE_BUILD_ROOT/xcframework"

if [[ ${#SLICES[@]} -eq 0 ]]; then
  for s in simulator device; do
    if [[ -f "$OUT/$s/libGeneralsZHEngine_all.a" || -f "$GX_ENGINE_BUILD_ROOT/visionos-$s/GeneralsMD/Code/Main/libGeneralsZHEngine.a" ]]; then SLICES+=("$s"); fi
  done
fi
[[ ${#SLICES[@]} -gt 0 ]] || { echo "verify-engine.sh: nothing to verify; run build-engine.sh (and make-xcframework.sh) first" >&2; exit 1; }

# Regex (over "_symbol" names) of symbols that other packages / the host provide.
EXPECTED_RE='^_(Direct3DCreate8_GLES|d3d8gles_[A-Za-z0-9_]*|D3D8GLES_[A-Za-z0-9_]*|XrGameBoot_[A-Za-z0-9_]*|GX_XR_[A-Za-z0-9_]*|GXEngine[A-Za-z0-9_]*|GXHost[A-Za-z0-9_]*|VisionGameBoot[A-Za-z0-9_]*|egl[A-Z][A-Za-z0-9_]*|gl[A-Z][A-Za-z0-9_]*|EGL_[A-Za-z0-9_]*|GL_[A-Za-z0-9_]*)$'
if [[ -n "${GX_EXPECTED_UNDEFINED_EXTRA:-}" ]]; then EXPECTED_RE="$EXPECTED_RE|${GX_EXPECTED_UNDEFINED_EXTRA}"; fi

rc=0
for slice in "${SLICES[@]}"; do
  if [[ "$slice" == simulator ]]; then
    PLATFORM="VISIONOSSIMULATOR"; SDK=xrsimulator; TRIPLE="arm64-apple-xros${GX_XROS_DEPLOYMENT_TARGET}-simulator"
  else
    PLATFORM="VISIONOS"; SDK=xros; TRIPLE="arm64-apple-xros${GX_XROS_DEPLOYMENT_TARGET}"
  fi
  SYSROOT="$(xcrun --sdk "$SDK" --show-sdk-path)"
  ARCHIVE="$OUT/$slice/libGeneralsZHEngine_all.a"
  [[ -f "$ARCHIVE" ]] || ARCHIVE="$GX_ENGINE_BUILD_ROOT/visionos-$slice/GeneralsMD/Code/Main/libGeneralsZHEngine.a"
  WORK="$(mktemp -d "${TMPDIR:-/tmp}/gx-verify-$slice.XXXXXX")"
  echo
  echo "=== [$slice] $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1)) ==="

  # 1. architecture -----------------------------------------------------------------------
  arch_info="$(lipo -info "$ARCHIVE" 2>&1)"
  echo "arch: $arch_info"
  if [[ "$arch_info" != *"is architecture: arm64" ]]; then echo "FAIL: not arm64 only"; rc=1; fi

  # 2. platform / minos of every object ---------------------------------------------------
  mkdir -p "$WORK/obj"
  ( cd "$WORK/obj" && xcrun ar -x "$ARCHIVE" 2>/dev/null )
  nobj=$(find "$WORK/obj" -name '*.o' | wc -l | tr -d ' ')
  find "$WORK/obj" -name '*.o' -print0 | xargs -0 -n 1 -P 4 sh -c \
    'r=$(xcrun vtool -arch arm64 -show-build "$0" 2>/dev/null | awk "/platform/{p=\$2} /minos/{m=\$2} /sdk/{s=\$2} END{print (p?p:\"NONE\"), (m?m:\"-\"), (s?s:\"-\")}"); echo "$r"' \
    | sort | uniq -c | sort -rn > "$WORK/platforms.txt"
  echo "objects checked: $nobj (unique names after extraction; duplicates across merged archives overwrite each other)"
  echo "platform / minos / sdk histogram:"; sed 's/^/    /' "$WORK/platforms.txt"
  bad=$(grep -v -E "^ *[0-9]+ ${PLATFORM} ${GX_XROS_DEPLOYMENT_TARGET} " "$WORK/platforms.txt" || true)
  if [[ -n "$bad" ]]; then echo "FAIL: objects with a different platform or deployment target:"; echo "$bad" | sed 's/^/    /'; rc=1
  else echo "OK: all objects are $PLATFORM, minos $GX_XROS_DEPLOYMENT_TARGET"; fi

  # 3. undefined symbols ------------------------------------------------------------------
  # 3a. partial link: resolves everything the archive defines, keeps the rest undefined.
  if ! xcrun ld -r -arch arm64 -platform_version "$( [[ $slice == simulator ]] && echo xros-simulator || echo xros )" \
        "$GX_XROS_DEPLOYMENT_TARGET" "$(xcrun --sdk "$SDK" --show-sdk-version)" \
        -all_load "$ARCHIVE" -o "$WORK/merged.o" 2> "$WORK/ld-r.log"; then
    echo "FAIL: ld -r -all_load failed (duplicate definitions?):"; head -n 20 "$WORK/ld-r.log" | sed 's/^/    /'; rc=1
    (( KEEP )) || rm -rf "$WORK"; continue
  fi
  if grep -q "duplicate symbol" "$WORK/ld-r.log"; then echo "FAIL: duplicate symbols:"; grep "duplicate symbol" "$WORK/ld-r.log" | head | sed 's/^/    /'; rc=1; fi
  xcrun nm -u -arch arm64 "$WORK/merged.o" | sed 's/^ *//' | sort -u > "$WORK/undef-all.txt"
  echo "undefined after the partial link (including system libraries): $(wc -l < "$WORK/undef-all.txt" | tr -d ' ')"

  # 3b. resolve the system ones by test-linking against the SDK.
  FLAGS=()
  flagfile="$OUT/$slice/link-flags.txt"
  if [[ -f "$flagfile" ]]; then
    while IFS= read -r line; do [[ -n "$line" ]] && read -r -a parts <<< "$line" && FLAGS+=("${parts[@]}"); done < "$flagfile"
  else
    FLAGS=(-framework Foundation -framework CoreFoundation -framework CoreAudio -framework AudioToolbox -framework CoreMedia -framework CoreVideo -framework VideoToolbox -framework Security -framework SystemConfiguration -framework UIKit -framework ImageIO -framework CoreGraphics -lc++ -lz -liconv -lbz2)
    echo "note: $flagfile not found (make-xcframework.sh not run); using a default framework set"
  fi
  printf 'int main(void){return 0;}\n' > "$WORK/main.c"
  xcrun clang -target "$TRIPLE" -isysroot "$SYSROOT" "$WORK/main.c" "$WORK/merged.o" "${FLAGS[@]}" -lc++ \
        -Wl,-undefined,error -o "$WORK/test.out" > "$WORK/link.log" 2>&1
  # ld prints:  "_symbol", referenced from:
  grep -E '^  "_?.+", referenced from:' "$WORK/link.log" | sed -E 's/^  "(.*)", referenced from:$/\1/' | sort -u > "$WORK/undef-final.txt"
  if [[ ! -s "$WORK/undef-final.txt" ]] && ! grep -q "^Undefined symbols" "$WORK/link.log"; then
    if [[ -f "$WORK/test.out" ]]; then echo "test link succeeded: no undefined symbols at all"; else echo "test link failed for another reason:"; head -n 15 "$WORK/link.log" | sed 's/^/    /'; rc=1; fi
  fi
  # normalise: ld prints C++ symbols demangled; recover the raw names from the nm list.
  : > "$WORK/expected.txt"; : > "$WORK/unexpected.txt"
  xcrun nm -u -arch arm64 "$WORK/merged.o" | sed 's/^ *//' | sort -u > "$WORK/raw-undef.txt"
  # Raw symbols that the system libraries do NOT resolve = raw undefined minus what the
  # test link resolved. Do it by matching demangled names printed by ld.
  xcrun c++filt < "$WORK/raw-undef.txt" | paste -d '\t' "$WORK/raw-undef.txt" - > "$WORK/raw-demangled.tsv"
  awk -F '\t' 'NR==FNR { u[$0]=1; next } ($1 in u) || ($2 in u) { print $1 }' "$WORK/undef-final.txt" "$WORK/raw-demangled.tsv" | sort -u > "$WORK/unresolved-raw.txt"
  while IFS= read -r sym; do
    if [[ "$sym" =~ $EXPECTED_RE ]]; then echo "$sym" >> "$WORK/expected.txt"; else echo "$sym" >> "$WORK/unexpected.txt"; fi
  done < "$WORK/unresolved-raw.txt"
  n_exp=$(wc -l < "$WORK/expected.txt" | tr -d ' '); n_unexp=$(wc -l < "$WORK/unexpected.txt" | tr -d ' ')
  echo "undefined and NOT provided by the SDK: $((n_exp + n_unexp))   (expected from other packages: $n_exp, unexpected: $n_unexp)"
  if (( n_exp )); then
    echo "EXPECTED (other packages / host / ANGLE):"
    sed -E 's/^_//' "$WORK/expected.txt" | sed -E 's/^(d3d8gles_|XrGameBoot_|GX_XR_|GXEngine|egl|gl|Direct3DCreate8_).*/&/' | sed 's/^/    /'
  fi
  if (( n_unexp )); then
    echo "UNEXPECTED:"; c++filt < "$WORK/unexpected.txt" | sed 's/^/    /'
    (( ALLOW_UNEXPECTED )) || rc=$(( rc == 0 ? 2 : rc ))
  else
    echo "OK: no unexpected undefined symbols"
  fi
  if (( KEEP )); then echo "work dir kept: $WORK"; else rm -rf "$WORK"; fi
done
echo
[[ $rc -eq 0 ]] && echo "verify-engine: PASS" || echo "verify-engine: exit $rc"
exit $rc
