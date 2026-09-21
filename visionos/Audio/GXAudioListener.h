// Plain C API of the visionOS audio layer: a host-driven listener for the tabletop, the "Spatial battlefield audio"
// setting, mixer volumes for the SwiftUI settings window, and engine-side pause/resume.
//
// The functions declared here are DEFINED INSIDE THE ENGINE (Core/GameEngineDevice/Source/OpenALAudioDevice/
// OpenALAudioManager.cpp, compiled when GX_PLATFORM_VISIONOS is set), so any host that links the engine library can call
// them without extra glue. Except where noted they are thread-safe (a small mutex or atomics; no audio-device call
// happens on the caller's thread except GXAudio_EnginePause/Resume, which use the thread-safe ALC_SOFT_pause_device).
// Nothing here changes game rules: the listener only decides how already-simulated sounds are heard.
//
// Coordinate conventions are documented in GXAudioListenerMath.h ("BOARD SPACE": metres, origin at the board centre,
// +X right, +Y toward the far side of the board, +Z up out of the board).
#ifndef GX_AUDIO_LISTENER_H
#define GX_AUDIO_LISTENER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Host-driven listener ---------------------------------------------------------------------------------------

// The host calls this with true when it starts supplying head poses (immersive space presenting, engine attached) and
// with false when it stops (immersive space closed, engine paused). While false, or before the first pose after
// enabling, the engine listens exactly as the original game does (camera-relative "microphone").
void GXAudio_SetHostListener(bool enabled);

// Where the board sits in the game world: the world point shown at the board centre and the map view's world-plane
// "right" vector (the same numbers xrWorldToBoard() was built from). Optional: defaults are centre (0,0,0),
// right (1,0). Call whenever the map view pans or rotates (cheap; usually together with GXAudio_SetListenerPose).
void GXAudio_SetBoardFrame(const float centerWorld[3], float rightX, float rightY);

// The user's head in BOARD SPACE (metres) plus the board scale. boardScaleMetersPerWorldUnit = physical board width in
// metres / world-unit span shown across that width (i.e. how many metres one game unit occupies on the table).
// Call every frame from the engine thread before the engine frame. Non-finite input or a non-positive scale is
// ignored (the previous pose stays). headForward/headUp need not be normalised.
void GXAudio_SetListenerPose(float boardScaleMetersPerWorldUnit, const float headPosBoardSpace[3],
                             const float headForwardBoardSpace[3], const float headUpBoardSpace[3]);

// The user-facing "Spatial battlefield audio" switch. Default ON on visionOS. OFF makes the engine behave exactly as
// the original camera-relative sound system (host poses are ignored, per-source ranges are the ones from the INI).
void GXAudio_SetSpatialBattlefieldAudio(bool enabled);
bool GXAudio_GetSpatialBattlefieldAudio(void);

// True while the host listener is really driving the AL listener (host enabled AND switch on AND a pose received).
bool GXAudio_IsHostListenerActive(void);

// Tabletop attenuation in PHYSICAL metres (see GXAudioListenerMath.h, TabletopTuning). Any value <= 0 keeps the default
// (1.0 m reference distance, 0.35 rolloff, 8 m maximum distance). This is the scale setting.
void GXAudio_SetTabletopAttenuation(float refDistanceMeters, float rolloff, float maxDistanceMeters);

// Binaural rendering by OpenAL Soft's HRTF. 0 = automatic (OpenAL Soft's decision), 1 = force on, 2 = force off.
// Applied by the engine thread at the next audio update (a device reset, so avoid changing it every frame).
// Recommended with AVAudioSession spatial experience .bypassed (see GXXRAudioSession.h): on.
void GXAudio_SetBinauralMode(int mode);

// ---- Mixer volumes (0..1) for the settings window; no game rules involved --------------------------------------
// These set the same "system" volumes the in-game options sliders set (AudioManager::setVolume with
// AudioAffect_SystemSetting), so script/mission volume changes keep multiplying on top exactly as before.
enum {
	GXAUDIO_CATEGORY_MUSIC = 0,    // AT_Music streams (menu and battle music)
	GXAUDIO_CATEGORY_SPEECH = 1,   // AT_Streaming: unit voices, EVA, mission/briefing dialogue
	GXAUDIO_CATEGORY_SFX_2D = 2,   // AT_SoundEffect, non-positional: UI clicks, cameos, money, ambient stingers
	GXAUDIO_CATEGORY_SFX_3D = 3,   // AT_SoundEffect, positional: weapons, explosions, engines, footsteps
	GXAUDIO_CATEGORY_COUNT = 4
};
void GXAudio_SetMasterVolume(float volume);            // scales every category (AL listener gain)
float GXAudio_GetMasterVolume(void);
void GXAudio_SetCategoryVolume(int category, float volume);
float GXAudio_GetCategoryVolume(int category);         // last value set by the host, or the engine's value when never set

// ---- Lifecycle --------------------------------------------------------------------------------------------------
// Called by the host from any thread on interruption began, app backgrounded, immersive space closed, ... The output
// device is paused at once (alcDevicePauseSOFT, thread-safe) and the engine pauses its sources on its next audio update,
// mirroring SDL3GameEngine's mobile handling (pauseAudio(AudioAffect_All); resume restores only what the game itself does
// not have paused). Idempotent: unbalanced or repeated calls are harmless.
void GXAudio_EnginePause(void);
void GXAudio_EngineResume(void);
bool GXAudio_IsEnginePaused(void);

#ifdef __cplusplus
}
#endif

#endif
