/*
 * GXXRGameData.h - C surface of the game-data subsystem.
 *
 * Two audiences:
 *
 *  1. The engine bridge (visionos/Bridge, package D1). Before booting the engine it asks
 *     whether validated game data is available and where it lives:
 *
 *        if (!GXXRGameData_IsReady()) { ...show "no data", stay on the test scene... }
 *        char zh[1024], base[1024], user[1024];
 *        GXXRGameData_GetPaths(zh, sizeof zh, base, sizeof base, user, sizeof user);
 *        setenv("CNC_GENERALS_ZH_PATH", zh, 1);     // primary asset root (mounted first)
 *        setenv("CNC_GENERALS_PATH",   base, 1);    // base Generals (== zh when merged)
 *        setenv("GENERALSX_USERDATA_DIR", user, 1); // optional; the Apple branch of
 *                                                   // GlobalData already resolves the same dir
 *        chdir(zh);
 *
 *     All functions are thread-safe and cheap; they only read a snapshot published by the
 *     app model when validation succeeded. Paths are UTF-8, NUL-terminated, without a
 *     trailing slash. Security-scoped access (only used by the advanced "use in place"
 *     mode) is held open by the app for the whole process lifetime, so the paths stay
 *     readable while the process runs.
 *
 *  2. The shell (AppModel). GXXRGameData_SetPaths / GXXRGameData_Clear publish or retract
 *     the snapshot.
 */
#ifndef GXXR_GAME_DATA_H
#define GXXR_GAME_DATA_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when a validated Zero Hour + Generals install is available. */
bool GXXRGameData_IsReady(void);

/* Copies the resolved roots. Returns false (and writes empty strings) when not ready or
 * when a buffer is too small. Any buffer may be NULL with capacity 0 to skip that path.
 *   zhRoot       Zero Hour tree (contains INIZH.big)      -> CNC_GENERALS_ZH_PATH
 *   baseRoot     base Generals tree (contains Terrain.big) -> CNC_GENERALS_PATH
 *                (identical to zhRoot when both games live in one folder)
 *   userDataRoot writable folder for saves, Options.ini, maps, logs
 *                (<Application Support>/GeneralsX/GeneralsZH) */
bool GXXRGameData_GetPaths(char* zhRoot, size_t zhCap, char* baseRoot, size_t baseCap, char* userDataRoot,
                           size_t userCap);

/* Shell side: publish / retract the snapshot. */
void GXXRGameData_SetPaths(const char* zhRoot, const char* baseRoot, const char* userDataRoot);
void GXXRGameData_Clear(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GXXR_GAME_DATA_H */
