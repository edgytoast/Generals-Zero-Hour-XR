# GeneralsX - visionOS engine build (Apple Vision Pro)

This document explains how the Zero Hour engine is built as a **static library** for visionOS
(simulator slice `xrsimulator` and device slice `xros`, both arm64), reproducibly from scripts.
The library is linked into the native SwiftUI + Compositor Services app in `visionos/`
(see `docs/visionos-shell.md`). It contains the whole engine and its third-party dependencies; it
contains no `main()`, no window and no game data. The host app drives it: init, per-frame, input,
shutdown.

What this build does **not** provide (the host app and the engine-host package supply them; the archive is
built and verified with exactly these symbols left undefined, see "Undefined symbols" below):

* the `GX_XR_*` engine hooks (`GX_XR_BeginStereoWorld`, `GX_XR_PointerRay`, ... 12 symbols): the visionOS twin of
  the Quest `XrGameBoot.cpp`, to be added under `GeneralsMD/Code/Main/visionos/`;
* the host boundary (`GeneralsMD/Code/Main/visionos/*`: game boot, `GXEngineHost`);
* ANGLE itself (`scripts/build/visionos/build-angle.sh`), linked by the app. The archive holds the D3D8 backend
  (`Direct3DCreate8_GLES`, `d3d8gles_*` from `Core/Libraries/Source/d3d8gles`) but the backend never links ANGLE:
  the host passes it `eglGetProcAddress` through `d3d8gles_SetXRConfig`.

Retail game data is never part of the build. The user supplies their own Generals and Zero Hour files.

## Prerequisites

| Requirement | Version / detail |
| --- | --- |
| macOS + Xcode | Xcode 27 with the visionOS SDKs `xros27.0` and `xrsimulator27.0` (Settings > Components). `xcrun --sdk xros --show-sdk-path` and `--sdk xrsimulator` must work. |
| Deployment targets | Engine and all vcpkg libraries: visionOS **2.0** (`CMAKE_OSX_DEPLOYMENT_TARGET`, mirrored in the `-target` flags of `cmake/triplets/arm64-xros*.cmake`). The app: visionOS 26.0. |
| CMake | **>= 3.28** (visionOS needs it; the root file asks 3.25 for the other platforms). Tested: 4.4.3. |
| Ninja, pkg-config, git, python3 | `brew install cmake ninja pkg-config`. |
| vcpkg | A **full clone** (not shallow) at a commit that supports visionOS (vcpkg PR #45464, June 2025 or later), bootstrapped: `git clone https://github.com/microsoft/vcpkg.git $VCPKG_ROOT && $VCPKG_ROOT/bootstrap-vcpkg.sh -disableMetrics`. The manifest baseline in `vcpkg.json` only pins port versions; the scripts and triplets come from the checkout. |
| ANGLE | Only for linking the app and for the renderer: `scripts/build/visionos/build-angle.sh all`. **Not** needed to build the engine archive. |
| Network | The first configure downloads SDL3 / SDL3_image / openal-soft / GamespySDK / lzhl (CMake FetchContent), the DXVK header checkout and the vcpkg port sources. Later runs are offline once `$GX_DEPS_ROOT` caches are warm. |
| Disk / RAM | About 6 GB for both slices plus caches. The machine is often shared: the scripts default to 4 jobs. |

## Quick start

```sh
# 1. environment (sourced by every script; safe to run by hand to check prerequisites).
#    Works from zsh (the macOS default) and bash. It must be sourced, not executed.
source scripts/build/visionos/env.sh

# 2. build the engine static library, one slice or both
scripts/build/visionos/build-engine.sh --simulator          # xrsimulator arm64
scripts/build/visionos/build-engine.sh --device             # xros arm64
scripts/build/visionos/build-engine.sh --both --clean -j 4  # from a clean build tree

# 3. merge the transitive static closure per slice and wrap it into an xcframework
scripts/build/visionos/make-xcframework.sh
#   -> build/xcframework/GeneralsZHEngine.xcframework
#   -> build/xcframework/<slice>/libGeneralsZHEngine_all.a, link-inputs.txt, link-flags.txt

# 4. check platform / architecture / undefined symbols
scripts/build/visionos/verify-engine.sh
```

The scripts only wrap CMake presets; you can run them yourself:

```sh
source scripts/build/visionos/env.sh                 # exports VCPKG_ROOT, VCPKG_BINARY_SOURCES, ...
cmake --preset visionos-simulator                    # or visionos-device
cmake --build --preset visionos-simulator            # builds ONLY target z_generals
```

**Never build `all`.** `z_wwaudio` (the Miles wrapper) does not compile off Windows. The build
presets build the `z_generals` target only.

## Environment variables

`scripts/build/visionos/env.sh` (sourced by all scripts) reads and exports:

| Variable | Default | Meaning |
| --- | --- | --- |
| `GX_DEPS_ROOT` | `/Users/jvadala/CandC/deps` | Out-of-repo dependencies: vcpkg, ANGLE, DXVK header checkout, caches. |
| `VCPKG_ROOT` | `$GX_DEPS_ROOT/vcpkg` | vcpkg checkout used by the presets (toolchain file `$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`). |
| `VCPKG_DOWNLOADS` | `$GX_DEPS_ROOT/vcpkg-downloads` | Source tarballs, shared by both triplets. |
| `VCPKG_BINARY_SOURCES` | `clear;files,$GX_DEPS_ROOT/vcpkg-binary-cache,readwrite` | Compiled-package cache. Third-party libraries compile once per triplet; every later clean build restores them (24 packages in about 4 s). |
| `GX_ANGLE_INSTALL` | `$GX_DEPS_ROOT/angle/install` | ANGLE install (`{xrsimulator,xros}/{include,lib}`); CMake exposes `GX_ANGLE_INCLUDE_DIR` / `GX_ANGLE_LIB_DIR`. |
| `GX_ENGINE_BUILD_ROOT` | `<repo>/build` (git-ignored) | Build trees `visionos-simulator`, `visionos-device`, logs, `xcframework/`. |
| `GX_JOBS` | `4` | Build and vcpkg parallelism (`-j N` on `build-engine.sh` overrides it). |
| `GX_XROS_DEPLOYMENT_TARGET` | `2.0` | Used by `verify-engine.sh`. The value is fixed in the presets and triplets; change all three together. |
| `SAGE_DXVK_HEADERS_DIR` (CMake) | `$GX_DEPS_ROOT/src/fbraz3-dxvk` | Checkout of the DXVK fork with its header submodules; fetched if missing. |

## What the build does

1. **vcpkg (manifest mode, inside `cmake --preset`)** installs zlib, glm, gli, freetype (+ libpng, bzip2,
   brotli), curl (+ OpenSSL), FFmpeg, GameNetworkingSockets (+ protobuf, abseil, utf8-range) for the overlay
   triplet `arm64-xrsimulator` or `arm64-xros`, Release libraries only, plus the macOS host tools protobuf
   needs. Overlay triplets: `cmake/triplets/`. Overlay ports: `cmake/vcpkg-ports/` (GameNetworkingSockets, shared
   with Android; visionOS is added behind `VCPKG_TARGET_IS_VISIONOS`) and `cmake/vcpkg-ports-visionos/`
   (OpenSSL, visionOS only).
2. **FetchContent** builds, as static libraries: SDL3 3.4.2 (video, render, GPU, joystick, haptic, sensor, HIDAPI,
   Vulkan, Metal, OpenGL ES all OFF: only events, timers, audio, iconv), SDL3_image (no libpng: stb decodes),
   openal-soft 1.24.2 (CoreAudio; patched, see below), GamespySDK, lzhl.
3. **DXVK headers only** (`cmake/dxvk-headers.cmake`): the engine compiles against the fork's Wine-style D3D8
   headers. A shallow checkout of commit `46a3bc0` (the `references/fbraz3-dxvk` gitlink) with just the three
   header submodules; no meson, no MoltenVK, no Vulkan SDK, no glslang.
4. **The engine**: `z_gameengine`, `z_gameenginedevice`, WW3D2, WWLib, `d3d8gles`, ... and the `z_generals` STATIC
   library (`libGeneralsZHEngine.a`: `EngineGlobals.cpp`, `LinuxStubs.cpp`, `SDLVisionStubs.cpp`, and a glob of
   `GeneralsMD/Code/Main/visionos/*.cpp|*.mm`, today `VisionInteraction.cpp`). It has no `main()`.
5. **`make-xcframework.sh`** asks Ninja for the resolved link line of the never-built probe executable
   `z_generals_link_probe` (it lists every static archive in link order, vcpkg ones included, plus system
   frameworks), merges them with `libtool -static` into `libGeneralsZHEngine_all.a`, records the system flags the
   host must link in `link-flags.txt`, and creates `GeneralsZHEngine.xcframework` with an umbrella header and
   module map.

### Options set by the presets

`SAGE_BUILD_VISIONOS_LIB=ON` (default on visionOS; error elsewhere), `SAGE_USE_SDL3=ON`, `SAGE_USE_OPENAL=ON`,
`SAGE_USE_GLM=ON`, `RTS_BUILD_OPTION_FFMPEG=ON` (FFmpeg is mandatory with OpenAL: the audio cache decodes through
libavcodec), `SAGE_USE_DX8=OFF`, `SAGE_USE_MOLTENVK=OFF`, `SAGE_UPDATE_CHECK=OFF`, `RTS_CRASHDUMP_ENABLE=OFF`,
`RTS_BUILD_OPTION_SAGE_PATCH=OFF`, Generals/tools/extras OFF, **`RTS_GAMEMEMORY_ENABLE=OFF`** (the pool
allocator replaces global `operator new/delete` for the whole process and would also catch the Swift/ObjC
runtime, OpenAL and FFmpeg allocations; with it ON the Release build also hits an `OWNERSHIP_COOKIE` compile
error in `GameMemory.cpp`), `BUILD_SHARED_LIBS=OFF`, `LIBTYPE=STATIC`. (`CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY` is deliberately NOT
set: it would turn every link-based feature check, such as the ones openal-soft and SDL run, into a false
positive. Ninja links unsigned arm64 test executables for both SDKs without trouble.)

## Code map (what changed for visionOS)

| File | Change |
| --- | --- |
| `cmake/platform.cmake` (new) | `GX_PLATFORM_VISIONOS` (also passed to the compiler as `GX_PLATFORM_VISIONOS=1`), `SAGE_BUILD_VISIONOS_LIB`, ANGLE location, and `enable_language(OBJCXX)` (must happen here, at the top level; enabling it later from the Main subdirectory makes CMake fail with `CMAKE_OBJCXX_COMPILE_OBJECT not set`). |
| `cmake/sdl3.cmake` | visionOS branch: static, video stack off, libpng off. |
| `cmake/openal.cmake`, `cmake/patches/openal-soft-1.24.2-visionos.patch` | static, RTKit off, two one-line fixes (`TARGET_OS_VISION` in `alc/backends/coreaudio.cpp`; `visionOS` in the framework regex of openal's CMake). Applied through `FetchContent PATCH_COMMAND` with `cmake/patches/apply-patch.cmake` (idempotent, fails loudly). |
| `cmake/dx8.cmake`, `cmake/dxvk-headers.cmake` | headers-only visionOS branch. |
| `cmake/gamespy.cmake` | GamespySDK forced static on visionOS. |
| `Core/.../WW3D2/CMakeLists.txt` | `visionOS` added in the four platform lists (FreeType define, fontconfig exclusion, `Freetype::Freetype` link). |
| `GeneralsMD/Code/GameEngine/CMakeLists.txt` | GameNetworkingSockets link and `GENERALS_ONLINE_ENABLE_P2P_TRANSPORT` also for visionOS. |
| `GeneralsMD/Code/Main/CMakeLists.txt` | `z_generals` STATIC on visionOS, PUBLIC links (including `d3d8gles`), `visionos/*.cpp\|*.mm` glob (ARC for `.mm`), `visionos/xr_shim` + d3d8gles include directories for those sources only, link probe. |
| `GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/CMakeLists.txt` | `z_ww3d2` links `d3d8gles` (PUBLIC) on visionOS like it does on Android: `render2d.cpp`, `dx8wrapper.cpp`, ... include `d3d8gles.h` under `GX_USES_D3D8GLES`. The include directory and `GX_D3D8GLES_BACKEND` propagate to `z_gameenginedevice` (HeightMap, W3DSmudge, W3DProjectedShadow). |
| `GeneralsMD/Code/Main/EngineGlobals.cpp`, `SDL3Main.cpp` | globals and `CreateGameEngine()` moved verbatim out of `SDL3Main.cpp` (still linked into every other non-Windows target). |
| `GeneralsMD/Code/Main/SDLVisionStubs.cpp` | `SDL_IsIPad`, `SDL_IsAppleTV` (SDL built without its UIKit video driver). |
| `vcpkg.json` | `ffmpeg` and `gamenetworkingsockets` also on `visionos`; `fontconfig` and `openal-soft` (redundant with FetchContent) excluded on `visionos`. |
| `CMakePresets.json` | `visionos-simulator`, `visionos-device` (configure and build presets). |
| `cmake/triplets/arm64-xros.cmake`, `arm64-xrsimulator.cmake` | overlay triplets. |
| `cmake/vcpkg-ports-visionos/openssl` | OpenSSL 3.4.1 port copy with a visionOS branch. |

Every change is additive and gated on visionOS, except the pure move of the globals out of `SDL3Main.cpp`
(see the header of `EngineGlobals.cpp`). Android, Linux, macOS, iOS and Windows keep their code paths.

## Results of the reference run

Measured on the merged tree (`visionos-port` plus this branch), Apple M5, Xcode 27.0, CMake 4.4.3, `-j 4`, warm
vcpkg binary cache. Commands: `build-engine.sh --both --clean -j 4`, `make-xcframework.sh`, `verify-engine.sh`, each
from a fresh `zsh -c 'source scripts/build/visionos/env.sh && ...'`.

| Step | Simulator (xrsimulator) | Device (xros) |
| --- | --- | --- |
| Configure (vcpkg restore from cache, FetchContent, generate) | 51 s | 2 m 25 s |
| Build target `z_generals` (1404 Ninja steps) | 2 m 47 s | 14 m 26 s |
| Total | **3 m 38 s** | 16 m 51 s |
| Load on the machine | quiet | load average 600-900 (five other agents compiling) |
| `libGeneralsZHEngine.a` (engine only) | 1.1 MB | 1.1 MB |
| `libGeneralsZHEngine_all.a` (merged closure) | 758,621,576 bytes (723 MB) | 757,167,120 bytes (722 MB) |

The device time is not comparable with the simulator time: the machine was overloaded. Both builds compile the same 1404 steps.
`make-xcframework.sh` takes about 7 to 20 s; `GeneralsZHEngine.xcframework` is 1.4 GB (two slices, `xros-arm64` and
`xros-arm64-simulator`, each with `Headers/`: umbrella header, module map and the host headers of
`GeneralsMD/Code/Main/visionos`). `verify-engine.sh` takes 1.5 to 3.5 min for both slices.

Compiler warnings, per slice: 14,001 (no errors). Top categories: `-Wsuggest-override` 6405, `-Winvalid-offsetof` 4206,
`-Wswitch` 1073, `-Winconsistent-missing-override` 444, `-Wmacro-redefined` 375, `-Wdeprecated-literal-operator` 275,
`-Wnontrivial-memcall` 243, `-Wdeprecated-declarations` 234. All come from the unchanged engine sources.

`verify-engine.sh`: PASS on both slices.

* architecture arm64 only; 4499 objects, all `VISIONOSSIMULATOR` (simulator) or `VISIONOS` (device), minos 2.0
  (50 of them carry no SDK version field, which is harmless);
* linker emulation: 4079 of 4662 archive members pulled in from the 4 roots (`EngineGlobals`, `LinuxStubs`,
  `SDLVisionStubs`, `VisionInteraction`) plus `GameMain()`; 0 unexpected undefined symbols;
* real link test: an executable linked from the merged archive, the recorded system frameworks and stand-ins for the
  expected symbols: 46 MB (simulator) / 45 MB (device).

### Undefined symbols

What stays undefined after the SDK and the archive itself are taken into account (identical on both slices):

| Symbol | Kind | Provided by |
| --- | --- | --- |
| `GX_XR_OffscreenBoot` (data, `extern "C"`) | boot flag | engine-host package (`visionos/VisionGameBoot.cpp`; defined in `XrGameBoot.cpp` on Android) |
| `GX_XR_BeginStereoWorld()`, `GX_XR_EndStereoWorld()`, `GX_XR_RenderCamera()`, `GX_XR_BeginUILayer()`, `GX_XR_WorldRequested()`, `GX_XR_SplitUIAllowed()`, `GX_XR_ShadowCategory(int)`, `GX_XR_UpdateTerrainCoverage()`, `GX_XR_PresentLoadingFrame()`, `GX_XR_CullSphere(const SphereClass&)`, `GX_XR_PointerRay(const ICoord2D*, Vector3*, Vector3*)` (C++ linkage) | engine XR hooks, called from the shared engine code behind `GX_XR_HOST` | engine-host package |

Nothing else is undefined: `Direct3DCreate8_GLES` and every `d3d8gles_*` function are in the archive, and the
backend has no link-time reference to ANGLE (`egl*` / `gl*` are resolved at run time through the resolver the host
passes in `D3D8GLES_XRConfig::getProcAddress`).

**Host link test against ANGLE** (repeatable by hand): a tiny Objective-C++ `main()` (compiled with `-fobjc-arc`
and the ANGLE include directory), which takes the address of `eglGetProcAddress`, calls `d3d8gles_SetXRConfig`,
`d3d8gles_InvalidateCachedState`, `Direct3DCreate8_GLES` and `GameMain()`, plus assembly stand-ins for the 12 symbols
above, linked with

```sh
xcrun clang++ -target arm64-apple-xros2.0-simulator -isysroot "$(xcrun --sdk xrsimulator --show-sdk-path)" \
    host.o stubs.o build/xcframework/simulator/libGeneralsZHEngine_all.a \
    "$GX_ANGLE_INSTALL/xrsimulator/lib/libANGLE-shared.dylib" \
    $(tr '\n' ' ' < build/xcframework/simulator/link-flags.txt) -framework Foundation -lc++ -o linktest
```

linked without a single further undefined symbol on both slices (simulator: platform `VISIONOSSIMULATOR`, 47.8 MB;
device: platform `VISIONOS`, 46.8 MB), with `@rpath/libANGLE-shared.dylib` as a load command. (Under zsh, expand the flags
with `${=FLAGS}`.) That test was run in this session against the ANGLE install of the machine
(`deps/angle/install`); it links, it was not executed.


## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `unknown target triple 'unknown-apple-xros1.0.0-simulator'` (FFmpeg) or `/bin/sh: ARCH: No such file or directory` (OpenSSL) | CMake >= 3.28 spells the visionOS deployment flag as the unexpanded template `--target=<ARCH>-apple-xros<VERSION_MIN>-simulator`; vcpkg's make/autotools helpers copy it verbatim (`<ARCH>` is then a shell redirection). | Overlay triplets set explicit `-target arm64-apple-xros2.0[-simulator]` flags and must **not** set `VCPKG_OSX_DEPLOYMENT_TARGET`. |
| vcpkg `openssl` fails with `Unknown platform` | Upstream port has no visionOS branch. | `cmake/vcpkg-ports-visionos/openssl` (preset overlay). |
| `steam/steamnetworkingsockets.h` not found | GameNetworkingSockets was only wired for Android; its CMake aborts on `visionOS`. | vcpkg manifest platform `android \| visionos`; overlay port widens the `Darwin` checks. |
| `d3d8.h` / `d3d8types.h` not found (dozens of TUs) | The headers live in the DXVK fork's `include/native/directx` **submodule**; a plain clone lacks them, and the Linux tarball layout does not match. | `cmake/dxvk-headers.cmake` fetches the pinned commit plus the three header submodules. |
| SDL3 compile error `supportedInterfaceOrientationsForWindow: is unavailable: not available on visionOS` | SDL 3.4.2 UIKit video driver vs the xros SDK. | SDL built with `SDL_VIDEO=OFF` (`cmake/sdl3.cmake`). Undefined `_SDL_IsIPad` / `_SDL_IsAppleTV` are then provided by `SDLVisionStubs.cpp`. |
| SDL3_image configure: `Could NOT find PNG` or `SDL3::SDL3-shared` missing | The macOS branch hard-codes a Homebrew libpng dylib; `BUILD_SHARED_LIBS=ON` leaks in. | visionOS uses the "no libpng" branch; presets pass `BUILD_SHARED_LIBS=OFF`. |
| `IOKit/audio/IOAudioTypes.h` not found; `ld: framework 'AudioUnit' not found` | openal-soft 1.24.2 treats only iOS/tvOS as "no device enumeration" and links macOS-only frameworks. | `cmake/patches/openal-soft-1.24.2-visionos.patch`. |
| `libavcodec/avcodec.h` not found | vcpkg manifest platform expression `ios \| android` excludes visionOS. | Added `visionos` in `vcpkg.json`. |
| `find_package(Fontconfig REQUIRED)` fails | fontconfig branch lists only iOS/Android as exclusions. | WW3D2 CMake and `vcpkg.json` updated. |
| `OWNERSHIP_COOKIE` undeclared in `GameMemory.cpp` | Release + `RTS_GAMEMEMORY_ENABLE=ON`. | Preset sets it OFF (also correct for the process-wide allocator reason above). |
| `z_wwaudio` errors (`AIL_lock`, `HSTREAM`) with `cmake --build` | Miles wrapper is Windows-only. | Build target `z_generals` only, never `all`. |
| Host-machine libraries leak in (`-I/opt/homebrew/...`, wrong-platform dylibs) | pkg-config / find_package search the Homebrew prefix. | Presets set `PKG_CONFIG_LIBDIR` to the vcpkg install; openal RTKit (Homebrew dbus) is off. |
| `patch ... Unreversed (or previously applied) patch detected! Ignore -R?` answered "y" | Apple `patch -R --dry-run` succeeds on an unpatched tree when not attached to a terminal. | `apply-patch.cmake` probes with `--forward --dry-run`. |
| Simulator/device libraries mixed up at app link time | Both slices use the same file names. | Use the xcframework (per-slice platform variants) or the `build/xcframework/<slice>/` archive that matches the SDK. |
| `vcpkg` reports "no visionOS support" / `VCPKG_TARGET_IS_VISIONOS` unknown | Old vcpkg checkout. | Update `$VCPKG_ROOT` (full clone, `git pull`, re-bootstrap). |
| Killing a build with `pkill ninja` aborts a vcpkg package build | vcpkg drives its own Ninja. | Stop the script (Ctrl-C) instead; rerun, finished packages come from the binary cache. |

## Known limits

* Runtime behaviour is **not** verified by this package: no game data and no device are available and the
  renderer and the boot code are provided by other packages. The archives are verified by platform tags,
  architecture and symbol resolution only.
* GameNetworkingSockets is built and linked for visionOS, but has not been run there.
* Simulator success is not device acceptance (GPU capabilities, memory limits and thermal behaviour differ).
