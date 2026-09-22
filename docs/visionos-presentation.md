# visionOS presentation, world and feedback (package C2)

Ports the Quest host's per-frame decisions (`XrHello.cpp` `runLoop`, `XrViewMode.h`, `XrLayers.h`, `XrLayout.h`,
`XrWorld.h`) to the visionOS engine host: what mode the app is in (loading / menu / tabletop / ground view /
recovery), where the panels sit, what feedback (box-select rectangle, grab bar, cursor, placement/ground reticles,
comfort fade) gets drawn and where, blocking loads, text entry without a keyboard, and the graphics settings API
package F's Settings window is built on.

Contents: 1 State machine, 2 World frame and eye sizing, 3 Panel layout, 4 Readability, 5 Feedback rendering,
6 Loading presenter, 7 Ground View and audio listener, 8 Text input bridge, 9 Graphics settings API, 10 Verification
(fake-engine scripted mode), 11 What is unverified.

## 1. State machine

`GeneralsMD/Code/Main/visionos/VisionPresentationLogic.h` is the pure (no engine, no Apple, no GL) port of the
Quest's presentation decision. One `VisionPresentation` object (`VisionPresentation.h`/`.cpp`) is called on the
engine thread every frame, in this order, by `GXEngineHostEngine.cpp`:

```
update(in, driver, out)          before the engine frame: visionPresentationBegin (mode/split change detection,
                                  xrApplyWorldStartup), workspace restore/anchor, visionBuildWorldFrame, the eye
                                  extent, the panel table for the interaction step (from LAST frame's resolved state)
driver.step(...)                 package E: input, gestures, board, eyes into the world frame
XrGameBoot_SetWorldFrame / SetSplitEnabled / XrGameBoot_Frame     the engine frame (a loader may call describeLoading)
finish(post, driver, world, in, out)   after the frame: visionPresentationEnd (P17 recovery resolve), final layout
                                  (= the composite layers), feedback, workspace save
```

The panel table built by `update()` for frame N+1 is exactly the composite layers `finish()` published for frame N
(same `visionLayoutPanels` call, same resolved mode, same board) — the contract
`docs/visionos-interaction.md` section 9 states ("the pose given to `visionMakePanel` must be the pose of the
layer"). `VisionPresentation::plan()` / `GXEngineHostEngine.cpp`'s `s_plan.plan.panels` is what the interaction layer
picks against.

`VisionPresentationMode` (`VisionPresentationLogic.h:53`):

| Mode | Shown | Entered when |
| --- | --- | --- |
| `Idle` | compositor status indicator | engine not booted |
| `Loading` | engine's loading screen, upright panel, no stereo | a synchronous loader is presenting frames (`visionPresentationLoadingBegin`), or P17 recovery outside a match |
| `Menu` | composed frame, upright panel | not an interactive match |
| `Cinematic` | composed frame, upright panel | interactive match, UI split off (movie/letterbox/camera path/shell overlay) |
| `Tabletop` | stereo world on the board + engine UI beside it (or the world texture flat on the board when stereo is not eligible) | interactive match, split ready, not observing |
| `GroundView` | the tabletop's observer (human-scale) view, no panels | `Tabletop` facts plus an active observer |
| `Recovery` | explicit "recovery" card, full-world render requested | interactive match, world capture failed (P17) |

Transitions: `visionPresentationBegin` (`VisionPresentationLogic.h:101`) detects an `interactiveGame` or `splitReady`
change and resets grab/controls state exactly as `XrHello`'s runLoop does, then calls the Quest's own
`xrApplyWorldStartup` template unchanged so P12.1/P15 (remembering tabletop intent across a cinematic) work
identically to the Quest. `visionPresentationEnd` (`VisionPresentationLogic.h:127`) calls the Quest's own
`xrResolveCapturedView` template (P17) to decide `recoveryVisible`/`stereoVisible` from what the engine frame really
produced (`eyeTexture[0..1]`, `worldTexture`, `gameTexture`), and an observer rendered without a complete stereo
capture is *always* treated as a recovery case (never leaves Ground View looking at nothing with no way back). Mode
resolution is the `if`-chain at `VisionPresentationLogic.h:143-150`, in Quest priority order: not booted -> `Idle`;
inside a synchronous load -> `Loading`; P17 recovery -> `Recovery` (interactive) or `Loading` (not interactive, e.g.
loading a lobby); stereo eyes complete -> `GroundView` or `Tabletop`; interactive + split ready but no stereo ->
`Tabletop` (planar world); interactive, not split -> `Cinematic`; otherwise -> `Menu`.

## 2. World frame and eye sizing

`visionBuildWorldFrame` (`VisionPresentationLogic.h:220`) fills every `XrWorldFrame` field that is not the board,
coverage, observer or eye poses/fov (package E's `VisionFrameDriver::step` — `visionApplyToWorldFrame` /
`visionFillWorldFrameEyes` — owns those): `enabled` (the stereo-requested flag from `visionPresentationBegin`),
`width`/`height` (the eye extent below), `healthBars`/`unitRings`/`boardFrame` (from `XrLayout`),
`volumeShadows` (from the graphics settings' shadow mode), `multiviewStereo = false` (ANGLE has no
`GL_OVR_multiview`), `atlasStereo` (the ring layout) and `elideWorldCopy = true` (P14: skip the composed frame's
world copy while the eyes are complete).

Eye target size (`visionEyeExtent`, `VisionPresentationLogic.h:188`): the Quest's `xrStereoExtent` tier budget from
the compositor's eye viewport and the graphics settings' `eyeTier` (Conservative/Balanced/Ultra), times
`renderScale` (0.5..1.5, `GXGraphicsSettings`), then clamped to the ring's limits (`kVisionMaxEyeDim` = 4096 px/side,
`kVisionMaxEyePixels` = 8,000,000 px/eye — 4 ring slots x 2 eyes x 8 MP x 4 B = 256 MB of colour targets at the cap,
plus about 64 MB of D24S8 depth, comfortably inside a 16 GB device; the Ultra tier at 1.5x render scale would exceed
it and is scaled back down, `clamped = true`). Atlas mode halves the width limit (both eyes share one target).

## 3. Panel layout

`visionLayoutPanels` (`VisionPresentationLogic.h:381`) builds the panel table **and** the composite layers from one
function so they cannot diverge:

* **Upright** (`Menu`, `Cinematic`, `Loading`, `Recovery`): one panel showing the composed `GAME` target full-frame,
  at the workspace's upright-screen surface (`visionScreenSurface`, `VisionWorkspace::screenSurface`).
* **Tabletop, planar** (stereo not eligible): the `WORLD` target cropped to `XrGameBoot_WorldRect()`, flat on the
  board surface, plus the UI pieces below.
* **Tabletop, stereo**: the `UI` target's control-bar band (`xrUIPieceRect(2, ...)`, the whole canvas while a dialog
  owns it) and the transparent HUD band above it (`xrUIPieceRect(3, ...)`), positioned by `visionUiBarSurface` /
  `visionUiPieceSurface` — the direct port of the Quest's `XrLayout::applyTabletopPreset` / `displayedSurface`: the
  canvas stands behind the board's far edge, leaning back 24 degrees (`kVisionUiLeanBackRad`) toward the player.
* **GroundView**: no panels — the observer has the whole view, same as the Quest hiding every surface.

Panel/canvas numbers (`VisionPresentationLogic.h:248-255`): upright screen 1.35 m wide, 1.10 m ahead of the launch
heading, 0.02 m below eye level (Quest `relative[0]`); tabletop UI canvas 1.60 m wide (the Quest's 1.8 m targets a
1.65 m board; ours is proportional to `kVisionUiCanvasWidthM`), 0.10 m gap from the board's far edge, 0.03 m lift
above the table plane, 4 mm marker lift in front of a panel it shares a plane with.

## 4. Readability

`visionReadability` (`VisionPresentationLogic.h:426`) computes the visual angle of one engine UI texel
(`2*atan(texel_size / (2*distance))`, exact small-angle-free) and of a glyph `glyphTexels` tall, plus texel density
against a Vision Pro's roughly 34 display-pixels-per-degree foveal resolution. Measured by
`scripts/qa/vision-presentation-test.sh` against the real panel geometry above (1280-wide engine UI backbuffer):

```
tabletop UI canvas: 1.60 m wide at 1.37 m (eye to bar center): 3.13 arcmin/texel
  glyph 8/10/12 px = 25.1 / 31.3 / 37.6 arcmin      720p: 0.56 texel/display px   1080p: 0.84
upright screen:      1.35 m wide at 1.10 m:          3.30 arcmin/texel
  glyph 8/10/12 px = 26.4 / 33.0 / 39.5 arcmin
```

A comfortable minimum for body text is generally cited around 15-20 arcminutes; the smallest ControlBar glyphs (the
engine's 8 px bitmap font, used for secondary labels) land at 25-26 arcminutes at these distances/widths — legible,
with headroom. `texelsPerDisplayPixel` < 1 at 720p (0.56/0.84) means the panel is shown *larger* than native texel
resolution: the engine UI texture is magnified, not minified, so no engine-side text was made too small to resolve.
Package F's `GXUIResolution` setting (`GX_UI_1080P`) raises `texelsPerDisplayPixel` toward 1:1 for players who prefer
the extra sharpness over slightly softer default scaling; the geometry (panel size/distance) does not change with it.
`XrGameBoot_GameWidth/Height()` currently report a fixed 1280x720 engine backbuffer regardless of `uiResolution`
(`GXEngineHostConfig::renderWidth/Height`, boot-time only) — see section 11.

## 5. Feedback rendering

Two layers, matching what the Quest draws depth-correct in-world versus head/screen-space
(`GeneralsMD/Code/Main/visionos/VisionFeedback.h:4-9`):

* **Engine stereo pass** (unchanged from Quest): unit rings, health bars, board frame, the engine's own box-select
  outline (`XrGameBoot.cpp` `drawXrWorldDecorations` -> `d3d8gles_DrawXRDecorations`) — depth-tested, terrain
  following, drawn by the engine itself into the stereo targets.
* **Metal, over the composited eyes** (`visionos/Renderer/GXXRFeedbackRenderer.h`/`.mm`, new this wave): the
  translucent box-select fill + outline (visible even with the engine's own decorations off), the board grab bar,
  the pinch cursor, the panel-pointer dot, the hover outline, the building-placement legality ring, the Ground View
  teleport reticle + hold ring, the eye-to-cursor ray, the fading destination waypoint marker, and the full-view
  comfort-fade veil. `VisionFeedbackBuilder::build` (`VisionFeedback.h:24`) turns the interaction layer's
  `VisionInteractionOutput` into the flat `GXHostFeedback` struct every frame; `GXXRFeedbackRenderer` draws it in
  `visionos/Bridge/GXXRBridge.mm`'s `encodeEngineFrame`, in the SAME render pass as the composite layers (markers
  after the layers, so a marker sitting on a panel or the board picture draws in front of it) and with the same
  rendered-eye `clipFromWorld` the layers use (so the markers ride the same reprojection). World markers (box, grab
  bar, cursor, reticles, hover, waypoint) are projected from their true world position but drawn WITHOUT a depth
  test: the board/UI picture behind them is a flat 2D GL render composited as a full-screen quad with ONE constant
  depth written across it for reprojection (`docs/visionos-engine-host.md` section 4, `encodeEyeCompositeInto`'s
  `constantDepth`), not the real per-pixel depth of what it depicts, so testing a marker's true depth against it
  would discard the marker wherever its true distance differs from that single constant (most of the board away
  from the exact focus point) — an artifact of the technique, not a meaningful occlusion decision. (An earlier
  version of this renderer used a depth test here; it made every marker away from the board's focus point
  invisible, caught by looking at the section 10 screenshots below and fixed before this session's report.) The
  comfort-fade veil is a separate, also depth-ignoring, full-screen pass drawn last, per eye, with THIS drawable's
  own viewport (not the rendered-eye clip: it must always cover the current view, including a frame reprojected
  from an older head pose).
  `GXHostFeedback.pointerLayer` indexes `GXHostFrameOutput.layers` directly (not the filtered, texture-present-only
  array the compositor builds for drawing) so an index survives a frame where one layer's texture was momentarily
  unavailable.
* `GX_GFX_FOCUS_MARKER` / `GX_GFX_COMFORT_FADE` (`GXGraphicsSettings.flags`) gate the cursor/ray/pointer/hover
  markers and the fade veil respectively (`VisionFeedback.h:30-31`); the box, grab bar, placement and waypoint
  markers are not gated (they carry information the player needs to confirm an action, not just a convenience cue).

## 6. Loading presenter (mission item 3)

A synchronous loader (map load, campaign movie) runs entirely inside one call to `XrGameBoot_Frame()` and can block
for many seconds; the Quest calls back into the host so the loading screen keeps reaching the display. The visionOS
wiring:

* `GXEngineHostEngine_Boot` installs the callback: `XrGameBoot_SetLoadingPresenter([](void*) {
  GXEngineHostEngine_PresentLoading(); }, nullptr)` (`GXEngineHostEngine.cpp`).
* `GX_XR_PresentLoadingFrame()` (existing engine call, unchanged) invokes it from inside the blocked frame whenever
  the engine has a new loading-screen picture ready.
* `GXEngineHostEngine_PresentLoading()` (`GXEngineHostEngine.cpp`) reads the facts, calls
  `VisionPresentation::describeLoading` (fills a `GXHostFrameOutput` for the upright panel, no stereo, without
  touching the simulation) and publishes it through `GXEngineHost_PresentNested(frame, &out, &req)` — the same ring
  slot the outer (blocked) frame is writing into. The three outcomes (`GXEngineHostServices.h:204-209`) are handled
  exactly as documented: `GX_NESTED_KEPT` (no fresh head pose: keep drawing into the same slot, nothing to do),
  `GX_NESTED_CONTINUED` (published; re-arm `d3d8gles_SetXRHostTargets(frame->targets)` for the new slot before the
  engine draws again), `GX_NESTED_LOST` (published, but the ring had no free slot for the next one: clear the host
  targets with `d3d8gles_SetXRHostTargets(nullptr)` until the outer `XrGameBoot_Frame()` call returns).

**Verified without game data**: `visionos/ANGLE/GXXRFakeEngine.mm`'s scripted mode (section 10 below) runs the exact
same `GXEngineHost_PresentNested` call from a genuinely blocking loop (`PresentLoadingBurst`, 0.9-1.5 s, ~10 nested
frames at 100 ms apart) in the simulator, proving the nested-present protocol end to end: the compositor keeps
presenting fresh frames (rising frame counter, moving on-screen content) for the whole blocked interval. This is the
"fake-engine mode that blocks and presents" the mission asked for. What is **not** verified: the real engine's own
call to `GX_XR_PresentLoadingFrame` during an actual map load — that needs real game data and a boot, neither
available on this machine (see section 11).

## 7. Ground View and the audio listener

`VisionInteractionOutput`'s observer state (package E) flows into `XrWorldFrame.observer` (package E's
`VisionFrameDriver::step`), then to the engine's own `XrGameBoot_CanObserveGround`/`PickObserverGround`/
`ObserverStep`, unchanged from the Quest. `GX_XR_BeginStereoWorld()` (`XrGameBoot.cpp`) now also updates the audio
listener every frame, before the engine's own audio update, following `docs/visionos-audio.md` section 4.1:

* **Ground View** (`s_worldFrame.observer` true): `GXAudio_SetListenerPoseWorld(1 / kXrObserverUnitsPerMetre, pos,
  fwd, up)` — the observer camera position/forward/up are already in game units (1 unit = 1/10 m), converted to
  metres for the listener.
* **Tabletop** (not observing): `GXAudio_SetBoardFrame(centre, axisX, axisZ)` once per frame (board orientation as
  unit axes) then `GXAudio_SetListenerPose(scale, headBoard, forwardBoard, upBoard)` — the head pose transformed
  into board-local space by the inverse board pose, `scale = board.width / worldSpan` metres-per-game-unit.

Exit returns to the unchanged tabletop: `VisionPresentationLogic.h`'s mode resolution simply stops selecting
`GroundView` once `observerActive` goes false (no separate "exit" transition to get wrong).

`GXHostFrameOutput.groundView` (documented since an earlier wave as "the compositor clears to opaque black behind
them, no passthrough") was not actually wired into the compositor before this session — `GXXRBridge.mm`'s
`encodeEngineFrame` never read it, so an eye viewport with nothing to draw fell through to the normal alpha-0
(passthrough) clear regardless of mode. Fixed this session: `GXXRMetalRenderer.encodeClearInto:...:opaqueBlack:`
takes an explicit flag now, and the engine-mode branch passes `out.groundView`. Confirmed live in the simulator
(section 10): the ground-view phase screenshot shows solid black behind the launcher window and UI panel, where the
earlier build still showed the simulator's mock room through it.

**Verified**: the arithmetic (host tests, `vision-presentation-test.cpp`); that the call sites compile inside
`GX_XR_BeginStereoWorld()` guarded by `GX_PLATFORM_VISIONOS`; the opaque-black clear, live in the simulator (section
10, `final-ground-view.png`). **Not verified**: actually hearing spatial audio move as Ground View is entered — that
needs a booted engine with audio and a headset/simulator audio route, neither available in this session.

## 8. Text input bridge (mission item 6)

There is no hardware keyboard on Vision Pro and the engine runs offscreen (`GX_XR_OffscreenBoot`, no SDL window), so
`SDL3GameEngine::pollSDL3Events`/`updateTextInputState`/`forwardTextInputEvent` previously bailed out without a
window to read from. The bridge (engine thread only):

* `SDL3GameEngine::xrTextEntryFocused(std::string*)` (`SDL3GameEngine.h`/`.cpp`): true while
  `TheWindowManager->winGetFocus()` is a `GWS_ENTRY_FIELD` gadget (chat, save-game name, lobby name); optionally
  returns its current content, decoded from the gadget's `UnicodeString` to UTF-8 (BMP only — matches
  `forwardTextInputEvent`'s own `GWM_IME_CHAR` decode).
* `SDL3GameEngine::xrInjectText(utf8, backspaces, enter)`: synthesizes `backspaces` Backspace key down/up pairs
  through the existing `SDL3Keyboard` event queue, then calls the existing `forwardTextInputEvent(utf8)` path (the
  same `GWM_IME_CHAR` route real SDL text input uses) after pointing `m_TextInputFocusWindow` at the focused window,
  then optionally an Enter key press.
* `XrGameBoot_TextFieldFocused(std::string*)` / `XrGameBoot_TextInput(utf8, backspaces, enter)` (`XrGameBoot.h`/
  `.cpp`, visionOS only): thin `dynamic_cast<SDL3GameEngine*>(TheGameEngine)` forwarders, gated on the engine having
  booted.
* `GXEngineHost.h`/`.mm`: `GXEngineHost_TextFieldFocused(char*, size_t)` (thread-safe snapshot, refreshed once per
  frame by `applyGraphicsToTextures` in `GXEngineHostEngine.cpp` alongside the presentation status) and
  `GXEngineHost_SubmitText(utf8, replaceExisting, pressEnter)` (queues `GXEngineHostEngine_TextInput` on the engine
  thread via `GXEngineHost_Post`; `replaceExisting` first counts UTF-8 codepoints in the gadget's CURRENT content —
  read fresh, since it may have changed since the snapshot, e.g. autocomplete — and sends that many backspaces).
* `visionos/App/TextInputBridge.swift` (new, package C2's Swift file): a `ViewModifier`
  (`.textInputBridge()`) that polls `GXEngineHost_TextFieldFocused` every 0.2 s and, when focused, presents a
  `.sheet` with a system `TextField` (Cancel / Send); Send erases the gadget's existing content and types the
  sheet's text, then presses Enter (chat: sends the line; a name field: confirms it). **Self-contained by
  necessity**: package C2 does not own `visionos/App/LauncherView.swift` (package G) or `GeneralsZHXRApp.swift`
  (package F, scenes only), so this file cannot attach itself to a window. It is a one-line additive hookup —
  `.textInputBridge()` on the launcher's root view — documented at the bottom of the file and in
  `handoffNotesForLead` for the lead/package G to apply.

**Verified**: `GXEngineHost_TextFieldFocused`/`GXEngineHost_SubmitText`/`XrGameBoot_TextFieldFocused`/
`XrGameBoot_TextInput`/`SDL3GameEngine::xrTextEntryFocused`/`xrInjectText` all compile (they are part of the engine
static library; `vision-presentation-test.sh` / `vision-interaction-test.sh` link and pass, proving nothing else in
the engine broke). `TextInputBridge.swift` type-checks as Swift source against the existing `GXEngineHost.h`
bridging-header declarations. **Not verified**: actually focusing a text gadget and typing through the sheet — that
needs a booted real engine with a UI screen that has an entry gadget (chat/save/lobby), which needs game data this
machine does not have, and the one-line `LauncherView.swift` hookup is not yet applied (out of this package's
ownership).

## 9. Graphics settings API (mission item 7, consumed by package F)

`GXEngineHost.h`'s `GXGraphicsSettings` (extendable via `size`, matching the `D3D8GLES_XRConfig` convention):
`renderScale` (0.5..1.5, applied to the eye extent, section 2), `renderFpsCap` (30..120 fps, 0 = default 45, < 0 =
uncapped — `XrGameBoot_SetRenderFpsCap`), `shadowMode` (`GX_SHADOWS_OFF`/`DECALS`/`VOLUMES` —
`XrGameBoot_SetShadowMode` for the decals flag, `XrWorldFrame.volumeShadows` for the volumes flag, both applied in
`XrGameBoot_Frame`'s existing shadow-flag block), `eyeTier` (Conservative/Balanced/Ultra, section 2's tier budget),
`uiResolution` (720p/1080p — read once at boot only, `gxUIResolutionSize`), and a `flags` bitset
(`GX_GFX_FOCUS_MARKER`, `GX_GFX_COMFORT_FADE`, section 5). `GXEngineHost_SetGraphics`/`GetGraphics` validate and
clamp every field (`gxGraphicsSanitize`); a change is applied on the engine thread at the start of the next frame
(`GXGraphicsSettings_ConsumeForEngine`, generation-counted so `GXEngineHostStatus.graphicsApplied` can show whether a
requested change has taken effect) — never mid-frame. Defaults (`gxGraphicsDefaults`): render scale 1.0, 45 fps cap,
decal shadows, balanced eye tier, 720p UI, both flags on.

**Verified**: the full settings round-trip (set -> sanitize -> consume -> apply -> read back) is covered by
`vision-presentation-test.cpp`'s graphics-settings scenarios (part of the 303 checks, section 10) and by
`GXGraphicsSettings.cpp`'s own unit coverage. **Not verified**: the actual visual/performance effect of a setting
(e.g. that `renderScale = 0.5` measurably reduces GPU time) — that needs the real engine rendering real content.

## 10. Verification: fake-engine scripted mode (mission item 8)

`visionos/ANGLE/GXXRFakeEngine.mm`, launch argument `-fakeEngineScript` (implies `-fakeEngine`; per-phase durations
override with `-fakeEngineScriptLoading/-Menu/-Tabletop/-Ground <seconds>`, defaults 4/4/8/6 s): cycles
**loading -> menu -> tabletop -> ground-view -> tabletop -> ground-view -> ...** (loops after the first pass so a
soak run keeps exercising every mode) using the SAME `GXXRGLTestScene` the plain `-fakeEngine` test scene uses, with
`GXHostFrameOutput.presentationMode`/`stereoValid`/`groundView` set per phase and a synthetic `GXHostFeedback` built
directly (there is no real interaction layer driving a fake engine): a box-select rectangle + additive-select tint
(halfway through the tabletop phase) + cursor + grab bar + aging waypoint marker during `tabletop`; a teleport
reticle + comfort-fade ramp (fades to black over the first/last 30% of the phase, simulating the enter/exit cut)
during `ground-view`. The very first `loading` phase also runs `PresentLoadingBurst` (section 6) to prove the
nested-present path. `GXEngineHost_SetPresentationStatus` is called every frame so the launcher's engine status
(`GXEngineHostStatus.presentationMode`) reflects the scripted phase too.

Host tests (pure logic, no simulator, no game data):

```
$ scripts/qa/vision-presentation-test.sh
readability: tabletop UI canvas 1.60 m wide at 1.37 m ... (section 4 numbers)
vision-presentation-test: 303 checks passed

$ scripts/qa/vision-interaction-test.sh --existing
PASS 181909 vision interaction checks
PASS 109 vision bridge forwarding checks (0 failed)
  ... 27 existing Quest host tests, all PASS/SKIP (SKIP only the two that need the real OpenXR SDK)
existing Quest host tests: 27 passed, 0 failed (of 27 run)
```

Both commands, and their exact output, were run this session.

**Simulator run**: built and ran in a dedicated `GXR-c2-presentation` simulator device
(`xcrun simctl launch --console-pty ... -autoImmersive -fakeEngineScript -fakeEngineScriptLoading 6
-fakeEngineScriptMenu 6 -fakeEngineScriptTabletop 14 -fakeEngineScriptGround 14`), screenshots of all four phases
captured (`xcrun simctl io <udid> screenshot`) and looked at (the `Read` tool on each PNG, not just file existence):
`loading` and `menu` show the upright UI-panel test pattern with no stereo board (as designed); `tabletop` shows the
stereo board with the synthesized box-select rectangle (tint + outline), cursor and waypoint marker all visible;
`ground-view` shows solid black behind the launcher window and UI panel (no passthrough). The engine-host log for
the same run: `loading burst: 15 nested frames presented over 1.5 s (0 lost slot)` (section 6's nested-present
protocol) and `entering phase 'loading'/'menu'/'tabletop'/'ground-view'` for every transition, confirming the cycle
runs as scripted. Two real bugs were found and fixed by this same process (not merely "ran without crashing"):
the feedback markers were being almost entirely depth-culled against the board's flat composited picture (section
5), and `GXHostFrameOutput.groundView` was never wired to the opaque-black clear it was documented to drive
(section 7) — both fixed and re-verified with fresh screenshots before being called done here. This document
records what the doc author observed directly; the package's structured report is the source of truth for exactly
which claims are backed by evidence from this session (`verifiedWithEvidence`) versus not (`notVerified`).

## 11. What is unverified

* Anything that needs a **booted real engine** (loading a real map's loading screen through
  `GX_XR_PresentLoadingFrame`, real spatial audio during Ground View, real text entry through a chat/save/lobby
  gadget, the real ControlBar readability judged by eye rather than by the arithmetic in section 4): this machine
  has no Generals/Zero Hour game data and no attached headset, so none of it can be exercised for real. The pure
  logic and the wiring that will carry it are host-tested and/or type-checked (sections 6, 7, 8) but the real
  end-to-end path is not.
* `uiResolution` (section 9) is read once at engine boot (`GXEngineHostConfig::renderWidth/Height`); changing it
  after boot updates `GXGraphicsSettings` (so `GXEngineHostStatus.graphicsApplied` advances and package F's
  Settings window can show the new value) but does not resize the running engine's backbuffer. A future engine-side
  hook to resize `TheGameEngine`'s render target live is the next concrete step if this matters before a real boot
  is possible to test against.
* `TextInputBridge.swift`'s one-line hookup into `LauncherView.swift` (section 8) is documented but not applied
  (out of this package's file ownership); until it is, the sheet never appears even though the underlying bridge
  works.
