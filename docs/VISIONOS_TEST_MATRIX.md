# visionOS port: test matrix

Last updated: 2026-09-20 (branch `vp/h-docs`, base commit `039512c`). Toolchain: Xcode 27.0, visionOS SDK 27.0,
app deployment target visionOS 26.0, simulator runtime visionOS 26.5. No physical Apple Vision Pro has been used.

This file is the single source of truth for what works on the visionOS port and how we know. It is deliberately
pessimistic. A feature is not marked WORKING unless someone ran it and the evidence is written down.
Design background is in [VISIONOS_PORT_ARCHITECTURE.md](VISIONOS_PORT_ARCHITECTURE.md); the user-facing status is in
[../README_VISIONOS.md](../README_VISIONOS.md).

## Summary of today's evidence

- The app shell renders a Metal **test** tabletop (checkerboard board, coloured blocks, floor grid) over passthrough in
  the visionOS simulator at about 60 fps. That is the test scene, not the game. Simulator only.
- The ANGLE (OpenGL ES 3.0 on Metal) smoke test passes in the simulator: EGLImage import of Metal texture slices, draw,
  and Metal-side readback.
- 31 of the 40 Quest host-logic tests build and pass on macOS (311,016 assertions, 0 failures). The other nine need an
  Android device or NDK.
- Both shell builds succeed: simulator, and unsigned device SDK (compile only; it cannot be installed on a headset).
- The engine and its dependencies compiled and linked for the simulator in a probe tree with build-system edits that
  are not yet in the repository. The engine has never run on visionOS.
- Every gameplay feature is NOT YET IMPLEMENTED or has never been tested. No game data exists on the development machine.

## Status legend

| Status | Meaning |
| --- | --- |
| WORKING | Someone ran it and observed the intended behaviour. The Evidence column says exactly what, when and where. |
| PARTIAL | Part of the feature runs, or code exists and compiles but has never been exercised end to end. The Evidence column says which part. |
| BROKEN | It was run and fails. The Evidence column says how. |
| NOT YET IMPLEMENTED | No implementation exists on visionOS, or it exists only as a design. A Quest reference is named where the engine logic already exists. |

"Simulator" and "Physical Vision Pro" columns record where the feature has been checked: `verified <date>`,
`not tested`, or `cannot be verified here` with the reason. The simulator renders one 3840 by 2160 view at 60 Hz and has
no hand tracking, plane detection, scene reconstruction, foveation or layered layout, so several features can never
pass there. The Physical Vision Pro column is `not tested (no device)` for every row today.

Evidence format: the command or action, the date (ISO), and where the output is (a repository path, or a command that
reproduces it). Recon logs live outside the repository and are not committed; every row therefore gives a way to
reproduce the result.

## 1. Foundations (not features of the game, but what everything stands on)

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Shell app builds for the simulator | WORKING | `scripts/build/visionos/build-shell.sh simulator --derived-data <dir>`, 2026-09-20; exit 0, 0 errors, 1 warning (`appintentsmetadataprocessor: Metadata extraction skipped, no AppIntents.framework dependency found`). Prints the `.app` path on the last line. | verified 2026-09-20 | n/a (build) |
| Shell app renders the Metal test tabletop over passthrough | WORKING (test scene only) | `scripts/build/visionos/run-shell-simulator.sh --derived-data <dir> --out-dir <dir> --wait 20 --shots 2`, 2026-09-20. Console: `layer configured: layout=shared color=bgra8Unorm_srgb depth=depth32Float foveation=0`, `layer state -> 2 (running)`, `frame loop: fps=60.0 ... views=1`. Screenshots opened and inspected: `docs/media/visionos/shell-tabletop-direct.jpg`. Checkerboard board, four corner posts (red back-left, green back-right, blue front-right, yellow front-left: orientation not mirrored), blocks, orbiting cube, translucent floor grid, correct depth against the launcher window. | verified 2026-09-20 (56 to 61 fps in 1 s windows; more than 6,600 frames in one session; no drops or crashes in the successful runs) | not tested (no device) |
| Engine hand-off path in the shell (`-externalEyeTextures`) | WORKING (test scene only) | Same script with `-- -externalEyeTextures`, 2026-09-20. Each eye is rendered into an offscreen full-size texture and submitted through `XRPresentation_SubmitEyeTexture`; the bridge composites it with a fullscreen pass (flip-Y, premultiply). Image matches the direct path: `docs/media/visionos/shell-tabletop-external-textures.jpg`. | verified 2026-09-20 | not tested |
| Head tracking through ARKit device anchor | WORKING (simulator semantics) | Console line `tabletop at (0.00, -0.45, -1.25); head y=0.00 (head-relative origin), tracked`, 2026-09-20. In the simulator the ARKit origin is at head height, so placement falls back to head-relative. | verified 2026-09-20 | not tested (floor-origin placement branch unexercised) |
| Compositor layer layouts and foveation | PARTIAL | The simulator supports `shared` and `dedicated` (`-layout dedicated` renders identically, board-region mean colour 80,85,70 versus 81,85,70, 2026-09-20). `layered` is silently unavailable there. Foveation is configured off. | verified for shared and dedicated only; layered cannot be verified here | not tested |
| Spatial event stream (pinch, drag, two-hand) | PARTIAL | `visionos/Input/GXXRInput.mm` and `visionos/App/SpatialEventForwarder.swift` compile and are wired to `XRInteraction.h`. Nothing has fed them real gaze or pinch events; the launcher shows "No spatial events yet." (2026-09-20). | not tested (no injection path used; mouse-as-pinch behaviour unverified) | not tested |
| ANGLE builds for xrsimulator and xros | WORKING | `scripts/build/visionos/build-angle.sh all`, 2026-09-20. `nm -gU` shows 115 `egl*` and 828 `gl*` exports in both dylibs; `vtool`: platform 12 (simulator) and 11 (device), minos 2.0, sdk 27.0, arm64; `otool -L` shows only system frameworks; static archive holds 1,726 `rx::mtl::` symbols and no GL/Vulkan backend. Installed under `$DEPS_ROOT/angle/install`. | verified 2026-09-20 | device slice built, never run |
| ANGLE smoke test | WORKING | `xcrun simctl spawn <simulator-udid> $DEPS_ROOT/angle-smoke/smoke-xrsim` (dylib) and `smoke-xrsim-static` (static), 2026-09-20: `SMOKE TEST PASSED (0 failure(s))`. Checks: Metal device, EGL 1.5 on the Metal backend, ES 3.0 context, GLSL ES 3.00 program compiles, a two-slice `MTLTexture` array wrapped per slice by `eglCreateImageKHR(EGL_METAL_TEXTURE_ANGLE)`, cleared and drawn, read back through an independent Metal blit. The smoke source lives outside the repository (`$DEPS_ROOT/angle-smoke/smoke.mm`) and is not yet committed. | verified 2026-09-20 | not tested |
| d3d8gles running on ANGLE inside the app | NOT YET IMPLEMENTED | Host-targets contract and resolver-based GL loader are specified in the architecture document (section 9.6); implementation is in progress on parallel branches and not merged. No engine frame has been rendered through ANGLE. | not tested | not tested |
| Quest host-logic tests on macOS | WORKING (31 of 40) | 2026-09-20. Self-contained tests: `clang++ -std=c++17 -fsanitize=undefined -IGeneralsMD/Code/Main -ICore/Libraries/Source/d3d8gles/include -I<OpenXR-SDK>/include scripts/qa/xr-NAME-test.cpp` for NAME in board diorama endgame height input layers loading math menu menu-routing performance presentation tactics world, and (with one temp-file argument) camera comfort interaction placement; bridge tests `CXX=clang++ bash scripts/qa/xr-NAME-test.sh <dir>`. 31 PASS, 0 FAIL, 311,016 assertions. Not run: `xr-diorama-device`, `xr-mrt-device`, `xr-multiview-device`, `xr-performance-device`, `xr-world-device`, `xr-stereo-state`, `xr-uniform-cache`, `xr-world-copy`, `xr-terrain-device`, `xr-branding` (need an Android device, NDK, or APKs). No vcpkg tree existed, so the OpenXR headers came from a shallow clone of KhronosGroup/OpenXR-SDK. These test the Quest host logic that the port reuses; they are not visionOS tests. | verified on macOS 2026-09-20 | n/a |
| Engine and dependencies compile and link for xrsimulator | PARTIAL | Probe tree with build-system edits (SDL3 static with `SDL_VIDEO=OFF`, WW3D2 freetype gate, GameNetworkingSockets gate, header-only DXVK checkout, openal-soft patch, vcpkg overlay triplet), `ninja z_generals`, 2026-09-20: 730 steps, zero compile errors, links to a 44.5 MB arm64 executable (`vtool`: VISIONOSSIMULATOR, minos 2.0, sdk 27.0). It proves symbol resolution only. The edits are not in the repository yet; the static-library target `z_generals` (`SAGE_BUILD_VISIONOS_LIB`) has not been built. The `xros` slice was verified only for vcpkg leaf ports (zlib, freetype, glm, and others). | verified (link only) 2026-09-20 | not built |
| Game-data folder presence check | PARTIAL | `PlatformFS_GameDataLooksPresent` (`visionos/Bridge/GXXRPlatformFilesystem.mm`) reports whether `Documents/GameData` has any entry; the launcher shows "No game data yet" on an empty folder (2026-09-20 screenshot). It does not validate archives. | verified (empty-folder message only) 2026-09-20 | not tested |

## 2. Engine, data and rendering

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Engine startup | NOT YET IMPLEMENTED | No engine boot exists for visionOS. The Quest boot `XrGameBoot_Init` (`GeneralsMD/Code/Main/XrGameBoot.cpp:220`) is Android-only (JNI, ends with `#error` off Android at line 1588). A non-JNI twin is specified. The engine links in the probe (see Foundations) but has never executed. | not tested | not tested |
| Game-data detection | NOT YET IMPLEMENTED | Only the presence heuristic above exists. The archive validator (20 required `.big` archives, string table, `Weather.ini`) is specified in [GAME_DATA_SETUP.md](GAME_DATA_SETUP.md); the Android `GameDataValidator.java` is the model. No real game data has been available to test against. | not tested | not tested |
| Loading a map | NOT YET IMPLEMENTED | Needs engine startup and game data. | not tested | not tested |
| Rendering a battlefield | NOT YET IMPLEMENTED | Only the Metal test tabletop renders (Foundations). No engine frame has been drawn on visionOS. Quest reference: `docs/media/quest-tabletop/` headset captures, Quest hardware only. | not tested | not tested |
| Stereo rendering | PARTIAL | The shell renders one pass per compositor view with per-view pose, fov and projection (`cp_view_get_transform`, `cp_drawable_compute_projection`) and `first_use_of_target` handling for shared textures. The simulator gives one view, so real two-view stereo has never been seen: it is verified by construction only. Engine stereo (per-eye replay in `d3d8gles`) has not run on ANGLE. | cannot be verified here (one view) | not tested |
| Tabletop transform: move, rotate, scale | PARTIAL | Initial placement (yaw-only from the head forward direction; floor-origin and head-relative branches) and `XRPresentation_Recenter` exist; placement observed in the console log (2026-09-20). Grab-move and two-hand rotate and scale are computed as `TWO_HAND_*` events in `GXXRInput.mm` but are not connected to the board transform and have never received real input. Quest logic reference: `XrSurfaceGrab` (`GeneralsMD/Code/Main/XrPlacement.h`), tested on macOS (`xr-placement` 106 checks). | placement verified 2026-09-20; gestures cannot be verified (no hand tracking) | not tested |

## 3. Selection and commands

The engine's command logic already exists and is shared with the Quest edition (`XrGameBoot_SpatialClick`,
`TouchInput`, the command translators; `GeneralsMD/Code/Main/XrGameBoot.cpp:900-1010`). None of it is connected on
visionOS: there is no engine boot and no path from `XRInteraction` events into it. Quest logic tests on macOS cover the
decision code (`xr-tactics` 2,030 checks, `xr-trigger-bridge` 28, `xr-tactical-bridge` 1,023) but not a running game.

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Unit selection | NOT YET IMPLEMENTED | Look and pinch to select is designed ([visionos-interaction.md](visionos-interaction.md)); no engine hookup. | not tested | not tested |
| Add and remove selection | NOT YET IMPLEMENTED | Quest reference: add/remove modes in `XrGameBoot_SpatialClick`. Needs a modifier gesture on visionOS (design pending). | not tested | not tested |
| Box selection | NOT YET IMPLEMENTED | Pinch-drag to box is designed. Quest reference: `XrTriggerGesture` click versus drag (`XrTactics.h`). | not tested | not tested |
| Move | NOT YET IMPLEMENTED | Quest reference: `xrIssueMovement`. | not tested | not tested |
| Attack | NOT YET IMPLEMENTED | Quest reference: context command through `TouchInput::tap` and `evaluateContextCommand`. | not tested | not tested |
| Attack-move | NOT YET IMPLEMENTED | Quest reference: tactical action 6 (`XrGameBoot_TacticalAction`). | not tested | not tested |
| Guard | NOT YET IMPLEMENTED | Quest reference: `MSG_DO_GUARD_OBJECT` and `MSG_DO_GUARD_POSITION`. | not tested | not tested |
| Stop | NOT YET IMPLEMENTED | Quest reference: `MSG_META_STOP`. | not tested | not tested |
| Waypoints | NOT YET IMPLEMENTED | Quest reference: waypoint mode kept on while plotting. | not tested | not tested |
| Formations | NOT YET IMPLEMENTED | Quest reference: `MSG_META_CREATE_FORMATION`. | not tested | not tested |
| Unit abilities | NOT YET IMPLEMENTED | Uses the engine control bar and armed-command aiming. | not tested | not tested |
| Special and general powers | NOT YET IMPLEMENTED | Uses armed commands and targeting; needs the UI path below. | not tested | not tested |
| Aircraft commands | NOT YET IMPLEMENTED | Quest reference: select-all-aircraft action; airfield and aircraft commands through the control bar. | not tested | not tested |
| Transport load and unload | NOT YET IMPLEMENTED | Context command and control bar. | not tested | not tested |

## 4. Building placement

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Building placement: ghost preview | NOT YET IMPLEMENTED | Engine draws the placement icon; needs boot and aiming from a ray. Quest reference: `TouchInput::beginAiming`. | not tested | not tested |
| Building placement: validation feedback | NOT YET IMPLEMENTED | Engine placement validity is native (`PlaceEventTranslator`); untested on visionOS. | not tested | not tested |
| Building placement: rotate | NOT YET IMPLEMENTED | Quest reference: `XrBuildRotation::update` and `W3DInGameUI::rotateXrPlacement` (`W3DInGameUI.cpp`); stick-driven on Quest, needs a visionOS gesture. | not tested | not tested |
| Building placement: confirm | NOT YET IMPLEMENTED | Quest reference: synthetic left click through `XrGameBoot_Pointer`. | not tested | not tested |
| Building placement: cancel | NOT YET IMPLEMENTED | Quest reference: `XrGameBoot_SpatialClick(true)` and `TouchInput::cancelOrDeselect`. | not tested | not tested |

## 5. UI

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| UI interaction | NOT YET IMPLEMENTED | The Quest edition routes a ray to the engine 2D UI through fake mouse events (`XrGameBoot_Pointer`, `XrGameBoot.cpp:1491`). Not connected on visionOS. | not tested | not tested |
| Context menus | NOT YET IMPLEMENTED | Engine and control-bar behaviour; untested. | not tested | not tested |
| Build menu | NOT YET IMPLEMENTED | Quest shows the engine UI as a detached window texture (`XrGameBoot_UITexture`); a visionOS presentation is not chosen or built. | not tested | not tested |
| Production queue | NOT YET IMPLEMENTED | Engine control bar; untested. | not tested | not tested |
| Radar and minimap | NOT YET IMPLEMENTED | Engine radar uses render-target readback paths that need checking on ANGLE (`readbackRenderTarget`, `gles_pipeline.cpp:3156`). | not tested | not tested |
| UI readability | NOT YET IMPLEMENTED | The 1280 by 720 composed frame and UI texture sizes are the Quest values; legibility at Vision Pro angular resolution is unknown. Legibility can only be judged on a device. | cannot be verified here (resolution and stereo differ) | not tested |

## 6. Camera, modes and lifecycle

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Ground View enter and exit | NOT YET IMPLEMENTED | Quest feature (experimental). Pure state logic tested on macOS (`xr-ground-observer` 64 checks), no visionOS host. Comfort design is open (no vignette, haptics or snap turn exist in the code). | not tested | not tested |
| Pause and resume | PARTIAL | The shell maps `cp_layer_renderer_state` to `XRSessionState` and `PlatformLifecycle_Notify` (`visionos/Bridge/GXXRBridge.mm`, around line 365); the transition to running was observed (`layer state -> 2`, 2026-09-20). Paused and invalidated paths were not exercised. The engine does not react to any lifecycle event yet (no simulation or audio pause on backgrounding exists in the Quest XR flavour either). | running observed 2026-09-20; pause not exercised | not tested |
| Save and load | NOT YET IMPLEMENTED | Engine save uses `fopen` under the user data directory (`Core/GameEngine/Source/Common/System/XferSave.cpp:123`); the directory resolves inside the app container on Apple. Not run. | not tested | not tested |

## 7. Game modes

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Skirmish | NOT YET IMPLEMENTED | Needs everything above. Quest reference: offline skirmish is the supported Quest path. | not tested | not tested |
| Campaign | NOT YET IMPLEMENTED | Needs movie and loading presenter support (Bink/FFmpeg video; nested loading presenter). | not tested | not tested |
| AI | NOT YET IMPLEMENTED | The AI code compiles in the probe; it has never run on visionOS. Whether it behaves identically is untested. | not tested | not tested |
| Multiplayer and LAN | NOT YET IMPLEMENTED | GameNetworkingSockets builds through vcpkg overlay ports in the probe; no session source or local-network permission exists on visionOS. Known upstream problem: Quest to PC LAN games fail simulation synchronisation (`docs/WORKDIR/planning/MULTIPLAYER_STATUS.md`). Out of scope until resolved. | not tested | not tested |

## 8. Audio

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Audio: voices | NOT YET IMPLEMENTED | OpenAL Soft 1.24.2 with the two-line visionOS patch (CoreAudio backend gate, CMake system-name regex) compiles and links for xrsimulator in the probe (2026-09-20). It has never produced sound. FFmpeg is a hard dependency of the OpenAL path and also built. | not tested | not tested |
| Audio: weapons | NOT YET IMPLEMENTED | As above. | not tested | not tested |
| Audio: music | NOT YET IMPLEMENTED | The engine quits if the music archive is missing unless the Android relaxation is widened (`GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp:646-664`). | not tested | not tested |
| Audio: ambient | NOT YET IMPLEMENTED | As above. | not tested | not tested |
| Audio: UI sounds | NOT YET IMPLEMENTED | As above. | not tested | not tested |
| Spatial audio | NOT YET IMPLEMENTED | Design only: feed the head pose into the OpenAL listener (`OpenALAudioManager::setDeviceListenerPosition`) and configure `AVAudioSession`; system head-tracked sound stage behaviour with RemoteIO is unverified. Headphone and speaker behaviour need a device. | cannot be verified here (no spatial rendering) | not tested |

## 9. Performance

Nothing has been measured for the game. The 60 fps figure for the test scene reflects the simulator's 60 Hz display, not
the engine or a device.

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Performance: frame rate | NOT YET IMPLEMENTED | No game frame exists. Reference only: the Quest edition measured 20.7 to 42.0 ms per frame in its reference scenes (`docs/WORKDIR/planning/XR_CURRENT_STATUS.md`), on a different GPU and driver. The Vision Pro target is 90 Hz (11.1 ms) or a reduced cadence. Simulator timing is not representative. | cannot be verified here | not tested |
| Performance: draw calls | NOT YET IMPLEMENTED | Quest backend logs report about 1,200 to 2,500 draws per frame, with up to three submissions per world draw on ANGLE without multiview (estimate from reading, unmeasured). | not tested | not tested |
| Performance: texture memory | NOT YET IMPLEMENTED | DXT textures decode to RGBA8 when S3TC is absent (4x memory for DXT1); the device memory limit and the increased-memory entitlement are unverified. | cannot be verified here | not tested |
| Game speed (logic time scale) | NOT YET IMPLEMENTED | Open risk R1 in the architecture document: the host must enable the 30 Hz logic time scale, else game speed follows display rate. Test: logic-frame counter versus wall time, including after pressing the in-game time-scale key. | not tested | not tested |

## 10. Builds and deployment

| Feature | Status | Evidence | Simulator | Physical Vision Pro |
| --- | --- | --- | --- | --- |
| Build for simulator | WORKING (shell app only) | See Foundations. The engine static library is a separate row below. Reproduce: `scripts/build/visionos/build-shell.sh simulator --derived-data <dir>`. | verified 2026-09-20 | n/a |
| Build for simulator: engine static library | PARTIAL | Probe link only (Foundations). Presets and `z_generals` static target: see [BUILD/VISIONOS.md](BUILD/VISIONOS.md); not verified here. | verified (probe link) 2026-09-20 | n/a |
| Build for device (unsigned) | WORKING (shell app only) | `scripts/build/visionos/build-shell.sh device --derived-data <dir>` (SDK `xros`, `CODE_SIGNING_ALLOWED=NO`), 2026-09-20: exit 0, 0 errors, same 1 warning; output `Debug-xros/GeneralsZHXR.app`: arm64 Mach-O, `UIDeviceFamily` 7, `MinimumOSVersion` 26.0. Compile check only; cannot be installed. | n/a | n/a (compile check) |
| Build for device: engine static library | NOT YET IMPLEMENTED | Not built for the `xros` slice. Only vcpkg leaf ports were built for it. | n/a | not tested |
| Signed device deploy | NOT YET IMPLEMENTED | The procedure is documented (Xcode with your own team, or `xcrun devicectl device install app`) and has never been executed: no Apple Vision Pro is attached and no team is configured. | n/a | not tested (no device) |

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
7. **Template for a new evidence entry:**
   `<command or action>; <YYYY-MM-DD>; <Xcode/SDK/runtime or device+OS>; <result in one sentence>; <path or reproduce command>`.
8. **Do not paste private paths.** Screenshots and logs committed to `docs/media/visionos/` must not show container paths,
   user names or team identifiers; redact them before committing.
