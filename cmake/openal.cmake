# GeneralsX @build fbraz 24/02/2026
# GeneralsX @bugfix fbraz 10/03/2026 Use FetchContent for ALL platforms (macOS, Linux, Windows)
# OpenAL audio library via FetchContent (openal-soft v1.24.2)
#
# On Linux, openal-soft is managed via vcpkg (see vcpkg.json). The vcpkg build compiles
# openal-soft with ALSA-only backend (no PipeWire, no PulseAudio), which avoids a SIGSEGV
# crash in the system libopenal1 1.25.1 Debian package. find_package(OpenAL) picks up the
# vcpkg-installed version automatically when the vcpkg toolchain is active.
#
# On macOS, CMake's FindOpenAL prefers Apple's deprecated OpenAL.framework which uses
# <OpenAL/al.h> instead of the standard <AL/al.h> expected by the Linux-compatible code.
# Prefer openal-soft (brew install openal-soft) which matches the Linux layout.
# Strategy: FetchContent for ALL platforms -- no Homebrew/system detection.
# - macOS:   CoreAudio backend. Compiled natively (arm64 on Apple Silicon).
#            Apple's deprecated OpenAL.framework is avoided -- it uses <OpenAL/al.h>
#            which is incompatible with the standard <AL/al.h> used throughout the codebase.
#            Homebrew openal-soft was unreliable: Intel Homebrew (/usr/local) installs
#            x86_64-only binaries that fail to link against native arm64 builds.
# - Linux:   ALSA/PipeWire backend.
# - Windows: WASAPI backend (modern, low-latency).
#
# FetchContent_MakeAvailable is idempotent: safe to include from multiple CMakeLists.
# Callers guard with: if(NOT TARGET OpenAL::OpenAL) find_package... endif()
#
# Reference: jmarshall OpenAL implementation uses <AL/al.h> throughout.

if(SAGE_USE_OPENAL)
    message(STATUS "Configuring OpenAL Soft (v1.24.2) with FetchContent...")

    include(FetchContent)

    set(_openal_patch_args)
    if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
        # GeneralsX @build visionOS port: openal-soft 1.24.2 does not know visionOS.
        # Two one-line fixes, stored as cmake/patches/openal-soft-1.24.2-visionos.patch:
        #  - alc/backends/coreaudio.cpp includes IOKit/audio/IOAudioTypes.h unless
        #    TARGET_OS_IOS/TARGET_OS_TV; that header does not exist in the xros SDKs
        #    (add TARGET_OS_VISION so device enumeration is compiled out).
        #  - CMakeLists.txt links -framework AudioUnit,ApplicationServices (macOS
        #    only) unless the system name is iOS/tvOS; add visionOS to that regex.
        set(_openal_patch_args PATCH_COMMAND ${CMAKE_COMMAND}
            "-DPATCH_FILE=${CMAKE_SOURCE_DIR}/cmake/patches/openal-soft-1.24.2-visionos.patch"
            -P "${CMAKE_SOURCE_DIR}/cmake/patches/apply-patch.cmake")
        # A static archive is what the host app links (openal-soft defaults to
        # SHARED unless LIBTYPE says otherwise).
        set(LIBTYPE STATIC)
        # RTKit (realtime thread priority through D-Bus) is a Linux feature. Left ON,
        # openal-soft picks up whatever dbus pkg-config finds on the BUILD HOST (Homebrew's
        # dbus, a macOS library) and compiles against its headers.
        set(ALSOFT_RTKIT OFF CACHE BOOL "No RTKit/D-Bus on visionOS" FORCE)
    endif()

    FetchContent_Declare(
        openal_soft
        URL "https://github.com/kcat/openal-soft/archive/refs/tags/1.24.2.tar.gz"
        URL_HASH "SHA256=7efd383d70508587fbc146e4c508771a2235a5fc8ae05bf6fe721c20a348bd7c"
        ${_openal_patch_args}
    )

    # Minimal build: no utilities, examples, or tests
    set(ALSOFT_INSTALL_RUNTIME_LIBS  ON  CACHE BOOL "Install runtime libs" FORCE)
    set(ALSOFT_EXAMPLES              OFF CACHE BOOL "Build examples"       FORCE)
    set(ALSOFT_TESTS                 OFF CACHE BOOL "Build tests"          FORCE)
    set(ALSOFT_UTILS                 OFF CACHE BOOL "Build utils"          FORCE)
    set(ALSOFT_NO_CONFIG_UTIL        ON  CACHE BOOL "Disable config util"  FORCE)

    if(WIN32)
        # Windows: WASAPI is the modern low-latency audio API
        set(ALSOFT_REQUIRE_WASAPI ON CACHE BOOL "Require WASAPI backend on Windows" FORCE)
    endif()

    FetchContent_MakeAvailable(openal_soft)

    # Force the vendored fmt 11.1.1 headers ahead of any system include dirs.
    # A Homebrew fmt (e.g. 12.x at /opt/homebrew/include) earlier on the include
    # path makes openal sources compile against fmt::v12 inline-namespace headers
    # while linking the vendored v11 static lib -> unresolved fmt::v12 symbols.
    foreach(_alsoft_tgt OpenAL alsoft.common alsoft.excommon)
        if(TARGET ${_alsoft_tgt})
            target_include_directories(${_alsoft_tgt} BEFORE PRIVATE
                "${openal_soft_SOURCE_DIR}/fmt-11.1.1/include")
        endif()
    endforeach()

    # openal-soft FetchContent creates the OpenAL::OpenAL imported target
    message(STATUS "OpenAL Soft configured: target OpenAL::OpenAL available")
endif()
