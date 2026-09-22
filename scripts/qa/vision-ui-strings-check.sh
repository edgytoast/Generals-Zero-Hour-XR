#!/usr/bin/env bash
# Checks the localisation table of the visionOS spatial UI (visionos/UI/UIStrings.swift, English key -> German).
#
#   scripts/qa/vision-ui-strings-check.sh
#
# Fails when
#   * a t("...") call site, a titleKey or a camera-preset tuple in visionos/UI or the window files has no German entry,
#   * a German entry is empty, or equal to its English key without being on the short list of words that are the same in both,
#   * a store.xr("...") key is missing from the Quest string table (GeneralsMD/Code/Main/XrStrings.h) or the extra table of
#     VisionPanelModel.cpp (those keys are German sources that the C++ model translates).
# Dead keys (in the table, used nowhere) are reported as notes.
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_dir"
python3 - <<'PY'
import re, sys, glob
table_src = open('visionos/UI/UIStrings.swift', encoding='utf-8').read()
body = table_src[table_src.index('static let german'):]
entries = dict(re.findall(r'^\s*"((?:[^"\\]|\\.)*)"\s*:\s*"((?:[^"\\]|\\.)*)",\s*$', body, re.M))
files = glob.glob('visionos/UI/*.swift') + glob.glob('visionos/App/*Window.swift') + ['visionos/App/HudOrnament.swift']
used, xr_used, errors = set(), set(), []
for f in files:
    if f.endswith('UIStrings.swift'): continue
    src = open(f, encoding='utf-8').read()
    for m in re.finditer(r'\bt\("((?:[^"\\]|\\.)*)"\)', src): used.add(m.group(1))
    for m in re.finditer(r'\bxr\("((?:[^"\\]|\\.)*)"\)', src): xr_used.add(m.group(1))
    for m in re.finditer(r'return "((?:[^"\\]|\\.)*)"', src):
        if 'titleKey' in src[max(0, m.start()-800):m.start()]: used.add(m.group(1))
    for m in re.finditer(r'\("((?:[^"\\]|\\.)*)", \d\)', src): used.add(m.group(1))
    for m in re.finditer(r'\bxr\(\s*([a-z]+)\s*\)', src): pass
same_ok = {'Audio', 'Decals', 'Ultra', 'Pause', 'Engine', 'Renderer', 'Menu', 'Phase', 'Synchronisation', 'Simulator', '720p', '1080p'}
for k in sorted(used):
    if k not in entries: errors.append(f'no German entry for t("{k}")')
for k, v in entries.items():
    if not v.strip(): errors.append(f'empty German text for "{k}"')
    elif v == k and k not in same_ok: errors.append(f'German text equals the key (untranslated?): "{k}"')
quest = open('GeneralsMD/Code/Main/XrStrings.h', encoding='utf-8').read() + open('GeneralsMD/Code/Main/visionos/VisionPanelModel.cpp', encoding='utf-8').read()
for k in sorted(xr_used):
    if '{"' + k + '"' not in quest and '{"'+k+'",' not in quest: errors.append(f'xr("{k}") is not a key of the Quest / extra string table')
dead = sorted(set(entries) - used)
for k in dead: print(f'  note: unused key "{k}"')
print(f'keys: {len(entries)}, used by call sites: {len(used)}, xr keys: {len(xr_used)}, unused: {len(dead)}')
for e in errors: print('ERROR', e)
sys.exit(1 if errors else 0)
PY
