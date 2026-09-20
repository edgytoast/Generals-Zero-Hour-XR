#!/bin/bash
# Install and launch the GeneralsZHXR shell in the visionOS simulator with -autoImmersive,
# wait, and capture a screenshot.
#
#   run-shell-simulator.sh [--derived-data DIR] [--build] [--wait SECONDS] [--shots N]
#                          [--out-dir DIR] [--udid UDID] [--no-gui] [-- <extra app launch args>]
#
# Prints the screenshot path(s) on stdout (one per line); everything else goes to stderr.
# Console output from the app (frame-loop log lines) is written to <out-dir>/app-console.log.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
UDID="${GXX_VISIONOS_SIM_UDID:-7D09C753-EEE9-45BD-9DED-4BC3215E2AA3}"
DERIVED="${GXX_VISIONOS_DERIVED_DATA:-/private/tmp/GeneralsZHXR-DerivedData}"
OUT="${GXX_VISIONOS_SHOT_DIR:-/private/tmp/GeneralsZHXR-shots}"
WAIT=15
SHOTS=1
BUILD=0
GUI=1
BUNDLE_ID="com.generalsx.zerohour.xr.vision"
EXTRA=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --derived-data) DERIVED="$2"; shift 2 ;;
    --out-dir) OUT="$2"; shift 2 ;;
    --udid) UDID="$2"; shift 2 ;;
    --wait) WAIT="$2"; shift 2 ;;
    --shots) SHOTS="$2"; shift 2 ;;
    --build) BUILD=1; shift ;;
    --no-gui) GUI=0; shift ;;
    --) shift; EXTRA=("$@"); break ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

APP="$DERIVED/Build/Products/Debug-xrsimulator/GeneralsZHXR.app"
if [[ $BUILD -eq 1 || ! -d "$APP" ]]; then
  APP="$("$ROOT/scripts/build/visionos/build-shell.sh" simulator --derived-data "$DERIVED" | tail -1)"
fi
mkdir -p "$OUT"

STATE="$(xcrun simctl list devices | grep "$UDID" | sed -E 's/.*\(([A-Za-z]+)\)[[:space:]]*$/\1/' | head -1)"
if [[ "$STATE" != "Booted" ]]; then
  echo "==> Booting $UDID" >&2
  xcrun simctl boot "$UDID" >&2 || true
fi
if [[ $GUI -eq 1 ]]; then
  open -a Simulator --args -CurrentDeviceUDID "$UDID" >&2 || true
fi
xcrun simctl bootstatus "$UDID" -b >&2

echo "==> Installing $APP" >&2
xcrun simctl terminate "$UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
xcrun simctl install "$UDID" "$APP" >&2

CONSOLE="$OUT/app-console.log"
echo "==> Launching with -autoImmersive ${EXTRA[*]:-}" >&2
: > "$CONSOLE"
xcrun simctl launch --console-pty --terminate-running-process "$UDID" "$BUNDLE_ID" -autoImmersive ${EXTRA[@]+"${EXTRA[@]}"} >"$CONSOLE" 2>&1 &
LAUNCH_PID=$!

echo "==> Waiting ${WAIT}s for the immersive space" >&2
sleep "$WAIT"
STAMP="$(date +%Y%m%d-%H%M%S)"
for ((i = 1; i <= SHOTS; i++)); do
  SHOT="$OUT/shot-$STAMP-$i.png"
  xcrun simctl io "$UDID" screenshot "$SHOT" >&2
  echo "$SHOT"
  [[ $i -lt $SHOTS ]] && sleep 2
done
echo "==> App console: $CONSOLE (launch pid $LAUNCH_PID keeps the app attached; kill it or run 'xcrun simctl terminate $UDID $BUNDLE_ID')" >&2
