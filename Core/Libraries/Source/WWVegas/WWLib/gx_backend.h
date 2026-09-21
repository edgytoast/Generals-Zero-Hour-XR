/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
** gx_backend.h - the two feature macros that replace bare __ANDROID__ tests at
** every site that is really about "the native GLES3 D3D8 backend is in use" or
** "an XR host owns the frame loop", not about Android itself.
**
** GeneralsX @feature visionOS port - the Meta Quest build (Android, native
** OpenXR + GLES3) and the Apple Vision Pro build (visionOS, Compositor
** Services + ANGLE-on-Metal) run the same engine, the same d3d8gles backend
** and the same XR engine hooks. Those hooks used to be guarded by
** __ANDROID__ because Android was the only platform that had them; they are
** now guarded by the macros below so both platforms compile them in.
**
**   GX_XR_HOST         The engine is driven by an XR host that owns the frame loop
**                      and renders without an SDL window: the GX_XR_* engine hooks
**                      (GX_XR_BeginStereoWorld, GX_XR_PointerRay, GX_XR_CullSphere,
**                      GX_XR_OffscreenBoot, ...) are compiled in and the host must
**                      define them. True on Android (Quest) and, when CMake defines
**                      GX_PLATFORM_VISIONOS, on visionOS.
**   GX_USES_D3D8GLES   Core/Libraries/Source/d3d8gles is the D3D8 device
**                      implementation (Direct3DCreate8_GLES, d3d8gles_* C API).
**                      True on Android and wherever the build defines
**                      GX_D3D8GLES_BACKEND (the d3d8gles CMake target defines it
**                      PUBLIC; on visionOS this header also derives it from
**                      GX_PLATFORM_VISIONOS so a consumer that does not link the
**                      target directly still sees the same answer).
**
** Rule for editing a site: if the code is only meaningful on Android (adb log
** tags, /sdcard paths, JNI, ANativeWindow, Mali/Adreno driver workarounds,
** Android storage layout) it stays on __ANDROID__. If it is a Quest XR hook or
** a d3d8gles hook it uses these macros. Android behaviour must stay logically
** identical: on Android both macros are 1, exactly like the __ANDROID__ they
** replace.
*/

#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// visionOS is built with the d3d8gles backend on ANGLE (Metal); see
// docs/visionos-gles-backend.md.
#if defined(GX_PLATFORM_VISIONOS) && !defined(GX_D3D8GLES_BACKEND)
#define GX_D3D8GLES_BACKEND 1
#endif

#if defined(__ANDROID__) || defined(GX_PLATFORM_VISIONOS)
#ifndef GX_XR_HOST
#define GX_XR_HOST 1
#endif
#endif

#if defined(__ANDROID__) || defined(GX_D3D8GLES_BACKEND)
#ifndef GX_USES_D3D8GLES
#define GX_USES_D3D8GLES 1
#endif
#endif
