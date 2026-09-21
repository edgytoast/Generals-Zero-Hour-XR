// AVAudioSession configuration and lifecycle for the visionOS app: category, spatial experience, interruption, route change,
// media-services reset, app background/foreground and scene phase, turned into ONE pause/resume decision for the engine.
//
// Independent of the engine (plain C API over Objective-C++); the engine hookup is in GXXRAudioHost.mm.
//
// Threading: every function may be called from any thread. The pause/resume callbacks are invoked on whichever thread
// delivered the triggering notification (normally the main thread), never while an internal lock is held, and only on a
// real transition (pause when the first reason appears, resume when the last one disappears).
//
// Evidence for the spatial experience choice is in docs/visionos-audio.md and visionos-api.md section 4.
#ifndef GX_XR_AUDIO_SESSION_H
#define GX_XR_AUDIO_SESSION_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the system does with the app's already-rendered sound.
typedef enum GXAudioSessionSpatialMode {
	// AVAudioSessionSpatialExperienceBypassed: the system adds no spatialisation and "mixes the application's sound straight
	// to the output". This is the default because the engine renders its own head-driven listener (GXAudio_SetListenerPose);
	// any system spatialisation on top would double-spatialise (and head-tracked anchoring would counter-rotate our
	// head-driven listener).
	GXAUDIO_SESSION_SPATIAL_BYPASSED = 0,
	// AVAudioSessionSpatialExperienceFixed with a medium sound stage: the system places the stereo output as a fixed,
	// non-head-tracked stage. Fallback if a device listening test shows bypass sounds inside the head.
	GXAUDIO_SESSION_SPATIAL_FIXED = 1,
	// AVAudioSessionSpatialExperienceHeadTracked with the automatic anchoring strategy: the system head-tracks the stereo
	// output. Correct only if the engine listener is NOT head-driven (GXAudio_SetSpatialBattlefieldAudio(false)).
	GXAUDIO_SESSION_SPATIAL_HEAD_TRACKED = 2,
} GXAudioSessionSpatialMode;

typedef void (*GXAudioSessionPauseFn)(void *ctx);
typedef void (*GXAudioSessionResumeFn)(void *ctx);
typedef void (*GXAudioSessionEventFn)(int event, void *ctx);   // GXAUDIO_SESSION_EVENT_*

enum {
	GXAUDIO_SESSION_EVENT_ROUTE_CHANGED = 1,        // output route changed (AVAudioSessionRouteChangeReason in the log line)
	GXAUDIO_SESSION_EVENT_MEDIA_SERVICES_RESET = 2, // mediaserverd restarted: the session was reconfigured, the audio device must be reopened
	GXAUDIO_SESSION_EVENT_MEDIA_SERVICES_LOST = 3,
};

// Category playback, mode default, mixing off (the game is the primary audio app; other apps' audio is interrupted and
// the interruption path below pauses and resumes the game), then the spatial experience. Registers the notification
// observers (once). Idempotent; call before the engine opens its audio device (OpenAL Soft never touches AVAudioSession).
// Returns false when the category or the spatial experience could not be set (see GXAudioSession_LastError).
bool GXAudioSession_Configure(GXAudioSessionSpatialMode spatial, bool mixWithOthers);

// Activate / deactivate the session. Activate is called by Configure automatically when `activate` is true in the info
// below; hosts call these around the immersive space if they want the system to know when audio is really in use.
bool GXAudioSession_Activate(void);
void GXAudioSession_Deactivate(void);

// Change the spatial experience later (settings window / A-B listening on device).
bool GXAudioSession_SetSpatialMode(GXAudioSessionSpatialMode spatial);

// The pause/resume the engine host wires to GXAudio_EnginePause / GXAudio_EngineResume, and an optional event callback
// (route change, media services reset/lost). Pass NULL to clear. May be set before or after Configure.
void GXAudioSession_SetCallbacks(GXAudioSessionPauseFn pauseFn, GXAudioSessionResumeFn resumeFn, void *ctx);
void GXAudioSession_SetEventCallback(GXAudioSessionEventFn eventFn, void *ctx);

// Lifecycle inputs. Notifications from UIApplication are observed automatically; SwiftUI scene phase and immersive-space
// state are not visible to Objective-C, so the host reports them.
//   phase: 0 = active, 1 = inactive, 2 = background   (SwiftUI ScenePhase order)
void GXAudioSession_NotifyScenePhase(int phase);
// The host asks for a pause independent of the system (immersive space dismissed while the app keeps running, Settings
// window "mute when window closed", ...). Balanced by pausing=false.
void GXAudioSession_SetHostPause(bool pausing);
// Inactive scene phase (system overlay, Control Center, looking away) does not pause by default; opt in here.
void GXAudioSession_SetPauseOnInactive(bool pause);

// Pause reasons currently active (bitmask: 1 interruption, 2 background, 4 inactive, 8 host request) and whether the
// callbacks last saw "paused".
int GXAudioSession_PauseReasons(void);
bool GXAudioSession_IsPaused(void);

typedef struct GXAudioSessionInfo {
	char category[64];        // e.g. AVAudioSessionCategoryPlayback
	char mode[64];
	unsigned long categoryOptions;
	double sampleRate;        // session (hardware) sample rate
	double ioBufferDuration;
	int outputChannels;
	char outputRoute[128];    // "<portType>:<portName>" of the first output, empty if none
	bool otherAudioPlaying;
	bool active;              // last activation succeeded
	int spatialMode;          // GXAudioSessionSpatialMode last requested
	int spatialExperienceRaw; // AVAudioSession.intendedSpatialExperience read back (visionOS only, -1 elsewhere)
	int interruptions;        // counters since Configure
	int routeChanges;
	int mediaServicesResets;
} GXAudioSessionInfo;
void GXAudioSession_GetInfo(GXAudioSessionInfo *out);
const char *GXAudioSession_LastError(void);   // last NSError description, "" when none

#ifdef __cplusplus
}
#endif

#endif
