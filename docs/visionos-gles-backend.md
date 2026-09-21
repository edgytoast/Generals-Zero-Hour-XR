# d3d8gles on visionOS (ANGLE-on-Metal)

The Quest build renders through `Core/Libraries/Source/d3d8gles`, a native D3D8 -> GLES 3 backend that
runs on the headset's GLES driver. The Vision Pro has no GLES driver, so the same backend runs on
[ANGLE](https://github.com/google/angle)'s Metal backend, and the finished frame is handed to
Compositor Services as Metal textures. This document is the contract between the backend and the
visionOS host (the SwiftUI shell + Objective-C++ bridge under `visionos/`), and the record of what
changed in shared code to make that work without changing the Quest build.

Scope of this document: the backend and its engine hooks. The host (ANGLE display/context, MTLTexture
ring, Compositor Services) is described in `docs/visionos-shell.md`.

## 1. What runs where

```
Swift/ObjC++ host (render thread)                     engine + d3d8gles (same thread)
------------------------------------                  -----------------------------------------
ANGLE EGL display, surfaceless ES3 context   ----->   d3d8gles_SetXRConfig({getProcAddress = eglGetProcAddress})
ring of MTLTextures on ANGLE's MTLDevice,             Direct3DCreate8_GLES -> CreateDevice -> WebGLPipeline::initContextExternal
  wrapped as GL textures (EGLImage)                    (loads every gl* entry point through the resolver)
per frame: d3d8gles_SetXRHostTargets(slot names) ->   d3d8gles_BeginXRFrame, engine frame (Clear, BeginXRStereo,
                                                       world draws, EndXRStereo, BeginXRUI, UI draws, Present)
glFlush + eglCreateSync(SHARED_EVENT)          <-----  finished frame sits in the host's textures
d3d8gles_InvalidateCachedState()
composite the slot's MTLTextures after the event
```

* The backend never links ANGLE. It needs ANGLE's *headers* (`GX_ANGLE_INCLUDE_DIR`) and a resolver.
* The engine is built as a static library (`z_generals`, `SAGE_BUILD_VISIONOS_LIB`) and the host drives
  frames: no SDL window, no main loop.
* One render thread owns the ANGLE context and runs the engine frame (ANGLE contexts are not thread safe).

## 2. Build

| Item | Value |
| --- | --- |
| Target | `d3d8gles` (static). `Core/Libraries/Source/d3d8gles/CMakeLists.txt` builds it when `ANDROID` or `GX_PLATFORM_VISIONOS` (or `CMAKE_SYSTEM_NAME` is `visionOS`). |
| ANGLE headers | cache var `GX_ANGLE_INCLUDE_DIR` (`<angle install>/<xrsimulator\|xros>/include`), or env `GX_ANGLE_ROOT` (the `install` dir written by `scripts/build/visionos/build-angle.sh`); configure fails with a message if neither is usable. |
| Definitions | `GX_D3D8GLES_BACKEND=1` (PUBLIC, all platforms); `GX_PLATFORM_VISIONOS=1` (PUBLIC, visionOS). |
| Linked libraries | `d3d8lib sdl3lib ${CMAKE_DL_LIBS}` (Android: `d3d8lib sdl3lib log`). SDL3 is needed for `SDL_GetTicks` (perf log) and the windowed GL path that XR mode never enters. |
| GL symbols | Off Android the wrappers in `gles_dispatch.cpp` are named `d3d8gles_gl*` (`src/gles_symbols.h`), so they cannot collide with an ANGLE that exports the standard `gl*` names, and a resolver can never recurse into them. On Android they stay the global `gl*` symbols, unchanged. |

Shared macros: `Core/Libraries/Source/WWVegas/WWLib/gx_backend.h`

* `GX_XR_HOST` = `defined(__ANDROID__) || defined(GX_PLATFORM_VISIONOS)`: the Quest XR engine hooks
  (`GX_XR_*`, `GX_XR_OffscreenBoot`, ...) are compiled in and the host must define them.
* `GX_USES_D3D8GLES` = `defined(__ANDROID__) || defined(GX_D3D8GLES_BACKEND)`: the d3d8gles backend is the
  D3D8 device (`Direct3DCreate8_GLES`, `d3d8gles_*`). `gx_backend.h` derives `GX_D3D8GLES_BACKEND` from
  `GX_PLATFORM_VISIONOS`, so consumers that do not link the target (device layer) agree.

On Android both macros are 1, exactly like the `__ANDROID__` they replace. True Android-only code
(adb log tags, `/sdcard` paths, JNI, Android storage, Mali/Adreno workarounds) stays on `__ANDROID__`.

## 3. Host contract

Everything is in `Core/Libraries/Source/d3d8gles/include/d3d8gles.h`; the visionOS mirror is
`visionos/Platform/GXXRD3D8GLES.h` (same names).

### 3.1 Configuration (before the engine creates its D3D device)

```c
struct D3D8GLES_XRConfig {
    void *eglDisplay;                              // informational
    void *eglContext;                              // informational
    void *(*getProcAddress)(const char *name);     // NEW, appended: eglGetProcAddress of the host's ANGLE display
    unsigned flags;                                // NEW, appended: D3D8GLES_XRFLAG_*
};
enum { D3D8GLES_XRFLAG_NO_MULTIVIEW = 1, D3D8GLES_XRFLAG_FORCE_ATLAS = 2 };
void d3d8gles_SetXRConfig(const D3D8GLES_XRConfig *cfg);   // pointer must outlive device creation
```

* Zero-initialise the struct (`D3D8GLES_XRConfig cfg = {};`); the Quest host's static instance already is.
* With `getProcAddress` set, **every** `gl*` entry point is resolved through it
  (`d3d8gles_LoadGLESDispatchFromResolver`), and multiview is never used (`d3d8gles_ConfigureXRMultiview`
  is ignored). Without it, Android keeps loading system `libGLESv3.so`; any other platform fails
  device creation with a clear message.
* `NO_MULTIVIEW` also disables multiview when there is no resolver (Quest debugging). `FORCE_ATLAS`
  makes *backend-allocated* stereo targets a single 2W x H atlas (one FBO, per-eye viewport + scissor);
  it never overrides host-supplied targets, whose `atlas` field decides.

### 3.2 Host-supplied render targets

```c
enum { D3D8GLES_XRT_STEREO_LEFT=0, D3D8GLES_XRT_STEREO_RIGHT=1, D3D8GLES_XRT_GAME=2,
       D3D8GLES_XRT_WORLD=3, D3D8GLES_XRT_UI=4, D3D8GLES_XRT_COUNT=5 };
struct D3D8GLES_XRHostTarget { unsigned glTexture; int width; int height; };   // glTexture==0: backend allocates
struct D3D8GLES_XRTargets { struct D3D8GLES_XRHostTarget slot[D3D8GLES_XRT_COUNT]; int atlas; int eyeRect[2][4]; };
void d3d8gles_SetXRHostTargets(const D3D8GLES_XRTargets *targets);   // NULL clears
```

| Slot | What is drawn into it | Size rule |
| --- | --- | --- |
| `STEREO_LEFT` / `STEREO_RIGHT` | the per-eye 3D world (`d3d8gles_BeginXRStereo` ... `EndXRStereo`) | separate mode (`atlas == 0`): both slots, identical size, 64..16384; the size is the per-eye render size (it supersedes the width/height passed to `BeginXRStereo`, which are only validated). |
| `STEREO_LEFT` (atlas) | both eyes, side by side or any layout | `atlas != 0`: `slot[LEFT]` is the whole atlas; `eyeRect[e] = {x,y,w,h}` is eye e's viewport in texel coordinates of that texture (y counted from texel row 0), both rects the same size (>= 64) and inside the texture. `slot[RIGHT]` is ignored. |
| `GAME` | the composed frame (the engine's backbuffer redirect) | exactly the engine backbuffer size (the `-xres/-yres` the host booted with). |
| `WORLD` | planar world snapshot for the split (world + detached UI) path | same as GAME |
| `UI` | UI-only layer (second colour attachment of the composed FBO) | same as GAME |

Rules:

* Call it **every frame**, on the render thread with the context current, **before** `d3d8gles_BeginXRFrame`. The
  names may change every frame (ring slots); the backend re-points its FBO attachments (cheap) and never
  reallocates because of a rotation. The GL names must stay valid until the frame is finished. The backend
  never deletes a host name.
* A slot that is 0, mis-sized, or rejected (the texture is not framebuffer-complete as a colour attachment)
  falls back to a backend-allocated texture exactly as on Android, with one log line. The accessors always
  return the name **actually in use**, so compare them with what you passed:
  `d3d8gles_XRStereoTexture(eye)`, `d3d8gles_GetGameTexture()`, `d3d8gles_GetXRWorldTexture()`,
  `d3d8gles_GetXRUITexture()`. `d3d8gles_XRStereoAtlas()` reports the layout in use.
* World/UI names are only meaningful while `d3d8gles_XRSplitReady()` is true; the game texture is 0 in a frame
  whose ordinary world copy was elided (the host then composites stereo + UI).
* Depth/stencil stays backend-private (`GL_DEPTH24_STENCIL8` renderbuffers; ANGLE maps them to
  `Depth32Float_Stencil8`).
* All targets are **GL-native bottom-up**: measured on ANGLE-Metal, MTLTexture row 0 is GL window y = 0
  (the bottom of the picture). The compositor must flip V when sampling. Colour is premultiplied by alpha for
  the world (alpha = coverage over a (0,0,0,0) clear), so composite with `ONE, ONE_MINUS_SRC_ALPHA`.
* Textures must be `RGBA8`/`BGRA8`, usage `RenderTarget | ShaderRead`, created on **ANGLE's** MTLDevice
  (`eglQueryDisplayAttribEXT(EGL_DEVICE_EXT)` then `eglQueryDeviceAttribEXT(EGL_METAL_DEVICE_ANGLE)`);
  wrap with `eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_METAL_TEXTURE_ANGLE, mtl, {EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE, s, EGL_NONE})`
  + `glEGLImageTargetTexture2DOES`. Private storage is fine and expected.

### 3.3 Frame protocol

```
makeCurrent (render thread, once)
per frame:
  d3d8gles_SetXRHostTargets(&targets_for_this_ring_slot)
  d3d8gles_BeginXRFrame(split, elide)         // then the engine frame
  ... engine: Clear, d3d8gles_BeginXRStereo/EndXRStereo, d3d8gles_BeginXRUI, Present ...
  glFlush(); eglCreateSync(EGL_SYNC_METAL_SHARED_EVENT_ANGLE, value)   // GPU->GPU, no CPU stall
  d3d8gles_InvalidateCachedState()            // after ANY host GL use
  composite the slot on the host's Metal queue after waiting for the event
```

Never `glReadPixels` a host-wrapped texture: ANGLE calls `-[MTLTexture getBytes:]`, which is illegal for
Private storage and takes the Metal host down on the simulator. The backend follows the rule itself
(section 4.3); host-side inspection must be a Metal blit into a Shared buffer.

### 3.3.1 Complete list of API additions (everything else in `d3d8gles.h` is unchanged)

| Where | Addition |
| --- | --- |
| `d3d8gles.h` | `D3D8GLES_XRConfig::getProcAddress`, `::flags` (appended); `D3D8GLES_XRFLAG_NO_MULTIVIEW`, `D3D8GLES_XRFLAG_FORCE_ATLAS`; `enum D3D8GLES_XRT_*`; `struct D3D8GLES_XRHostTarget`; `struct D3D8GLES_XRTargets`; `d3d8gles_SetXRHostTargets`; `d3d8gles_ShouldUseVulkanBackend` / `d3d8gles_ShouldUseANGLE` now exist on every platform (constant `false` off Android). |
| `gles_dispatch.h` | `bool d3d8gles_LoadGLESDispatchFromResolver(void *(*getProcAddress)(const char *))`; `glReadBuffer` joined the dispatch table (92 entry points). Off Android the wrappers are `d3d8gles_gl*` (`gles_symbols.h`). |
| `d3d8gles_XRStereoTexture / GetXRWorldTexture / GetXRUITexture / GetGameTexture / XRStereoAtlas` | semantics extended, signatures unchanged: they return the host-supplied names when host targets are in use. |
| environment | `D3D8GLES_DISABLE_S3TC=1` forces the software DXT path on a GPU that does expose S3TC (test switch; ANGLE-Metal on visionOS never has it). |

### 3.4 Symbols the host must define

Widening the engine hooks to `GX_XR_HOST` makes the engine reference the same host entry points the Quest
build gets from `XrGameBoot.cpp`. The visionOS boot must define (signatures from the call sites):

```
bool  GX_XR_OffscreenBoot;                                         // SDL3GameEngine/W3DGameClient/SDL3Mouse/LookAt/shadows
bool  GX_XR_PointerRay(const ICoord2D *screen, Vector3 *start, Vector3 *end);   // W3DView
CameraClass *GX_XR_RenderCamera();                                 // W3DView
bool  GX_XR_UpdateTerrainCoverage();                               // W3DView
int   GX_XR_ShadowCategory(int);                                   // W3DShadow
void  GX_XR_BeginStereoWorld(); void GX_XR_EndStereoWorld();       // W3DDisplay
void  GX_XR_PresentLoadingFrame();                                 // W3DDisplay
bool  GX_XR_SplitUIAllowed(); bool GX_XR_BeginUILayer(); bool GX_XR_WorldRequested();   // W3DInGameUI
int   GX_XR_CullSphere(const SphereClass &);                       // W3DScene
```

## 4. Behaviour and performance changes (all inert on Android)

Everything below is gated on `m_hostGL` (a resolver was supplied) or on host targets being set;
Android/Quest takes the original code path instruction for instruction.

1. **World elision is not tied to multiview.** `canOmit(..., multiviewHealthy)` now also accepts an active
   host-GL stereo target. Without it, every world draw is submitted three times (ordinary + two eyes).
2. **No per-draw FBO ping-pong.** The eye FBO is bound only when it changes and stays bound; the ordinary FBO
   is restored lazily (`leaveXRStereoFBO`) by the next non-omitted draw, `clear`, `setRenderTarget`, the
   readbacks, `BeginXRUI`, `Present`, and once in `EndXRStereo`. ANGLE-Metal ends the render encoder whenever
   the bound FBO's pass descriptor changes; with elision + an atlas, N world draws now cost O(1) binds.
3. **Coverage probe without `glReadPixels` on host textures.** The first-activation / every-120-frames probe
   blits each eye rect into a backend-owned scratch texture (GPU) and reads that.
4. **`readbackRenderTarget` / `debugSampleRenderTarget` restore the logical FBO** (`m_offFBO` in XR mode) instead
   of `m_curFBO == 0`, which on a surfaceless ANGLE context is a non-existent default framebuffer.
5. **Fragment varyings `vUV0`, `vUV1`, `vFogDepth` are `highp`.** ANGLE-Metal translates `mediump` to `half`;
   see section 6.3.
6. `/sdcard` marker files (`gx_xr_camoffset.txt`, `gx_xr_offscreen.txt`, the PPM path) are Android-only;
   `__android_log_print` was already guarded. `GX_XR_CAPTURE_PPM` is disabled with a host resolver (it would
   `glReadPixels` the host GAME texture).
7. Backend-allocated targets are byte-for-byte the old code (`gxCreateColorTexture`, same parameters).

### 4.1 Known performance risks (unmeasured on hardware)

* **No multiview.** Without `GL_OVR_multiview` each world draw is replayed once per eye. Prefer atlas mode
  (one FBO, viewport + scissor per eye, encoder stays open) over separate eyes (two FBO binds per draw).
  Elision removes the third (ordinary) submission.
* Projected-shadow updates still read back a render target (`readbackRenderTarget` -> `glReadPixels`), a full
  GPU sync in Metal; the first activation coverage probe is a one-off sync.
* Program builds go GLSL -> SPIR-V -> MSL -> Metal at first use of each state key (hundreds over a level);
  `KHR_parallel_shader_compile` is exposed but not used. Expect load-time hitches, not steady-state cost.
* Every UP draw orphans two buffers (`glBufferData`); every UI draw goes through it.
* `glGetError` runs once per frame in `present()`.
* Terrain, decals and UI text use `D3DFMT_DXT*` and 4/5-bit formats that are expanded to RGBA8 on the CPU at
  load time (4x the memory of DXT for terrain textures). See 6.1.
* Polygon offset units differ: Metal `setDepthBias` scales by the depth format's resolution, and ANGLE maps
  `D24S8` to `Depth32Float_Stencil8`; decal/road z-fighting constants may need tuning.

## 5. Guards changed

Sites are given as function/context, not line numbers.

**`GX_USES_D3D8GLES`** (was `__ANDROID__`):
`WW3D2/dx8wrapper.cpp` (d3d8gles.h include; pillarbox V-flip query; **backend switch in `DX8Wrapper::Init`,
now ahead of the `__APPLE__` DXVK branch**; the "no generic dlopen resolution" block), `dx8renderer.cpp` (include, skin and
category hooks x3), `sortingrenderer.cpp` (include + flush hook), `render2dsentence.cpp` (include, two UI
timers), `GeneralsMD/.../render2d.cpp` (include, screen-UV-bias query, UI timer, draw category x2),
`HeightMap.cpp` (draw-category declaration, `TERRAIN_TILES_PER_VERTEX_BUFFER = 10`, both batched terrain
draw loops, category RAII), `W3DSmudge.cpp` (forward declaration, `GeneralsX_SmudgeUsableOnThisBackend`),
`W3DProjectedShadow.cpp` (forward declaration, category RAII).

**`GX_XR_HOST`** (was `__ANDROID__`):
`W3DView.cpp` (`GX_XR_PointerRay` x3, `GX_XR_RenderCamera`, `GX_XR_UpdateTerrainCoverage`),
`W3DDisplay.cpp` (loading-frame presenter x2, `GX_XR_BeginStereoWorld/EndStereoWorld`, mouse-cursor suppression,
**fixed-resolution list in `buildFilteredResolutions`**), `W3DInGameUI.cpp` + `W3DInGameUI.h` (rotate placement,
attack-hint members and overrides: header and source must agree or the class layout differs),
`W3DScene.cpp` (`GX_XR_CullSphere`), `W3DShadow.cpp` (shadow category), `W3DVolumetricShadow.cpp` and
`W3DProjectedShadow.cpp` (`GX_XR_OffscreenBoot` forces shadow volumes/decals on), `SDL3Mouse.cpp`
(normal mouse stream for XR hosts), `LookAtXlat.cpp` (no edge scroll in XR), `dx8wrapper.cpp`
(`Resize_And_Position_Window`: there is no OS window to resize).

**Stayed on `__ANDROID__`** (true Android-only): `texture.cpp` (`dladdr` texture-churn diagnostic),
`W3DDisplay.cpp` SDL fullscreen-mode call, `dx8wrapper.cpp` `kPillarboxRenderScale` (identical branches),
`GameLOD.cpp`, `GlobalData.cpp` (terrain multipass, storage layout), `GameEngine.cpp` (music missing),
`W3DVideoBuffer.cpp` (movie texture padding), `FFmpegVideoPlayer.cpp`, `INI.cpp`, `MemoryDiagnostics.h`,
`GameMemory.cpp`, the JNI/crash-handler files. **Not in this package's ownership, recommendation for the
lead:** `GeneralsMD/Code/GameEngine/Source/Common/GameLOD.cpp` `getRecommendedStaticLODLevel` (skip the legacy
PC preset matching and stay at LOW) should be widened to `GX_XR_HOST` for the same reason it exists on
Android: there is no PCI vendor/device id or x86 CPU family to match on Apple silicon.

### 5.1 The 22 `TARGET_OS_IPHONE` sites (visionOS defines `TARGET_OS_IPHONE == TARGET_OS_VISION == 1`)

| Site | Decision |
| --- | --- |
| `InGameUI.cpp` x8 (`handleRadiusCursor` re-assert, radius cursor position, placement ghost, hint timers, radius at panel, touch command icon, targeting reticle, touch debug overlay) | **Keep (touch-aim path).** Gaze + pinch, like a finger or the Quest ray, has no live mouse position; the XR host feeds `InGameUI::setTouchAimPoint`. |
| `ControlBar.cpp` (hold a command button to read its description) | **Keep.** A pinch-and-hold has no hover, same as a finger hold. |
| `LookAtXlat.cpp` (`canScrollAtScreenEdge`) | **Keep.** The `GX_XR_HOST` block above it already returns false for the XR host; the fall-through keeps the real-pointer exception. |
| `SDL3GameEngine.cpp` x3 (`SAGE_MOBILE_PLATFORM`, pointer-scroll enforcement, touch target feedback) | **Keep.** The touch translator is inert without SDL finger events, but `TheMouse->touchSelecting()/sawRealMouse()` need the mobile build. |
| `SDL3Mouse.h/.cpp` x2 (mobile `createStreamMessages`, `setTouchCursorPos`) | **Keep.** XR hosts enable the normal stream from inside `createStreamMessages` (now `GX_XR_HOST`). |
| `render2dsentence.cpp` + `.h` (fonts) | **Keep.** No fontconfig on visionOS; fonts resolve from `fonts/` beside the game data. The host must stage an Arial-compatible face (never a retail font); unresolved fonts fall back to `arial.ttf` and then fail loudly. |
| `dx8wrapper.cpp` (DXVK dylib path) | **Keep for iOS, unreachable on visionOS** (the `GX_USES_D3D8GLES` branch wins). |
| `SDL3Main.cpp` x2 (not touched here) | **n/a.** `SDL3Main.cpp` is replaced by the host on visionOS (`SAGE_BUILD_VISIONOS_LIB` does not compile it); if it is ever compiled, `SAGE_MOBILE_PLATFORM` and the iOS stderr capture are correct for visionOS as well. |

## 6. Texture and render-target formats on ANGLE-Metal

### 6.1 DXT / S3TC

ANGLE-Metal advertises no `GL_EXT_texture_compression_s3tc`, `_dxt1` or `GL_ANGLE_texture_compression_dxt*`
(ETC2/EAC and ASTC-LDR only). `initContextExternal` therefore leaves `m_hasS3TC` false and the backend's
software BC1-3 decode (`DecodeDXTLevel`, one 4x4 block at a time) runs for every DXT1/2/3/4/5 level, uploading
RGBA8. `vision-gles-device-test` proves it through the D3D8 API (`CreateTexture(D3DFMT_DXTn)`, `LockRect`
block writes, draw, Metal readback) against an independent reference decoder, including DXT1 punch-through
alpha, DXT3 explicit alpha and both DXT5 alpha modes. A truncated level still falls back to magenta, loudly.

### 6.2 Uncompressed formats

Every format the engine requests works through the ordinary `glTexImage2D` path on ANGLE-Metal
(exercised by the device test, each sampled with point filtering and compared to the exact expected RGBA):
`A8R8G8B8`, `X8R8G8B8` (alpha forced opaque on the CPU), `R5G6B5` (`GL_RGB565`), `A1R5G5B5` and
`X1R5G5B5` (`GL_RGB5_A1`), `A4R4G4B4` (`GL_RGBA4`), `A8` (`GL_ALPHA`), `L8` (`GL_LUMINANCE`), `A8L8`
(`GL_LUMINANCE_ALPHA`). Depth/stencil renderbuffers are `GL_DEPTH24_STENCIL8`; colour targets are `GL_RGBA8`.

### 6.3 `mediump` varyings

ANGLE-Metal turns `mediump` into `half`. The generated fragment shaders used to declare
`in vec2 vUV0, vUV1; in float vFogDepth;` under `precision mediump float`, so texture coordinates ran through fp16
interpolation: above about 64 texture repeats the error exceeds a texel (terrain and tiled UI are exactly that).
With a host resolver the three varyings are now `highp` (`gles_pipeline.cpp`, fragment shader
generator); Android keeps the original declarations. The device test measures the raw ANGLE behaviour and
the backend's shaders on a 64x-repeated ramp.

## 7. Tests and measured results

All numbers below were produced on the development Mac (Apple M5, visionOS 26.5 simulator runtime, Xcode 27),
never on a Vision Pro. The simulator GPU is a host Metal device behind a simulated runtime: correctness results
transfer, millisecond figures are only indicative (and the machine was shared, so they are noisy).

| Test | How to run | Result |
| --- | --- | --- |
| `scripts/qa/vision-gles-device-test.sh` (`vision-gles-device-test.mm`, `vision-gles-device-cases.cpp`, `vision-gles-harness.h`, `vision-gles-sdl-stubs.cpp`) | `scripts/qa/vision-gles-device-test.sh --udid <own simulator UDID>` (`--build-only` builds only; `--macos` is a harness debugging aid, not evidence about visionOS) | **PASSED: 90 checks, 0 failures**, run with `xcrun simctl spawn` inside the visionOS 26.5 simulator, renderer `ANGLE (Apple, ANGLE Metal Renderer: Apple xrOS simulator GPU)`, `OpenGL ES 3.0 (ANGLE 2.1.28778)`. Cases: API surface; separate-eye stereo (Private MTLTextures wrapped through EGLImage, lit textured fixed-function quad, per-eye clip matrices, texel-row convention, transparent surround, per-eye parallax, N.L = 0.5 lighting); atlas (both eyeRects, no leakage across the scissor); board clipping (tabletop board bounds discard fragments, `aspect == -1` observer mode does not); ring rotation (slot A untouched by frame 2, names re-attached in frame 3); backend-allocated fallback; world/UI/GAME slots with elision without multiview (24 world draws omitted, 13 FBO binds per frame regardless of draw count); DXT1/3/5; eight texture formats; `mediump` probe; cost of the stereo path. All pixels verified through Metal blits into Shared buffers; `glReadPixels` is never called on a wrapped texture. |
| `scripts/qa/vision-gles-formats-test.sh` | `bash scripts/qa/vision-gles-formats-test.sh` (host, no GPU; the production conversion functions are extracted verbatim from `gles_pipeline.cpp`; built with `-fsanitize=undefined`) | **PASSED: 48 checks, 0 failures**: DXT1/2/3/4/5 against an independent S3TC reference including punch-through and both DXT5 alpha modes, every uncompressed format, an unimplemented format reported (magenta upload) instead of mis-decoded. |
| existing `scripts/qa/xr-*-test.*` (41 test names) | Quest host tests built on macOS with `clang++`, `-I GeneralsMD/Code/Main/visionos/xr_shim`, `-static-libstdc++` dropped (Linux-only flag) | **27 pass / 14 fail, identical per test on the pre-change tree (`039512c`, before this package) and after**. The 14 failures are environmental, not regressions: 6 need an Android EGL device (`diorama-device`, `mrt-device`, `multiview-device`, `performance-device`, `terrain-device`, `world-device`), `branding` needs `aapt2` and an APK, `stereo-state`, `uniform-cache` and `world-copy` compile with the d3d8/EGL include paths but link `-lEGL` for an Android device, and `scene`, `interaction`, `loading-presenter`, `menu-routing` include the full OpenXR SDK headers (types such as `XrSession`, `XrCompositionLayerProjection`) that the in-repo `xr_shim` stand-in does not provide. Those four passed in the earlier recon run that had the real SDK, which is where the 31/40 baseline comes from (27 + those 4). |

Measured (simulator GPU under load, indicative): `casePerf` in the device test replays 200 small world draws per
frame at 1024x1024 per eye:

| Mode | GL draws / frame | `glBindFramebuffer` / frame | CPU ms / frame incl. GPU wait |
| --- | --- | --- | --- |
| separate eyes, ordinary world draw kept | PERF_SEP_DRAWS | PERF_SEP_BINDS | PERF_SEP_MS |
| atlas, ordinary world draw kept | PERF_ATL_DRAWS | PERF_ATL_BINDS | PERF_ATL_MS |
| atlas, ordinary world draw elided | PERF_ELI_DRAWS | PERF_ELI_BINDS | PERF_ELI_MS |

Software DXT decode (host, `-O2`, one 1024x1024 level): DXT1 about 10 ms, DXT5 about 12 ms (roughly 340-410 MB of RGBA
per second). A full mip chain adds a third. This is the shipping path on ANGLE-Metal and the first thing to
optimise (NEON, or transcoding to ASTC/ETC2 offline) if level load time misses.

## 8. Open items

* Nothing here has run on a physical Vision Pro: shared-event latency, BC availability
  (`supportsBCTextureCompression` is irrelevant now that the decode is on the CPU), real frame times and the
  thermal behaviour of the CPU DXT decode are unmeasured.
* Program-build hitches (section 4.1) and the projected-shadow readback are the first candidates if frame
  pacing misses on device.
* The host must own: the game-data-independent font, `GX_XR_*` symbols (3.4), `GX_XR_OffscreenBoot`, and the
  logic time scale (see the platform recon).
