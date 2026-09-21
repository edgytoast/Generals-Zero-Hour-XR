# GeneralsX @build Android port 06/07/2026 On Android the game is not a process
# of its own: SDL3's Java shell (SDLActivity in android/) loads the game as a
# shared library named libmain.so and calls its SDL_main. Everywhere else the
# game remains a regular executable.
# GeneralsX @build visionOS port: on visionOS the engine is a STATIC library
# (libGeneralsZHEngine.a) linked into the SwiftUI + Compositor Services host app,
# which owns main(), the run loop and the frame pump; the target NAME stays
# z_generals so every existing reference to it (root CMakeLists, install rules,
# presets) keeps working. See GeneralsMD/Code/Main/visionos/README.md.
if(ANDROID)
    add_library(z_generals SHARED)
elseif(SAGE_BUILD_VISIONOS_LIB)
    add_library(z_generals STATIC)
else()
    add_executable(z_generals WIN32)
endif()

# Use a binary name that doesn't conflict with original game.
# GeneralsX @build BenderAI 15/02/2026 Linux branding update
if("${CMAKE_SYSTEM}" MATCHES "Windows")
    set_target_properties(z_generals PROPERTIES OUTPUT_NAME "generalszh${RTS_BUILD_OUTPUT_SUFFIX}")
elseif(ANDROID)
    # SDLActivity.getLibraries() loads "main" -> libmain.so (SDL convention)
    set_target_properties(z_generals PROPERTIES OUTPUT_NAME main)
elseif(SAGE_BUILD_VISIONOS_LIB)
    # libGeneralsZHEngine.a (merged with its dependencies into
    # libGeneralsZHEngine_all.a by scripts/build/visionos/make-xcframework.sh)
    set_target_properties(z_generals PROPERTIES OUTPUT_NAME GeneralsZHEngine
                                                POSITION_INDEPENDENT_CODE ON)
else()
    # GeneralsX @build BenderAI 15/02/2026 Linux branding update
    set_target_properties(z_generals PROPERTIES OUTPUT_NAME GeneralsXZH)
endif()

# GeneralsX @build BenderAI 13/02/2026 Windows-specific libraries
if(WIN32)
    target_link_libraries(z_generals PRIVATE
        binkstub
        comctl32
        core_debug
        d3d8
        dinput8
        dxguid
        imm32
        milesstub
        vfw32
        winmm
    )
endif()

# GeneralsX @build visionOS port: the static engine library links its libraries
# PUBLIC, so that the host app sees their include directories and the merge script
# can walk the whole transitive static closure (see z_generals_link_probe below).
# Every other target keeps the original PRIVATE linkage.
if(SAGE_BUILD_VISIONOS_LIB)
    set(_z_generals_scope PUBLIC)
else()
    set(_z_generals_scope PRIVATE)
endif()

# Cross-platform core libraries
target_link_libraries(z_generals ${_z_generals_scope}
    core_profile_legacy
    d3dx8
    z_gameengine
    z_gameenginedevice
    zi_always
)

# Linux SDL3 windowing/input (Phase 1)
# GeneralsX @build BenderAI 11/02/2026
if(SAGE_USE_SDL3)
    target_link_libraries(z_generals ${_z_generals_scope} sdl3lib)
endif()

# GeneralsX @feature Android port 10/07/2026 libadrenotools (custom Vulkan
# driver loading, see cmake/adrenotools.cmake) -- linked into libmain.so
# itself since TryLoadCustomVulkanDriver() in SDL3Main.cpp must call
# adrenotools_open_libvulkan() before DXVK's separately-built .so gets a
# chance to dlopen the stock libvulkan.so on its own.
if(ANDROID)
    target_link_libraries(z_generals PRIVATE adrenotools)
endif()

# GeneralsX @build Android port GLES experiment - statically linked
# alternative D3D8 backend (native GLES3 via SDL3, see
# Core/Libraries/Source/d3d8gles/), selected at runtime instead of DXVK's
# dlopen'd D3D8->Vulkan translation -- see DX8Wrapper::Init()'s backend
# switch in dx8wrapper.cpp.
if(ANDROID)
    target_link_libraries(z_generals PRIVATE d3d8gles)
endif()

# GeneralsX @build visionOS port: the same statically linked D3D8 -> GLES3 backend
# (d3d8gles), here running on ANGLE's Metal backend instead of a native GLES
# driver. The d3d8gles target is defined by Core/Libraries/Source/d3d8gles (added
# before this directory); link it when it exists and say so when it does not, so
# the archive of an engine without a renderer is never mistaken for a complete one.
if(SAGE_BUILD_VISIONOS_LIB)
    if(TARGET d3d8gles)
        target_link_libraries(z_generals PUBLIC d3d8gles)
    else()
        message(WARNING "visionOS: target d3d8gles does not exist; libGeneralsZHEngine will have no D3D8 backend "
                        "(Direct3DCreate8_GLES stays undefined until Core/Libraries/Source/d3d8gles supports visionOS).")
    endif()
endif()

# GeneralsX @spike XR port Phase 0.4 - OpenXR loader (vcpkg openxr-loader,
# static) for the native XR tabletop. Android-only: the XR hello loop and
# everything after it lives in XrHello.cpp behind the same gate.
if(ANDROID)
    find_package(OpenXR CONFIG REQUIRED)
    target_link_libraries(z_generals PRIVATE OpenXR::openxr_loader EGL GLESv3 log)
endif()

# GeneralsX @feature Android port 12/07/2026 CI passes the GitHub Actions run
# number (the same value baked into the artifact filename, e.g.
# GeneralsXZH-android-run108.apk) via -DANDROID_CI_BUILD_NUMBER so it can be
# logged at app startup -- without this a crash log has no way to prove which
# build produced it, which has already caused us to misattribute a tester's
# log to the wrong build once. Local/non-CI builds don't pass the define, so
# default to 0.
if(NOT DEFINED ANDROID_CI_BUILD_NUMBER)
    set(ANDROID_CI_BUILD_NUMBER 0)
endif()

# TODO Originally referred to build host and user, replace with git info perhaps?
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/GeneratedVersion.h
"#pragma once

#define VERSION_LOCALBUILDNUM 0
#define VERSION_BUILDUSER \"\"
#define VERSION_BUILDLOC \"\"
#define ANDROID_CI_BUILD_NUMBER ${ANDROID_CI_BUILD_NUMBER}
"
)

# Based on original binary values for these variables.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BuildVersion.h
"#pragma once

#define VERSION_MAJOR 1
#define VERSION_MINOR 4
#define VERSION_BUILDNUM 601
"
)

if(MSVC)
    target_link_options(z_generals PRIVATE "/NODEFAULTLIB:libci.lib")
endif()

target_include_directories(z_generals PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_sources(z_generals PRIVATE
    $<$<BOOL:${WIN32}>:WinMain.cpp>
    $<$<BOOL:${WIN32}>:WinMain.h>
    # Linux SDL3 entry point (Phase 1.5 - TheSuperHackers @build 10/02/2026)
    # Not part of the visionOS static library: it defines main(), the Vulkan ICD
    # picker and the SDL window bootstrap; the host app replaces all of that.
    $<$<AND:$<NOT:$<BOOL:${WIN32}>>,$<NOT:$<BOOL:${SAGE_BUILD_VISIONOS_LIB}>>>:SDL3Main.cpp>
    # GeneralsX @build visionOS port: globals + CreateGameEngine() the engine
    # libraries link-depend on, moved out of SDL3Main.cpp (pure move, see the file).
    $<$<NOT:$<BOOL:${WIN32}>>:EngineGlobals.cpp>
    # Linux stubs for P2-P3 platform functions (P2-P3 - TheSuperHackers @build 13/02/2026 BenderAI Session 37)
    $<$<NOT:$<BOOL:${WIN32}>>:LinuxStubs.cpp>
    # GeneralsX @build Android port 07/07/2026 Installs as soon as libmain.so is
    # dlopen'd (ELF constructor), before any engine code — captures crashes that
    # happen before SDL3Main.cpp's own logging is ready.
    $<$<BOOL:${ANDROID}>:AndroidCrashHandler.cpp>
    # GeneralsX @spike XR port Phase 0.4 - native OpenXR hello loop, driven
    # by XrHelloActivity (own icon/process); the game never starts there.
    $<$<BOOL:${ANDROID}>:XrHello.cpp>
    # GeneralsX @feature XR port Phase 1.0 - offscreen game boot + frame
    # driver for the XR tabletop (XrHello.cpp calls into this).
    $<$<BOOL:${ANDROID}>:XrGameBoot.cpp>
)

# RC files optional for MinGW builds
# Icon: LoadIcon() handles missing resource gracefully (uses default system icon)
# Manifest: DPI awareness metadata (nice to have but not essential)
if(MSVC)
    # VS2005 and later adds default manifest, we need to turn it off to prevent conflict with custom manifest
    if(NOT IS_VS6_BUILD)
        target_link_options(z_generals PRIVATE
            "/MANIFEST:NO"
        )
    endif()

    target_sources(z_generals PRIVATE
        RTS.RC
    )
endif()

# Strip debug symbols to separate file for MinGW Release builds
# This creates generalszh${RTS_BUILD_OUTPUT_SUFFIX}.exe.debug (similar to MSVC .pdb files)
if(MINGW AND COMMAND add_debug_strip_target)
    add_debug_strip_target(z_generals)
endif()

# GeneralsX @bugfix Android port 16/07/2026 Hide the engine's global operator
# new/delete from libmain.so's dynamic symbol table via a linker version script.
#
# On Android all loaded libraries share a flat symbol namespace, so libmain.so's
# exported operator new/delete interpose over the copies inside DXVK
# (libdxvk_d3d8.so), SDL3 and OpenAL. Our operator new (GameMemoryNull.cpp)
# memset()s every allocation to zero before returning it -- harmless for the
# engine's own objects, but it corrupts DXVK's internal resource-pool
# bookkeeping (DxvkResourceAllocationPool), which stores live free-list state in
# memory it manages itself and does not expect a foreign new/delete to touch.
# The corruption is silent and only bites frames later, when DXVK next allocates
# a GPU resource and dereferences a zeroed/garbage free-list pointer -- matching
# the silent, delayed, post-menu crash reported in issue #2 (no signal reaches
# our crash handler). Diagnosis and version-script approach independently found
# by a sibling GeneralsX Android fork (tarek369/GeneralsZH-Android, commit
# 467d129) on Exynos/Adreno hardware.
#
# Marking the mangled operator new/new[]/delete/delete[] symbols "local" keeps
# them internal to libmain.so: the engine still resolves its own calls to them
# statically, while DXVK/SDL3/OpenAL fall back to libc++'s plain malloc-based
# operators. Safe here specifically because RTS_GAMEMEMORY_ENABLE is OFF on
# Android (GameMemoryNull's operators are already malloc/free under the hood, so
# cross-library alloc/free stays consistent).
if(ANDROID)
    set(_gx_hide_alloc_map "${CMAKE_CURRENT_BINARY_DIR}/gx_hide_alloc.map")
    file(WRITE "${_gx_hide_alloc_map}" [=[
{
  global:
    *;
  local:
    _Znw*;
    _Zna*;
    _Zdl*;
    _Zda*;
};
]=])
    target_link_options(z_generals PRIVATE
        "-Wl,--version-script,${_gx_hide_alloc_map}"
    )
endif()

# ---------------------------------------------------------------------------------------
# GeneralsX @build visionOS port: sources and helper targets of the static engine library
# ---------------------------------------------------------------------------------------
if(SAGE_BUILD_VISIONOS_LIB)
    # SDL3 is built with SDL_VIDEO=OFF, see SDLVisionStubs.cpp.
    target_sources(z_generals PRIVATE SDLVisionStubs.cpp)

    # The host boundary and the C++ game boot (VisionGameBoot.cpp, GXEngineHost.mm, ...)
    # live in visionos/ and are added by the packages that own them; CONFIGURE_DEPENDS
    # re-globs on every build so a new file there needs no CMake edit. The directory
    # currently holds only a README.md, in which case nothing is added.
    file(GLOB _visionos_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/visionos/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/visionos/*.mm")
    if(_visionos_sources)
        target_sources(z_generals PRIVATE ${_visionos_sources})
    endif()
    # Objective-C++ (.mm) needs the OBJCXX language, enabled at the top level in
    # cmake/platform.cmake. Compile the bridge with ARC; the C++ engine sources are not
    # Objective-C and are unaffected.
    target_compile_options(z_generals PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

    # SDL's main-callback helper (src/main/ios/SDL_sysmain_callbacks.m) uses CADisplayLink,
    # which lives in QuartzCore; SDL3's own CMake does not declare that framework. It is
    # only linked when something pulls that member in (an -ObjC host link), but then the
    # framework must be there. The host app links QuartzCore anyway.
    target_link_libraries(z_generals PUBLIC "-framework QuartzCore")

    # Public headers for host code that includes engine headers (BaseType.h etc.).
    target_include_directories(z_generals PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/visionos)

    # ANGLE headers (EGL / GLES3 / the EGL_ANGLE_metal_texture_client_buffer extension
    # headers) for the code in this library that talks to ANGLE. GX_ANGLE_INCLUDE_DIR is
    # resolved in cmake/platform.cmake from the ANGLE install of the selected sysroot.
    if(GX_ANGLE_INCLUDE_DIR AND EXISTS "${GX_ANGLE_INCLUDE_DIR}")
        target_include_directories(z_generals PUBLIC "${GX_ANGLE_INCLUDE_DIR}")
    endif()

    # Never-built probe executable. Its only purpose is to make CMake/Ninja generate the
    # FULL resolved link line of everything z_generals needs (all static archives,
    # including the vcpkg ones, in link order, plus frameworks and -l flags).
    # make-xcframework.sh reads that line with `ninja -t commands z_generals_link_probe`
    # to merge the transitive static closure into libGeneralsZHEngine_all.a and to record
    # the system frameworks the host app must add. EXCLUDE_FROM_ALL: it would not link
    # (renderer and boot symbols come from other packages and the host).
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/z_generals_link_probe.cpp"
        "// generated: see GeneralsMD/Code/Main/CMakeLists.txt\nint main() { return 0; }\n")
    add_executable(z_generals_link_probe EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_BINARY_DIR}/z_generals_link_probe.cpp")
    target_link_libraries(z_generals_link_probe PRIVATE z_generals)
endif()
