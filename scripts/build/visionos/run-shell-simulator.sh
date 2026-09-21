#!/bin/bash
# Install and launch the GeneralsZHXR shell in the visionOS simulator with -autoImmersive,
# wait, and capture a screenshot.
#
#   run-shell-simulator.sh [--derived-data DIR] [--build] [--wait SECONDS] [--shots N]
#                          [--out-dir DIR] [--udid UDID] [--no-gui]
#                          [--soak SECONDS [--sample-interval SECONDS]] [-- <extra app launch args>]
#
# Prints the screenshot path(s) on stdout (one per line); everything else goes to stderr.
# Console output from the app (frame-loop log lines) is written to <out-dir>/app-console.log.
#
# --soak SECONDS keeps the app running and samples its resident memory (ps rss, vmmap physical
# footprint) and the frame counter every --sample-interval seconds (default 30) into
# <out-dir>/soak.csv, then prints first/last/growth. Use --udid (or GXX_VISIONOS_SIM_UDID) with
# YOUR OWN simulator device; the default below is only a convenience for a single-developer machine.
#
# Extra app launch arguments go after "--", e.g. -angleTestScene, -layout dedicated,
# -cycleImmersive 5 -cycleHold 8, -angleAtlas, -angleSync glfinish.
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
SOAK=0
SAMPLE=30
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
    --soak) SOAK="$2"; shift 2 ;;
    --sample-interval) SAMPLE="$2"; shift 2 ;;
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

if [[ "$SOAK" -gt 0 ]]; then
  CSV="$OUT/soak.csv"
  echo "elapsed_s,pid,rss_kb,footprint,frames,fps" > "$CSV"
  T0=$(date +%s)
  echo "==> Soaking for ${SOAK}s (sampling every ${SAMPLE}s) -> $CSV" >&2
  while :; do
    ELAPSED=$(( $(date +%s) - T0 ))
    PID="$(pgrep -f "$UDID.*GeneralsZHXR.app/GeneralsZHXR" | head -1 || true)"
    if [[ -n "$PID" ]]; then
      RSS="$(ps -o rss= -p "$PID" | tr -d ' ')"
      FOOT="$(vmmap -summary "$PID" 2>/dev/null | grep -i "^Physical footprint:" | head -1 | awk '{print $3}' || true)"
      LINE="$(grep "frame loop:" "$CONSOLE" | tail -1 || true)"
      FRAMES="$(echo "$LINE" | sed -nE 's/.*frames=([0-9]+).*/\1/p')"
      FPS="$(echo "$LINE" | sed -nE 's/.*fps=([0-9.]+).*/\1/p')"
      echo "$ELAPSED,$PID,$RSS,$FOOT,$FRAMES,$FPS" >> "$CSV"
    else
      echo "$ELAPSED,,,,," >> "$CSV"
    fi
    [[ $ELAPSED -ge $SOAK ]] && break
    sleep "$SAMPLE"
  done
  FIRST="$(sed -n '2p' "$CSV")"; LAST="$(tail -1 "$CSV")"
  echo "==> soak first sample: $FIRST" >&2
  echo "==> soak last  sample: $LAST" >&2
  awk -F, 'NR>1 && $3!="" { if (!f) { f=$3; ff=$5 } l=$3; lf=$5 } END { if (f) printf "==> rss growth: %d kB -> %d kB (%+d kB); frames %d -> %d\n", f, l, l-f, ff, lf }' "$CSV" >&2
fi
echo "==> App console: $CONSOLE (launch pid $LAUNCH_PID keeps the app attached; kill it or run 'xcrun simctl terminate $UDID $BUNDLE_ID')" >&2
