set(GS_OPENSSL FALSE)
set(GAMESPY_SERVER_NAME "server.cnc-online.net")

FetchContent_Declare(
    gamespy
    GIT_REPOSITORY https://github.com/TheAssemblyArmada/GamespySDK.git
    GIT_TAG        07e3d15c500415abc281efb74322ab6d9c857eb8
)

# GeneralsX @build visionOS port: GamespySDK builds whatever BUILD_SHARED_LIBS says
# (SHARED by default). The visionOS host app links the engine statically and has no
# place to embed and re-sign a dylib, so force STATIC for this dependency only.
if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    set(_gx_saved_shared_libs "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF)
endif()

FetchContent_MakeAvailable(gamespy)

if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    set(BUILD_SHARED_LIBS "${_gx_saved_shared_libs}")
    unset(_gx_saved_shared_libs)
endif()

# GeneralsX @build Android port 07/07/2026 bionic ships no pthread_cancel;
# GamespySDK's gsthreadlinux.c calls it in its thread-cancel helper (a
# last-resort kill used at shutdown). Rewrite the call to a successful no-op
# at the preprocessor level — the SDK is a pinned remote FetchContent, so a
# source patch would need a fork for one line.
# NOTE: must be a raw compile OPTION, not target_compile_definitions --
# CMake silently drops function-style macros from COMPILE_DEFINITIONS
# (run #8 showed the flag missing from the gscommon compile line).
if(ANDROID AND TARGET gscommon)
    target_compile_options(gscommon PRIVATE "-Dpthread_cancel(t)=(0)")
endif()
