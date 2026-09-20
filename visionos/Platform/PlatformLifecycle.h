/*
 * PlatformLifecycle.h - pause / resume / suspend notifications for the engine.
 *
 * The host shell (SwiftUI app on visionOS) translates scene-phase changes,
 * immersive-space state and memory warnings into these callbacks so engine
 * code needs no OS knowledge. Callbacks may arrive on any thread; the engine
 * should post to its own loop.
 *
 * Expected engine reactions:
 *   PAUSE     - the immersive space stopped asking for frames (headset removed,
 *               system overlay, Digital Crown). Stop simulation time, mute audio.
 *   RESUME    - frames are being requested again. Resume audio and simulation.
 *   SUSPEND   - app is going to the background: flush saves, release GPU caches.
 *   MEMORY    - system memory warning: drop caches.
 *   TERMINATE - immersive space is gone for good (Leave Tabletop): tear down XR resources.
 */
#ifndef GX_PLATFORM_LIFECYCLE_H
#define GX_PLATFORM_LIFECYCLE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PlatformLifecycleEvent {
    PLATFORM_LIFECYCLE_PAUSE = 1,
    PLATFORM_LIFECYCLE_RESUME = 2,
    PLATFORM_LIFECYCLE_SUSPEND = 3,
    PLATFORM_LIFECYCLE_MEMORY_WARNING = 4,
    PLATFORM_LIFECYCLE_TERMINATE = 5
} PlatformLifecycleEvent;

typedef struct PlatformLifecycleCallbacks {
    void* user;
    void (*on_event)(void* user, PlatformLifecycleEvent event);
} PlatformLifecycleCallbacks;

/* Register (or clear with NULL) the engine's callbacks. */
void PlatformLifecycle_SetCallbacks(const PlatformLifecycleCallbacks* callbacks);

/* Called by the shell. Safe from any thread. Ignored if no callbacks are set. */
void PlatformLifecycle_Notify(PlatformLifecycleEvent event);

/* Pull-style query for the last delivered lifecycle event (0 if none yet). */
PlatformLifecycleEvent PlatformLifecycle_LastEvent(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GX_PLATFORM_LIFECYCLE_H */
