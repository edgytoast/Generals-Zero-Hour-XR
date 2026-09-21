/*
 * GXEngineHost.h - the plain-C surface of the visionOS engine host (Swift-importable).
 *
 * The engine (boot: about a minute; map loads that block inside one engine frame for many seconds) runs on
 * ITS OWN THREAD ("GXXR.Engine"), which also owns the ANGLE/GL context. The compositor thread never runs
 * engine code and never touches GL; it composites the latest completed engine frame at display rate.
 * docs/visionos-engine-host.md has the thread model, the boot sequence, the mailbox/ring protocol and the
 * step-by-step guide for running with real game data.
 *
 * Everything here is thread safe unless a comment says otherwise. Strings are UTF-8, NUL terminated.
 */
#ifndef GX_ENGINE_HOST_H
#define GX_ENGINE_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where the engine is in its life. Only ever moves forward, except running <-> paused. */
typedef enum GXEngineHostPhase {
    GX_ENGINE_IDLE = 0,     /* not started */
    GX_ENGINE_BOOTING = 1,  /* thread started, engine initialising (progress says what) */
    GX_ENGINE_RUNNING = 2,  /* engine booted, frames are produced whenever the compositor supplies head poses */
    GX_ENGINE_PAUSED = 3,   /* booted, parked by GXEngineHost_Pause (audio silenced) */
    GX_ENGINE_FAILED = 4,   /* boot failed or the engine stopped; lastError explains, the log has the details */
    GX_ENGINE_STOPPING = 5  /* the game asked to quit / shutdown in progress */
} GXEngineHostPhase;

/* Why the engine is parked. Bits; the engine runs only while no bit is set. */
typedef enum GXEngineHostPauseReason {
    GX_PAUSE_LAYER = 1,       /* compositor layer paused / immersive space not running */
    GX_PAUSE_SCENE = 2,       /* SwiftUI scene inactive or in the background */
    GX_PAUSE_USER = 4         /* explicit request */
} GXEngineHostPauseReason;

/* Bits of GXEngineHostConfig.policy (same meaning as XrGameBootPolicy in XrGameBoot.h). */
enum {
    GX_ENGINE_POLICY_SEED_OPTIONS = 1u << 0,
    GX_ENGINE_POLICY_LOGIC_TIME_SCALE = 1u << 1,
    GX_ENGINE_POLICY_RENDER_CAP = 1u << 2,
    GX_ENGINE_POLICY_GUARD_TIME_KEYS = 1u << 3,
    GX_ENGINE_POLICY_DEFAULT = 0xFu
};

typedef struct GXEngineHostConfig {
    /* Paths from GXXRGameData_GetPaths (visionos/GameData/GXXRGameData.h). zhRoot and userDataRoot are required. */
    const char* zhRoot;         /* Zero Hour tree (contains INIZH.big): working directory + CNC_GENERALS_ZH_PATH */
    const char* baseRoot;       /* base Generals tree (CNC_GENERALS_PATH); NULL/"" = same as zhRoot */
    const char* userDataRoot;   /* saves, Options.ini, maps: GENERALSX_USERDATA_DIR */
    const char* appSupportRoot; /* layout / camera / language cfg files, HOME (registry.ini); created if missing */
    const char* logPath;        /* engine log file (stderr is teed into it); NULL = <appSupportRoot>/generals-xr-stderr.log */
    int renderWidth;            /* engine backbuffer (-xres/-yres); 0 = 1280 x 720 */
    int renderHeight;
    const char* textLanguage;   /* GENERALSX_TEXT_LANGUAGE override, NULL = game_language.cfg / default */
    unsigned policy;            /* GX_ENGINE_POLICY_*; 0 means GX_ENGINE_POLICY_DEFAULT */
    int logicHz;                /* simulation rate with the LOGIC_TIME_SCALE policy; 0 = 30 */
    int renderFpsCap;           /* engine frames per second with the RENDER_CAP policy; 0 = 45, < 0 = uncapped */
    bool forceAtlas;            /* pack both eyes into one stereo target (D3D8GLES_XRFLAG_FORCE_ATLAS) */
} GXEngineHostConfig;

typedef struct GXEngineHostStatus {
    GXEngineHostPhase phase;
    bool fake;                  /* the synthetic test client (-fakeEngine) is running instead of the real engine */
    bool servicesInstalled;     /* the app installed the ring/compositor services */
    bool waitingForCompositor;  /* booted, but no fresh head pose (immersive space closed or paused) */
    uint32_t pauseMask;         /* GXEngineHostPauseReason bits */
    char progress[160];         /* human readable, e.g. "Initializing the engine (about a minute)..." */
    char lastError[512];        /* why the boot failed; empty otherwise */
    char lastLogLine[256];      /* last line of the engine log (for the launcher while booting / after a failure) */
    char logPath[512];
    double bootSeconds;         /* seconds spent in the engine boot (so far, or total) */
    /* Rates over the last full window, measured on the engine thread. */
    double engineFps;           /* engine frames per second (published frames), 1 s window */
    double logicHz;             /* effective simulation frames per wall-clock second over the last 10 s window; 0 = n/a */
    bool logicInGame;           /* logicHz was measured during an unpaused match */
    uint64_t framesProduced;    /* engine frames published to the compositor since start */
    uint64_t framesSkipped;     /* frames dropped because no ring slot was free */
    double lastFrameMs;         /* wall time of the last engine frame (includes map-load stalls) */
    double longestFrameMs;      /* longest engine frame so far */
    /* Ring / sync (filled by the app's services once the ring exists). */
    uint32_t ringSlots;
    uint32_t ringSlotsInUse;
    double ringMegabytes;
    char syncMode[40];          /* "metal-shared-event" | "glFinish" | "" */
    char renderer[112];         /* GL_RENDERER */
} GXEngineHostStatus;

/* Starts the REAL engine. Non-blocking: spawns the engine thread, which creates the ANGLE context, boots the
 * engine through XrGameBoot_InitHost (about a minute) and then produces frames. The engine is not restart-safe:
 * one start per process. Returns false when already started or the config is unusable; boot failures are
 * reported asynchronously through GXEngineHost_GetStatus (phase FAILED, lastError). */
bool GXEngineHost_Start(const GXEngineHostConfig* config);

/* Latest status. Cheap; call as often as the UI refreshes. */
void GXEngineHost_GetStatus(GXEngineHostStatus* outStatus);

/* Sets or clears a pause reason. While any reason is set the engine thread parks after its current frame and
 * the engine is told to silence audio and release input (like SDL3GameEngine's DID_ENTER_BACKGROUND). */
void GXEngineHost_Pause(GXEngineHostPauseReason reason, bool paused);

/* Runs `work` on the engine thread, in order, before the next engine frame (UI actions that touch engine state).
 * Returns false when the engine thread is not running. */
#if defined(__BLOCKS__)
bool GXEngineHost_Post(void (^work)(void));
#endif

/* The engine log file (the path the launcher shows). Valid after GXEngineHost_BeginLogging or Start. */
const char* GXEngineHost_LogPath(void);

/* Starts logging into `path` right away (stderr is teed: the console still gets everything). The launcher
 * calls it at startup so the compositor / host lines from before the engine start are in the file too.
 * Idempotent. Start() does it for config.logPath if it was not done. */
void GXEngineHost_BeginLogging(const char* path);

/* Copies up to `maxLines` last lines of the log (newest last) into `out` (NUL terminated, truncated). */
size_t GXEngineHost_ReadLogTail(char* out, size_t capacity, int maxLines);

/* True when an engine (real or fake) has been started: the compositor then presents engine frames (or the
 * loading indicator) instead of the built-in test scene. */
bool GXEngineHost_IsActive(void);

#ifdef __cplusplus
}
#endif

#endif /* GX_ENGINE_HOST_H */
