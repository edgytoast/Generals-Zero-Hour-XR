# visionOS host boundary of the engine library

This directory holds the sources that turn the Zero Hour engine into a library a
native visionOS app can drive. It is compiled into the static library target
`z_generals` (`libGeneralsZHEngine.a`) when `SAGE_BUILD_VISIONOS_LIB` is ON, which is the
default for `CMAKE_SYSTEM_NAME=visionOS` (presets `visionos-simulator` and
`visionos-device`, script `scripts/build/visionos/build-engine.sh`).

## How files get here

`GeneralsMD/Code/Main/CMakeLists.txt` adds every `*.cpp` and `*.mm` in this directory to
`z_generals` with a `CONFIGURE_DEPENDS` glob. Dropping a file in is enough: no CMake
edit, and a new file re-configures the build by itself. Other packages add, for example:

* `VisionGameBoot.cpp`: the C++ twin of `XrGameBoot.cpp` (engine init, per-frame
  driver, pointer/key injection, shutdown). Must define `bool GX_XR_OffscreenBoot`
  (on Android that definition lives in `XrGameBoot.cpp`; the visionOS library does not
  compile that file).
* `GXEngineHost.mm` and `GXEngineHost.h`: the `extern "C"` boundary the Swift/Objective-C++
  shell calls. Headers in this directory are on the public include path of the target and
  are copied into `GeneralsZHEngine.xcframework` by `make-xcframework.sh`.

Objective-C++ (`.mm`) files are compiled with ARC (`-fobjc-arc`) and `-std=c++20`.

## What the library already provides

Nothing in this directory is needed to link the engine itself. The library contains:

* the whole Zero Hour engine (`z_gameengine`, `z_gameenginedevice`, WW3D2, WWLib, ...),
  and, statically, SDL3 + SDL3_image, OpenAL Soft, GameSpy, lzhl, and (through the vcpkg
  manifest) FFmpeg, libcurl, freetype, GameNetworkingSockets (+ protobuf, abseil,
  OpenSSL), zlib and glm;
* `EngineGlobals.cpp` (in the parent directory): `__argc`, `__argv`, `ApplicationHWnd`,
  `TheSDL3Window`, `g_csfFile`, `g_strFile` and `CreateGameEngine()`, moved out of
  `SDL3Main.cpp` (which is not part of the library: no `main()`);
* `LinuxStubs.cpp` (OSDisplay* stubs) and `SDLVisionStubs.cpp` (`SDL_IsIPad`,
  `SDL_IsAppleTV`, needed because SDL3 is built with `SDL_VIDEO=OFF`).

## Symbols the library expects from other packages

`scripts/build/visionos/verify-engine.sh` prints the exact list. Today it is the 12 `GX_XR_*` hooks that the shared
engine code calls behind `GX_XR_HOST`: `bool GX_XR_OffscreenBoot` (`extern "C"`) and the C++ functions
`GX_XR_BeginStereoWorld`, `GX_XR_EndStereoWorld`, `GX_XR_RenderCamera`, `GX_XR_BeginUILayer`,
`GX_XR_WorldRequested`, `GX_XR_SplitUIAllowed`, `GX_XR_ShadowCategory`, `GX_XR_UpdateTerrainCoverage`,
`GX_XR_PresentLoadingFrame`, `GX_XR_CullSphere`, `GX_XR_PointerRay`. The D3D8 backend (`Direct3DCreate8_GLES`,
`d3d8gles_*`) is part of the archive; ANGLE's `egl*` / `gl*` are resolved at run time through the resolver
the host hands the backend.

`VisionInteraction.cpp` is compiled with `visionos/xr_shim` (a stand-in for `<openxr/openxr.h>`) and the d3d8gles
include directory on its include path; CMake scopes both to the sources of this directory.

## Build outputs and ANGLE

`cmake/platform.cmake` defines `GX_ANGLE_INCLUDE_DIR` and `GX_ANGLE_LIB_DIR`
(`$GX_ANGLE_INSTALL/<xrsimulator|xros>/{include,lib}`, default
`$GX_DEPS_ROOT/angle/install`); the target adds the include directory to its public
interface. ANGLE itself is not merged into the engine archive: the host app links it.
