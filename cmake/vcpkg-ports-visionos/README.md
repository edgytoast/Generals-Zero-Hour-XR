# visionOS-only vcpkg overlay ports

These ports are added to `VCPKG_OVERLAY_PORTS` by the `visionos-*` CMake presets
only (next to `cmake/vcpkg-ports`). They are kept apart from `cmake/vcpkg-ports`
so that the Android preset, which consumes that directory, never sees them.

* `openssl/` is a copy of vcpkg's openssl 3.4.1 port exactly as it appears at the
  manifest baseline (`builtin-baseline` in `vcpkg.json`), plus one added
  `elseif(VCPKG_TARGET_IS_VISIONOS)` branch in `unix/portfile.cmake` (upstream
  aborts with "Unknown platform" on visionOS). When the manifest baseline moves,
  re-copy the port from the new baseline and re-apply that branch.
