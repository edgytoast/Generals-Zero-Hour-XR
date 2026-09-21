# d3d8gles - D3D8 -> native GLES3 renderer (Android + visionOS)
# GeneralsX @build Android port GLES experiment
#
# Ported from Lolendor/Generals-WebAssembly's d3d8webgl (D3D8 -> WebGL2 for
# Emscripten). Implements the subset of the Direct3D 8 API the engine uses
# (see Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp) on top of native
# GLES3, as an alternative to DXVK's D3D8->Vulkan translation. Statically
# linked into libmain.so on Android (unlike DXVK, which is dlopen'd at
# runtime) -- see DX8Wrapper::Init()'s backend switch.
#
# GeneralsX @feature visionOS port - also built for visionOS, where the GLES3
# implementation is ANGLE running on Metal. The backend never links ANGLE: the
# XR host owns the ANGLE EGL display/context and hands the backend a GL entry
# point resolver (D3D8GLES_XRConfig::getProcAddress, in practice
# eglGetProcAddress), so this target only needs ANGLE's *headers*
# (GX_ANGLE_INCLUDE_DIR). See docs/visionos-gles-backend.md.
#
# Not wired into any other platform's build.

set(_gx_d3d8gles_visionos OFF)
if(GX_PLATFORM_VISIONOS OR CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    set(_gx_d3d8gles_visionos ON)
endif()

if(NOT (ANDROID OR _gx_d3d8gles_visionos))
    return()
endif()

add_library(d3d8gles STATIC
    src/d3d8gles.cpp
    src/gles_dispatch.cpp
)

target_include_directories(d3d8gles PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(d3d8gles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# D3D8 headers come from the same DXVK checkout our own D3D8 code already
# uses (d3d8lib, see GeneralsMD/Code/CompatLib/CMakeLists.txt); Win32 shim
# types (HWND, WINAPI, DWORD, ...) come from CompatLib transitively through
# it. sdl3lib provides the SDL3 headers (SDL_GetTicks and, on Android, the
# SDL3 GL context API this module's non-XR window path uses).
#
# GeneralsX @build Android port ANGLE experiment - deliberately NOT linked
# against system GLESv3 (unlike the original port). gl* symbols are instead
# defined by src/gles_dispatch.cpp, which forwards them to function pointers
# resolved at runtime -- via dlopen/dlsym against a chosen GLES implementation
# library on Android (ANGLE's bundled libGLESv2_angle.so, or the system
# libGLESv3.so), and via the host-supplied resolver (eglGetProcAddress of the
# host's ANGLE display) in XR mode on every platform. Direct DT_NEEDED linking
# against libGLESv3.so can't be repointed at a same-named bundled library --
# Android's linker namespace reserves that name for the system library -- so
# an explicit dispatch table is the only way to route these calls through an
# app-bundled, differently-named implementation.
if(ANDROID)
    target_link_libraries(d3d8gles PRIVATE d3d8lib sdl3lib log)
else()
    # visionOS: no liblog. dlfcn is part of libSystem.
    target_link_libraries(d3d8gles PRIVATE d3d8lib sdl3lib ${CMAKE_DL_LIBS})

    # GLES3 / GLES2-ext / KHR headers. They come from the ANGLE install that
    # scripts/build/visionos/build-angle.sh produces (<root>/<slice>/include),
    # never from a system OpenGLES.framework (unavailable on visionOS). Only
    # headers are needed: the host owns the ANGLE library and its resolver.
    set(GX_ANGLE_INCLUDE_DIR "" CACHE PATH
        "Directory containing the ANGLE headers (GLES3/gl3.h, GLES2/gl2ext.h, KHR/khrplatform.h, EGL/*.h) for the d3d8gles backend on visionOS")
    if(NOT GX_ANGLE_INCLUDE_DIR)
        # Documented default layout of build-angle.sh, overridable with GX_ANGLE_ROOT
        # (the script's "$DEPS_ROOT/angle/install").
        if(DEFINED ENV{GX_ANGLE_ROOT})
            set(_gx_angle_root "$ENV{GX_ANGLE_ROOT}")
            if(CMAKE_OSX_SYSROOT MATCHES "simulator")
                set(_gx_angle_slice xrsimulator)
            else()
                set(_gx_angle_slice xros)
            endif()
            set(GX_ANGLE_INCLUDE_DIR "${_gx_angle_root}/${_gx_angle_slice}/include" CACHE PATH "" FORCE)
        endif()
    endif()
    if(NOT GX_ANGLE_INCLUDE_DIR OR NOT EXISTS "${GX_ANGLE_INCLUDE_DIR}/GLES3/gl3.h")
        message(FATAL_ERROR
            "d3d8gles on visionOS needs the ANGLE headers: set -DGX_ANGLE_INCLUDE_DIR=<angle install>/<xrsimulator|xros>/include "
            "(or export GX_ANGLE_ROOT=<angle install>). Build them with scripts/build/visionos/build-angle.sh.")
    endif()
    target_include_directories(d3d8gles PRIVATE ${GX_ANGLE_INCLUDE_DIR})

    # Symbols shared with the engine libraries; also guarantees the macros of
    # WWLib/gx_backend.h are set for every consumer of the backend.
    target_compile_definitions(d3d8gles PUBLIC GX_PLATFORM_VISIONOS=1)
endif()

# Every consumer (WW3D2, the device layer) sees the same answer to "is the
# GLES3 D3D8 backend in use" -- see Core/Libraries/Source/WWVegas/WWLib/gx_backend.h.
target_compile_definitions(d3d8gles PUBLIC GX_D3D8GLES_BACKEND=1)

target_compile_features(d3d8gles PRIVATE cxx_std_20)
