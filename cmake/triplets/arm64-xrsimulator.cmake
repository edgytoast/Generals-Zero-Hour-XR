# Overlay triplet: visionOS Simulator (xrsimulator), arm64, static libraries.
#
# vcpkg ships a community triplet only for the device (arm64-visionos) and it
# sets neither a sysroot nor a deployment target. This triplet pins both, so
# every vcpkg-built static library carries platform VISIONOSSIMULATOR and
# minos 2.0, the same as the engine (CMAKE_OSX_DEPLOYMENT_TARGET in the
# visionos-simulator preset). The deployment target travels in the explicit
# "-target" flags at the end of this file; keep the "2.0" in them in sync with
# that preset value.
#
# Do NOT set VCPKG_OSX_DEPLOYMENT_TARGET here. CMake would then add its
# unexpanded "--target=<ARCH>-apple-xros<VERSION_MIN>-simulator" template flag
# to every port, and the make-based ones (openssl) die with
# "/bin/sh: ARCH: No such file or directory" because "<ARCH>" is parsed as a
# shell redirection.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME visionOS)
set(VCPKG_OSX_SYSROOT xrsimulator)
set(VCPKG_OSX_ARCHITECTURES arm64)
# Release libraries only: the app links optimised static archives, and building
# the Debug flavour as well would double the (long) FFmpeg/protobuf/abseil builds.
set(VCPKG_BUILD_TYPE release)

# CMake >= 3.28 expresses the visionOS deployment target flag as the unexpanded
# template "--target=<ARCH>-apple-xros<VERSION_MIN>-simulator"
# (Modules/Platform/Apple-Clang.cmake). vcpkg's make/autotools helpers (ffmpeg,
# openssl) copy CMAKE_C_FLAGS verbatim and the compiler then fails with
# "unknown target triple 'unknown-apple-xros1.0.0-simulator'" (or the shell
# error above). Spelling the target triple out fixes every make-based port;
# CMake-based ports are unaffected.
set(VCPKG_C_FLAGS "-target arm64-apple-xros2.0-simulator")
set(VCPKG_CXX_FLAGS "-target arm64-apple-xros2.0-simulator")
set(VCPKG_LINKER_FLAGS "-target arm64-apple-xros2.0-simulator")
