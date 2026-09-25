#!/bin/zsh
# build-angle-visionos.sh  --  build ANGLE (GLES3-on-Metal) for visionOS.
#
# Lives in the repo as scripts/build/visionos/build-angle.sh.
#
# usage: build-angle-visionos.sh <simulator|device|all> [DEPS_ROOT]
#   DEPS_ROOT defaults to $DEPS_ROOT or $HOME/CandC/deps
#
# Source : WebKit's vendored ANGLE (Source/ThirdParty/ANGLE) from https://github.com/WebKit/WebKit,
#          pinned to WEBKIT_SHA below (override with WEBKIT_SHA=... ).  Built with the ANGLE.xcodeproj
#          that WebKit ships (targets "ANGLE (dynamic)" and "ANGLE (static)"), Metal backend only.
# Output : $DEPS_ROOT/angle/install/<xrsimulator|xros>/{lib,include}
#          $DEPS_ROOT/angle/install/ANGLE.xcframework   (only with 'all')
#
# Requirements: Xcode with xros/xrsimulator SDK, git, python3.  ~10-15 min per slice on an M-series Mac.
# Nothing in this script touches the game repo.
set -euo pipefail

MODE="${1:-all}"
DEPS_ROOT="${2:-${DEPS_ROOT:-$HOME/CandC/deps}}"
WEBKIT_SHA="${WEBKIT_SHA:-e2f19a74a3bbffb907f739523cc547b6c1707c35}"   # WebKit main, 2026-09-20
XROS_DEPLOYMENT_TARGET="${XROS_DEPLOYMENT_TARGET:-2.0}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

WEBKIT_DIR="$DEPS_ROOT/WebKit"
ANGLE_SRC="$WEBKIT_DIR/Source/ThirdParty/ANGLE"
BUILD_ROOT="$DEPS_ROOT/angle-build"
INSTALL_ROOT="$DEPS_ROOT/angle/install"

case "$MODE" in
  simulator) SLICES=(xrsimulator) ;;
  device)    SLICES=(xros) ;;
  all)       SLICES=(xrsimulator xros) ;;
  *) echo "usage: $0 <simulator|device|all> [DEPS_ROOT]" >&2; exit 2 ;;
esac

mkdir -p "$DEPS_ROOT" "$BUILD_ROOT" "$INSTALL_ROOT"

# ---------------------------------------------------------------------------------------------
# 1. Fetch (sparse, blobless, single commit) -- ANGLE + the xcconfigs its xcodeproj #includes
# ---------------------------------------------------------------------------------------------
if [[ ! -d "$WEBKIT_DIR/.git" ]]; then
  git clone --filter=blob:none --no-checkout --sparse https://github.com/WebKit/WebKit.git "$WEBKIT_DIR"
fi
git -C "$WEBKIT_DIR" sparse-checkout set Source/ThirdParty/ANGLE Configurations Tools/ccache
if [[ "$(git -C "$WEBKIT_DIR" rev-parse HEAD 2>/dev/null || true)" != "$WEBKIT_SHA" || ! -f "$ANGLE_SRC/ANGLE.xcodeproj/project.pbxproj" ]]; then
  git -C "$WEBKIT_DIR" cat-file -e "$WEBKIT_SHA^{commit}" 2>/dev/null || git -C "$WEBKIT_DIR" fetch --depth 1 --filter=blob:none origin "$WEBKIT_SHA"
  git -C "$WEBKIT_DIR" checkout -f "$WEBKIT_SHA"
fi

# ---------------------------------------------------------------------------------------------
# 2. Patches (idempotent).  Equivalent .patch: angle/patches/0001-angle-visionos-std-entrypoints.patch
#    a) khrplatform.h : KHRONOS_APICALL = visibility("default") on Apple.  WebKit's build is
#       -fvisibility=hidden and only exports the internal EGL_*/GL_* names; without this the
#       standard eglXxx/glXxx names are hidden.
#    b) BaseTarget.xcconfig : define *_PROTOTYPES so the Khronos prototypes (with that attribute)
#       are visible when libEGL_autogen.cpp / libGLESv2_autogen.cpp are compiled.
#    c) project.pbxproj : add libEGL_autogen.cpp (eglXxx -> EGL_Xxx) to both targets and
#       libGLESv2_autogen.cpp (glXxx -> GL_Xxx) to the static target (already in the dynamic one).
# ---------------------------------------------------------------------------------------------
python3 - "$ANGLE_SRC" <<'PYEOF'
import re, sys
root = sys.argv[1]

# (a)
p = root + '/include/KHR/khrplatform.h'
s = open(p).read()
old = '#elif defined(__ANDROID__)\n#   define KHRONOS_APICALL __attribute__((visibility("default")))'
new = '#elif defined(__ANDROID__) || defined(__APPLE__)\n#   define KHRONOS_APICALL __attribute__((visibility("default")))'
if new not in s:
    assert old in s, 'khrplatform.h changed upstream; re-do patch (a)'
    open(p, 'w').write(s.replace(old, new, 1))

# (b)
p = root + '/Configurations/BaseTarget.xcconfig'
s = open(p).read()
old = 'ANGLE_ENABLE_METAL_OWNERSHIP_IDENTITY $(GCC_PREPROCESSOR_DEFINITIONS_$(WK_PLATFORM_NAME))'
new = 'ANGLE_ENABLE_METAL_OWNERSHIP_IDENTITY GL_GLES_PROTOTYPES=1 EGL_EGL_PROTOTYPES=1 GL_GLEXT_PROTOTYPES=1 EGL_EGLEXT_PROTOTYPES=1 $(GCC_PREPROCESSOR_DEFINITIONS_$(WK_PLATFORM_NAME))'
if new not in s:
    assert old in s, 'BaseTarget.xcconfig changed upstream; re-do patch (b)'
    open(p, 'w').write(s.replace(old, new, 1))

# (c)
p = root + '/ANGLE.xcodeproj/project.pbxproj'
s = open(p).read()
if 'XRPORT' not in s:
    EGL_REF, GLES_REF = '7BB9723C2DE4827800455D13', '7BB972342DE480F000455D13'  # existing PBXFileReferences
    new = {'EGL_DYN': ('A1C0DE0000000000000E6D01', EGL_REF, 'libEGL_autogen.cpp'),
           'EGL_STA': ('A1C0DE0000000000000E6D02', EGL_REF, 'libEGL_autogen.cpp'),
           'GLES_STA': ('A1C0DE0000000000000E6D03', GLES_REF, 'libGLESv2_autogen.cpp')}
    bf = ''.join(f'\t\t{i} /* {n} in Sources (XRPORT_{k}) */ = {{isa = PBXBuildFile; fileRef = {r} /* {n} */; }};\n'
                 for k, (i, r, n) in new.items())
    s = s.replace('/* End PBXBuildFile section */', bf + '/* End PBXBuildFile section */', 1)
    def add(s, phase, entries):
        m = re.search(re.escape(phase) + r' /\* Sources \*/ = \{\n\t\t\tisa = PBXSourcesBuildPhase;\n\t\t\tbuildActionMask = \d+;\n\t\t\tfiles = \(\n', s)
        assert m, phase
        ins = ''.join(f'\t\t\t\t{i} /* {n} in Sources (XRPORT) */,\n' for i, n in entries)
        return s[:m.end()] + ins + s[m.end():]
    s = add(s, '31CDFDF12491819E00486F27', [(new['EGL_DYN'][0], 'libEGL_autogen.cpp')])                       # ANGLE (dynamic)
    s = add(s, '7BB971FD2DE47B8A00455D13', [(new['EGL_STA'][0], 'libEGL_autogen.cpp'), (new['GLES_STA'][0], 'libGLESv2_autogen.cpp')])  # ANGLE (static)
    open(p, 'w').write(s)
PYEOF

[[ "${STOP_AFTER_PATCH:-0}" == 1 ]] && { echo "patched checkout ready at $ANGLE_SRC"; exit 0; }

# ---------------------------------------------------------------------------------------------
# 3. Build each slice
#    - WK_AVAILABILITY_OVERLAY_* empty : the VFS overlay is generated by WebKit's WTF target, which we do not clone
#    - LLVM_LTO=NO                     : plain Mach-O objects/dylib (inspectable with nm; avoids ThinLTO bitcode in the .a)
#    - ANGLE_ALLOWABLE_CLIENTS empty   : otherwise LC_SUB_CLIENT restricts linking to WebCore
#    - DYLIB_INSTALL_NAME_BASE=@rpath  : embeddable in an app bundle (Frameworks/)
# ---------------------------------------------------------------------------------------------
build_slice() {
  local sdk="$1" target="$2"
  local slice_dir="$BUILD_ROOT/$sdk"
  echo "==> xcodebuild [$target] sdk=$sdk deployment=$XROS_DEPLOYMENT_TARGET"
  ( cd "$ANGLE_SRC" && xcodebuild build -project ANGLE.xcodeproj -target "$target" \
      -sdk "$sdk" -configuration Release -arch arm64 ONLY_ACTIVE_ARCH=YES \
      SYMROOT="$slice_dir/products" OBJROOT="$slice_dir/obj" \
      XROS_DEPLOYMENT_TARGET="$XROS_DEPLOYMENT_TARGET" CODE_SIGNING_ALLOWED=NO \
      WK_AVAILABILITY_OVERLAY_FLAGS= WK_AVAILABILITY_OVERLAY_SWIFT_FLAGS= LLVM_LTO=NO \
      ANGLE_ALLOWABLE_CLIENTS= DYLIB_INSTALL_NAME_BASE=@rpath \
      -jobs "$JOBS" > "$BUILD_ROOT/$sdk-${target//[ ()]/_}.log" 2>&1 ) \
    || { echo "BUILD FAILED, see $BUILD_ROOT/$sdk-${target//[ ()]/_}.log" >&2; tail -30 "$BUILD_ROOT/$sdk-${target//[ ()]/_}.log" >&2; exit 1; }
}

for sdk in "${SLICES[@]}"; do
  build_slice "$sdk" "ANGLE (dynamic)"
  build_slice "$sdk" "ANGLE (static)"

  prod="$BUILD_ROOT/$sdk/products/Release-$sdk"
  out="$INSTALL_ROOT/$sdk"
  rm -rf "$out"; mkdir -p "$out/lib" "$out/include"

  # ---- packaging ----
  cp "$prod/libANGLE-shared.dylib" "$out/lib/libANGLE-shared.dylib"
  strip -x "$out/lib/libANGLE-shared.dylib"                      # keep exported (global) symbols
  # static: libANGLE.a + libtranslator.a ; strip debug info (500MB -> much smaller)
  cp "$prod/libANGLE.a" "$out/lib/libANGLE.a"; cp "$prod/libtranslator.a" "$out/lib/libtranslator.a"
  strip -S "$out/lib/libANGLE.a" "$out/lib/libtranslator.a" 2>/dev/null || true
  # public headers (Khronos + ANGLE extensions)
  for d in EGL GLES GLES2 GLES3 KHR; do cp -R "$ANGLE_SRC/include/$d" "$out/include/$d"; done
  cp "$ANGLE_SRC/include/angle_gl.h" "$out/include/"
  cp "$ANGLE_SRC/LICENSE" "$out/LICENSE.ANGLE-BSD-3-Clause"
  echo "==> $sdk artifacts:"; ls -l "$out/lib"; lipo -info "$out/lib/libANGLE-shared.dylib"
  otool -l "$out/lib/libANGLE-shared.dylib" | grep -A4 LC_BUILD_VERSION | tr -s ' ' | paste -sd' ' -
done

# ---------------------------------------------------------------------------------------------
# 4. xcframework (dynamic library slices) when both slices were built
# ---------------------------------------------------------------------------------------------
if [[ "$MODE" == "all" ]]; then
  rm -rf "$INSTALL_ROOT/ANGLE.xcframework"
  xcodebuild -create-xcframework \
    -library "$INSTALL_ROOT/xros/lib/libANGLE-shared.dylib"        -headers "$INSTALL_ROOT/xros/include" \
    -library "$INSTALL_ROOT/xrsimulator/lib/libANGLE-shared.dylib" -headers "$INSTALL_ROOT/xrsimulator/include" \
    -output "$INSTALL_ROOT/ANGLE.xcframework"
  echo "==> $INSTALL_ROOT/ANGLE.xcframework"
fi
echo "done."
