#!/bin/bash
# Merge the engine's transitive static closure into ONE archive per slice and wrap the
# slices into GeneralsZHEngine.xcframework.
#
#   make-xcframework.sh [--simulator] [--device] [--output DIR] [--no-xcframework]
#
# For every slice that was built by build-engine.sh (default: all slices whose build tree
# exists) this
#   1. asks Ninja for the resolved link line of the never-built probe target
#      z_generals_link_probe (GeneralsMD/Code/Main/CMakeLists.txt), which lists every
#      static archive z_generals needs, vcpkg ones included, plus system frameworks / -l flags;
#   2. merges the archives with `libtool -static` into
#         <out>/<slice>/libGeneralsZHEngine_all.a
#      and writes <out>/<slice>/link-inputs.txt (the archives, in link order) and
#      <out>/<slice>/link-flags.txt (system frameworks and -l libraries the host app must
#      link; each line is a ready-to-use linker flag, e.g. "-framework CoreAudio");
#   3. runs `xcodebuild -create-xcframework` over the merged archives with a staged
#      headers directory (an umbrella header GeneralsZHEngine.h and a module map, plus
#      every *.h found in GeneralsMD/Code/Main/visionos/) into
#         <out>/GeneralsZHEngine.xcframework
#
# <out> defaults to <build root>/xcframework (GX_ENGINE_BUILD_ROOT, default <repo>/build).
# ANGLE is not merged: the host app links it (scripts/build/visionos/build-angle.sh).
set -euo pipefail

SLICES=()
OUT=""
DO_XCF=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --simulator) SLICES+=(simulator) ;;
    --device)    SLICES+=(device) ;;
    --output)    OUT="${2:?--output needs a directory}"; shift ;;
    --no-xcframework) DO_XCF=0 ;;
    -h|--help)   sed -n '2,24p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "make-xcframework.sh: unknown argument '$1' (see --help)" >&2; exit 2 ;;
  esac
  shift
done

# shellcheck source=env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/env.sh"
OUT="${OUT:-$GX_ENGINE_BUILD_ROOT/xcframework}"
command -v ninja >/dev/null || { echo "ninja not found" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 1; }

if [[ ${#SLICES[@]} -eq 0 ]]; then
  for s in simulator device; do
    [[ -f "$GX_ENGINE_BUILD_ROOT/visionos-$s/GeneralsMD/Code/Main/libGeneralsZHEngine.a" ]] && SLICES+=("$s")
  done
fi
[[ ${#SLICES[@]} -gt 0 ]] || { echo "make-xcframework.sh: no built slice found under $GX_ENGINE_BUILD_ROOT; run build-engine.sh first" >&2; exit 1; }

mkdir -p "$OUT"

merge_slice() {
  local slice="$1" bdir="$GX_ENGINE_BUILD_ROOT/visionos-$1" sdir="$OUT/$1"
  local engine="$bdir/GeneralsMD/Code/Main/libGeneralsZHEngine.a"
  [[ -f "$engine" ]] || { echo "make-xcframework.sh: $engine missing; build the $slice slice first" >&2; exit 1; }
  mkdir -p "$sdir"

  echo "==> [$slice] reading the resolved link line of z_generals_link_probe"
  # The last command of the probe's build plan is its link. Build first the archives it
  # would need (a no-op after build-engine.sh) so every path in it exists.
  cmake --build "$bdir" --target z_generals -j "$GX_JOBS" >/dev/null
  ninja -C "$bdir" -t commands z_generals_link_probe | tail -n 1 > "$sdir/probe-link-command.txt"
  [[ -s "$sdir/probe-link-command.txt" ]] || { echo "empty link command; is the target z_generals_link_probe defined?" >&2; exit 1; }

  python3 - "$bdir" "$sdir" <<'PYEOF'
import os, shlex, sys
bdir, sdir = sys.argv[1], sys.argv[2]
toks = shlex.split(open(os.path.join(sdir, "probe-link-command.txt")).read())
archives, flags, seen_a, seen_f = [], [], set(), set()
i = 0
def add_flag(f):
    if f not in seen_f:
        seen_f.add(f); flags.append(f)
while i < len(toks):
    t = toks[i]
    if t == "-framework" and i + 1 < len(toks):
        add_flag("-framework " + toks[i + 1]); i += 2; continue
    if t.startswith("-Wl,-framework,"):
        add_flag("-framework " + t.split(",")[2]); i += 1; continue
    if t.startswith("-l"):
        add_flag(t)
    elif t.endswith(".a"):
        p = t if os.path.isabs(t) else os.path.normpath(os.path.join(bdir, t))
        if not os.path.exists(p):
            sys.exit("link input does not exist: %s" % p)
        if p not in seen_a:
            seen_a.add(p); archives.append(p)
    i += 1
open(os.path.join(sdir, "link-inputs.txt"), "w").write("\n".join(archives) + "\n")
open(os.path.join(sdir, "link-flags.txt"), "w").write("\n".join(flags) + "\n")
print("    %d static archives, %d system flags" % (len(archives), len(flags)))
PYEOF

  local merged="$sdir/libGeneralsZHEngine_all.a"
  rm -f "$merged"
  echo "==> [$slice] libtool -static merge -> $merged"
  # shellcheck disable=SC2046
  xcrun libtool -static -arch_only arm64 -o "$merged" $(cat "$sdir/link-inputs.txt") 2> "$sdir/libtool.log" || { cat "$sdir/libtool.log" >&2; exit 1; }
  echo "    $(du -h "$merged" | cut -f1)  $(xcrun ar t "$merged" | grep -c '\.o$') members;" \
       "$(grep -c 'same member name' "$sdir/libtool.log" || true) duplicate-member-name warnings (see $sdir/libtool.log)"
  echo "    system flags the host app must link:"; sed 's/^/      /' "$sdir/link-flags.txt"
}

for s in "${SLICES[@]}"; do merge_slice "$s"; done

(( DO_XCF )) || { echo "==> --no-xcframework: done"; exit 0; }

# --- headers -----------------------------------------------------------------------------
HDR="$OUT/headers"
rm -rf "$HDR"; mkdir -p "$HDR"
VISIONOS_SRC="$GX_REPO_ROOT/GeneralsMD/Code/Main/visionos"
shopt -s nullglob
host_headers=("$VISIONOS_SRC"/*.h)
shopt -u nullglob
{
  echo "// Generated by scripts/build/visionos/make-xcframework.sh. Do not edit."
  echo "// Umbrella header of GeneralsZHEngine.xcframework."
  echo "#pragma once"
  echo "#define GX_ENGINE_XCFRAMEWORK 1"
  for h in ${host_headers[@]+"${host_headers[@]}"}; do
    cp "$h" "$HDR/"
    echo "#include \"$(basename "$h")\""
  done
} > "$HDR/GeneralsZHEngine.h"
cat > "$HDR/module.modulemap" <<'MMEOF'
module GeneralsZHEngine {
    umbrella header "GeneralsZHEngine.h"
    export *
}
MMEOF
echo "==> headers: ${#host_headers[@]} host header(s) from GeneralsMD/Code/Main/visionos + umbrella + module map"

# --- xcframework -------------------------------------------------------------------------
XCF="$OUT/GeneralsZHEngine.xcframework"
rm -rf "$XCF"
args=()
for s in "${SLICES[@]}"; do args+=(-library "$OUT/$s/libGeneralsZHEngine_all.a" -headers "$HDR"); done
echo "==> xcodebuild -create-xcframework -> $XCF"
xcodebuild -create-xcframework "${args[@]}" -output "$XCF"
echo "==> slices in the xcframework:"
plutil -p "$XCF/Info.plist" | grep -E "LibraryIdentifier|SupportedPlatform|SupportedPlatformVariant|SupportedArchitectures" | sed 's/^/      /'
du -sh "$XCF" | sed 's/^/    size: /'
