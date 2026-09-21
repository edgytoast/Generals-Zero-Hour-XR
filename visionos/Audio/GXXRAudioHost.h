// The one call the host makes to bring audio up on visionOS: configures the AVAudioSession and wires its lifecycle decisions
// to the engine (GXAudio_EnginePause / GXAudio_EngineResume / device reopen). Compiled into the app target next to the
// engine library (it references the engine's GXAudio_* symbols); the session module itself (GXXRAudioSession.*) has no such dependency.
#ifndef GX_XR_AUDIO_HOST_H
#define GX_XR_AUDIO_HOST_H

#include "GXXRAudioSession.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call once, early (before GXXRBridge starts the engine so the session category is set before OpenAL opens its device).
// spatialMode: see GXAudioSessionSpatialMode. mixWithOthers: false for the game to be the primary audio app.
bool GXXRAudioHost_Start(GXAudioSessionSpatialMode spatialMode, bool mixWithOthers);

// SwiftUI: .onChange(of: scenePhase) { GXXRAudioHost_SetScenePhase(phase == .active ? 0 : phase == .inactive ? 1 : 2) }
void GXXRAudioHost_SetScenePhase(int phase);
// Immersive space state: audio continues while the immersive space is closed only if the app is still active; the host may
// choose to pause it (open = false) and resume on open = true.
void GXXRAudioHost_SetImmersiveOpen(bool open);

#ifdef __cplusplus
}
#endif

#endif
