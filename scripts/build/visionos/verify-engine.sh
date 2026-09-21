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
#   3. undefined symbols and a real link test: symbols the archive references but does not define (nm set
#      difference), minus the ones the SDK provides (checked by linking a reference object
#      against the system frameworks recorded in link-flags.txt). What stays undefined is
#      classified into
#        EXPECTED    supplied by other packages or the host app: the D3D8 -> GLES backend
#                    (Direct3DCreate8_GLES, d3d8gles_*), the Quest XR hooks the shared engine
#                    code calls behind GX_XR_HOST (XrGameBoot_*, GX_XR_*), the host boundary
#                    (GXEngine*), and ANGLE's EGL/GLES entry points (egl*, gl*);
#        UNEXPECTED  anything else: a real unresolved dependency.
#      Extra "expected" patterns can be added with GX_EXPECTED_UNDEFINED_EXTRA (an extended
#      regex over the demangled-free C symbol names, e.g. '^_MyHostSymbol').
#
# Exit status: 1 on an architecture/platform/deployment-target mismatch; 2 when UNEXPECTED undefined symbols remain (unless --allow-unexpected).
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
# The GX_XR_* hooks are C++ functions (mangled: __Z<len>GX_XR_...), the XrGameBoot_* / d3d8gles_* / GXEngine* / GXHost*
# ones are extern "C" (plain _name); both spellings are accepted.
EXPECTED_RE='^__Z[0-9]+(GX_XR_|XrGameBoot_|GXEngine|GXHost|VisionGameBoot)[A-Za-z0-9_]*|^_(Direct3DCreate8_GLES|d3d8gles_[A-Za-z0-9_]*|D3D8GLES_[A-Za-z0-9_]*|XrGameBoot_[A-Za-z0-9_]*|GX_XR_[A-Za-z0-9_]*|GXEngine[A-Za-z0-9_]*|GXHost[A-Za-z0-9_]*|VisionGameBoot[A-Za-z0-9_]*|egl[A-Z][A-Za-z0-9_]*|gl[A-Z][A-Za-z0-9_]*|EGL_[A-Za-z0-9_]*|GL_[A-Za-z0-9_]*)$'
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
  # Static archives are linked lazily: a member is only pulled in when something needs one of
  # its symbols. The merged archive also holds members nobody references (the profile library
  # needs the Windows-only debug library, soundrobj needs the Miles wrapper, SDL's main-callback
  # helper needs UIKit, ...), and the SAME translation unit more than once (INTERFACE sources
  # are compiled into several targets). So a flat "what is undefined anywhere" list is noise.
  # Emulate the linker instead: start from the roots (the object files of z_generals' own sources:
  # EngineGlobals, LinuxStubs, SDLVisionStubs and whatever visionos/*.cpp|*.mm adds,
  # plus the symbols in GX_ENGINE_ROOT_SYMBOLS, default the mangled GameMain(): __Z8GameMainv), pull the first member of the
  # archive that defines each needed symbol, and report what stays undefined.
  DUPES=$(xcrun ar t "$ARCHIVE" | grep '\.o$' | sort | uniq -c | awk '$1 > 1' | wc -l | tr -d ' ')
  echo "member names that occur more than once (same TU compiled into several archives, or same basename in different libraries): $DUPES"
  # Roots: the object files of the z_generals target's own sources (CMake names them <file>.o).
  MAIN_SRC="$GX_REPO_ROOT/GeneralsMD/Code/Main"
  { for f in EngineGlobals.cpp LinuxStubs.cpp SDLVisionStubs.cpp; do echo "$f.o"; done
    shopt -s nullglob; for f in "$MAIN_SRC"/visionos/*.cpp "$MAIN_SRC"/visionos/*.mm; do echo "$(basename "$f").o"; done; shopt -u nullglob
  } > "$WORK/root-members.txt"
  xcrun nm -arch arm64 -m "$ARCHIVE" > "$WORK/nm-members.txt" 2>/dev/null
  python3 - "$WORK/nm-members.txt" "$WORK/root-members.txt" "${GX_ENGINE_ROOT_SYMBOLS:-__Z8GameMainv}" "$WORK" <<'PYEOF'
import re, sys
nm_file, roots_file, root_syms, work = sys.argv[1:5]
members = []                    # [name, defined(set), strong_undef(set), weak_undef(set)]
cur = None
head = re.compile(r'^(?:.*\()?([^() ]+\.o)\)?:$')
for line in open(nm_file, errors="replace"):
    line = line.rstrip("\n")
    m = head.match(line)
    if m:
        cur = [m.group(1), set(), set(), set()]; members.append(cur); continue
    if cur is None or not line.strip():
        continue
    if "(undefined)" in line:
        name = line.split()[-1]
        (cur[3] if " weak " in line or "weak external" in line else cur[2]).add(name)
    elif " external " in line or " weak external " in line:
        name = line.split()[-1]
        cur[1].add(name)
first_def = {}
for i, (n, d, su, wu) in enumerate(members):
    for sym in d:
        first_def.setdefault(sym, i)
root_names = set(l.strip() for l in open(roots_file) if l.strip())
pulled, work_q = set(), []
for i, (n, *_r) in enumerate(members):
    if n in root_names:
        pulled.add(i); work_q.append(i)
for sym in root_syms.split():
    if sym in first_def and first_def[sym] not in pulled:
        pulled.add(first_def[sym]); work_q.append(first_def[sym])
defined = set()
for i in pulled: defined |= members[i][1]
unresolved = set()
while work_q:
    i = work_q.pop()
    defined |= members[i][1]
    for sym in members[i][2]:
        if sym in defined:
            continue
        j = first_def.get(sym)
        if j is None:
            unresolved.add(sym)
        elif j not in pulled:
            pulled.add(j); work_q.append(j); defined |= members[j][1]
unresolved -= defined
# ld synthesises the Objective-C selector stubs (_objc_msgSend$<selector>) itself
unresolved = set(u for u in unresolved if not u.startswith('_objc_msgSend$'))
open(work + "/residual.txt", "w").write("\n".join(sorted(unresolved)) + ("\n" if unresolved else ""))
if not members or not pulled:
    sys.exit("verify-engine: could not parse the archive symbol table (%d members, %d pulled)" % (len(members), len(pulled)))
print("linker emulation: %d of %d members pulled in from %d root member(s)%s; %d symbols referenced but not defined by the archive (system libraries included)"
      % (len(pulled), len(members), sum(1 for m in members if m[0] in root_names),
         " + " + root_syms if root_syms.strip() else "", len(unresolved)))
PYEOF
  if [[ $? -ne 0 ]]; then echo "FAIL: linker emulation failed"; rc=1; (( KEEP )) || rm -rf "$WORK"; continue; fi

  # Resolve the system ones by linking a reference object against the SDK: an assembly file
  # that takes the address of every residual symbol; ld lists what the SDK does not provide.
  FLAGS=()
  flagfile="$OUT/$slice/link-flags.txt"
  if [[ -f "$flagfile" ]]; then
    while IFS= read -r line; do
      [[ -n "$line" ]] || continue
      read -r -a parts <<< "$line"; FLAGS+=("${parts[@]}")
    done < "$flagfile"
  else
    FLAGS=(-framework Foundation -framework CoreFoundation -framework CoreAudio -framework AudioToolbox -framework CoreMedia -framework CoreVideo -framework VideoToolbox -framework Security -framework SystemConfiguration -framework UIKit -framework ImageIO -framework CoreGraphics -lobjc -lm)
    echo "note: $flagfile not found (make-xcframework.sh not run); using a default framework set"
  fi
  { echo ".data"; echo ".p2align 3"; while IFS= read -r sym; do printf '.quad "%s"\n' "$sym"; done < "$WORK/residual.txt"; } > "$WORK/refs.s"
  printf 'int main(void){return 0;}\n' > "$WORK/main.c"
  if ! xcrun clang -target "$TRIPLE" -isysroot "$SYSROOT" -c "$WORK/refs.s" -o "$WORK/refs.o" 2> "$WORK/asm.log"; then
    echo "FAIL: could not assemble the reference object:"; head -n 10 "$WORK/asm.log" | sed 's/^/    /'; rc=1
    (( KEEP )) || rm -rf "$WORK"; continue
  fi
  xcrun clang -target "$TRIPLE" -isysroot "$SYSROOT" "$WORK/main.c" "$WORK/refs.o" "${FLAGS[@]}" -lc++ \
        -o "$WORK/test.out" > "$WORK/link.log" 2>&1
  # ld prints:  "_symbol", referenced from:   (C++ names demangled)
  grep -E '^  "_?.+", referenced from:' "$WORK/link.log" | sed -E 's/^  "(.*)", referenced from:$/\1/' | sort -u > "$WORK/undef-final.txt"
  if [[ ! -s "$WORK/undef-final.txt" && ! -f "$WORK/test.out" ]]; then
    echo "FAIL: the reference link failed for a reason other than undefined symbols:"; head -n 15 "$WORK/link.log" | sed 's/^/    /'; rc=1
  fi
  # Map ld's (possibly demangled) names back to the raw symbol names.
  xcrun c++filt < "$WORK/residual.txt" | paste -d '\t' "$WORK/residual.txt" - > "$WORK/residual-demangled.tsv"
  awk -F '\t' 'NR==FNR { u[$0]=1; next } ($1 in u) || ($2 in u) { print $1 }' "$WORK/undef-final.txt" "$WORK/residual-demangled.tsv" | sort -u > "$WORK/unresolved-raw.txt"
  : > "$WORK/expected.txt"; : > "$WORK/unexpected.txt"
  while IFS= read -r sym; do
    if [[ "$sym" =~ $EXPECTED_RE ]]; then echo "$sym" >> "$WORK/expected.txt"; else echo "$sym" >> "$WORK/unexpected.txt"; fi
  done < "$WORK/unresolved-raw.txt"
  n_exp=$(wc -l < "$WORK/expected.txt" | tr -d ' '); n_unexp=$(wc -l < "$WORK/unexpected.txt" | tr -d ' ')
  echo "undefined and NOT provided by the SDK: $((n_exp + n_unexp))   (expected from other packages: $n_exp, unexpected: $n_unexp)"
  if (( n_exp )); then
    echo "EXPECTED (other packages / host / ANGLE):"
    sed -E 's/^_//; s/^/    /' "$WORK/expected.txt"
  fi
  if (( n_unexp )); then
    echo "UNEXPECTED:"; xcrun c++filt < "$WORK/unexpected.txt" | sed 's/^/    /'
    (( ALLOW_UNEXPECTED )) || rc=$(( rc == 0 ? 2 : rc ))
  else
    echo "OK: no unexpected undefined symbols"
  fi

  # 4. real link test -----------------------------------------------------------------------
  # Link an actual executable the way the host app would: lazily, from a main() that calls
  # GameMain(), with assembly definitions standing in for the EXPECTED symbols. This proves
  # the merged archive and the recorded system flags resolve everything else, and that the
  # archive holds no conflicting duplicate definitions among the members that get pulled in.
  if (( n_unexp == 0 )); then
    { echo ".data"; echo ".p2align 3"
      while IFS= read -r sym; do printf '.globl "%s"\n"%s":\n.quad 0\n' "$sym" "$sym"; done < "$WORK/expected.txt"
    } > "$WORK/stubs.s"
    printf 'int GameMain(void);\nint main(void){return GameMain();}\n' > "$WORK/linkmain.cpp"
    if xcrun clang++ -target "$TRIPLE" -isysroot "$SYSROOT" -c "$WORK/stubs.s" -o "$WORK/stubs.o" 2> "$WORK/stubs.log" \
       && xcrun clang++ -target "$TRIPLE" -isysroot "$SYSROOT" "$WORK/linkmain.cpp" "$WORK/stubs.o" "$ARCHIVE" "${FLAGS[@]}" -lc++ \
            -o "$WORK/linktest" > "$WORK/linktest.log" 2>&1; then
      echo "OK: real link test produced an executable ($(du -h "$WORK/linktest" | cut -f1)); platform: $(xcrun vtool -arch arm64 -show-build "$WORK/linktest" | awk '/platform/{print $2}')"
    else
      echo "FAIL: real link test:"; grep -v "warning:" "$WORK/linktest.log" "$WORK/stubs.log" 2>/dev/null | head -n 25 | sed 's/^/    /'; rc=1
    fi
  fi
  if (( KEEP )); then echo "work dir kept: $WORK"; else rm -rf "$WORK"; fi
done
echo
[[ $rc -eq 0 ]] && echo "verify-engine: PASS" || echo "verify-engine: exit $rc"
exit $rc
