# visionOS port: test matrix

Last updated: 2026-09-22 (branch `visionos-port`, commit `2ff1555`). Toolchain: Xcode 27.0, visionOS SDK 27.0, app
deployment target visionOS 26.0, engine library deployment target visionOS 2.0, simulator runtime visionOS 26.5.
**No physical Apple Vision Pro has been used at any point in this port.** Since 2026-09-22 the developer's own,
legally owned retail Zero Hour and base Generals install is on the development machine (outside the repository,
used in place; nothing from it is committed). Rows marked "real data" below were run against it; older rows used the
fake engine or tiny fabricated `.big` fixtures.

This file is the single source of truth for what works on the visionOS port and how we know. It is deliberately
pessimistic. A feature is not marked WORKING unless someone ran it and the evidence is written down. Design
background is in [VISIONOS_PORT_ARCHITECTURE.md](VISIONOS_PORT_ARCHITECTURE.md); the user-facing status is in
[../README_VISIONOS.md](../README_VISIONOS.md); the engine-host protocol is in
[visionos-engine-host.md](visionos-engine-host.md).

## Summary of today's evidence

**Real-data run, 2026-09-22 (commits `bb18e54`, `e175d88`, `2ff1555`):**

- **The real game runs in the visionOS simulator with real data.** The engine boots, the main menu renders, and the
  campaign map `MD_USA01` loads (about 180–230 s after launch in the simulator). The battlefield renders as a stereo
  3D miniature on a virtual table (real terrain, units and buildings), with the engine HUD (radar, money, command
  buttons) as a panel beside it. Four real bugs were found and fixed on the way (fonts, one-eye target ring, stuck
  Loading mode, HUD alpha/crop).
- **Gameplay input works end to end in the simulator, with an injected gaze ray.** Look-and-pinch selects a unit,
  a pinch on the ground moves it, a pinch-drag box-selects, a drag on the board rim pans, and a pinch on the HUD
  minimap orders the selection there. The simulator's own automation pinches carry no gaze ray at all (zero ray, zero
  pose), so these runs use the `-testInput` hook, which feeds pinch events with a ray into the real input path.
  Real gaze on a device is still unverified.
- **Not yet observed with real data:** attacking an enemy, building placement (no builder at the start of
  `MD_USA01`), Commands-window buttons in a match, two-hand gestures, skirmish setup, save/load, two-eye separation.
- Compositor 60 fps steady; engine 12–15 fps in the simulator during the mission (a Mac simulator, not a device
  number). 11 of 11 host suites pass after these changes.

**Earlier evidence (before real data):**

- **The engine now boots on visionOS and runs on its own thread.** With fabricated (non-retail) game-data fixtures,
  the real Zero Hour engine (linked as a static library) starts, mounts synthetic `.big` archives, and reaches
  `Data\INI\Default\GameData` before stopping for lack of real content — the expected, correct failure mode for
  fake data. This is real engine code (`GameEngine::init`, `TheArchiveFileSystem`, `INI` parsing) executing on
  visionOS for the first time. It has never been run against real retail data.
- **A decoupled engine/compositor architecture is built and soak-tested.** The engine thread (its own ANGLE/GLES3
  context) and the Compositor Services thread run independently, connected by a reference-counted texture ring and
  GPU-GPU fences (`EGL_ANGLE_metal_shared_event_sync`). A 340-second simulator soak with a fake engine held steady
  60 fps compositor / ~45 fps engine, 0 skipped frames, 0 GL errors, flat memory.
- **d3d8gles (the Quest's native GLES3 D3D8 backend) runs on ANGLE-Metal** in the simulator: 90/90 device-test
  checks (fixed-function draws, atlas and separate-eye stereo modes, host-supplied render targets via EGLImage,
  Metal-blit readback) and 48/48 texture-format checks (DXT software decode, all D3D8 formats the engine uses).
- **Presentation, panels, gaze/pinch interaction, spatial audio, game-data import and a native SwiftUI UI are all
  implemented and unit-tested**, with a scripted fake-engine sequence (loading → menu → tabletop → ground view)
  exercised and screenshotted in the simulator. Two real bugs were found and fixed by that verification pass (a
  depth-test bug hiding feedback markers, and a missing opaque-black clear for Ground View).
- **11 of 11 host-side test suites pass** (`scripts/qa/vision-run-all.sh`, no simulator needed): interaction state
  machine, engine bridge forwarding, game-data validator/importer, d3d8gles texture formats, audio listener math,
  Android-equivalence check, presentation state machine, UI panel model, UI localisation, plus the original 27
  Quest host-logic tests unchanged.
- **Both app configurations build with zero errors** on the fully merged tree: visionOS Simulator and an unsigned
  visionOS device (compile-only; cannot be installed without a device and a signing team).
- **Every row that needs a physical headset, two simultaneous eye views, hand tracking, plane detection, or scene
  reconstruction is still PARTIAL or not tested**, because none of those exist in the visionOS Simulator and no
  device is attached.

## Status legend

| Status | Meaning |
| --- | --- |
| WORKING | Someone ran it and observed the intended behaviour. The Evidence column says exactly what, when and where. |
| PARTIAL | Part of the feature runs, or code exists and compiles but has never been exercised end to end. The Evidence column says which part. |
| BROKEN | It was run and fails. The Evidence column says how. |
| NOT YET IMPLEMENTED | No implementation exists on visionOS, or it exists only as a design. A Quest reference is named where the engine logic already exists. |

"Simulator" and "Physical Vision Pro" columns record where the feature has been checked: `verified <date>`,
`not tested`, or `cannot be verified here` with the reason. The visionOS 26.5 Simulator renders one view (not two),
and has no hand tracking, plane detection, scene reconstruction, foveation, or `layered` compositor layout, so
several rows can never pass there. The Physical Vision Pro column is `not tested (no device)` for every row.

Evidence format: the command or action, the date (ISO), and where the output is (a repository path, or a command
that reproduces it). Every row gives a way to reproduce the result. Run `scripts/qa/vision-run-all.sh` (host tier)
or `scripts/qa/vision-run-all.sh --udid <your own simulator>` (host + simulator tier) to reproduce the bulk of this
file's host-side evidence in one command.

## 1. Foundations (not features of the game, but what everything stands on)

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Shell app builds for the simulator | WORKING | `scripts/build/visionos/build-shell.sh simulator`, 2026-09-22: `xcodebuild exit=0 errors=0 warnings=1` (the one warning is a benign AppIntents metadata notice). | verified 2026-09-22 | n/a (build) |
| Shell app builds for the device (unsigned) | WORKING (compile-only) | `scripts/build/visionos/build-shell.sh device`, 2026-09-22: `xcodebuild exit=0 errors=0 warnings=1`, arm64 Mach-O, `UIDeviceFamily 7`, `MinimumOSVersion 26.0`. No signing team configured; cannot be installed. Re-checked 2026-09-22 at `2ff1555` with both engine slices rebuilt: `xcodebuild exit=0 errors=0`. | n/a | not tested (no device, no team) |
| Engine builds as a static library, both slices | WORKING | `scripts/build/visionos/build-engine.sh --simulator` and `--device`, 2026-09-22, from the fully merged tree: 0 compile errors on either slice. `scripts/build/visionos/make-xcframework.sh --simulator --device` merges both into `GeneralsZHEngine.xcframework` (1.4 GB, `xros-arm64` + `xros-arm64-simulator` slices, `vtool` confirms platform/minos). `scripts/build/visionos/verify-engine.sh`: 0 unexpected undefined symbols (the engine's `GX_XR_*` hooks are all defined by `XrGameBoot.cpp`; only symbols the app itself provides remain, as expected). | verified 2026-09-22 | not run |
| Shell app links the real engine and both configurations build together | WORKING | `scripts/build/visionos/build-shell.sh simulator` / `device` on the fully merged tree (after resolving one textual merge conflict in `GXEngineHost.h` between the C2 and F packages — no semantic overlap, both additive), 2026-09-22: both `xcodebuild exit=0 errors=0 warnings=1`. | verified 2026-09-22 | not tested |
| ANGLE (OpenGL ES 3.0 on Metal) builds for xrsimulator and xros | WORKING | `scripts/build/visionos/build-angle.sh all`. `nm -gU` shows 115 `egl*` + 828 `gl*` exports in both dylibs; `vtool` shows platform 12 (simulator) / 11 (device), minos 2.0, arm64; static archive holds 1,726 `rx::mtl::` symbols and no GL/Vulkan backend. | verified | device slice built, never run |
| ANGLE smoke test (EGLImage import of Metal textures, draw, Metal-blit readback) | WORKING | `xcrun simctl spawn <udid> $DEPS_ROOT/angle-smoke/smoke-xrsim[-static]`: `SMOKE TEST PASSED (0 failures)`. | verified | not tested |
| d3d8gles (Quest D3D8-on-GLES3 backend) runs on ANGLE-Metal, in-app | WORKING | `scripts/qa/vision-gles-device-test.sh --udid <own device>`: **90/90 checks passed** — fixed-function lit textured draws, atlas and separate-eye stereo modes, host-supplied render targets imported via EGLImage, board clipping, Metal-blit readback (never `glReadPixels` on a wrapped/private texture — that crashes the simulator's Metal host). | verified | not tested |
| d3d8gles texture-format handling (DXT software decode + all D3D8 surface formats) | WORKING | `scripts/qa/vision-gles-formats-test.sh` (host, no simulator): **48/48 checks passed**. ANGLE-Metal has no S3TC/DXT/BPTC; the backend's software DXT1/3/5 decode path is exercised and verified pixel-correct. | verified (host) | not tested |
| Engine thread + decoupled compositor architecture | WORKING | `scripts/qa/vision-engine-host-fake-test.sh --udid <own device> --duration 340 --stall 3`: compositor median 60.0 fps (min 60.0) for the whole run; engine median ~44.9 fps; last published frame kept presenting through simulated multi-second engine stalls (frame age up to 4.0 s, compositor fps unaffected); 0 skipped engine frames; 0 GL errors; RSS/footprint flat after the first ~12 s; immersive-space close/re-open cycles (`-cycleImmersive N`) survive with the ANGLE context, ring, and engine state intact across every cycle. | verified | not tested |
| Real engine boots on visionOS (fabricated, non-retail fixtures) | WORKING (boot only, no real data) | `scripts/qa/vision-engine-host-fixture-test.sh --udid <own device>`, using `vision-gamedata-test --make-fixtures` (tiny fabricated `.big` trees, never retail assets): engine thread starts, `XrGameBoot_InitHost` runs, 21 synthetic archives mount, `TheArchiveFileSystem` initialises, init reaches and fails inside `Data\INI\Default\GameData` loading (`[INI] ERROR: No files read from directory ...`) — the correct, expected failure for fake data. With the `ReleaseCrash`-hook patch applied (committed, `Core/GameEngine/Source/Common/System/Debug.cpp`), the failure is reported to the host and the process survives instead of exiting; the launcher shows the reason and log tail. | verified (boot + graceful-failure path) | not tested |
| Quest host-logic tests on macOS (the engine's own decision code, reused as-is) | WORKING (27/27 of what can run without Android/NDK) | `scripts/qa/vision-interaction-test.sh --existing`: 27 passed, 0 failed. 2 of the original 40 XR test files (`xr-interaction`, `xr-menu-routing`) need the real OpenXR SDK and are skipped by design (a 25-line POD shim stands in for the rest); the remaining ~11 need Android/NDK/APK tooling not present here. | verified (macOS) | n/a |
| One-command test runner | WORKING | `scripts/qa/vision-run-all.sh` — 7 host tests, all PASS, plus 4 more added this session (below); `--udid <own device>` adds 6 more simulator-tier tests. | verified 2026-09-22 | n/a |

## 2. Engine, data and rendering

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Engine startup | WORKING (boot sequence only; stops at data load, correctly, with fake data) | See "Real engine boots on visionOS" above. `XrGameBoot_InitHost` (host-neutral twin of the Quest's `XrGameBoot_Init`, `GX_XR_HOST` macro) mirrors the Quest boot sequence: critsecs, memory manager, `Version`, command line, offscreen flag, `d3d8gles_SetXRConfig`, `FramePacer`, `CreateGameEngine()->init()`. | verified (against fixtures) | not tested |
| Game-data detection | WORKING | `scripts/qa/vision-gamedata-test.sh`: **726/726 checks passed**. C++ port of the Android `GameDataValidator` rules: the two 10-archive lists, case-insensitive names, marker files (`INIZH.big`, `Terrain.big`), bounded search (depth 2, ≤128 folders), nested Steam `ZH_Generals` layout, in-archive string-table and `Weather.ini` checks (BIGF header parsing), minimal-vs-complete detection, corrupted-archive detection. In-app: simulator screenshots of every state (not configured / complete / minimal / incomplete / damaged / installer-only / ambiguous-multiple-installs / import-in-progress / interrupted-import-resume / post-crash-recovery) were captured and read. | verified (host, 726 checks) + verified in-app (all fixture states screenshotted) | not tested |
| Game-data import (folder pick, validate, resumable/atomic copy) | WORKING (via launch-argument test hook; system file picker not drivable in the simulator) | Same test run: import via `-importFrom <path>`, cancel via `-importCancelAfter <s>` (leaves a resumable partial state), a real `kill -9` mid-copy followed by relaunch (partial file cleaned up, Resume offered and completes correctly, final manifest byte/file counts match). "Use in place" (security-scoped bookmark) is implemented but not exercised in the simulator. | verified (launch-argument path) | not tested |
| Loading a map | WORKING (real data, simulator) | 2026-09-22, real data: `SIMCTL_CHILD_GX_START_MAP='Maps\MD_USA01\MD_USA01.map' xcrun simctl launch <udid> com.generalsx.zerohour.xr.vision -autoImmersive -autoStartEngine` (the `GX_START_MAP` test hook in `XrGameBoot.cpp` queues `MSG_NEW_GAME` after init). The mission loads and becomes interactive about 180–230 s after launch; the engine log shows `presentation diag: interactive=1 ... stereoValid=1`. Earlier mechanism check: the loading-presenter plumbing (`XrGameBoot_SetLoadingPresenter` → `GXEngineHost_PresentNested`, so a blocking synchronous load still publishes frames instead of freezing the compositor) is built and verified with a real blocking burst from the fake engine (`loading burst: 15 nested frames presented over 1.5 s, 0 lost slots`). No real map has ever been loaded. | verified (mechanism only, via fake engine) | not tested |
| Rendering a battlefield | WORKING (real data, simulator, one view) | 2026-09-22, real data, `MD_USA01`: real terrain, units and buildings render as a miniature on the virtual table (simulator screenshots; eye textures dumped with `GX_DEBUG_DUMP_FRAMES=<sec>` into `<app tmp>/gx-frame-dump`). Fixed on the way (commit `bb18e54`): the target ring allocated only one eye in separate-eye mode, so the stereo target stayed empty. Earlier: the stereo world-frame build (`XrWorldFrame`, eye clip matrices, board mapping), panel layout, and feedback rendering are all implemented and unit-tested (303 checks, `vision-presentation-test`) and exercised visually via the fake engine's scripted tabletop phase. No real engine frame (real terrain/units) has ever been rendered. Quest reference only: `docs/media/quest-tabletop/` (Quest hardware, not this port). | verified (fake-engine substitute only) | not tested |
| Stereo rendering (two eyes) | PARTIAL | 2026-09-22, real data: the engine renders both eye targets (the ring now always allocates both; eye dump shows the board). The compositor renders one pass per view, correctly parameterised for however many views the drawable reports (`cp_view_get_transform`, `cp_drawable_compute_projection`, per-view viewport into the target ring). The visionOS 26.5 Simulator only ever reports **one** view, so true two-eye separation has never been observed, only verified by construction and code review. | cannot be verified here (1 view only) | not tested |
| Tabletop transform: move, rotate, scale | PARTIAL | Initial placement (head-relative and floor-origin branches), recenter, and the pure two-hand rotate/scale/pan math (`VisionInteraction` — part of the 181,909-assertion interaction test suite) are implemented and unit-tested. Map pan by pinch-drag on the board rim: observed 2026-09-22 with real data via `-testInput` (`drag` starting on the rim; terrain and minimap view box moved). Two-hand rotate/scale of the table has not been driven end to end. | placement/recenter/rim pan verified 2026-09-22 (injected ray); two-hand math host only | not tested |
| Graphics/performance settings (render scale, fps cap, shadow mode, eye-size tier, UI resolution) | WORKING (API + settings UI; effect on a real frame unverified) | `GXEngineHost_SetGraphics`/`GetGraphics`/`GetGraphicsDefaults` (extendable, versioned struct) implemented, clamped, and round-tripped (format/parse for persistence); bound live in the SwiftUI Settings → Graphics page. Verified: the struct round-trips and the visible top row of the Settings page matches the real default in a simulator screenshot. Not verified: changing a setting's effect on an actual rendered frame (no real engine content to look at). | verified (API + partial UI) | not tested |

## 3. Selection and commands

The engine's command logic is unmodified and shared with the Quest edition (`XrGameBoot_SpatialClick`,
`TouchInput`, the command translators). Since 2026-09-22 it has been driven against a real running match
(`MD_USA01`, real data) in the simulator. **Input source caveat:** the simulator's automation pinches arrive with a
zero `selectionRay`, zero hand pose and zero location (also after adding visionOS 26 tracking areas), so they cannot
aim. The `-testInput` hook (`visionos/Input/TestInputInjector.swift`) writes pinch events with a gaze ray into the
real path (`GXXRInput` → `VisionInteraction` → `XrGameBoot`); only the OS event source is replaced.
`GX_DEBUG_INPUT=1` logs one line per pinch start and end. Real gaze + pinch on a device is not tested.

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Unit selection (look + pinch) | WORKING (simulator, injected gaze ray, real data) | 2026-09-22: `tap` on a Humvee in `MD_USA01` → Commands status "1 selected", "Last target: Humvee", selection ring and health bar on the unit, HUD shows its buttons; log `[vision-input] start ... role=4 picked=1` then `P7.4 spatial commit`. Routing also covered by the 109-check forwarding test. | verified 2026-09-22 (injected ray) | not tested |
| Add / remove selection | PARTIAL | Additive-toggle modifier implemented and unit-tested. | not tested | not tested |
| Box selection (pinch-drag on terrain) | WORKING (simulator, injected gaze ray, real data) | 2026-09-22: `drag` across two units inside the board → role Box, "2 selected", both units ringed. Rectangle math unit-tested; marker verified earlier with synthetic data. A drag that starts on the board rim pans instead (by design). | verified 2026-09-22 (injected ray) | not tested |
| Move (contextual pinch on ground) | WORKING (simulator, injected gaze ray, real data) | 2026-09-22: Humvee selected, `tap` on open ground → it drove to that point (`XrGameBoot_SpatialClick` → `TouchInput::tap` → the engine's own context command). | verified 2026-09-22 (injected ray) | not tested |
| Move via HUD minimap | WORKING (simulator, injected gaze ray, real data) | 2026-09-22: pinch on the engine radar panel with two units selected → both moved toward that map point (native Generals rule) and the minimap view box changed. | verified 2026-09-22 (injected ray) | not tested |
| Attack / Attack-move / Guard / Stop | PARTIAL | Same routing as Move, forwarding-tested; the engine's command resolution is reused unmodified. Not observed against an enemy yet. A pinch on the HUD STOP button reached the panel (role Panel) but has no visible effect to confirm. | not tested (real flow) | not tested |
| Waypoints | PARTIAL | Waypoint-mode routing implemented; a waypoint marker renders as an in-world feedback element (verified visually with synthetic data). | not tested (real flow) | not tested |
| Formations | PARTIAL | `XrGameBoot_TacticalAction`/`FormationActive` forwarding implemented and tested against the real declarations. | not tested | not tested |
| Unit abilities / special / general powers | PARTIAL | Routed through the SwiftUI Commands window's action tables, ported faithfully from the Quest's `applyCommandAction`/`applyMenuAction` switch tables (1,665 host-tested checks confirm every control id maps to the same engine call as Quest, in both English and German). Never exercised against a running match. | not tested (real flow) | not tested |
| Aircraft commands / Transport load-unload | PARTIAL | Same routing path as above; no aircraft/transport-specific visionOS logic exists beyond the generic command forwarder (as intended — game rules stay in the engine). | not tested | not tested |
| Groups (1–10, save/add/center) and camera bookmarks | PARTIAL | `XrGameBoot_TacticalGroup`/`Bookmark`/`CameraPreset` forwarding implemented and tested; the Commands window renders group count badges from a scripted snapshot (verified in a simulator screenshot, English and German). | verified (UI rendering with synthetic snapshot) | not tested |

## 4. Building placement

Not observed with real data yet: no builder was found at the start of `MD_USA01`. Next check: a map that starts with
a dozer, then pick a structure on the HUD and pinch the ground.

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Ghost preview (follows gaze/pinch aim) | PARTIAL | Placement-aim routing and a ghost-validity feedback marker are implemented (`GXXRFeedbackRenderer`); `XrGameBoot_PlacementLegal` hookup is flagged as an open item by package E (the preview icon's legality state is currently `-1`/unknown pending a small engine-side accessor — see "Known bugs / open items" below). | not tested (real flow) | not tested |
| Validation feedback | PARTIAL | 2026-09-23: the legality source exists now (the engine's own ghost check, `XrGameBoot_PlacementLegal`, forwarding-tested); the ring is green when legal, red when not. Not yet watched in a real placement (no builder in `MD_USA01`). | not tested (real flow) | not tested |
| Rotate (hand twist while pinched) | PARTIAL | `XrGameBoot_RotatePlacement`/`CanRotatePlacement`/`PlacementDegrees` forwarding implemented and tested. | not tested | not tested |
| Confirm / Cancel | PARTIAL | Routed through the same pinch state machine as selection (release confirms, second-hand tap or look-away cancels, per the interaction design doc). | not tested | not tested |

## 5. UI

Hybrid design as specified: the engine's own HUD (ControlBar, radar, health bars, production queue) renders as
textured panels placed in space by the presentation layer; a native SwiftUI "Commands"/"Settings"/"Help" window
set — ported faithfully from the Quest's `XrPanelLayout.h`/`XrCommandUI.h`/`XrMenuUI.h` tables — adds spatial-native
controls without reimplementing any game rule.

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| UI panel layers (engine UI/world/game textures as world-anchored quads) | WORKING (rendering mechanism; no real engine content yet) | `GXXRCompositeLayer`s composited after the eye pass, premultiplied alpha, correct V-flip and (when needed) sRGB decode; poses match the panel table handed to the interaction layer. Verified visually with the fake engine's UI-panel test pattern and the scripted presentation sequence (upright panel for menu/loading, tabletop+panel for interactive). | verified (mechanism, synthetic content) | not tested |
| Engine HUD panel (ControlBar, radar/minimap, money, command buttons) | WORKING (real data, simulator) | 2026-09-22: renders as a panel layer beside the board in `MD_USA01`; HUD pinch input resolves to the panel and the minimap acts (see section 3). Fixed on the way (commit `bb18e54`): missing fonts crashed `InGameUI::postDraw` (Liberation fonts now bundled under the Microsoft names, `GENERALSX_FONTS_DIR`); the Loading presentation mode stuck after the first map load so the HUD never appeared (regression test in `vision-presentation-test`); HUD alpha was written as 0; the HUD was drawn twice because the layer `uvRect` crop was not passed to the compositor. | verified 2026-09-22 | not tested |
| Build menu / Production queue / Context menus | PARTIAL | Same engine-rendered HUD as above; not exercised (no builder or factory used yet). | not tested | not tested |
| Native Commands window (Orders, Instant, Selection, Groups, Waypoints, Formations, Tactics, Camera bookmarks) | WORKING (UI + action routing; not driven by a real match) | 2026-09-22, real data: the status card follows the match live ("1 selected", "2 selected", "Last target: Humvee"). Its buttons could not be pressed in a match: simulator automation taps go to the immersive layer, not to SwiftUI windows. Earlier: SwiftUI window built, opens without crashing (a real SDK bug — `.ornament(...)` + `@Environment(Observable)` in the same view crashing with "No Observable object found" — was found and fixed in 3 places this session). Screenshotted and read in English/light and German/dark: correct sections, correct armed/on/pending tints, correct status card text, correct group-key highlight and localisation (`"Zwangsangriff"`, `"Wegpunkte AN"`). | verified (rendering + localisation, synthetic snapshot) | not tested |
| Settings window (Workspace / Presentation / Graphics / Audio / Controls & language / Data / Diagnostics) | PARTIAL | All pages implemented and wired to their respective APIs (graphics settings, audio listener/session, game-data service, engine-host diagnostics). Only the Graphics page's top row was actually screenshotted and visually confirmed this session; the rest is code-reviewed against the real APIs but not screenshotted (a simulator camera-framing limitation, not a known defect). | verified (Graphics page top row); rest code-reviewed only | not tested |
| Help window | PARTIAL | Implemented (English/German controls guide mirroring `docs/visionos-interaction.md`); not screenshotted this session. | not tested | not tested |
| HUD ornament (Ground View toggle, Recenter, Pause, Leave Tabletop) | WORKING (opens without crash) | Fixed as part of the ornament/Environment crash class above; opens correctly in the simulator run. The Windows menu now also has "Open Launcher". | verified (opens, no crash) | not tested |
| Pinch preview (what a pinch will do) | WORKING (simulator, injected gaze ray, real data) | 2026-09-23: while a board pinch is held the engine answers what a tap there would do (`TouchInput::previewTap`, the same branches and `evaluateContextCommand` EVALUATE_ONLY question as the real tap; `XrGameBoot_PointerIntent`). The cursor and a ring around the target take its colour: select cyan, move green, attack red, interact amber, deselect grey; after the tap the destination marker and target ring fade out in that colour. Seen in `MD_USA01`: cyan ring around a Humvee while pinching it, green marker on open ground with it selected. Attack (red) not seen yet (no enemy in view). Host tests: 11 preview checks in `vision-presentation-test`, 5 in the forwarding test. | verified 2026-09-23 (injected ray) | not tested |
| Quick orders within reach | WORKING (simulator, rendering) | 2026-09-23: the controls strip has a second row: STOP, Attack move, Guard position, Scatter, All units, groups 1–5 with member counts, and More commands (opens the Commands window). Same control ids and actions as the Commands window. Screenshot-checked; pressing them was not tested (simulator automation cannot tap windows). | verified 2026-09-23 (rendering only) | not tested |
| Commands window reorganised | PARTIAL | 2026-09-23: grouped by intent — Orders (pick, then pinch the target), Right now (stop, scatter, cancel), Select, Groups, a Tactics and map views fold-out, Communicator; Help moves to the header. Same controls and actions. Builds; not yet screenshot-checked in a match. | not tested | not tested |
| Tabletop HUD layout: control bar on a console in front of the near edge, HUD band and dialogs behind the far edge; small controls strip instead of the big Commands window | WORKING (simulator, real data) | 2026-09-22: the first real-data run showed an opaque control bar standing across the back of the map and a 0.75 × 1 m Commands window over the right side. Now the bar lies in front of the near edge (1.10 m wide, tilted 18°), the HUD band stands behind the far edge, and a small strip (Ground View, Recenter, Pause, Menu, Leave Tabletop, Windows) opens instead of the Commands window (open it from Windows). Screenshot-checked in `MD_USA01`: map unobstructed; injected pinches select a Humvee on the map just behind the console and press the console minimap (the console's see-through rows pass pinches to the map, `XrGameBoot_HasUIAt`). `vision-presentation-test` 321 checks, `vision-interaction-test` 181,909 checks. Not yet seen on a headset. | verified 2026-09-22 | not tested |
| Launcher steps aside during a match | WORKING (simulator, real data) | 2026-09-22 (commit `2ff1555`): the launcher (about 1 m tall) blocked the board. It now closes once the tabletop opens with real data; Leave Tabletop or a system dismissal reopens it; `-keepLauncher` keeps it. Observed: board and HUD unobstructed, engine keeps running with the launcher closed (scene-phase pause and status polling moved to app level). | verified 2026-09-22 | not tested |
| Selected-unit / hover info | PARTIAL | Reads `XrGameBoot_WorldHoverInfo`/`HoverInfo` through the same forwarder; displayed as "last pinched/hand-pointed target" (there is no continuous gaze on visionOS — documented honestly, not a bug). Never exercised against real content. | not tested | not tested |
| UI readability (fonts, contrast, panel distance/scale) | PARTIAL | Panel-size/distance numbers and legibility arithmetic (arcminutes for the smallest ControlBar text at the chosen panel size/distance) are computed and documented in `docs/visionos-presentation.md`; SwiftUI window contrast/target-size criteria documented in `docs/visionos-ui.md`. Real ControlBar text has never been looked at (no real engine content). | verified (arithmetic only) | not tested |
| Text input (chat, save name, lobby name — no hardware keyboard) | WORKING (mechanism; not exercised with a real focused field) | Offscreen SDL3 text-injection path added (`SDL3GameEngine` `pollSDL3Events`/`updateTextInputState` no longer bail without a window); `XrGameBoot_TextInput`/`TextFieldFocused` plus a SwiftUI `TextInputBridge` sheet, wired into the launcher this session (`.textInputBridge()`). Compiles and links into both app builds. Never focused a real game text field (no running match). | verified (compiles/links); not exercised | not tested |

## 6. Camera, modes and lifecycle

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Ground View enter/exit | PARTIAL | Full plumbing exists end to end: interaction layer observer state → `XrWorldFrame.observer` → `XrGameBoot_CanObserveGround`/`PickObserverGround`/`ObserverStep`, spatial-audio listener repositioning, and an opaque-black compositor clear (a real bug — this clear was documented but never wired — was found and fixed this session; screenshot before/after confirms the fix). No real terrain or units have ever been observed this way (no game data). | verified (mechanism + visual fix, synthetic scene) | not tested |
| Pause / resume | WORKING (mechanism) | Since `2ff1555` the app-level `scenePhase` (active while any window or the immersive space is active) drives the pause, not the launcher window's own phase. `scenePhase` changes and layer pause/invalidation map to `GXEngineHost_Pause` (engine thread parks, audio pauses, mouse loses focus, per `SDL3GameEngine`'s existing mobile-pause logic) and `PlatformLifecycle_Notify`; the engine, ANGLE context, and texture ring all survive immersive-space close/re-open (verified in the close/re-open soak cycles). | verified | not tested |
| Save / load | NOT YET IMPLEMENTED (needs real game data) | Engine save path (`fopen` under the user-data directory) is unmodified from the Quest/desktop code and resolves correctly inside the app container by construction (Apple branch of `GlobalData::BuildUserDataPathFromRegistry` already targets `~/Library/Application Support/...` unmodified). Never exercised. | not tested | not tested |

## 7. Game modes

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Skirmish | NOT YET IMPLEMENTED (not yet tried) | Real data now loads and a campaign map plays (see Campaign); a skirmish game (setup screen, AI slots, start) has not been started yet. | not tested | not tested |
| Campaign | PARTIAL (real data, simulator) | 2026-09-22: mission `MD_USA01` loads through the `GX_START_MAP` hook and is playable with injected input (select, move, box select, pan, minimap). Not yet: starting it from the in-game menu, briefing movies, winning a mission. | verified 2026-09-22 (one mission) | not tested |
| AI | PARTIAL | The mission's scripted/AI side runs with real data (the match advances; objectives and money update). No skirmish AI opponent has been started yet. | verified 2026-09-22 (mission scripts only) | not tested |
| Multiplayer / LAN | NOT YET IMPLEMENTED | GameNetworkingSockets is built for both visionOS slices via a vcpkg overlay-port extension (Darwin\|visionOS) as part of the engine build; no session source or `NSLocalNetworkUsageDescription` flow exists yet. Out of scope until skirmish works (per the engineering priority order). | not tested | not tested |

## 8. Audio

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| OpenAL Soft runs on visionOS | WORKING | `scripts/qa/vision-audio-openal-test.sh --udid <own device>`: 63/63 checks passed. **A real blocker was found and fixed**: OpenAL Soft's `alsem.h` only used libdispatch semaphores for iOS/tvOS; on visionOS it fell back to POSIX `sem_init`, which Darwin lacks, and `alcCreateContext` aborted the process. Fixed with a one-line patch (`TARGET_OS_VISION` added), now part of the committed `cmake/patches/openal-soft-1.24.2-visionos.patch`. Default device opens (48000 Hz), a tone plays, `alcDevicePauseSOFT`/`ResumeSOFT` work correctly. | verified | not tested |
| FFmpeg audio decode chain (mirrors `OpenALAudioCache`/`FFmpegFile`) | WORKING | `scripts/qa/vision-audio-ffmpeg-test.sh --udid <own device>`: 32/32 checks passed — WAV PCM/ADPCM, OGG Vorbis, FLAC, and a synthetic MP3 all decode correctly through the exact `libavformat`/`libavcodec` call sequence the engine uses. | verified | not tested |
| Spatial (head-driven) battlefield audio model | WORKING (mechanism, math, and loopback numerics; not heard on real hardware) | `scripts/qa/vision-audio-listener-test.sh`: 71/71 host checks (board↔world transform matches the real `xrWorldToBoard()` for 300 random frames; yaw/head/world invariances hold). `scripts/qa/vision-audio-openal-test.sh` loopback numerics: left/right ILD ±5.4 dB at tabletop scale, yaw-180 swaps channels exactly, distance attenuation matches the configured curve with 0% deviation. A "Spatial battlefield audio" toggle (default ON) falls back to the original camera-relative model when off. | verified (numerics) | not tested (cannot judge by ear here) |
| AVAudioSession lifecycle (interruptions, route changes, background) | WORKING (mechanism) | `scripts/qa/vision-audio-session-test.sh --udid <own device>`: 36/36 checks — category/mode/mixing set and read back correctly; interruption, route-change, and media-services-reset handling verified by posting the real system notifications with real payloads; a genuine `UIApplicationWillEnterForeground` was observed from a real running app process. | verified | not tested |
| Voices, weapon sounds, music, ambient, UI sounds | PARTIAL | Real data is now present and the engine's audio subsystem runs during the mission; nobody has listened to it or checked the audio log yet. | not tested | not tested |

## 9. Performance

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Compositor holds display rate independent of engine load | WORKING | 340 s fake-engine soak: compositor median 60.0 fps throughout, including during simulated multi-second engine stalls (the architecture's entire point — see section 1). | verified | not tested |
| Frame rate with the real engine / real content | PARTIAL (simulator only) | 2026-09-22, real data, `MD_USA01` in the simulator: compositor 60 fps steady, engine 12–15 fps with the HUD split (launcher Engine/Tabletop status lines). A Mac simulator number, not a device number. The Quest edition's own reference numbers (20.7–42.0 ms/frame on Adreno hardware) do not transfer to Apple GPUs or ANGLE-Metal. | not tested | not tested |
| Draw calls, texture memory, shadow cost | NOT YET IMPLEMENTED | Same — needs real content to measure. | not tested | not tested |
| Game speed / logic time scale (risk R1 from the architecture doc) | WORKING (fix implemented; effect on a real 30 Hz vs display-rate frame not observed with real gameplay) | `enableLogicTimeScale(TRUE)` / `setLogicTimeScaleFps(30)` called after `FramePacer` creation on visionOS only (Quest unaffected); a configurable render-fps cap (default 45) decouples engine frame rate from display rate; a logic-Hz self-check logs the effective rate every 10 s. Verified by code review and by the engine-host pipeline log showing the engine and compositor running at their intended, different rates during the soak. Never observed against real, ongoing gameplay logic. | verified (mechanism) | not tested |

## 10. Builds and deployment

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Build for simulator (shell + linked engine) | WORKING | See section 1. | verified 2026-09-22 | n/a |
| Build for device, unsigned (shell + linked engine) | WORKING (compile-only) | See section 1. | n/a | not tested (no device, no team) |
| Engine static library, both slices, merged xcframework | WORKING | See section 1. | verified 2026-09-22 | not run |
| Signed device deploy | NOT YET IMPLEMENTED | Procedure documented (`docs/BUILD/VISIONOS.md`: Xcode with your own team, or `xcrun devicectl device install app`); never executed — no Apple Vision Pro is attached and no signing team is configured in this environment. | n/a | not tested (no device) |

## Known bugs / open items found by this session's own verification (not yet closed)

These are real defects or gaps found while running the tests above, kept here until a future pass closes them —
do not re-discover them from scratch:

1. **Placement legality is wired but not yet seen in a real placement** (2026-09-23): `InGameUI` now keeps the
   result of the ghost's own `isLocationLegalToBuild` check (`getPlacementLegalState`), `XrGameBoot_PlacementLegal`
   returns it and the Metal placement ring turns green/red. Forwarding test covers the call; no builder was available
   in `MD_USA01` to watch it live.
2. **SwiftUI/visionOS-26-SDK crash class**: any View/ViewModifier that reads `@Environment(SomeObservableType.self)`
   while also attaching `.ornament(...)` anywhere in its own body crashes at first layout ("No Observable object of
   type X found", inside `TransformOrnament.updateValue()`). Fixed in the three places found
   (`HoverInfoOrnament`, `UIWindowsCoordinator`, `CommandsWindow`) by passing the value as a stored property
   instead of reading it from the environment; watch for the same pattern in any new ornament code.
3. **Depth-test trap for in-world feedback markers**: the composited eye texture (real engine or fake) carries one
   constant depth value across the whole picture (for system reprojection), not real per-pixel depth. Code that
   depth-tests genuine 3D feedback geometry against it will make that geometry invisible — this exact bug hid the
   box-select rectangle, cursor, and waypoint markers until fixed (now drawn unconditionally on top). Documented at
   length in `docs/visionos-presentation.md` §5 so it is not reintroduced.
4. **Settings window pages beyond Graphics' top row** have not been visually confirmed in the simulator (a camera-
   framing limitation of the fixed simulator viewpoint, not a known code defect) — needs a real device or a better
   simulator camera-control approach to close out.
5. **Simulator automation pinches carry no gaze ray**: `selectionRay`, hand pose and location are all zero, also
   after adding visionOS 26 tracking areas (commit `e175d88`). Use `-testInput` in the simulator; real gaze + pinch
   must be verified on a device.
6. **Advisory board hit in `GXXRInput` becomes NaN when the ray is zero** (seen in the input log). Harmless — the
   interaction layer does its own ray math — but worth a guard.
7. **Possible GL-name recycling hazard**: when the target ring is reallocated, new GL texture names can equal old
   ones, and d3d8gles skips re-attaching an FBO when the name is unchanged. That could leave a stale attachment.
   Not reproduced yet.
8. **Commands-window buttons are not drivable by simulator automation** (taps go to the immersive layer); press them
   by hand in the Simulator app or on a device.
9. **vp/f-ui does not build standalone** without package C2's `GXGraphicsSettings`/`GXEngineHost.h` additions —
   resolved by merging both into `visionos-port` (done, commit `f9ba0dd`); flagging only so nobody tries to build
   an isolated `vp/f-ui` checkout and gets confused.

## How to update this file

1. **One change, one row, one piece of evidence.** Change a row only when you ran something. Put the command or action,
   the ISO date and where the output lives in the Evidence column. If the output is not in the repository, give the
   command that reproduces it. "It should work" is not evidence.
2. **Never promote without a run.** WORKING needs an observed result. Code that compiles but was never exercised is
   PARTIAL at best. When a feature regresses, set BROKEN and write how it fails; do not delete the row.
3. **Keep the two environment columns honest.** `verified <date>` means you ran it there. Anything that needs
   hand tracking, two-view stereo, real timing, comfort or thermals stays `not tested` or
   `cannot be verified here` until it runs on a physical Apple Vision Pro. Fill the Physical column with the device
   model, the visionOS version and the build number.
4. **Record the toolchain.** For every new result note Xcode, visionOS SDK and runtime versions if they differ from the
   header. Update the header line and the "Last updated" date when you edit.
5. **Add rows, do not merge them.** A new feature gets its own row in the fitting section. Sub-features that can fail
   independently (for example placement rotate versus confirm) get separate rows.
6. **Keep the summary and README in step.** If a status changes, update "Summary of today's evidence" and the
   "Status" section of [../README_VISIONOS.md](../README_VISIONOS.md) in the same commit, and the risk register in
   [VISIONOS_PORT_ARCHITECTURE.md](VISIONOS_PORT_ARCHITECTURE.md) if a risk is closed or opened.
7. **Run `scripts/qa/vision-run-all.sh` before editing this file** so every host-tier claim is freshly reproduced,
   not copied from memory; use `--udid <your own simulator device>` to also reproduce the simulator tier.
8. **Template for a new evidence entry:**
   `<command or action>; <YYYY-MM-DD>; <Xcode/SDK/runtime or device+OS>; <result in one sentence>; <path or reproduce command>`.
9. **Do not paste private paths.** Screenshots and logs committed to `docs/media/visionos/` must not show container paths,
   user names or team identifiers; redact them before committing.
