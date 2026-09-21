# Overlay triplet: visionOS device (xros), arm64, static libraries.
#
# See arm64-xrsimulator.cmake for the reasoning, including why
# VCPKG_OSX_DEPLOYMENT_TARGET is deliberately not set. This is the same triplet
# with the device sysroot and target triple. Keep the "2.0" in the -target
# flags below in sync with CMAKE_OSX_DEPLOYMENT_TARGET in the visionos-device
# preset.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME visionOS)
set(VCPKG_OSX_SYSROOT xros)
set(VCPKG_OSX_ARCHITECTURES arm64)
# Release libraries only: the app links optimised static archives, and building
# the Debug flavour as well would double the (long) FFmpeg/protobuf/abseil builds.
set(VCPKG_BUILD_TYPE release)

# Explicit target triple for make/autotools based ports, see arm64-xrsimulator.cmake.
set(VCPKG_C_FLAGS "-target arm64-apple-xros2.0")
set(VCPKG_CXX_FLAGS "-target arm64-apple-xros2.0")
set(VCPKG_LINKER_FLAGS "-target arm64-apple-xros2.0")
