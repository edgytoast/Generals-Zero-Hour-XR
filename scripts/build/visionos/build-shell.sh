#!/bin/bash
# Build the native visionOS shell (GeneralsZHXR) with XcodeGen + xcodebuild.
#
#   build-shell.sh simulator|device [--derived-data DIR] [--configuration Debug|Release] [--clean]
#
#   simulator  xrsimulator SDK, runs in the visionOS simulator (no signing needed)
#   device     xros SDK, UNSIGNED compile-only check (CODE_SIGNING_ALLOWED=NO); the resulting
#              .app cannot be installed on a headset. For a signed build open the generated
#              project in Xcode and set your team, or pass DEVELOPMENT_TEAM=... in the environment.
#
# Prints the built .app path on the last line of stdout (everything else goes to stderr).
set -euo pipefail

MODE="${1:-}"
[[ "$MODE" == "simulator" || "$MODE" == "device" ]] || { echo "usage: $0 simulator|device [--derived-data DIR] [--configuration Debug|Release] [--clean]" >&2; exit 2; }
shift

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SPEC="$ROOT/visionos/project.yml"
DERIVED="${GXX_VISIONOS_DERIVED_DATA:-/private/tmp/GeneralsZHXR-DerivedData}"
CONFIG="Debug"
CLEAN=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --derived-data) DERIVED="$2"; shift 2 ;;
    --configuration) CONFIG="$2"; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

command -v xcodegen >/dev/null || { echo "xcodegen not found (brew install xcodegen)" >&2; exit 1; }

if [[ "$MODE" == "simulator" ]]; then
  SDK="xrsimulator"; DEST="generic/platform=visionOS Simulator"; PRODUCTS="$CONFIG-xrsimulator"
  SIGN_ARGS=()
else
  SDK="xros"; DEST="generic/platform=visionOS"; PRODUCTS="$CONFIG-xros"
  SIGN_ARGS=(CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="")
fi

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
