/*
 * GXEngineHostServices.h - the seam between the engine thread (GXEngineHost.mm, in the engine library) and the
 * app's compositor side (visionos/Bridge, visionos/ANGLE): who owns the ANGLE context and the render-target ring,
 * how the compositor's head / eye poses reach the engine thread, and how a finished engine frame is published
 * back. Plain C, no Apple types, so both sides include it.
 *
 *   compositor thread                                           engine thread
 *   -----------------                                           -------------
 *   builds XRFrameInfo per display frame
 *   publishes it (latest wins, lock protected) ------------->   services.acquireHead()   newest head/eye snapshot
 *                                                               client.describe()        which ring targets / sizes
 *                                                               services.beginFrame()    free ring slot -> targets
 *                                                               client.frame()           GL rendering (engine or fake)
 *   composites the LATEST COMPLETED published frame  <--------  services.endFrame()      glFlush + signal + publish
 *   (drawable anchor = the anchor that frame used)
 *
 * Every service call runs on the engine thread. The app implements the services; GXEngineHost.mm implements the
 * real engine client and the loop; the fake engine (-fakeEngine) is another client of the same loop.
 */
#ifndef GX_ENGINE_HOST_SERVICES_H
#define GX_ENGINE_HOST_SERVICES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "GXEngineHost.h"
/* Relative on purpose: on a case-insensitive filesystem "XRPresentation.h" must not resolve to a Quest header. */
#include "../../../../visionos/Platform/XRPresentation.h"

#ifdef __cplusplus
extern "C" {
#endif

struct D3D8GLES_XRTargets; /* d3d8gles.h / visionos/Platform/GXXRD3D8GLES.h */

/* ---- what the engine thread asks the host for, and what it publishes ---------------------------------------- */

/* Ring targets a client wants this frame (bit = 1 << D3D8GLES_XRT_*). */
enum {
    GX_TARGET_STEREO = 1u << 0, /* the stereo pair (or atlas) at eyeWidth x eyeHeight */
    GX_TARGET_GAME = 1u << 2,
    GX_TARGET_WORLD = 1u << 3,
    GX_TARGET_UI = 1u << 4
};

typedef struct GXHostFrameRequest {
    unsigned targetMask;        /* GX_TARGET_* */
    int eyeWidth, eyeHeight;    /* stereo target size per eye (only with GX_TARGET_STEREO) */
    int gameWidth, gameHeight;  /* GAME / WORLD / UI target size (the engine backbuffer) */
    int eyeCount;               /* 1 (mono fallback) or 2 */
} GXHostFrameRequest;

/* Ring target indices (identical to D3D8GLES_XRT_*; GXEngineHostEngine.cpp static_asserts it). */
enum { GX_XRT_STEREO_LEFT = 0, GX_XRT_STEREO_RIGHT = 1, GX_XRT_GAME = 2, GX_XRT_WORLD = 3, GX_XRT_UI = 4 };

/* A world-anchored textured quad the compositor draws from one ring target of the published slot. */
enum {
    GX_LAYER_FLIP_Y = 1u << 0,
    GX_LAYER_PREMULTIPLIED = 1u << 1,
    GX_LAYER_HAS_UVRECT = 1u << 2   /* uvRect crops the source (GL bottom-up UVs, XrGameRect); without it the whole target is shown */
};
typedef struct GXHostLayer {
    char name[16];
    int32_t target;             /* D3D8GLES_XRT_GAME / WORLD / UI */
    float position[3];          /* world space, metres */
    float orientation[4];       /* unit quaternion x,y,z,w; local +Z is the visible face */
    float size[2];              /* width, height in metres */
    uint32_t flags;             /* GX_LAYER_* */
    float uvRect[4];            /* x, y, w, h of the visible part of the target when GX_LAYER_HAS_UVRECT (control bar band, HUD, world crop) */
} GXHostLayer;
enum { GX_HOST_MAX_LAYERS = 8 };

/* Screen-space and world-space feedback the compositor draws in Metal on top of the eyes and layers (package C2). Everything is world
 * space (room space, metres); the flags say which parts are valid. Built from VisionInteractionOutput by VisionFeedback.h. */
enum {
    GX_FB_BOX = 1u << 0,          /* box-select rectangle on the board: boxCorners (counter-clockwise from above) */
    GX_FB_GRAB_BAR = 1u << 1,     /* the board grab bar under the near edge */
    GX_FB_CURSOR = 1u << 2,       /* pinch cursor on the board: cursor */
    GX_FB_PANEL_POINTER = 1u << 3,/* pointer dot on a panel layer: pointerLayer, pointerPos */
    GX_FB_PLACEMENT = 1u << 4,    /* building placement cue at placementPoint: legal tint, cancel cue */
    GX_FB_GROUND_TARGET = 1u << 5,/* Ground View teleport reticle at groundTarget, hold ring */
    GX_FB_RAY = 1u << 6,          /* eye-to-cursor ribbon */
    GX_FB_HOVER = 1u << 7,        /* hover / focus highlight on a panel layer or the grab bar: hoverQuad */
    GX_FB_WAYPOINT = 1u << 8      /* destination marker at waypoint (a confirmed move / placement point, fades) */
};
typedef struct GXHostFeedback {
    uint32_t flags;             /* GX_FB_* */
    float boardOrientation[4];  /* board pose orientation: flat markers lie in its XY plane, z is the board normal */
    float boardWidth;           /* metres, for marker sizes */
    float boxCorners[4][3];
    int32_t boxAdditive;
    float grabPosition[3];
    float grabOrientation[4];   /* board orientation */
    float grabLength, grabThickness;
    int32_t grabActive;
    float cursor[3];
    int32_t cursorOnBoard;
    int32_t pointerLayer;       /* index into GXHostFrameOutput.layers, -1 = none */
    float pointerPos[3];
    int32_t placementLegal;     /* -1 unknown, 0 illegal, 1 legal */
    int32_t placementCancelArmed;
    float placementPoint[3];
    float groundTarget[3];
    int32_t groundTargetValid;
    float groundHold;           /* 0..1 hold-to-exit progress */
    float rayStart[3], rayEnd[3];
    float hoverQuad[4][3];
    int32_t hoverActive;
    float waypoint[3];
    float waypointAge;          /* seconds since the waypoint was set; the marker fades over 1.2 s */
    float fadeAlpha;            /* comfort fade veil 0..1 over the whole view (drawn last) */
} GXHostFeedback;

typedef struct GXHostFrameOutput {
    /* A short text card the compositor draws near the head (title, detail) with the Metal status panel: the recovery notice and the
     * Ground View hint. Empty title = none. */
    char noticeTitle[64];
    char noticeDetail[160];
    int32_t presentationMode;   /* VisionPresentationMode as an int (diagnostics; GXEngineHostStatus.presentationMode has the name) */
    bool groundView;            /* the eyes show the observer view: the compositor clears to opaque black behind them (no passthrough) */
    GXHostFeedback feedback;
    bool stereoValid;           /* the stereo targets hold a picture for the eyes (composited full screen per eye) */
    bool atlas;                 /* both eyes live in the STEREO_LEFT target, left half / right half */
    bool hasFocus;              /* focus[] is valid: a world point (the table) whose distance sets the constant depth of the
                                   eye composite, so system reprojection treats the picture as a plane there */
    float focus[3];
    uint32_t layerCount;
    GXHostLayer layers[GX_HOST_MAX_LAYERS];
} GXHostFrameOutput;

/* One engine-thread frame. `info` is the head/eye snapshot the frame renders for (its native handles are NULL). */
typedef struct GXHostFrame {
    XRFrameInfo info;
    uint64_t headSeq;           /* sequence number of that snapshot (compositor frame counter) */
    struct D3D8GLES_XRTargets* targets; /* ring slot targets for d3d8gles_SetXRHostTargets; valid until endFrame */
    int32_t slot;               /* ring slot being written */
    void* token;                /* services private (retained head snapshot); do not touch */
} GXHostFrame;

/* ---- services: implemented by the app ------------------------------------------------------------------------ */

typedef struct GXHostGLInfo {
    void* eglDisplay;
    void* eglContext;
    void* (*getProcAddress)(const char*);
    unsigned flags;             /* D3D8GLES_XRFLAG_* the ring layout needs (NO_MULTIVIEW, FORCE_ATLAS) */
    bool atlas;
} GXHostGLInfo;

typedef struct GXHostRingInfo {
    uint32_t slots, slotsInUse;
    double megabytes;
    char syncMode[40];
    char renderer[112];
} GXHostRingInfo;

typedef struct GXEngineHostServices {
    void* user;
    /* Creates (first call) and makes current the ANGLE context on the CALLING thread, creates the ring. */
    bool (*attach)(void* user, bool forceAtlas, GXHostGLInfo* out);
    /* Releases the context and drops the ring (engine shutdown). */
    void (*detach)(void* user);
    /* Age in seconds of the newest head snapshot from the compositor (a large value when none arrived yet). */
    double (*headAge)(void* user);
    /* Copies the newest head snapshot (retaining its device anchor) into `frame->info` / headSeq / token. */
    bool (*acquireHead)(void* user, GXHostFrame* frame);
    /* Configures the ring for `request`, acquires a free slot, fills frame->targets / slot. false = skip the frame. */
    bool (*beginFrame)(void* user, GXHostFrame* frame, const GXHostFrameRequest* request);
    /* glFlush + signal the slot, then publish (slot, head snapshot, anchor, layers) as the compositor's latest frame. */
    void (*endFrame)(void* user, GXHostFrame* frame, const GXHostFrameOutput* output);
    /* Drops a frame that was begun but not completed. */
    void (*abortFrame)(void* user, GXHostFrame* frame);
    void (*describeRing)(void* user, GXHostRingInfo* out);
} GXEngineHostServices;

/* The app installs its services once, before the engine starts (a constructor in visionos/Bridge does it). */
void GXEngineHost_SetServices(const GXEngineHostServices* services);

/* ---- clients: the real engine (GXEngineHost.mm) or the fake engine (app) ---------------------------------- */

typedef struct GXEngineClient {
    void* user;
    const char* name;
    /* Engine-thread boot, ANGLE context current. Blocking (a minute for the real engine). `progress` may be called
     * with a status line while booting. Return false and fill `error` on failure. */
    bool (*boot)(void* user, const GXHostGLInfo* gl, void (*progress)(const char* text), char* error, size_t errorCapacity);
    /* Called before beginFrame with the head snapshot the frame will render for: say which targets are needed. */
    void (*describe)(void* user, const XRFrameInfo* head, GXHostFrameRequest* request);
    /* One frame. GL rendering into frame->targets. Fill `output`. Return false when the client wants to stop
     * (the game quit). May block for seconds (map load); the compositor keeps presenting the last frame. */
    bool (*frame)(void* user, GXHostFrame* frame, GXHostFrameOutput* output);
    void (*setPaused)(void* user, bool paused);
    void (*shutdown)(void* user);
    /* 1: the client paces itself (the real engine's FramePacer sleeps inside the frame); 0: the loop sleeps to
     * `fpsCap`. */
    int selfPaced;
    int fpsCap;
} GXEngineClient;

/* Called by a client from INSIDE GXEngineClient.frame (engine thread), typically by a synchronous loader's presenter (the XrGameBoot loading
 * presenter): publishes the slot rendered so far as a finished frame described by `output` and continues in a NEW slot acquired for
 * `request` with the newest head snapshot, updating `frame` in place (targets, slot, info, token). The simulation is not stepped.
 *   GX_NESTED_KEPT       nothing published (no fresh head: the compositor is gone): keep drawing into the current slot;
 *   GX_NESTED_CONTINUED  published; `frame` now describes the new slot: pass frame->targets to d3d8gles_SetXRHostTargets again;
 *   GX_NESTED_LOST       published, but no slot was free for the next frame (compositor starved the ring): frame->slot is -1 and
 *                        frame->targets NULL; the client must stop drawing into ring targets (d3d8gles_SetXRHostTargets(NULL)). The host
 *                        loop then publishes nothing for this engine frame. */
enum { GX_NESTED_LOST = -1, GX_NESTED_KEPT = 0, GX_NESTED_CONTINUED = 1 };
int GXEngineHost_PresentNested(GXHostFrame* frame, const GXHostFrameOutput* output, const GXHostFrameRequest* request);

/* Starts the engine thread with a custom client (the fake engine). Non-blocking; same rules as GXEngineHost_Start. */
bool GXEngineHost_StartClient(const GXEngineClient* client);

/* Status text setters for clients (engine thread). */
void GXEngineHost_SetProgress(const char* text);
/* Presentation status for GXEngineHostStatus (engine thread, once per frame): mode name ("tabletop" ...), stereo eye target size (0 = none), UI size. */
void GXEngineHost_SetPresentationStatus(const char* modeName, int eyeWidth, int eyeHeight, int uiWidth, int uiHeight);
/* Text entry state for GXEngineHostStatus / GXEngineHost_TextFieldFocused (engine thread, once per frame). */
void GXEngineHost_SetTextFieldState(bool focused, const char* utf8Text);

#ifdef __cplusplus
}
#endif

#endif /* GX_ENGINE_HOST_SERVICES_H */
