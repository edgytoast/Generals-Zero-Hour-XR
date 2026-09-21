#!/usr/bin/env bash
# Boots the REAL engine (linked into the app) against the synthetic game-data fixtures of package G in the simulator and reports
# where init stopped. The fixtures are tiny fabricated .big trees, not retail data: the engine cannot get through init with them,
# so this proves the engine starts on the engine thread on visionOS and shows how it fails, not that it plays.
#
#   scripts/qa/vision-engine-host-fixture-test.sh --udid UDID [--derived-data DIR] [--out DIR] [--wait SEC]
#
# Two outcomes are checked, depending on whether the ReleaseCrash hook (scripts/qa/vision-engine-host-releasecrash-hook.patch,
# a change to Core/GameEngine/Source/Common/System/Debug.cpp that this package does not own) is applied to the engine build:
#   hook applied     the process stays alive, engine phase FAILED, the reason is in the compositor indicator / launcher / log;
#   hook not applied ReleaseCrash ends the process (_exit); the NEXT launch shows the reason and the last log lines from the boot marker.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
UDID=""; DERIVED="${GXX_VISIONOS_DERIVED_DATA:-/private/tmp/GeneralsZHXR-DerivedData}"; OUT="${TMPDIR:-/tmp}/gx-fixture-test"; WAIT=35
while [[ $# -gt 0 ]]; do
  case "$1" in --udid) UDID="$2"; shift 2 ;; --derived-data) DERIVED="$2"; shift 2 ;; --out) OUT="$2"; shift 2 ;; --wait) WAIT="$2"; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;; esac
done
[[ -n "$UDID" ]] || { echo "--udid is required" >&2; exit 2; }
BUNDLE=com.generalsx.zerohour.xr.vision
APP="$DERIVED/Build/Products/Debug-xrsimulator/GeneralsZHXR.app"
[[ -d "$APP" ]] || APP="$("$ROOT/scripts/build/visionos/build-shell.sh" simulator --derived-data "$DERIVED" | tail -1)"
mkdir -p "$OUT"
OUT_DIR="$OUT" "$ROOT/scripts/qa/vision-gamedata-test.sh" >/dev/null
"$OUT/vision-gamedata-test" --make-fixtures "$OUT/fixtures" >/dev/null
xcrun simctl bootstatus "$UDID" -b >/dev/null 2>&1 || xcrun simctl boot "$UDID" >/dev/null 2>&1 || true
xcrun simctl uninstall "$UDID" "$BUNDLE" >/dev/null 2>&1 || true
xcrun simctl install "$UDID" "$APP"
xcrun simctl launch --console-pty --terminate-running-process "$UDID" "$BUNDLE" -autoImmersive -importFrom "$OUT/fixtures/merged" -importInPlace -autoStartEngine > "$OUT/console.log" 2>&1 &
sleep "$WAIT"; xcrun simctl io "$UDID" screenshot "$OUT/shot.png" >/dev/null 2>&1 || true
ALIVE=0; pgrep -f "$UDID.*GeneralsZHXR.app/GeneralsZHXR" >/dev/null && ALIVE=1
echo "process alive after ${WAIT}s: $ALIVE"
echo "engine thread started: $(grep -c 'engine thread started' "$OUT/console.log")   boot reached init: $(grep -c 'engine init starting' "$OUT/console.log")"
echo "last engine log lines (compositor lines removed):"; grep -v '^\[GXXR' "$OUT/console.log" | grep -v gxbig | tail -6 | cut -c1-220 | sed 's/^/    /'
grep -q '\[engine-host\] FAILED' "$OUT/console.log" && grep '\[engine-host\] FAILED' "$OUT/console.log" | sed 's/^/graceful: /'
if (( ! ALIVE )); then
  echo "process ended (ReleaseCrash _exit); relaunching to read the boot marker notice"
  xcrun simctl launch --console-pty --terminate-running-process "$UDID" "$BUNDLE" -importFrom "$OUT/fixtures/merged" -importInPlace > "$OUT/console2.log" 2>&1 &
  sleep 12; xcrun simctl io "$UDID" screenshot "$OUT/shot-relaunch.png" >/dev/null 2>&1 || true
  echo "screenshot with the notice: $OUT/shot-relaunch.png"
fi
xcrun simctl terminate "$UDID" "$BUNDLE" >/dev/null 2>&1 || true
echo "artifacts in $OUT"
