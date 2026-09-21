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
** EngineGlobals.cpp
**
** GeneralsX @build visionOS port
**
** Global definitions the engine libraries link-depend on but that historically lived
** in SDL3Main.cpp next to main(): __argc/__argv, ApplicationHWnd, TheSDL3Window,
** g_csfFile, g_strFile and the CreateGameEngine() factory. They are moved here
** VERBATIM (no behaviour change) so that a build without SDL3Main.cpp -- the visionOS
** static engine library, which is driven by a host app and has no main() -- can still
** resolve them. Every other non-Windows z_generals target keeps linking SDL3Main.cpp
** and now gets these definitions from this file instead (see CMakeLists.txt).
**
** The set was measured with nm: the symbols defined by SDL3Main.cpp that the engine
** archives leave undefined are exactly __argc, __argv, ApplicationHWnd,
** TheSDL3Window, g_csfFile, g_strFile and CreateGameEngine().
*/

#ifndef _WIN32

// SYSTEM INCLUDES
#include <SDL3/SDL.h>
#include <cstdio>

// USER INCLUDES
#include "Lib/BaseType.h"
#include "Common/GameEngine.h"
#include "Common/GameMemory.h"
#include "SDL3GameEngine.h"

// GLOBAL COMMAND LINE ARGUMENTS
// TheSuperHackers @build felipebraz 13/02/2026
// Store argc/argv from main() for use by CommandLine.cpp parseCommandLine() on Linux
// Windows provides these automatically; Linux needs explicit globals
int __argc = 0;          ///< global argument count
char** __argv = nullptr; ///< global argument vector

// GLOBAL WINDOW HANDLE
// TheSuperHackers @build felipebraz 13/02/2026
// ApplicationHWnd is declared extern in GeneralsMD/Code/Main/WinMain.h
// On Linux, we cast SDL_Window* to HWND type for compatibility
HWND ApplicationHWnd = nullptr;  ///< our application window handle

// GLOBAL SDL3 WINDOW
// GeneralsX @feature felipebraz 16/02/2026
// SDL3 window created in main() before GameMain(), stored globally for engine access
SDL_Window* TheSDL3Window = nullptr;

// GAME TEXT FILE PATHS
// TheSuperHackers @build felipebraz 13/02/2026
// GameText.cpp uses these paths to load CSF and STR files (game localization)
// Format %s is replaced with language code in GameTextManager::init()
// GeneralsX @bugfix BenderAI 13/02/2026 - Fix case-sensitivity on Linux (generals.csf vs Generals.csf)
const Char *g_csfFile = "data/%s/generals.csf";  ///< CSF file path (lowercase for Linux compatibility)
// GeneralsX @feature Android port 09/09/2026 Per-language, like g_csfFile above it.
// GameTextManager::init() fills the %s in with the current language, and prefers this
// plain-text file over the compiled .csf when it exists -- which is what makes a language
// pack a text file someone can translate and send as a pull request, rather than a binary
// nobody can review. See languages/README.md.
const Char *g_strFile = "data/%s/generals.str";  ///< STR file path, per language

/**
 * CreateGameEngine
 *
 * Factory function for SDL3GameEngine on Linux.
 * Called by GameMain() to instantiate platform-specific engine.
 *
 * @return SDL3GameEngine instance
 */
GameEngine *CreateGameEngine(void)
{
	fprintf(stderr, "INFO: CreateGameEngine() - Creating SDL3GameEngine for Linux\n");
	SDL3GameEngine *engine = NEW SDL3GameEngine();
	return engine;
}

#endif // !_WIN32
