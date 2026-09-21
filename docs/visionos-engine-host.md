# visionOS engine host

How the real Zero Hour engine runs inside the visionOS app: on its **own thread**, with its own ANGLE context, publishing
finished frames that the **compositor thread** presents at display rate. The compositor is never blocked by the engine
(boot takes about a minute; a map load blocks inside one engine frame for many seconds).

Contents: 1 Threads, 2 Files, 3 Boot sequence, 4 Frame sequence, 5 Mailbox and ring protocol, 6 API, 7 Frame pacing and
simulation speed, 8 Pause, resume and the immersive space, 9 Launch arguments and log locations, 10 Fake engine,
11 Running with real game data, 12 Verification (what was run, what only compiles), 13 Known limits, 14 Android equivalence.

## 1. Threads

```
 main thread (SwiftUI)           engine thread "GXXR.Engine"                 compositor thread "GXXR.Compositor"
 ---------------------           ----------------------------                ------------------------------------
 launcher, Start Game            owns the ANGLE EGLContext (current here)    Compositor Services frame loop, display rate
 GXEngineHost_Start  ---------->  attach: context + ring                     ARKit device anchor, XRFrameInfo per frame
 GXEngineHost_Pause              boot (XrGameBoot_InitHost, ~1 min)          publishes the LATEST head/eye snapshot
 GXEngineHost_GetStatus          per frame: head -> ring slot -> GL          composites the LATEST COMPLETED engine frame
 spatial events -> GXXRInput ---> VisionFrameDriver::step drains them        (Metal only; never touches GL)
                                 XrGameBoot_Frame (may block seconds)         loading indicator until a frame exists
                                 publish (slot, head snapshot, anchor)
```

Rules that follow (all enforced by construction, see section 5):

* Only the engine thread makes the ANGLE context current, runs engine code and issues GL. The compositor thread never
  calls GL, never waits for the engine, never runs engine code.
* The compositor thread keeps presenting the last published frame while the engine boots or stalls. The frame carries the
  ARKit device anchor it was rendered with; the drawable's `deviceAnchor` is set to exactly that anchor, so the system
  reprojects the (possibly old) picture to the present head pose.
* The context, the ring and the engine survive the immersive space closing and re-opening: only the compositor loop restarts.

## 2. Files

| File | Role |
| --- | --- |
| `GeneralsMD/Code/Main/visionos/GXEngineHost.h` | Swift-importable C API: config, start, status, pause, post, log |
| `GeneralsMD/Code/Main/visionos/GXEngineHostServices.h` | C seam between engine thread and app: services (context, ring, mailbox), client vtable, frame request/output structs |
| `GeneralsMD/Code/Main/visionos/GXEngineHost.mm` | the engine thread loop, pause/park, posted work, status, log tail (no engine headers) |
| `GeneralsMD/Code/Main/visionos/GXEngineHostEngine.{h,cpp}` | the real-engine client (pure C++): boot, describe, frame, pause, shutdown, logic-rate sample |
| `GeneralsMD/Code/Main/visionos/VisionPresentation.{h,cpp}` | `VisionPresentation::update()`: what to show and where (minimal; the seam for the Quest presentation state machine) |
| `GeneralsMD/Code/Main/XrGameBoot.{h,cpp}` | host-neutral engine boot (`GX_XR_HOST`); `XrGameBoot_InitHost`; frame policy, host pause, log sink |
| `visionos/Bridge/GXXREngineSession.{h,mm}` | app side of the services: ANGLE context, ring, mailboxes; installs itself from a constructor |
| `visionos/ANGLE/GXXRFrameMailbox.{h,mm}` | head mailbox (compositor -> engine) and frame mailbox (engine -> compositor) |
| `visionos/ANGLE/GXXRTargetRing.{h,mm}` | reference-counted ring of Metal textures imported into GL |
| `visionos/ANGLE/GXXRFakeEngine.{h,mm}`, `GXXRGLTestScene.{h,mm}` | the fake engine (section 10) |
| `visionos/Bridge/GXXRBridge.mm` | compositor loop: publish head, composite latest frame, loading indicator |
| `visionos/Renderer/GXXRStatusPanel.{h,mm}` | the Metal loading indicator |
| `visionos/App/AppModel+Engine.swift`, `LauncherView.swift` | Start Game, engine status in the launcher |

## 3. Boot sequence

1. Launch: a constructor in `GXXREngineSession.mm` installs the services into GXEngineHost (no Swift call needed).
   `AppModel.startEngineInfrastructure()` calls `GXEngineHost_BeginLogging(<Application Support>/GeneralsX/generals-xr-stderr.log)`:
   stderr is teed into the file (the previous log becomes `generals-xr-stderr-prev.log`).
2. The player validates or imports game data (`visionos/GameData`). **Start Game** (or `-autoStartEngine`) calls
   `GXEngineHost_Start(&cfg)` with the paths from `GXXRGameData_GetPaths`. It returns immediately.
3. The engine thread starts and:
   1. `attach`: creates the ANGLE display and surfaceless ES 3.0 context, makes it current **on this thread**, creates the target ring
      (4 slots, `MTLSharedEvent` sync when `EGL_ANGLE_metal_shared_event_sync` exists, `glFinish` fallback otherwise);
   2. `XrGameBoot_InitHost`: sets `HOME`, `GENERALSX_USERDATA_DIR`, `CNC_GENERALS_ZH_PATH`, `CNC_GENERALS_PATH`, the text-language
      override, `chdir(zhRoot)`, seeds `Options.ini`, then the shared tail (SDL events, critical sections, memory manager, `Version`,
      command line `-xres/-yres`, `GX_XR_OffscreenBoot = true`, `d3d8gles_SetXRConfig` with `eglGetProcAddress` and
      `NO_MULTIVIEW`, `new FramePacer()` + frame policy, `CreateGameEngine()->init()`). The launcher shows "Booting the engine... N s" and the
      last log line; the compositor shows the Metal loading indicator if the tabletop is open;
   3. while the real engine boots a marker file `engine-boot.marker` exists next to the log; it is removed when the boot returns (success or graceful failure).
      If the process ends inside the boot (a fatal engine error calls `_exit`, see section 13) the next launch finds it and shows the previous log's last lines;
   4. failure: any error (bad paths, `init()` throwing, ...) sets phase `FAILED`, `lastError` (the `XrGameBoot_LastError()` text) and the log;
      the launcher shows the reason, the last log lines and the log path. The compositor shows the same on the indicator.
5. Phase `RUNNING`: the frame loop starts. It only produces frames while a fresh head snapshot (< 0.5 s old) exists, i.e. while
   the immersive space is open and running.

The engine is not restart-safe: one start per process. After a failure or a quit, restart the app.

## 4. Frame sequence

Engine thread, per frame (`GXEngineHost.mm` loop + `GXEngineHostEngine.cpp`):

1. run posted work (`GXEngineHost_Post`), honour the pause mask, check the head is fresh;
2. `acquireHead`: newest `XRFrameInfo` (retaining its device anchor);
3. `describe` (real engine: `VisionPresentation::update`): stereo world or flat panel, ring targets and sizes (`GXHostFrameRequest`);
4. `beginFrame`: configure the ring, wait for a free slot, `fillTargets`;
5. `d3d8gles_SetXRHostTargets(targets)`;
6. `VisionFrameDriver::step` (package E): drains `XRInteraction_PollEvent`, applies gestures through the engine bridge, adopts the board,
   fills eye poses and fov into the `XrWorldFrame`; then `XrGameBoot_SetWorldFrame`, `XrGameBoot_SetSplitEnabled`;
7. `XrGameBoot_Frame` (the engine frame; the frame limiter sleeps inside it; a map load blocks inside it);
8. `d3d8gles_InvalidateCachedState()`;
9. `endFrame`: `ring endGLWork` (`eglCreateSync(SHARED_EVENT)` + `glFlush`), publish `(slot, XRFrameInfo used, anchor, layers)`.

Compositor thread, per display frame (`GXXRBridge.mm`):

1. anchor query, `XRFrameInfo` from the drawable's views, board placement;
2. publish the head snapshot to the head mailbox;
3. take the latest completed frame from the frame mailbox (a ring-slot reference is taken for this composite);
4. if there is one: set `drawable.deviceAnchor` to that frame's anchor, encode `waitForEvent(glEvent, value)`, composite the stereo eye textures
   full screen per eye (constant reverse-Z depth at the table distance) and the world-anchored layers with the eye matrices the frame was rendered
   with, and release the slot reference in the command buffer's completed handler;
5. if there is none (boot, failure): the Metal loading indicator; present; commit.

## 5. Mailbox and ring protocol

Two lock-protected hand-offs (`GXXRFrameMailbox`, lock order mailbox then ring):

* **head mailbox** compositor -> engine: latest `XRFrameInfo` + `ar_device_anchor_t` + sequence + timestamp. Latest wins, nothing queues.
  Native handles (drawable textures, command buffer) are cleared in the copy.
* **frame mailbox** engine -> compositor: latest completed frame (slot, `XRFrameInfo` and anchor it was rendered with, stereo flag, layer list,
  focus point for the constant depth).

`GXXRTargetRing` slots are **reference counted**:

| Event | Thread | Reference |
| --- | --- | --- |
| `beginFrame` picks a slot with count 0 | engine | count = 1 (the writer) |
| `publishFrame` | engine | the writer reference is transferred to the mailbox; the previous latest frame's reference is dropped |
| compositor takes the latest frame | compositor | +1 (under the mailbox lock) |
| composite command buffer completes (`addCompletedHandler`) | Metal | -1 |
| `abortFrame` | engine | -1 |

A slot is reusable exactly when its count is 0. A published slot therefore stays in use across as many composites as re-present it and is
released only after the last composite command buffer finished. There is no GPU-side "release" event: the completed handler is the release.
GL side methods (`beginFrame`, `fillTargets`, `endGLWork`, `abortFrame`) run on the engine thread; Metal side methods
(`encodeWaitForGLSlot:into:`, `releaseSlot:afterCommandBuffer:`) on the compositor thread. Ring size 4: latest + previous latest still being composited +
one being written, plus one spare. When no slot is free within 500 ms the engine frame is skipped and counted (`framesSkipped`).
A resize (viewport change after re-opening the space) first drops the mailbox's reference, then drains.

GL to Metal ordering uses one `MTLSharedEvent` with a monotonically increasing value; each slot remembers the value of its last GL work, every composite
waits for it (a wait for a value already reached costs nothing).

## 6. API

`GXEngineHost.h` (Swift): `GXEngineHostConfig` (paths, `logPath`, `renderWidth/Height`, `textLanguage`, `policy`, `logicHz`, `renderFpsCap`, `forceAtlas`),
`GXEngineHost_Start` (non-blocking), `GXEngineHost_GetStatus` (`phase`: idle / booting / running / paused / failed / stopping, `progress`, `lastError`,
`lastLogLine`, `bootSeconds`, `engineFps`, `logicHz`, frame counters, ring slots / in use / MB, sync mode, renderer),
`GXEngineHost_Pause(reason, paused)` (bits `GX_PAUSE_LAYER`, `GX_PAUSE_SCENE`, `GX_PAUSE_USER`), `GXEngineHost_Post(block)` (run on the engine thread before the
next engine frame; used for UI actions that touch engine state), `GXEngineHost_LogPath`, `GXEngineHost_BeginLogging`, `GXEngineHost_ReadLogTail`, `GXEngineHost_IsActive`.

Input: spatial events are queued by `GXXRInput` (mutex, bounded, ordered, balanced) and consumed on the **engine thread** by `VisionFrameDriver::step`
(`XRInteraction_PollEvent`). Nothing else touches engine input state.

`GXEngineHostServices.h` is the seam for other clients: implement `GXEngineClient` (`boot`, `describe`, `frame`, `setPaused`, `shutdown`) and call
`GXEngineHost_StartClient`. The fake engine is such a client.

`XrGameBoot.h` (visionOS additions): `XrGameBootHostConfig`, `XrGameBoot_InitHost`, `XrGameBoot_LastError`, `XrGameBoot_InstallLogSink`,
`XrGameBoot_SetHostPaused`, `XrGameBoot_SampleLogicRate`. Everything else is the unchanged Quest surface (`GX_XRGAMEBOOT_HOST` is defined, so the
interaction bridge `VisionEngineBridgeXr.cpp` is the real forwarder, not the `nullptr` factory).

## 7. Frame pacing and simulation speed (risk R1)

The engine advances one 30 Hz logic step per rendered frame unless a logic time scale is active. On the Quest nothing enables one (the host has
no frame limiter), so game speed follows the render rate. On visionOS, right after `new FramePacer()`:

```
enableLogicTimeScale(TRUE); setLogicTimeScaleFps(30);            // simulation at 30 Hz whatever the host frame rate
enableFramesPerSecondLimit(TRUE); setFramesPerSecondLimit(45);   // render cap, host configurable (renderFpsCap)
```

`GameEngine::canUpdateRegularGameLogic` then takes the accumulator branch (30 < 45): one logic step per 1/30 s of real time, skipped on the frames in
between, which still redraw. The limiter sleeps inside `executeSingleFrame` on the engine thread, which is fine now (it used to stall the compositor loop).
`GameEngine::init` re-reads the FPS limit from the options, so the policy is applied again after `init()`, and `m_useFpsLimit` is forced on.

**The in-game speed keys** (`CommandXlat.cpp` `changeLogicTimeScale` / `changeMaxRenderFps`) compare against `getFramesPerSecondLimit()` and switch the
logic scale off once it reaches that limit; the options / LOD code can also switch the limiter off. Unguarded, one key press could leave
"logic = render rate, no limiter" (the R1 bug). `xrFramePolicyGuard()` runs before every engine frame and keeps this envelope:

* the render limiter stays on, with a limit in `[logicHz, max(cap, 60)]`;
* with the scale switched off (the key's "as fast as the render rate" state) the game runs at the *limited* render rate, so a speed-up is bounded by
  `max(cap, 60) / 30` (1.5x at the default cap) and never by the display rate;
* every new match (game-mode change or the logic frame counter going backwards) starts from the policy default again (30 Hz, cap), so a speed change never
  leaks into the next match;
* network games keep the network frame rate (`getActualLogicTimeScaleFps` returns it) and are left alone.

Self-check: the engine thread counts logic frames (`TheGameLogic->getFrame()`) against wall-clock time and logs
`effective logic rate: 29.9 Hz at 44.8 engine fps over 10 s (match running; target 30 Hz)` every 10 s; `GXEngineHostStatus.logicHz` carries it.
Windows without an unpaused match are marked "not meaningful". Quest could adopt the same policy (Android keeps its current behaviour); the code is in
`XrGameBoot.cpp` behind `GX_PLATFORM_VISIONOS`.

## 8. Pause, resume and the immersive space

* Layer paused (`cp_layer_renderer_state_paused`), invalidated, immersive space closed (loop exit) -> `GXEngineHost_Pause(GX_PAUSE_LAYER, true)`;
  layer running -> false. SwiftUI scene not active -> `GX_PAUSE_SCENE`.
* While any bit is set the engine thread parks after its current frame and calls the client's `setPaused(true)`. For the real engine
  (`XrGameBoot_SetHostPaused`) that is what `SDL3GameEngine` does on `DID_ENTER_BACKGROUND`: release the pointer / held buttons, `TheMouse->loseFocus()`,
  `TheLookAtTranslator->cancelScrolling()`, `TheAudio->pauseAudio(AudioAffect_All)`; resume: `regainFocus()`, `refreshCursorCapture()`, `resumeAudio()`
  (music only when the game itself is paused). The interaction layer is reset (`kVisionResetFocus`) so a pinch in flight releases the engine balanced.
* The compositor stops presenting engine frames only when the layer is not running; it keeps presenting the last frame otherwise.
* Closing and re-opening the immersive space restarts only the compositor loop (`loopGeneration`): the ANGLE context, the ring, the engine and the latest published frame survive.
  While no compositor runs the engine thread reports `waitingForCompositor` and produces nothing.

## 9. Launch arguments and log locations

| Argument | Effect |
| --- | --- |
| `-autoStartEngine` | start the real engine as soon as game data is ready |
| `-fakeEngine` (alias `-angleTestScene`) | the GLES3 test scene as an engine client (no game data) |
| `-fakeEngineBoot S`, `-fakeEngineStall S`, `-fakeEngineFps N` | fake boot time (default 3), sleep S seconds inside a frame every 10 s, frame cap (45) |
| `-allowNoData` | enable Enter Tabletop without game data |
| `-autoImmersive`, `-cycleImmersive N -cycleHold S`, `-layout ...` | as in `docs/visionos-shell.md` |
| `-engineFpsCap N`, `-engineLogicHz N` | frame policy overrides (0 = default, negative cap = uncapped) |
| `-angleAtlas`, `-angleEyeScale F`, `-angleSync glfinish`, `-angleTargetFormat rgba\|bgra`, `-angleNoUIPanel` | ring options (`GXXRAngleOptions.h`) |
| `-importFrom PATH [-importBaseFrom PATH] [-importInPlace]` | game-data import without the picker (simulator / CI) |

Logs: `<app container>/Library/Application Support/GeneralsX/generals-xr-stderr.log` (previous run: `generals-xr-stderr-prev.log`). Everything on stderr is in
it: `[xr-boot]` (boot), `[engine-host]` (thread, status every 5 s, logic rate every 10 s), `[GXXR]` (compositor, one `pipeline:` line per second with compositor fps,
engine fps, new frames / repeats per second, frame age, ring use), `[GXXR/ring]`, `[GXXR/ANGLE]`, `[fake-engine]`, and the engine's own output including `[GX-RELEASECRASH]`.
The launcher shows the path and the tail after a failure. In the simulator:
`xcrun simctl get_app_container <udid> com.generalsx.zerohour.xr.vision data`. os_log subsystem `com.generalsx.zerohour.xr.vision`
(categories `engine-boot`, `engine-host`, `compositor`).

## 10. Fake engine

`-fakeEngine` runs the GLES3 test scene (board, units, UI panel pattern) as a `GXEngineClient` on the engine thread through the same head mailbox, ring,
publish and compositor path as the real engine. It proves the decoupling without game data: `-fakeEngineStall 5` makes it sleep 5 s inside a frame every 10 s
and the compositor must keep running at display rate (the `pipeline:` line shows `new frames/s=0.0 repeats/s=60.0` and a growing frame age).

## 11. Running with real game data (step by step)

1. Build: `scripts/build/visionos/build-angle.sh all` (once), then `scripts/build/visionos/build-shell.sh simulator` (or `device`, unsigned compile check). The
   script builds the engine libraries first when `build/xcframework/GeneralsZHEngine.xcframework` lacks the slice (a few minutes for the simulator, 15+ for the device)
   or prints the exact commands with `--no-build-engine`.
2. Put your legally owned **Generals and Zero Hour** installs in the app: Files app -> this app's `Documents/GameData`, or "Choose game folder..." in the launcher
   (copies about 2.7 GB once), or in the simulator `-importFrom <folder>`. See `docs/GAME_DATA_SETUP.md`. The Game data box must say Ready.
3. Tap **Start Game** (or launch with `-autoStartEngine`). The Engine box shows "Booting the engine... N s" and the last log line. Boot takes about a minute on a fast
   machine (unmeasured on a device).
4. Tap **Enter Tabletop** at any time (also while booting): the loading indicator shows until the first engine frame; the menu appears on the upright panel above
   the far edge of the board; an interactive skirmish / campaign switches to the stereo tabletop with the UI panel in front.
5. If it fails, the Engine box shows the reason, the last log lines and the log path.

## 12. Verification

All runs on the visionOS 26.5 **simulator** (Apple silicon host, Xcode 27.0, Debug app build, one 3840x2160 view, layout `shared`, ANGLE 2.1.28778 on
"Apple xrOS simulator GPU", `metal-shared-event` sync, ANGLE device == compositor device). Nothing was run on a physical Vision Pro or with real game data.
Scripts: `scripts/qa/vision-engine-host-fake-test.sh`, `vision-engine-host-fixture-test.sh`, `vision-engine-host-policy-test.cpp`,
`vision-android-preprocess-check.sh`.

| Check | Result |
| --- | --- |
| Fake engine, compositor vs engine (`-fakeEngine -fakeEngineBoot 6 -fakeEngineStall 4`, 71 one-second windows) | compositor median **60.0 fps** (min 60.0, max 61.0); engine median **44.9 fps** (cap 45; before deadline pacing the fake engine slept the remainder and reached only 35 fps); per display frame composite CPU 0.05-0.13 ms, total 0.08-0.19 ms; ring 1-2 of 4 slots in use; 0 skipped engine frames; 0 GL errors |
| Stall: fake engine sleeps 4 s inside a frame every 10 s | 12 stalled windows: compositor **60.0 fps in every one**, `new frames/s=0.0 repeats/s=60.0`, frame age grew to 4.0 s, then `[fake-engine] stall over` and 44 new frames/s again. Screenshot taken during a stall: the UI panel counter is frozen while the compositor keeps running |
| Loading indicator | while the fake engine boots (`engine(booting)`) the compositor presents the Metal indicator ("Starting fake engine", progress text) at 60 fps; screenshots read back and looked at |
| Immersive close / re-open while the fake engine runs | `-cycleImmersive 3` and `4` (`-cycleHold 8/10`): 4 and 5 compositor loop generations; the engine thread reported `parked (pause mask 0x1)` on every close and `unparked` on every re-open; the ring was allocated **once** (`target stereoLeft` logged once, no `ring torn down`); engine and context untouched; no crash, no GL errors |
| Soak, 349 s, fake engine with 3 s stalls (`--soak`) | first run: physical footprint 49.3 -> 66.2 MB (+2.9 MB/min): **a leak, found by this soak**: the engine thread ran one autorelease pool for its whole life, so per-frame Objective-C temporaries piled up. Fixed with a per-iteration `@autoreleasepool`. After the fix: 58.6 -> 59.3 MB over 349 s (flat, +0.7 MB, all of it in the first minute), 12,654 engine frames, compositor 60.0 fps in every sample; final build, 340 s via `--soak`: 350 one-second windows, compositor median 60.0 fps (min 55.7), engine median 44.9 fps, 73 stalled windows all at >= 59 fps, footprint 48.3 MB at 5 s, 51.0 MB at 12 s and 51.0-51.2 MB from then on (flat), 0 skipped frames, 0 GL errors |
| Real engine on the synthetic fixtures (`-importFrom .../fixtures/merged -importInPlace -autoStartEngine`) | the engine boots on the engine thread on visionOS: `XrGameBoot_InitHost` runs, 21 fixture archives are mounted (`[gxbig] loaded`), `TheArchiveFileSystem` initialises, `TheWritableGlobalData` starts loading `Data\INI\Default\GameData` and stops: `[INI] ERROR: No files read from directory 'Data\INI\Default\GameData'`, then `[GX-RELEASECRASH] ReleaseCrash reason='Uncaught Exception during initialization.'`. Expected: the fixtures hold fabricated archive headers, not INI data |
| ...how it fails, without the ReleaseCrash hook (the branch as committed) | `ReleaseCrash` ends in `_exit(1)`: the process ends (no hang). The boot marker `engine-boot.marker` stays; the **next launch** shows in the Engine box: "The last start ended when the app closed during the engine boot" and the last log lines (`[INI] ERROR: ...`, `[GX-RELEASECRASH] ReleaseCrash reason=...`) plus the log path (screenshot read back) |
| ...how it fails, with the hook applied (`scripts/qa/vision-engine-host-releasecrash-hook.patch`, uncommitted local build, then reverted) | the process stays alive; `[xr-boot] ERROR: engine init threw std::exception: fatal engine error: Uncaught Exception during initialization.`, `[engine-host] FAILED: ...`, phase FAILED; the compositor indicator (in the tabletop) shows "Engine stopped" + the reason + the log path (screenshot read back); the ring is torn down cleanly |
| Frame policy | `vision-engine-host-policy-test.cpp` (a MODEL of the engine algorithm, not the engine): unpatched 90 Hz display -> 90 Hz logic; policy -> 30.0 Hz at 90/60/45 Hz displays (29.95 at 36); unguarded keys + lost limiter -> 90 Hz again; guarded -> 45 Hz; new match -> 30 Hz. **Not measured on the real engine** (an unpaused match needs game data); the 10 s self-check will report it |
| Quest host tests | `scripts/qa/vision-interaction-test.sh --existing`: 27 passed, 0 failed (the same 27 as before), plus the forwarding bridge test (109 checks) and the interaction scenarios |
| Android equivalence | section 14 |
| Game data host test | `scripts/qa/vision-gamedata-test.sh`: 726 checks passed |
| Builds | `build-shell.sh simulator`: 0 errors. `build-shell.sh device` (unsigned): 0 errors (engine device slice built by the script first: configure + 2 m 18 s build on a quiet machine, total 3 m 18 s including the app). Engine libs verified by `verify-engine.sh`: both slices `VISIONOSSIMULATOR` / `VISIONOS`, minos 2.0, arm64 only; the 12 `GX_XR_*` symbols are now defined by `XrGameBoot.cpp` (`nm`: `GX_XR_OffscreenBoot`, `BeginStereoWorld`, `EndStereoWorld`, `RenderCamera`, `BeginUILayer`, `WorldRequested`, `SplitUIAllowed`, `ShadowCategory`, `UpdateTerrainCoverage`, `PresentLoadingFrame`, `CullSphere`, `PointerRay`). It now reports two new undefined symbols, `XRInteraction_PollEvent` / `XRInteraction_SetBoardTransform`: they are the app's (`visionos/Input/GXXRInput.mm`), referenced by `VisionFrameDriver.h`; its expected-symbol list (a file this package does not own) needs `XRInteraction_` added |

What only compiles (not run): the real engine frame path (`XrGameBoot_Frame`, `VisionFrameDriver::step`, `VisionPresentation::update`, host targets, the world/UI layers of a real game),
the pause path on the real engine (`XrGameBoot_SetHostPaused`), the frame policy guard and the logic-rate self-check, `GXEngineHost_Post`, two-view stereo, the atlas
option with the real engine, the device slice at run time.

## 13. Known limits

* **Presentation is minimal.** `VisionPresentation::update()` shows the stereo world when the engine reports an interactive skirmish / campaign, the engine UI
  texture on one tilted panel, and the composed frame on one upright panel otherwise. The layout persistence, workspace arrangement, Commands console, result
  card, Ground View veil, UI crop and world/UI split logic of the Quest host are package C2's job and replace the body of `update()`.
* **Engine crashes end the process unless the ReleaseCrash hook is applied.** `ReleaseCrash` (`Core/GameEngine/Source/Common/System/Debug.cpp`) ends with `_exit(1)`; a
  `RELEASE_CRASH` during engine init or a frame therefore terminates the app (the reason is in the log: `[GX-RELEASECRASH]`, and the next launch tells the player, see
  section 12). Exceptions thrown out of `init()` and the frame are caught and reported gracefully. `XrGameBoot.cpp` already defines the host half of the hook,
  `GX_XR_OnReleaseCrash(reason)` (throws inside a guarded boot / frame, returns otherwise); the engine half is a 12-line change to `Debug.cpp` under
  `GX_PLATFORM_VISIONOS` (`scripts/qa/vision-engine-host-releasecrash-hook.patch`; Android is untouched). This package does not own that file, so the patch is delivered as a patch,
  not committed. Applied to the engine build it was tested (section 12): the boot failure becomes a `FAILED` phase and the app keeps running.
* Loading screens: the engine's own loading frames are drawn inside the blocked engine frame; the compositor keeps showing the last published frame during a load
  (no progress from the engine during a map load).
* The engine is single-start per process; a quit ends the engine (phase `STOPPING`); restart the app.
* No audio route handling (AVAudioSession interruptions) and no head-pose-driven listener: package/Wave later.
* All numbers are simulator numbers (Apple silicon host, one view 3840x2160). Nothing was measured on a device. Two-view stereo, layered layout and foveation are untested.

## 14. Android equivalence

`scripts/qa/vision-android-preprocess-check.sh` preprocesses `XrGameBoot.cpp` / `XrGameBoot.h` with `-D__ANDROID__` (stub `<jni.h>` / `<android/log.h>`,
no `GX_PLATFORM_VISIONOS`) before (`visionos-port`) and after this package and diffs the output modulo line markers. Header: identical. Source: the only differences are
the boot tail moved into `static bool xrBootTail(const XrBootTail &tail)` (same statements, same order), its parameter struct with defaults
(`width/height = kXrGameWidth/Height`, `getProcAddress = nullptr`, `flags = 0`), `s_xrConfig.getProcAddress/flags` assigned from those defaults (the zero values a static
`D3D8GLES_XRConfig` already had), and `XrGameBoot_Init` ending in `return xrBootTail(tail)`. The order-insensitive diff is 28 lines, all of these. The 27 Quest host tests
(`scripts/qa/vision-interaction-test.sh --existing`) pass 27/27 after the change.
