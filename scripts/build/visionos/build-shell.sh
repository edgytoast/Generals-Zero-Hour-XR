#!/bin/bash
# Build the native visionOS shell (GeneralsZHXR) with XcodeGen + xcodebuild.
#
#   build-shell.sh simulator|device [--derived-data DIR] [--configuration Debug|Release] [--clean] [--no-build-engine]
#
#   simulator  xrsimulator SDK, runs in the visionOS simulator (no signing needed)
#   device     xros SDK. Without DEVELOPMENT_TEAM: an UNSIGNED compile-only check (CODE_SIGNING_ALLOWED=NO)
#              that cannot be installed on a headset. With DEVELOPMENT_TEAM=<your team id> in the environment:
#              a signed development build (automatic signing, -allowProvisioningUpdates). Add
#              GX_DEVICE_ID=<headset UDID from `xcrun devicectl list devices`> so Xcode can register the headset
#              in your team's provisioning profile.
#
# ANGLE (GLES 3.0 on Metal) must be built first with scripts/build/visionos/build-angle.sh. The install
# root defaults to build-angle.sh's default; override it with GX_ANGLE_ROOT=/path/to/angle/install.
# GX_ANGLE_LINK=static (default) links libANGLE.a + libtranslator.a into the app; GX_ANGLE_LINK=shared
# embeds and signs libANGLE-shared.dylib in Frameworks/.
#
# The Zero Hour engine is linked into the app as GeneralsZHEngine.xcframework (scripts/build/visionos/make-xcframework.sh).
# When the slice for the selected mode is missing this script builds it first (build-engine.sh + make-xcframework.sh,
# minutes: pass --no-build-engine to get the exact commands instead). GX_ENGINE_XCFRAMEWORK overrides the location
# (default <repo>/build/xcframework/GeneralsZHEngine.xcframework); GX_ENGINE_BUILD_ROOT the engine build root.
#
# Prints the built .app path on the last line of stdout (everything else goes to stderr).
set -euo pipefail

MODE="${1:-}"
[[ "$MODE" == "simulator" || "$MODE" == "device" ]] || { echo "usage: $0 simulator|device [--derived-data DIR] [--configuration Debug|Release] [--clean] [--no-build-engine]" >&2; exit 2; }
shift

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SPEC="$ROOT/visionos/project.yml"
DERIVED="${GXX_VISIONOS_DERIVED_DATA:-/private/tmp/GeneralsZHXR-DerivedData}"
CONFIG="Debug"
CLEAN=0
BUILD_ENGINE=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --derived-data) DERIVED="$2"; shift 2 ;;
    --configuration) CONFIG="$2"; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    --no-build-engine) BUILD_ENGINE=0; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

command -v xcodegen >/dev/null || { echo "xcodegen not found (brew install xcodegen)" >&2; exit 1; }

# ---- ANGLE ----------------------------------------------------------------------------------
ANGLE_ROOT="${GX_ANGLE_ROOT:-$HOME/CandC/deps/angle/install}"   # build-angle.sh's default install root
ANGLE_LINK="${GX_ANGLE_LINK:-static}"
[[ "$ANGLE_LINK" == "static" || "$ANGLE_LINK" == "shared" ]] || { echo "GX_ANGLE_LINK must be static or shared" >&2; exit 2; }
ANGLE_SLICE="xrsimulator"; [[ "$MODE" == "device" ]] && ANGLE_SLICE="xros"
if [[ "$ANGLE_LINK" == "static" ]]; then ANGLE_NEED="$ANGLE_ROOT/$ANGLE_SLICE/lib/libANGLE.a"; else ANGLE_NEED="$ANGLE_ROOT/$ANGLE_SLICE/lib/libANGLE-shared.dylib"; fi
if [[ ! -f "$ANGLE_NEED" || ! -d "$ANGLE_ROOT/$ANGLE_SLICE/include/EGL" ]]; then
  BUILD_MODE="simulator"; [[ "$MODE" == "device" ]] && BUILD_MODE="device"
  cat >&2 <<EOF
error: ANGLE for the $ANGLE_SLICE slice was not found (missing $ANGLE_NEED).
       The visionOS shell renders GLES 3.0 through ANGLE (Metal). Build it first:

         scripts/build/visionos/build-angle.sh $BUILD_MODE      # or "all" for both slices

       It installs to $ANGLE_ROOT by default (override with the DEPS_ROOT argument of that script),
       or point this script at an existing install with GX_ANGLE_ROOT=/path/to/angle/install.
EOF
  exit 1
fi
export GX_ANGLE_ROOT="$ANGLE_ROOT" GX_ANGLE_LINK="$ANGLE_LINK"

# ---- Engine library (xcframework) ------------------------------------------------------------
# shellcheck source=env.sh
source "$ROOT/scripts/build/visionos/env.sh" >&2
ENGINE_SLICE_DIR="xros-arm64-simulator"; ENGINE_FLAG="--simulator"; ENGINE_SLICE_NAME="simulator"
if [[ "$MODE" == "device" ]]; then ENGINE_SLICE_DIR="xros-arm64"; ENGINE_FLAG="--device"; ENGINE_SLICE_NAME="device"; fi
export GX_ENGINE_XCFRAMEWORK="${GX_ENGINE_XCFRAMEWORK:-$GX_ENGINE_BUILD_ROOT/xcframework/GeneralsZHEngine.xcframework}"
if [[ ! -f "$GX_ENGINE_XCFRAMEWORK/$ENGINE_SLICE_DIR/libGeneralsZHEngine_all.a" ]]; then
  CMDS="scripts/build/visionos/build-engine.sh $ENGINE_FLAG && scripts/build/visionos/make-xcframework.sh"
  if [[ "$BUILD_ENGINE" == 0 ]]; then
    {
      echo "error: the engine library for the $ENGINE_SLICE_NAME slice is missing ($GX_ENGINE_XCFRAMEWORK/$ENGINE_SLICE_DIR)."
      echo "       Build it (a few minutes for the simulator, 15+ for the device) with:"
      echo
      echo "         $CMDS"
      echo
      echo "       or run this script without --no-build-engine."
    } >&2
    exit 1
  fi
  echo "==> engine library for the $ENGINE_SLICE_NAME slice not found; building it first: $CMDS" >&2
  ( cd "$ROOT" && scripts/build/visionos/build-engine.sh "$ENGINE_FLAG" && scripts/build/visionos/make-xcframework.sh ) >&2   # no slice flag: merges every slice that has a build tree
  [[ -f "$GX_ENGINE_XCFRAMEWORK/$ENGINE_SLICE_DIR/libGeneralsZHEngine_all.a" ]] || { echo "error: the engine build did not produce $GX_ENGINE_XCFRAMEWORK/$ENGINE_SLICE_DIR" >&2; exit 1; }
fi
LDFLAGS_FILE="$GX_ENGINE_BUILD_ROOT/xcframework/$ENGINE_SLICE_NAME/link-flags.txt"
ENGINE_LDFLAGS=""
if [[ -f "$LDFLAGS_FILE" ]]; then ENGINE_LDFLAGS="$(tr '\n' ' ' < "$LDFLAGS_FILE") -lc++"; fi

if [[ "$MODE" == "simulator" ]]; then
  SDK="xrsimulator"; DEST="generic/platform=visionOS Simulator"; PRODUCTS="$CONFIG-xrsimulator"
  SIGN_ARGS=()
else
  SDK="xros"; DEST="generic/platform=visionOS"; PRODUCTS="$CONFIG-xros"
  if [[ -n "${DEVELOPMENT_TEAM:-}" ]]; then
    SIGN_ARGS=(DEVELOPMENT_TEAM="$DEVELOPMENT_TEAM" CODE_SIGN_STYLE=Automatic -allowProvisioningUpdates)
    if [[ -n "${GX_DEVICE_ID:-}" ]]; then
      DEST="id=$GX_DEVICE_ID"
      SIGN_ARGS+=(-allowProvisioningDeviceRegistration)
    fi
  else
    SIGN_ARGS=(CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="")
  fi
fi

# ---- UI fonts ---------------------------------------------------------------------------------
# The engine renders UI text with FreeType and asks for Windows faces (arial.ttf, ...). The app bundles the
# metric-compatible Liberation fonts (SIL OFL, freely redistributable) as <bundle>/fonts, staged by the same
# pinned, checksummed script the iOS port uses. Without them every menu label and briefing text is missing.
export GX_FONTS="$ROOT/build/visionos-resources/fonts"
"$ROOT/scripts/build/ios/stage-fonts.sh" >&2 || { echo "error: could not stage the UI fonts into $GX_FONTS" >&2; exit 1; }

echo "==> Generating Xcode project from $SPEC" >&2
xcodegen generate --spec "$SPEC" --project "$ROOT/visionos" --quiet >&2

LOG_DIR="$DERIVED/logs"; mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/build-$MODE.log"
ACTION=(build)
[[ "$CLEAN" == 1 ]] && ACTION=(clean build)

echo "==> xcodebuild ($MODE, $CONFIG); log: $LOG" >&2
set +e
xcodebuild -project "$ROOT/visionos/GeneralsZHXR.xcodeproj" \
  -scheme GeneralsZHXR \
  -configuration "$CONFIG" \
  -sdk "$SDK" \
  -destination "$DEST" \
  -derivedDataPath "$DERIVED" \
  GX_ANGLE_ROOT="$ANGLE_ROOT" GX_ANGLE_LINK="$ANGLE_LINK" \
  GX_ENGINE_XCFRAMEWORK="$GX_ENGINE_XCFRAMEWORK" \
  ${ENGINE_LDFLAGS:+GX_ENGINE_LDFLAGS="$ENGINE_LDFLAGS"} \
  ${SIGN_ARGS[@]+"${SIGN_ARGS[@]}"} \
  "${ACTION[@]}" >"$LOG" 2>&1
STATUS=$?
set -e

WARN=$(grep -c "warning:" "$LOG" || true)
ERR=$(grep -c "error:" "$LOG" || true)
echo "==> xcodebuild exit=$STATUS errors=$ERR warnings=$WARN" >&2
if [[ $STATUS -ne 0 ]]; then
  grep -E "error:|BUILD FAILED" "$LOG" | head -40 >&2 || true
  exit $STATUS
fi

APP="$DERIVED/Build/Products/$PRODUCTS/GeneralsZHXR.app"
[[ -d "$APP" ]] || { echo "build succeeded but $APP is missing" >&2; exit 1; }
echo "$APP"
