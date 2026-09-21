# Platform detection for the native visionOS port.
#
# Included right after project() in the top level CMakeLists.txt.
#
# GX_PLATFORM_VISIONOS
#     TRUE when cross-compiling for visionOS (CMAKE_SYSTEM_NAME=visionOS, i.e. the
#     xros device SDK or the xrsimulator SDK; needs CMake >= 3.28). It is also
#     passed to the compiler as GX_PLATFORM_VISIONOS=1 for every target, so C++
#     can use `#if defined(GX_PLATFORM_VISIONOS)`.
#     Note: on visionOS TARGET_OS_IPHONE and TARGET_OS_VISION are 1 while
#     TARGET_OS_IOS is 0, and CMake's IOS variable is NOT set. Code and CMake
#     that mean "iOS-like" keep testing TARGET_OS_IPHONE / CMAKE_SYSTEM_NAME
#     explicitly; only visionOS-specific code uses this flag.
#
# SAGE_BUILD_VISIONOS_LIB
#     Build z_generals as a STATIC library (libGeneralsZHEngine.a, no main())
#     that the SwiftUI + Compositor Services host app links, instead of an
#     executable. Defaults to ON on visionOS; it is an error to turn it on for any
#     other platform. The static library is driven by the host (init / frame /
#     shutdown), see GeneralsMD/Code/Main/visionos/README.md.
if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    set(GX_PLATFORM_VISIONOS TRUE)
    if(CMAKE_VERSION VERSION_LESS 3.28)
        message(FATAL_ERROR "Building for visionOS needs CMake >= 3.28 (found ${CMAKE_VERSION}).")
    endif()
    add_compile_definitions(GX_PLATFORM_VISIONOS=1)

    # ANGLE (GLES3 on Metal) headers and libraries for the selected sysroot, from the
    # install made by scripts/build/visionos/build-angle.sh:
    #   <root>/{xrsimulator,xros}/{include,lib}
    # The root comes from -DGX_ANGLE_ROOT=..., else $GX_ANGLE_INSTALL, else
    # $GX_DEPS_ROOT/angle/install (scripts/build/visionos/env.sh exports both).
    # These variables are optional inputs for the targets that talk to ANGLE
    # (d3d8gles, the host bridge); the engine archive itself does not need ANGLE.
    if(NOT GX_ANGLE_ROOT)
        if(DEFINED ENV{GX_ANGLE_INSTALL} AND NOT "$ENV{GX_ANGLE_INSTALL}" STREQUAL "")
            set(GX_ANGLE_ROOT "$ENV{GX_ANGLE_INSTALL}")
        elseif(DEFINED ENV{GX_DEPS_ROOT} AND NOT "$ENV{GX_DEPS_ROOT}" STREQUAL "")
            set(GX_ANGLE_ROOT "$ENV{GX_DEPS_ROOT}/angle/install")
        endif()
    endif()
    # CMake resolves CMAKE_OSX_SYSROOT to the full SDK path after project(), so derive the
    # short SDK name (xrsimulator / xros) that names the ANGLE install subdirectory.
    string(TOLOWER "${CMAKE_OSX_SYSROOT}" _gx_sysroot_lc)
    if(_gx_sysroot_lc MATCHES "xrsimulator")
        set(GX_VISIONOS_SDK xrsimulator)
    elseif(_gx_sysroot_lc MATCHES "xros")
        set(GX_VISIONOS_SDK xros)
    else()
        message(FATAL_ERROR "visionOS: CMAKE_OSX_SYSROOT must be xros or xrsimulator (got '${CMAKE_OSX_SYSROOT}').")
    endif()
    unset(_gx_sysroot_lc)
    if(GX_ANGLE_ROOT AND EXISTS "${GX_ANGLE_ROOT}/${GX_VISIONOS_SDK}/include")
        set(GX_ANGLE_INCLUDE_DIR "${GX_ANGLE_ROOT}/${GX_VISIONOS_SDK}/include")
        set(GX_ANGLE_LIB_DIR "${GX_ANGLE_ROOT}/${GX_VISIONOS_SDK}/lib")
        message(STATUS "ANGLE: ${GX_ANGLE_ROOT}/${GX_VISIONOS_SDK}")
    else()
        message(STATUS "ANGLE: not found (GX_ANGLE_ROOT='${GX_ANGLE_ROOT}'); only needed by the renderer and the host app")
    endif()
    message(STATUS "Platform: visionOS (sysroot '${CMAKE_OSX_SYSROOT}', deployment target '${CMAKE_OSX_DEPLOYMENT_TARGET}')")
else()
    set(GX_PLATFORM_VISIONOS FALSE)
endif()

option(SAGE_BUILD_VISIONOS_LIB
    "Build z_generals as a static library for the native visionOS host app (visionOS only)"
    ${GX_PLATFORM_VISIONOS})
if(SAGE_BUILD_VISIONOS_LIB AND NOT GX_PLATFORM_VISIONOS)
    message(FATAL_ERROR "SAGE_BUILD_VISIONOS_LIB requires CMAKE_SYSTEM_NAME=visionOS (use the visionos-simulator or visionos-device preset).")
endif()
