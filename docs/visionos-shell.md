# visionOS shell (Apple Vision Pro)

Native visionOS app shell for the Generals: Zero Hour XR tabletop port. It renders stereoscopically through
Compositor Services (Metal) with gaze + pinch input. The engine keeps its native GLES 3 D3D8 backend
(`Core/Libraries/Source/d3d8gles`) and runs it on **ANGLE's Metal backend**; the shell owns the ANGLE context,
the render targets the engine draws into, the GPU-GPU synchronisation and the Metal composite that hands the
result to Compositor Services. Until the engine is attached, two temporary scenes validate the paths: a direct
Metal test tabletop and a GLES 3.0 test tabletop that goes through ANGLE exactly the way the engine will.

Sources live in `visionos/`. Build and run scripts live in `scripts/build/visionos/`.

## Build

Requires Xcode with the visionOS SDK, `xcodegen` (`brew install xcodegen`) and a built ANGLE.

ANGLE (GLES 3.0 on Metal) is built once, outside the repo, by `scripts/build/visionos/build-angle.sh`:

```sh
scripts/build/visionos/build-angle.sh all        # or: simulator | device   (about 10-15 min per slice)
```

It installs `xrsimulator/` and `xros/` slices (`include/` + `lib/{libANGLE.a,libtranslator.a,libANGLE-shared.dylib}`)
under `<DEPS_ROOT>/angle/install`. The shell finds it through **`GX_ANGLE_ROOT`** (default: the install root of
`build-angle.sh`, `/Users/jvadala/CandC/deps/angle/install`; override with the environment variable, or as a
user-defined build setting in the Xcode GUI). If ANGLE is missing `build-shell.sh` stops with a message that names
the missing file and the `build-angle.sh` command to run.

```sh
# visionOS simulator build (no signing)
scripts/build/visionos/build-shell.sh simulator --derived-data /tmp/GeneralsZHXR-DD

# Unsigned device compile check (cannot be installed on a headset)
scripts/build/visionos/build-shell.sh device --derived-data /tmp/GeneralsZHXR-DD-device

# ANGLE from somewhere else / as a signed, embedded dylib instead of static libs
GX_ANGLE_ROOT=/path/to/angle/install GX_ANGLE_LINK=shared scripts/build/visionos/build-shell.sh simulator
```

Both print the built `.app` path on the last line. The generated `visionos/GeneralsZHXR.xcodeproj` is git-ignored.
For a signed device build open that project in Xcode and select your own team. Do not commit team IDs.

How ANGLE gets into the app (`visionos/project.yml`):

* `GX_ANGLE_LINK=static` (default): `libANGLE.a` + `libtranslator.a` (plus `-lz` and the QuartzCore, CoreGraphics,
  IOSurface, Foundation frameworks) are linked into the executable. Nothing to embed, rpath or sign; the standard
  `egl*`/`gl*` symbols are exported by the executable and resolved through `eglGetProcAddress`.
* `GX_ANGLE_LINK=shared`: `libANGLE-shared.dylib` (install name `@rpath/libANGLE-shared.dylib`) is linked with
  `-lANGLE-shared`, copied into `GeneralsZHXR.app/Frameworks/` by a post-build script phase and re-signed with the
  app's identity (ad-hoc on the simulator; signing is skipped for `CODE_SIGNING_ALLOWED=NO` device builds).
  `LD_RUNPATH_SEARCH_PATHS` is `@executable_path/Frameworks`.
* A pre-build script phase fails the build with the `build-angle.sh` hint when the selected ANGLE artifacts are missing
  (this also covers building from the Xcode GUI).
* `ARCHS = arm64`: ANGLE is arm64 only, so there is no x86_64 simulator slice.

## Run in the simulator

```sh
scripts/build/visionos/run-shell-simulator.sh --udid <YOUR-SIM-UDID> --derived-data /tmp/GeneralsZHXR-DD \
    --out-dir /tmp/shots --wait 20 --shots 2 -- -angleTestScene
```

Boots the Vision Pro simulator (`--udid` or `GXX_VISIONOS_SIM_UDID`; use your own device, not a shared one),
installs the app, launches it with `-autoImmersive` plus the arguments after `--`, waits, and saves screenshots.
App console output (frame-loop and timing lines) goes to `<out-dir>/app-console.log`. `--soak SECONDS` keeps the app
running and samples RSS / physical footprint / frame counter into `<out-dir>/soak.csv`.

Launch arguments:

| Argument | Effect |
| --- | --- |
| `-autoImmersive` | open the immersive space on launch |
| `-layout layered\|shared\|dedicated` | force the compositor texture layout (the simulator offers shared and dedicated) |
| `-externalEyeTextures` | direct-Metal scene rendered into offscreen per-eye textures, submitted through `XRPresentation_SubmitEyeTexture` |
| `-angleTestScene` | GLES 3.0 test scene through ANGLE into the host target ring (this document, "ANGLE path") |
| `-angleEyeScale <f>` | ring eye targets = drawable viewport x f (default 1.0; the simulator viewport is 3840x2160) |
| `-angleAtlas` | pack both eyes side by side into one STEREO_LEFT target (contract `atlas`/`eyeRect`) |
| `-angleSync glfinish` | force the CPU `glFinish` fallback instead of MTLSharedEvent fences |
| `-angleTargetFormat rgba\|bgra` | preferred ring pixel format (default RGBA8Unorm; falls back to BGRA8 if ANGLE rejects it) |
| `-angleNoUIPanel` | skip the 1280x720 UI panel test |
| `-cycleImmersive N` `-cycleHold S` | close and re-open the immersive space N times, holding it open S seconds each round (needs `ImmersiveCycleDriver`, see below) |

## Rendering paths

```
                        render thread (GXXR.Compositor, one per immersive-space lifetime)
 default        Metal test scene ------------------------------> compositor drawable
 -external...   Metal test scene -> offscreen eye texture ------> XRPresentation_SubmitEyeTexture -> composite
 -angleTestScene / engine
                ANGLE (GLES3) -> ring slot (MTLTextures, imported as GL textures)
                          | glFlush + MTLSharedEvent signal        (GPU-GPU, no CPU stall)
                          v
                Metal composite (waits on the event) -> compositor drawable (+ world-anchored layers)
                          | MTLSharedEvent signal "slot free"
                          v
                next use of the slot (CPU waits only if the compositor is > 2 frames behind)
```

### Host module `visionos/ANGLE/`

* `GXXRANGLEContext`: process-wide `EGLDisplay` (`EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE`) and surfaceless ES 3.0
  context. Created once per process (the engine's singletons are not restart-safe, so its GL objects must outlive the
  immersive space); each render loop makes it current on its thread and releases it on exit. Exposes
  `eglGetProcAddress` as the resolver for the engine, ANGLE's `MTLDevice`, the GL renderer string and whether
  `EGL_ANGLE_metal_shared_event_sync` / `EGL_ANGLE_metal_texture_client_buffer` exist.
* `GXXRTargetRing`: 3 slots. Each slot has one `MTLTexture` per named target of the d3d8gles contract
  (`D3D8GLES_XRT_STEREO_LEFT/RIGHT` at eye size, `GAME/WORLD/UI` at independent sizes), created from **ANGLE's**
  device (`RenderTarget | ShaderRead`, private storage, RGBA8Unorm with BGRA8Unorm fallback), imported once per slot
  into a GL texture name (`eglCreateImageKHR(EGL_METAL_TEXTURE_ANGLE)` + `glEGLImageTargetTexture2DOES`) and checked
  framebuffer-complete. `fillTargets:` produces a `struct D3D8GLES_XRTargets` for the current slot. Sizes are declared
  with `setSizeWidth:height:forTarget:` / `configureStereoEyeWidth:...`; a change drains the ring and re-creates the
  affected targets (this is how a drawable resize is handled). `teardown` deletes every GL name/sync and drops the
  textures.
* `GXXRGLTestScene`: the GLSL ES 3.00 test renderer (see "Test scenes").
* `GXXRHostFrameClient.h` + `Bridge/GXXRBridgeHost.h`: the protocol an engine bridge implements and
  `GXXRBridgeSetHostFrameClient()` to install it (see "Engine attach guide").
* `Platform/GXXRD3D8GLES.h`: identical copy of the d3d8gles host-targets contract
  (`D3D8GLES_XRT_*`, `D3D8GLES_XRHostTarget`, `D3D8GLES_XRTargets`, `d3d8gles_SetXRHostTargets`, the
  `getProcAddress`/`flags` config additions). It must stay in sync with `d3d8gles.h`.

### ANGLE device versus the compositor device

`GXXRANGLEContext checkDeviceMatchesCompositorDevice:` compares `cp_layer_renderer_get_device()` with the device ANGLE
reports through `EGL_ANGLE_device_metal`. ANGLE's `DisplayMtl` only does device selection on macOS
(`TARGET_OS_OSX`); everywhere else it calls `MTLCreateSystemDefaultDevice()`, and Compositor Services hands out the
system device, so on visionOS the two are the same object (there is one GPU). The result is logged at every loop start
and exposed as `GXXRBridgeStatus.devicesMatch`; see "Measured results". If they ever differ the ring textures could not
be sampled by the compositor queue, so the check is a loud log line, not a silent assumption.

**Confirmed by measurement (simulator, ANGLE 2.1.28778):** the two devices are the same object. The check is done
by registry ID (`0x10000055b`), not by pointer, because `EGL_ANGLE_device_metal` returns the id ANGLE itself holds.
The textures the ring creates from ANGLE's device were sampled successfully by the compositor command buffer in every
run. On a physical Vision Pro the same code path applies (`MTLCreateSystemDefaultDevice`, one GPU) but this has not
been observed there.

### GPU-GPU synchronisation

ANGLE renders on its own `MTLCommandQueue`; the composite runs on the compositor queue. Per frame:

1. `ring beginFrame`: pick the next slot, CPU-wait (normally 0 ms) until the *release* event reached the value the
   previous composite of this slot will signal.
2. GL rendering into the slot.
3. `ring endGLWork`: `eglCreateSync(EGL_SYNC_METAL_SHARED_EVENT_ANGLE, glEvent, n)` followed by `glFlush()`. ANGLE
   commits its command buffer and signals `glEvent` with value `n` on the GPU when the work completes.
4. `ring encodeWaitForGLInto:cb` puts `encodeWaitForEvent(glEvent, n)` at the start of the compositor command buffer.
5. Composite passes sample the slot's textures.
6. `ring encodeReleaseInto:cb` appends `encodeSignalEvent(releaseEvent, m)`.

**Mechanism actually used: `EGL_ANGLE_metal_shared_event_sync` (present in this ANGLE build).** The `glFinish`
fallback exists and is selectable with `-angleSync glfinish`, but it costs about 4.4 - 5.3 ms of CPU per frame in the
simulator against 0.07 ms for the shared event, and it is only taken automatically when the extension is missing.

If the extension is missing (or `-angleSync glfinish`) step 3 becomes `glFinish()`, step 4 is a no-op, and the log says
`glFinish (fallback...)`; the mode in use is printed at ring creation and in every timing line.

**Never `glReadPixels` from a ring texture or any wrapped private texture**: ANGLE turns it into
`-[MTLTexture getBytes:]`, which is illegal for private storage and crashes the simulator's Metal host
(`SimMetalHost`), killing every Metal client in that simulator. Read back with a Metal blit into a shared buffer.

### Composite pass

`XRPresentation_SubmitEyeTexture` textures are composited by a fullscreen pass per view:

* **V orientation**: GL targets are bottom-up (ANGLE-Metal keeps GL window y = 0 at texel row 0). Submit with
  `XR_SUBMIT_FLIP_Y`; the shader samples `v' = 1 - v`.
* **Premultiplied alpha**: the engine's targets are cleared to (0,0,0,0) and hold coverage-premultiplied colour;
  submit with `XR_SUBMIT_PREMULTIPLIED_ALPHA`. Pixels with alpha < 1e-4 are discarded so passthrough shows and depth
  stays 0 (far).
* **Gamma**: the D3D8 pipeline writes gamma-encoded (sRGB) values and blends in gamma space into non-sRGB RGBA8
  targets, while the compositor drawable is `bgra8Unorm_srgb`. The composite therefore decodes sRGB to linear when
  the source pixel format is not an sRGB format (`GXXR_COMPOSITE_SRGB_DECODE`: un-premultiply, decode, re-premultiply).
* **Depth**: the engine shares no depth, so the pass writes one constant reverse-Z depth for every pixel with alpha > 0:
  the tabletop distance projected with the eye's own projection matrix. Alpha-0 pixels keep depth 0 (far).
* **Atlas**: with `-angleAtlas` both eyes live in one target and the pass samples the eye's half (`uvRect`).
* **Layers**: `GXXRCompositeLayer` = named texture + position + orientation + size in metres + flip flag (+ premultiplied
  and gamma flags derived from the texture). `[GXXRMetalRenderer encodeLayers:into:color:...clipFromWorld:]` draws them
  as world-anchored quads after the eye composite with premultiplied blending and reverse-Z depth test/write. This is
  how the engine's UI / world / game textures are presented in space.

### Test scenes

`-angleTestScene` (`GXXRGLTestScene`) draws the same board, units, animated orbiter, floor grid and contact shadow as
the direct-Metal scene (both use `Renderer/GXXRTestGeometry.h`), with GLSL ES 3.00 shaders, PER EYE, using each eye's
`clip_from_world` from `XRFrameInfo` (converted from Metal clip z in [0,w] to GL clip z in [-w,w]; reverse-Z with
`GL_GEQUAL`, private D24S8 renderbuffers). Output is gamma-encoded and coverage-premultiplied, like the engine's.

It also renders a **1280x720 UI panel** test pattern (title bar with frame counter, orientation markers in the corners
red/green/blue/yellow, text-like bars, an animated progress bar, four button-like rectangles) into the ring's UI target
and the bridge composites it as a 0.64 x 0.36 m world-anchored quad standing to the right of the board, turned toward the player.

### Instrumentation

Once per second the console gets a line like

```
[GXXR] timing (CPU ms/frame over 59 frames): glSubmit=... syncWait=... composite=... total=... | angle=1 sync=metal-shared-event ring=...MB | ANGLE (Apple, ANGLE Metal Renderer: ...)
```

`glSubmit` = time spent inside the frame client issuing GL plus `glFlush`; `syncWait` = CPU time waiting for a ring slot
(or in `glFinish` fallback); `composite` = encoding the Metal composite passes; `total` = whole per-drawable submission.
These are CPU times; GPU time is not measured. The same numbers are in `GXXRBridgeStatus` (`glSubmitMs`, `syncWaitMs`,
`compositeMs`, `frameMs`, `renderer`, `syncMode`, `devicesMatch`, `loopGeneration`, `releaseTimeouts`).

### Lifecycle

* One render loop thread per immersive-space lifetime; a global mutex makes a re-opened space's loop wait until the
  previous loop has torn down its ring/scene and released the ANGLE context. The context itself is never destroyed.
* A frame from `cp_layer_renderer_query_next_frame` can become invalid at any moment (the space closes, the layer
  pauses). If `cp_frame_query_drawables` returns no drawables the loop must NOT call `cp_frame_end_submission`; doing so
  aborts the process. The loop drops the frame and re-reads the layer state.
* While the layer is paused the loop does no GL and no Metal work (it polls the layer state).
* On layer invalidation the loop drains the ring (waits for the last composite), lets the client detach
  (`hostWillDetach`), deletes the ring's GL textures/syncs and test-scene GL objects with the context current, releases
  the context, stops ARKit and exits.
* `-cycleImmersive N` exercises this: `visionos/Renderer/ImmersiveCycleDriver.swift` is a SwiftUI `ViewModifier`
  that closes and re-opens the space N times. It has to be applied once in a SwiftUI view that owns the environment
  actions: add `.modifier(ImmersiveCycleDriver())` to `LauncherView`'s body (that file belongs to the launcher/app
  package, so it is not wired in this package). It is inert without the launch argument.

## Engine attach guide

What the engine bridge (package C) must do, in order. Names are the ones in `visionos/ANGLE/` and
`visionos/Platform/GXXRD3D8GLES.h`.

**Once, before the immersive space opens** (any thread):

1. Implement `id<GXXRHostFrameClient>` and call `GXXRBridgeSetHostFrameClient(client)`
   (`visionos/Bridge/GXXRBridgeHost.h`). From then on the bridge runs the ANGLE path (context, ring, sync,
   composite) whether or not `-angleTestScene` is given.

**Every time a render loop starts** (`-hostDidAttachWithContext:ring:`; first loop and every re-opened space; the ANGLE
context is current, the engine survives between calls):

2. Resolver and config, first time only: `D3D8GLES_XRConfig cfg = {}; cfg.eglDisplay = ctx.eglDisplay;
   cfg.eglContext = ctx.eglContext; cfg.getProcAddress = ctx.getProcAddress; cfg.flags = D3D8GLES_XRFLAG_NO_MULTIVIEW
   (| D3D8GLES_XRFLAG_FORCE_ATLAS when `ring.atlas`)`, then the existing `d3d8gles_SetXRConfig(&cfg)`. Boot the engine
   once per process; on later attaches only remember the new `ring`.
3. Declare the non-stereo targets: `[ring setSizeWidth:1280 height:720 forTarget:D3D8GLES_XRT_GAME]` (and `WORLD`,
   `UI`); 0 x 0 disables a target. The stereo pair is sized by the bridge from the eye viewports each frame.
   Set `ring.atlas = YES` before the first frame to use one atlas target.

**Every frame** (`-renderFrame:`, on the render thread; the bridge has already made the context current and called
`[ring beginFrame]`, i.e. the slot is acquired and safe to write):

4. `struct D3D8GLES_XRTargets t; [ring fillTargets:&t]; d3d8gles_SetXRHostTargets(&t);`
5. Run the engine frame (`XrGameBoot_Frame` equivalent) with the per-eye matrices from `frame->eyes[i]`
   (`world_from_view`, `fov`, `pose`, `clip_from_view` is reverse-Z Metal convention: an engine that wants forward-Z GL
   builds its own from `fov` and `depth_near_m` / `depth_far_m`).
6. `d3d8gles_InvalidateCachedState()` after any host GL use (the ring's lazy allocation runs inside `beginFrame`,
   before step 4, and counts as host GL use only for the previous frame's cached state; call it once at the top of
   the frame as well if the engine caches GL state across frames).
7. Submit the eyes: for each eye, `XREyeSubmit s = {}; s.eye_index = i; s.texture = (__bridge void*)[ring
   textureForTarget:(atlas ? D3D8GLES_XRT_STEREO_LEFT : i == 0 ? D3D8GLES_XRT_STEREO_LEFT : D3D8GLES_XRT_STEREO_RIGHT)];
   s.width/height = eye size; s.flags = XR_SUBMIT_PREMULTIPLIED_ALPHA | XR_SUBMIT_FLIP_Y; s.render_pose = eye.pose;
   s.render_fov = eye.fov; XRPresentation_SubmitEyeTexture(&s);` and return `XR_FRAME_SUBMITTED_TEXTURES`.
8. Optionally implement `-compositeLayersForFrame:` returning `GXXRCompositeLayer`s (for example
   `[GXXRCompositeLayer layerWithName:@"ui" texture:[ring textureForTarget:D3D8GLES_XRT_UI] position:... orientation:...
   sizeMeters:... flipY:YES]`, positioned from `XRPresentation_GetTabletopPlacement`).

**Done by the bridge after `renderFrame:` returns (the client must not do these)**:

9. `[ring endGLWork]` (glFlush + shared-event signal, or `glFinish` fallback), `[ring encodeWaitForGLInto:cb]`, the
   Metal composite of the submitted textures and layers, `[ring encodeReleaseInto:cb]`, commit.

**When the loop ends** (`-hostWillDetach`, context still current): drop everything that references the ring's GL
textures (the engine's FBOs that had them attached must be re-created or re-attached at the next attach; the engine
gets fresh GL names from `fillTargets:` on the next loop). Do not shut the engine down.

Notes for the engine side that the test scene already exercises:

* The GL names in `D3D8GLES_XRTargets` **rotate** through three sets (one per ring slot), so the backend has to
  re-attach the texture to its framebuffer object every frame (or when the name differs from the cached one). Caching an
  FBO with the previous frame's attachment would render into a texture the compositor is still reading.
* Loop over `frame->eye_count` eyes: the simulator gives one view, a headset two (`dedicated` or `shared` layout). With
  `ring.atlas` both eyes go into the one STEREO_LEFT target at `eyeRect[e]` and the bridge samples that rectangle.
* Use `-angleTestScene` (or `GXXRBridgeSetHostFrameClient(nil)`) to check the host without the engine: if the test scene
  is right and the engine picture is not, the fault is on the engine side.
* Simulator numbers to compare against: 60 fps, about 0.3 ms of host CPU per frame in total (see "Measured results").

Rules: the GL names in `D3D8GLES_XRTargets` are valid for that frame only; never `glReadPixels` from them; never call
GL on any thread but the render thread; only the render thread may make the ANGLE context current.

## Engine integration seam (Platform headers)

The engine targets the C headers in `visionos/Platform/` (no Apple types):

| Header | Purpose |
| --- | --- |
| `XRPresentation.h` | frame callback, per-eye pose/view/projection/fov/viewport, head pose, submit-eye-texture, alpha mode, recenter, tabletop placement |
| `XRInteraction.h` | pinch/drag/two-hand event stream in world and board space |
| `PlatformFilesystem.h` | app data dir, game data dir, security-scoped access |
| `PlatformLifecycle.h` | pause / resume / suspend / memory / terminate |
| `GXXRD3D8GLES.h` | host-side mirror of the d3d8gles host-targets contract |

## Game data

No game data ships with the app. The player copies a legally owned Generals / Zero Hour install into the app's
`Documents/GameData` folder (visible in the Files app because `UIFileSharingEnabled` is set), or a later build will
let them pick a folder (`LSSupportsOpeningDocumentsInPlace`).

## Measured results

All numbers below were measured on the visionOS 26.5 **simulator** (Apple silicon host, Xcode 27.0, Debug build,
`-layout dedicated` unless stated) while the machine was heavily shared with other builds and simulators (load average
well above 500), so absolute CPU times are indicative only and nothing here says anything about a physical Vision Pro.
Raw logs, CSVs and screenshots of these runs are kept by the package owner outside the repository.

| Item | Result |
| --- | --- |
| ANGLE version / renderer | `OpenGL ES 3.0 (ANGLE 2.1.28778 git hash: e3fdc27d77e3)`, `ANGLE (Apple, ANGLE Metal Renderer: Apple xrOS simulator GPU, Version 26.5 (Build 23O470))`, EGL 1.5 |
| Extensions present | `EGL_ANGLE_metal_shared_event_sync`, `EGL_ANGLE_metal_texture_client_buffer`, surfaceless context, `GL_OES_EGL_image` |
| ANGLE `MTLDevice` vs compositor device | the **same object**: both `Apple xrOS simulator GPU`, registry ID `0x10000055b` (logged at every loop start as `SAME DEVICE`) |
| Ring | 3 slots, `RGBA8Unorm` accepted (framebuffer complete), stereo target 3840x2160 x 3 = 94.9 MB, UI target 1280x720 x 3 = 10.5 MB, 105 MB allocated |
| GPU-GPU sync in use | `metal-shared-event` (`EGL_ANGLE_metal_shared_event_sync`); the `glFinish` fallback was also run with `-angleSync glfinish` |
| Frame rate | median 60.0 fps over 359 one-second windows of the soak (min 52.0 under load, max 60.9); direct-Metal scene 60 fps too |
| CPU ms/frame, shared-event sync (mean of 359 windows) | glSubmit 0.184, syncWait 0.073, composite 0.032, total 0.321 |
| CPU ms/frame, `glFinish` fallback | syncWait 4.4 - 5.3, total 4.8 - 5.7 (the CPU waits for every frame; use only as a fallback) |
| CPU ms/frame, direct Metal scene (no ANGLE) | total 0.07 - 0.09 |
| 349 s soak (ANGLE test scene, dedicated) | 21105 frames; `vmmap` physical footprint 43.2 MB at start, 45.3 MB after 23 s and 45.5 MB at the end (flat, +0.2 MB in the last 5 minutes); `ps` RSS fell from 289 MB to 141 MB (the simulator process is swapped and compressed; RSS is not a leak signal here); 0 GL errors, no `error` lines in the console |
| Close / re-open cycles | `-cycleImmersive 6 -cycleHold 12`: 6 rounds plus the final re-open = 7 loop generations, every render loop exited cleanly, every ring reported `ring torn down (0 release timeouts over its life)` and every GL test scene `0 GL errors`; footprint 52.9 MB (generation 1) to 57.8 MB (generation 7), RSS falling. Seven cycles are too few to prove the +5 MB is not slow growth: treat it as "no large leak" |
| Layouts | `dedicated` (default in the simulator soak), `shared` (60 fps, same picture), `shared` + `-angleAtlas` (60 fps, same picture; the simulator only has one view, so the atlas is exercised with one eye rect) |
| Visual check | screenshots read back and looked at: ANGLE scene vs direct-Metal scene are pixel-for-pixel the same board, units, grid and shadow (only the animated orbiter cube differs); the UI panel quad shows the title bar with a live frame counter that matches the app's own frame count, and its red/green/blue/yellow corner markers are at top-left / top-right / bottom-right / bottom-left, i.e. correct orientation and no V flip |

Not measured: GPU time, two-view stereo (the simulator hands out one view), a physical device, foveation (off by design),
the engine itself (not attached yet).

### Defects found and fixed during verification

* **Abort when the immersive space closes.** Compositor Services aborts the process (`BUG IN CLIENT:
  cp_frame_end_submission() failed because the frame is not valid`, SIGABRT on `GXXR.Compositor`) if a frame that has
  no drawables is ended. Closing the space invalidates the layer between the state check and `cp_frame_query_drawables`,
  so the loop must drop such a frame instead of ending it. Fixed in `GXXRBridge.mm` (the loop `continue`s when the
  frame has no drawables and lets the state check at the top of the loop see the invalidation). Before the fix every
  `dismissImmersiveSpace` killed the app; after it six consecutive cycles pass.
* The UI panel test quad was partly outside the simulator's 90 degree view; it now stands 0.74 m to the right of the
  board centre and is fully visible.
