#!/usr/bin/env bash
# Runs every visionOS-port test that can run on this machine and prints one summary table.
#
#   scripts/qa/vision-run-all.sh                 host tier: pure C++ tests on macOS (no simulator, no build products needed)
#   scripts/qa/vision-run-all.sh --udid UDID     host tier + simulator tier (needs YOUR OWN simulator device, see below and the built app)
#   scripts/qa/vision-run-all.sh --list          print the tiers without running anything
#
# Simulator tier prerequisites: a device of your own (do not share one between test runs; a Metal crash in one client
# kills every client in that simulator):
#   xcrun simctl create GXR-tests com.apple.CoreSimulator.SimDeviceType.Apple-Vision-Pro-4K com.apple.CoreSimulator.SimRuntime.xrOS-26-5
# and the app built with scripts/build/visionos/build-shell.sh simulator (the app-driven tests use its derived-data folder).
#
# No test here needs game data or a headset. Exit code 0 = every test that ran passed.
set -u
cd "$(dirname "$0")/../.."
ROOT="$PWD"
UDID=""; LIST=0; OUT="${TMPDIR:-/tmp}/gx-vision-run-all"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --udid) UDID="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --list) LIST=1; shift ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
mkdir -p "$OUT"

# name|command (relative to the repo root)
HOST_TESTS=(
  "interaction (gaze/pinch state machine, ASan+UBSan)|scripts/qa/vision-interaction-test.sh"
  "interaction: existing Quest host tests|scripts/qa/vision-interaction-test.sh --existing"
  "engine bridge forwarder|scripts/qa/vision-bridge-forward-test.sh"
  "game-data validator + importer|scripts/qa/vision-gamedata-test.sh"
  "d3d8gles texture formats (DXT/etc)|scripts/qa/vision-gles-formats-test.sh"
  "audio listener math|scripts/qa/vision-audio-listener-test.sh"
  "Android preprocessor equivalence (XrGameBoot)|scripts/qa/vision-android-preprocess-check.sh"
)
SIM_TESTS=(
  "OpenAL Soft on xrsimulator|scripts/qa/vision-audio-openal-test.sh --udid @UDID@"
  "FFmpeg decode chain on xrsimulator|scripts/qa/vision-audio-ffmpeg-test.sh --udid @UDID@"
  "AVAudioSession|scripts/qa/vision-audio-session-test.sh --udid @UDID@"
  "d3d8gles on ANGLE-Metal (device test)|scripts/qa/vision-gles-device-test.sh --udid @UDID@"
  "engine host: fake engine pipeline|scripts/qa/vision-engine-host-fake-test.sh --udid @UDID@ --out $OUT/fake --duration 45 --stall 3"
  "engine host: real engine on fixtures|scripts/qa/vision-engine-host-fixture-test.sh --udid @UDID@ --out $OUT/fixture --wait 120"
)
# Tests added later (presentation, UI model, ...) are picked up automatically when they follow the naming scheme and take no
# arguments: any scripts/qa/vision-*-test.sh that is not listed above is reported as UNLISTED so that it is not forgotten.
listed() { local s="$1"; for t in "${HOST_TESTS[@]}" "${SIM_TESTS[@]}"; do [[ "$t" == *"$s"* ]] && return 0; done; return 1; }
UNLISTED=()
for f in scripts/qa/vision-*-test.sh; do listed "$(basename "$f")" || UNLISTED+=("$f"); done

if [[ $LIST -eq 1 ]]; then
  echo "host tier:"; printf '  %s\n' "${HOST_TESTS[@]}"
  echo "simulator tier (needs --udid):"; printf '  %s\n' "${SIM_TESTS[@]}"
  [[ ${#UNLISTED[@]} -gt 0 ]] && { echo "unlisted vision-*-test.sh (add them to this script):"; printf '  %s\n' "${UNLISTED[@]}"; }
  exit 0
fi

RESULTS=(); FAIL=0
run_one() {
  local name="${1%%|*}" cmd="${1#*|}" start end rc log
  cmd="${cmd//@UDID@/$UDID}"
  log="$OUT/$(echo "$name" | tr -c 'A-Za-z0-9' '_').log"
  printf '>> %s\n   %s\n' "$name" "$cmd"
  start=$(date +%s)
  ( cd "$ROOT" && bash -c "$cmd" ) >"$log" 2>&1
  rc=$?
  end=$(date +%s)
  local last; last="$(grep -E 'PASS|passed|checks|FAIL|error' "$log" | tail -1 | cut -c1-110)"
  if [[ $rc -eq 0 ]]; then RESULTS+=("PASS|$name|$((end-start))s|$last"); else RESULTS+=("FAIL|$name|$((end-start))s|rc=$rc $last (log: $log)"); FAIL=1; fi
}

for t in "${HOST_TESTS[@]}"; do run_one "$t"; done
if [[ -n "$UDID" ]]; then
  for t in "${SIM_TESTS[@]}"; do run_one "$t"; done
else
  RESULTS+=("SKIP|simulator tier|-|pass --udid <your own simulator device> to run it")
fi
for u in ${UNLISTED[@]+"${UNLISTED[@]}"}; do RESULTS+=("SKIP|$u|-|not listed in vision-run-all.sh"); done

echo; echo "==== visionOS port test summary ===="
printf '%-5s %-52s %-7s %s\n' RESULT TEST TIME LAST-LINE
for r in "${RESULTS[@]}"; do IFS='|' read -r a b c d <<<"$r"; printf '%-5s %-52s %-7s %s\n' "$a" "$b" "$c" "$d"; done
echo "logs: $OUT"
exit $FAIL
