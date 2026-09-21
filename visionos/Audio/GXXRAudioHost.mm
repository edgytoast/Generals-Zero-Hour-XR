// See GXXRAudioHost.h. Depends on the engine's C API (visionos/Audio/GXAudioListener.h).
#import "GXXRAudioHost.h"
#import "GXAudioListener.h"

#import <Foundation/Foundation.h>

extern "C" {

static void hostPause(void *) { GXAudio_EnginePause(); }
static void hostResume(void *) { GXAudio_EngineResume(); }
static void hostEvent(int event, void *) {
	// mediaserverd restarted: the RemoteIO AudioUnit under OpenAL Soft is dead, so reopen the device. A plain route change
	// (pods <-> Bluetooth) is left to the AudioUnit, which follows the route by itself; it is only logged by the session module.
	if (event == GXAUDIO_SESSION_EVENT_MEDIA_SERVICES_RESET) {
		GXAudio_EngineReopenDevice();
	}
}

bool GXXRAudioHost_Start(GXAudioSessionSpatialMode spatialMode, bool mixWithOthers) {
	GXAudioSession_SetCallbacks(hostPause, hostResume, nullptr);
	GXAudioSession_SetEventCallback(hostEvent, nullptr);
	const bool ok = GXAudioSession_Configure(spatialMode, mixWithOthers);
	// The engine renders its own head-driven listener; with the system spatialiser bypassed, binaural rendering is done by
	// OpenAL Soft (HRTF on). With a system spatial experience the panning stays plain stereo (HRTF off) so the two never stack.
	GXAudio_SetBinauralMode(spatialMode == GXAUDIO_SESSION_SPATIAL_BYPASSED ? 1 : 2);
	return ok;
}

void GXXRAudioHost_SetScenePhase(int phase) { GXAudioSession_NotifyScenePhase(phase); }

void GXXRAudioHost_SetImmersiveOpen(bool open) { GXAudioSession_SetHostPause(!open); }

}
