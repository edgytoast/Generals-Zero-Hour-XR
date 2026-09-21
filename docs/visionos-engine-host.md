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
   3. failure: any error (bad paths, `init()` throwing, ...) sets phase `FAILED`, `lastError` (the `XrGameBoot_LastError()` text) and the log;
      the launcher shows the reason, the last log lines and the log path. The compositor shows the same on the indicator.
4. Phase `RUNNING`: the frame loop starts. It only produces frames while a fresh head snapshot (< 0.5 s old) exists, i.e. while
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

See the results table in section 15 (filled from the runs in this session). Nothing in this document was verified on a physical Vision Pro.

## 13. Known limits

* **Presentation is minimal.** `VisionPresentation::update()` shows the stereo world when the engine reports an interactive skirmish / campaign, the engine UI
  texture on one tilted panel, and the composed frame on one upright panel otherwise. The layout persistence, workspace arrangement, Commands console, result
  card, Ground View veil, UI crop and world/UI split logic of the Quest host are package C2's job and replace the body of `update()`.
* **Engine crashes end the process.** `ReleaseCrash` (`Debug.cpp`) ends with `_exit(1)`; a `RELEASE_CRASH` during engine init or a frame therefore terminates the app
  (the reason is in the log: `[GX-RELEASECRASH]`). Exceptions thrown out of `init()` and the frame are caught and reported gracefully. A hook in
  `ReleaseCrash` / `ReleaseCrashLocalized` under `GX_XR_HOST` (call a host function that unwinds to the boot / frame `catch`) would make these graceful too;
  that file is not owned by this package (see the report).
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
