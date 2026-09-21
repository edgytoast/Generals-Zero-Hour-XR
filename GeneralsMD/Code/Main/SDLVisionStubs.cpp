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
** SDLVisionStubs.cpp
**
** GeneralsX @build visionOS port
**
** SDL3 is built for visionOS with SDL_VIDEO=OFF (its UIKit video driver does not
** compile against the xros SDK, and the SwiftUI + Compositor Services host owns
** windowing; see cmake/sdl3.cmake). The functions below are defined by that UIKit
** driver yet are still referenced by SDL's core, so without the driver the final
** link reports them undefined:
**
**   SDL_IsIPad / SDL_IsAppleTV   (src/video/uikit/SDL_uikitvideo.m), referenced by
**                                SDL_IsTablet / SDL_IsTV in src/SDL.c. The host is a
**                                Vision Pro: neither an iPad nor an Apple TV.
**   SDL_UpdateLifecycleObserver  (src/video/uikit/SDL_uikitevents.m), referenced by
**                                src/main/ios/SDL_sysmain_callbacks.m.o. Only linked
**                                when SDL's main-callback helper is pulled in (for
**                                example by an -ObjC link); there is no UIKit video
**                                driver whose lifecycle it could observe, so no-op.
**
** Only compiled into the visionOS static engine library.
*/

#if defined(GX_PLATFORM_VISIONOS)

extern "C" {

bool SDL_IsIPad(void)
{
	return false;
}

bool SDL_IsAppleTV(void)
{
	return false;
}

void SDL_UpdateLifecycleObserver(void)
{
}

} // extern "C"

#endif // GX_PLATFORM_VISIONOS
