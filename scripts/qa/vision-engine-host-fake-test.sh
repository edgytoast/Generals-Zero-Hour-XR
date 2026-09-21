#!/usr/bin/env bash
# Simulator test of the decoupled compositor with the FAKE engine (no game data): the GLES3 test scene runs as a client on the
# engine thread and the compositor must keep running at display rate, also while the fake engine stalls.
#
#   scripts/qa/vision-engine-host-fake-test.sh --udid UDID [--derived-data DIR] [--out DIR] [--duration SEC]
#                                              [--boot SEC] [--stall SEC] [--cycles N] [--soak]
#
#   --udid          YOUR OWN simulator device (xcrun simctl create GXR-x com.apple.CoreSimulator.SimDeviceType.Apple-Vision-Pro-4K
#                   com.apple.CoreSimulator.SimRuntime.xrOS-26-5)
#   --derived-data  where build-shell.sh put the app (default /private/tmp/GeneralsZHXR-DerivedData); built when missing
#   --duration      seconds to observe after launch (default 60)
#   --boot / --stall  -fakeEngineBoot / -fakeEngineStall (defaults 6 / 4)
#   --cycles N      also close and re-open the immersive space N times (-cycleImmersive N -cycleHold 8)
#   --soak          sample RSS and physical footprint every 20 s and print the growth (use with --duration 330 or more)
#
# Checks (parsed from the app console, the "[GXXR] pipeline:" line that is printed once per second):
#   * the compositor ran at display rate (median >= 50 fps) whenever engine frames or the loading indicator were shown
#   * the engine produced frames (median engine fps > 20 while running)
#   * with --stall: some windows had 0 new frames/s with repeats/s ~ compositor fps (last frame kept presenting) and the frame age grew
#   * no GL errors, no "no free ring slot" skips, the process is still alive at the end
# Exit code 0 = all checks passed. Screenshots (boot / running / stalled / end) are stored in --out.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
UDID=""; DERIVED="${GXX_VISIONOS_DERIVED_DATA:-/private/tmp/GeneralsZHXR-DerivedData}"; OUT="${TMPDIR:-/tmp}/gx-fake-test"
DURATION=60; BOOT=6; STALL=4; CYCLES=0; SOAK=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --udid) UDID="$2"; shift 2 ;; --derived-data) DERIVED="$2"; shift 2 ;; --out) OUT="$2"; shift 2 ;;
    --duration) DURATION="$2"; shift 2 ;; --boot) BOOT="$2"; shift 2 ;; --stall) STALL="$2"; shift 2 ;;
    --cycles) CYCLES="$2"; shift 2 ;; --soak) SOAK=1; shift ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
[[ -n "$UDID" ]] || { echo "--udid is required (use your own simulator device)" >&2; exit 2; }
BUNDLE=com.generalsx.zerohour.xr.vision
APP="$DERIVED/Build/Products/Debug-xrsimulator/GeneralsZHXR.app"
[[ -d "$APP" ]] || APP="$("$ROOT/scripts/build/visionos/build-shell.sh" simulator --derived-data "$DERIVED" | tail -1)"
mkdir -p "$OUT"; rm -f "$OUT/console.log" "$OUT/soak.csv"
xcrun simctl bootstatus "$UDID" -b >/dev/null 2>&1 || xcrun simctl boot "$UDID" >/dev/null 2>&1 || true
xcrun simctl install "$UDID" "$APP"
ARGS=(-autoImmersive -allowNoData -fakeEngine -fakeEngineBoot "$BOOT" -fakeEngineStall "$STALL")
(( CYCLES > 0 )) && ARGS+=(-cycleImmersive "$CYCLES" -cycleHold 8)
xcrun simctl launch --console-pty --terminate-running-process "$UDID" "$BUNDLE" "${ARGS[@]}" > "$OUT/console.log" 2>&1 &
sleep $((BOOT / 2 + 2)); xcrun simctl io "$UDID" screenshot "$OUT/shot-boot.png" >/dev/null 2>&1 || true
sleep $((BOOT / 2 + 4)); xcrun simctl io "$UDID" screenshot "$OUT/shot-running.png" >/dev/null 2>&1 || true
T0=$(date +%s); STALLSHOT=0
(( SOAK )) && echo "elapsed_s,rss_kb,footprint" > "$OUT/soak.csv"
while (( $(date +%s) - T0 < DURATION )); do
  sleep 5
  if (( ! STALLSHOT )) && tail -20 "$OUT/console.log" | grep -q "new frames/s=0.0 repeats/s=6"; then
    xcrun simctl io "$UDID" screenshot "$OUT/shot-stalled.png" >/dev/null 2>&1 || true; STALLSHOT=1
  fi
  if (( SOAK )); then
    PID=$(pgrep -f "$UDID.*GeneralsZHXR.app/GeneralsZHXR" | head -1 || true)
    [[ -n "$PID" ]] && echo "$(( $(date +%s) - T0 )),$(ps -o rss= -p "$PID" | tr -d ' '),$(vmmap -summary "$PID" 2>/dev/null | grep -i '^Physical footprint:' | head -1 | awk '{print $3}')" >> "$OUT/soak.csv"
  fi
done
xcrun simctl io "$UDID" screenshot "$OUT/shot-end.png" >/dev/null 2>&1 || true
ALIVE=0; pgrep -f "$UDID.*GeneralsZHXR.app/GeneralsZHXR" >/dev/null && ALIVE=1
python3 - "$OUT/console.log" "$STALL" "$ALIVE" "$CYCLES" <<'PYEOF'
import re, statistics, sys
log, stall, alive, cycles = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
rows = []
for line in open(log, errors="replace"):
    m = re.search(r"pipeline: compositor=([\d.]+) fps \| engine\((\w+)\)=([\d.]+) fps .*skipped (\d+).* \| new frames/s=([\d.]+) repeats/s=([\d.]+) \| frame age max=(\d+) ms", line)
    if m:
        rows.append(dict(comp=float(m[1]), phase=m[2], eng=float(m[3]), skipped=int(m[4]), new=float(m[5]), rep=float(m[6]), age=int(m[7])))
text = open(log, errors="replace").read()
run = [r for r in rows if r["phase"] == "running"]
ok = True
def check(name, cond, detail):
    global ok
    ok = ok and cond
    print(("PASS " if cond else "FAIL ") + name + ": " + detail)
check("pipeline lines seen", len(rows) > 10, "%d windows (%d with the engine running)" % (len(rows), len(run)))
if rows:
    check("compositor at display rate", statistics.median(r["comp"] for r in rows) >= 50, "median %.1f fps (min %.1f, max %.1f)" % (statistics.median(r["comp"] for r in rows), min(r["comp"] for r in rows), max(r["comp"] for r in rows)))
if run:
    check("engine produces frames", statistics.median(r["eng"] for r in run) > 20, "median engine %.1f fps" % statistics.median(r["eng"] for r in run))
    if stall > 0:
        held = [r for r in run if r["new"] == 0 and r["rep"] >= 50]
        check("last frame kept presenting during stalls", len(held) >= 1 and all(r["comp"] >= 50 for r in held), "%d stalled windows, compositor >= %.1f fps in all, max frame age %d ms" % (len(held), min([r["comp"] for r in held] or [0]), max(r["age"] for r in run)))
    check("no skipped engine frames", run[-1]["skipped"] == 0, "skipped %d" % run[-1]["skipped"])
check("no GL errors", "glGetError" not in text, "grep glGetError")
check("no crash", "BUG IN CLIENT" not in text and alive == 1, "process alive: %s" % bool(alive))
if cycles:
    gens = len(re.findall(r"compositor loop generation \d+ started", text))
    check("immersive re-opens", gens >= cycles + 1, "%d compositor loop generations" % gens)
    check("ring survives re-opens", len(re.findall(r"\[GXXR/ring\] target stereoLeft", text)) == 1 and "ring torn down" not in text, "one allocation, no teardown")
sys.exit(0 if ok else 1)
PYEOF
RC=$?
if (( SOAK )); then
  awk -F, 'NR>1 && $3!="" { if (!f) f=$3+0; l=$3+0 } END { printf "soak: physical footprint %.1f MB -> %.1f MB (%+.1f MB)\n", f, l, l-f }' "$OUT/soak.csv"
fi
xcrun simctl terminate "$UDID" "$BUNDLE" >/dev/null 2>&1 || true
echo "artifacts in $OUT"
exit $RC
