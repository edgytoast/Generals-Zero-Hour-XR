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
    /* Presentation (package C2; appended, older callers that zero-initialise keep working). */
    char presentationMode[24];  /* "loading" | "menu" | "cinematic" | "tabletop" | "ground-view" | "recovery" | "" */
    int32_t stereoEyeWidth;     /* current stereo target size per eye (0 = no stereo world right now) */
    int32_t stereoEyeHeight;
    int32_t uiWidth, uiHeight;  /* engine backbuffer / UI target size (the boot-time UI resolution) */
    uint32_t graphicsApplied;   /* generation of the GXGraphicsSettings the engine thread has applied (see GXEngineHost_GetGraphics) */
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

/* ------------------------------------------------------------------------------------------------------------------
 * Graphics / performance settings (package C2; the SwiftUI settings window of package F is built on exactly this).
 *
 * The struct is extendable: `size` is the caller's sizeof(GXGraphicsSettings). GXEngineHost_SetGraphics reads only the
 * fields inside `size` (a caller built against an older header cannot clobber newer fields), GXEngineHost_GetGraphics
 * writes only the fields inside `out->size`. Always start from GXEngineHost_GetGraphics (or ..Defaults) and change the
 * fields you mean to change.
 *
 * Every field is validated and clamped (the clamped value is what the getters report). Settings can be set at any time,
 * from any thread, before or after GXEngineHost_Start; they are applied ON THE ENGINE THREAD at the start of the next
 * engine frame (a change never races an engine frame) except `uiResolution`, which the engine reads once at boot.
 * ------------------------------------------------------------------------------------------------------------------ */

typedef enum GXShadowMode {
    GX_SHADOWS_OFF = 0,     /* no dynamic shadows: cheapest */
    GX_SHADOWS_DECALS = 1,  /* blob / decal shadows only (XrPerformance "B": volumes skipped). Default: what the Quest ships */
    GX_SHADOWS_VOLUMES = 2  /* original stencil shadow volumes plus decals (XrPerformance "A"): heaviest, best looking */
} GXShadowMode;

/* Stereo eye size tier, the same three tiers as XrLayout::resolutionTier / xrStereoExtent (XrWorld.h): the pixel budget of one
 * eye target. The compositor's eye viewport is scaled to this budget (aspect kept), then multiplied by renderScale. */
typedef enum GXEyeSizeTier {
    GX_EYE_BALANCED = 0,    /* 1536-wide class (about 2.4 MP per eye at the Vision Pro aspect). Default */
    GX_EYE_HIGH = 1,        /* 1920-wide class (about 3.7 MP) */
    GX_EYE_ULTRA = 2        /* 2304-wide class (about 5.3 MP): opt-in, allocation heavy (see the ring limits) */
} GXEyeSizeTier;

/* Resolution of the engine backbuffer, which is also the resolution of the UI target and of the upright screen panel.
 * Read ONCE at boot: changing it after GXEngineHost_Start only stores the value for the next launch. */
typedef enum GXUIResolution {
    GX_UI_720P = 0,         /* 1280 x 720: the resolution the whole Quest UI was tuned and tested at. Default */
    GX_UI_1080P = 1         /* 1920 x 1080: sharper text (about 1 UI texel per display pixel at the default panel), untested with the real engine */
} GXUIResolution;

enum {
    GX_GFX_COMFORT_FADE = 1u << 0,   /* black veil during Ground View enter / exit / teleport. Default on */
    GX_GFX_FOCUS_MARKER = 1u << 1,   /* pointer dot on panels, hover / grab-bar highlights. Default on */
    GX_GFX_FLAGS_DEFAULT = 0x3u
};

typedef struct GXGraphicsSettings {
    uint32_t size;              /* sizeof(GXGraphicsSettings) as seen by the caller. REQUIRED */
    float renderScale;          /* multiplier on the tier's eye extent, 0.5 ... 1.5. Default 1.0 (Vision Pro: tier extent as is) */
    int32_t renderFpsCap;       /* engine frames per second, 30 ... 120; 0 = the default (45); negative = uncapped. The simulation
                                   always runs at logicHz (30) whatever this is (docs/visionos-engine-host.md section 7). Default 45 */
    int32_t shadowMode;         /* GXShadowMode. Default GX_SHADOWS_DECALS */
    int32_t eyeTier;            /* GXEyeSizeTier. Default GX_EYE_BALANCED */
    int32_t uiResolution;       /* GXUIResolution; boot-time only. Default GX_UI_720P */
    uint32_t flags;             /* GX_GFX_*. Default GX_GFX_FLAGS_DEFAULT */
    /* New fields are appended here; `size` tells which ones the caller knows. */
} GXGraphicsSettings;

/* The documented defaults for a Vision Pro (the values above). `out->size` must be set; it is filled up to that size. */
void GXEngineHost_GetGraphicsDefaults(GXGraphicsSettings* out);

/* Validates, clamps and stores the settings (any thread). Returns false, changing nothing, when `settings` is NULL or its
 * `size` is smaller than the first field set (header version mismatch). The engine thread applies them at the start of its
 * next frame (GXEngineHostStatus.graphicsApplied then reaches the value returned by GXEngineHost_GetGraphicsGeneration). */
bool GXEngineHost_SetGraphics(const GXGraphicsSettings* settings);

/* The current (requested and clamped) settings. Before any Set call: the defaults. `out->size` must be set. */
void GXEngineHost_GetGraphics(GXGraphicsSettings* out);

/* The settings the engine thread is applying right now (what the last frame used); equals GetGraphics once
 * GXEngineHostStatus.graphicsApplied == GXEngineHost_GetGraphicsGeneration(). `out->size` must be set. */
void GXEngineHost_GetAppliedGraphics(GXGraphicsSettings* out);

/* Increments on every accepted GXEngineHost_SetGraphics that changed something. */
uint32_t GXEngineHost_GetGraphicsGeneration(void);

/* Clamps `settings` in place exactly like SetGraphics does (for a UI that wants to show the effective value while the user drags). */
void GXEngineHost_SanitizeGraphics(GXGraphicsSettings* settings);

/* Persistence helper: the settings as one text line and back ("scale=1.0 fps=45 shadows=1 tier=0 ui=0 flags=3"). The launcher
 * stores that line in its own preferences; ParseGraphics returns false for an unusable line and leaves `out` unchanged. */
size_t GXEngineHost_FormatGraphics(const GXGraphicsSettings* settings, char* out, size_t capacity);
bool GXEngineHost_ParseGraphics(const char* line, GXGraphicsSettings* out);

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
