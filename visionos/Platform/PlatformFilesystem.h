/*
 * PlatformFilesystem.h - where the engine may read and write on a sandboxed OS.
 *
 * All paths are UTF-8, NUL-terminated, without a trailing slash. Functions
 * return false (and write an empty string) when the location cannot be
 * resolved. No game data ships in the app: the player supplies their own
 * legally owned Generals / Zero Hour install.
 *
 * visionOS mapping (see visionos/Bridge/GXXRPlatformFilesystem.mm):
 *   app_data   -> <Application Support>/GeneralsZHXR      (private, backed up)
 *   caches     -> <Library/Caches>/GeneralsZHXR            (purgeable)
 *   game_data  -> <Documents>/GameData                     (Files-app visible via
 *                 UIFileSharingEnabled; player copies Data/, Maps/ etc. here)
 *   or a user-picked folder kept alive by a security-scoped bookmark
 *   (LSSupportsOpeningDocumentsInPlace), see the bookmark functions.
 */
#ifndef GX_PLATFORM_FILESYSTEM_H
#define GX_PLATFORM_FILESYSTEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writable, private, persistent. Created on first call. */
bool PlatformFS_GetAppDataDir(char* out_path, size_t capacity);
/* Writable, purgeable. Created on first call. */
bool PlatformFS_GetCacheDir(char* out_path, size_t capacity);
/* Root that contains the user's game files. Prefers a bookmarked folder when one
 * has been set and resolves; otherwise the app's shared Documents/GameData
 * (created on first call so it shows up in the Files app). */
bool PlatformFS_GetGameDataDir(char* out_path, size_t capacity);

/* True when GetGameDataDir looks populated (heuristic: contains any entry). */
bool PlatformFS_GameDataLooksPresent(void);

/* Opaque handle for a security-scoped access session. */
typedef struct PlatformFSAccess* PlatformFSAccessHandle;

/* Store / clear a bookmark for a user-picked folder. `bookmark` is the opaque
 * blob the shell produced from the picked URL; the engine never inspects it. */
bool PlatformFS_SetGameDataBookmark(const void* bookmark, size_t length);
void PlatformFS_ClearGameDataBookmark(void);

/* Start/stop security-scoped access to the resolved game data folder. Calls nest.
 * Begin returns NULL when no scoped access is needed or possible (e.g. the
 * Documents/GameData default) - that is not an error; paths are still readable. */
PlatformFSAccessHandle PlatformFS_BeginGameDataAccess(void);
void PlatformFS_EndGameDataAccess(PlatformFSAccessHandle handle);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GX_PLATFORM_FILESYSTEM_H */
