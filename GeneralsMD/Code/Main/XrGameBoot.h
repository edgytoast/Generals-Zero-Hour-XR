// GeneralsX @feature XR port Phase 1.0 - offscreen game boot for the XR
// tabletop (XrHello.cpp drives these from the XR thread).
//
// Boots the full game with no SDL window on the caller's already-current
// EGL context: GX_XR_OffscreenBoot skips the window binding, the XR config
// redirects rendering into the d3d8gles owned FBO, and each XR frame steps
// the game once via executeSingleFrame(). The finished frame is sampled
// from d3d8gles_GetGameTexture() by the XR quad.
//
// Single-threaded by design: init, frames, and shutdown all run on the XR
// thread, the only thread that ever touches this GL context.
//
// GeneralsX @feature visionOS port: the host-neutral surface (everything except
// the Android JNI entry) is compiled wherever an XR host owns the frame loop
// (GX_XR_HOST = Android or visionOS, see gx_backend.h). Android keeps
// XrGameBoot_Init(JNIEnv*, ...); visionOS boots through XrGameBoot_InitHost().
#pragma once

#include "gx_backend.h" // GX_XR_HOST

#if defined(GX_XR_HOST)

// Tells package E's forwarding bridge (visionos/VisionEngineBridgeXr.cpp) that the
// XrGameBoot_* API below is declared and defined on this platform.
#ifndef GX_XRGAMEBOOT_HOST
#define GX_XRGAMEBOOT_HOST 1
#endif

#ifdef __ANDROID__
#include <jni.h>
#endif
#include "XrEndgame.h"
#include "XrLayers.h"
#include "XrWorld.h"
#include <string>
std::string XrGameBoot_HoverInfo(float x,float y);
bool XrGameBoot_CanAdjustWorld();
// GeneralsX @feature Codex 14/09/2026 Native building preview orientation.
bool XrGameBoot_CanRotatePlacement();
bool XrGameBoot_RotatePlacement(float radians);
float XrGameBoot_PlacementDegrees();
void XrGameBoot_TacticalAction(int action);
bool XrGameBoot_ViewBase(); // Native command-center view; selection unchanged.
// GeneralsX @feature Codex 14/09/2026 Native tactics and session-only views.
std::string XrGameBoot_TacticalReason(int action);
bool XrGameBoot_FormationActive();
void XrGameBoot_Bookmark(int slot,bool save);
bool XrGameBoot_BookmarkKnown(int slot);
void XrGameBoot_CancelTarget();
void XrGameBoot_TacticalGroup(int group,int operation); // recall, save, add, center
void XrGameBoot_SpatialTrigger(bool down,bool available,bool additive);
std::string XrGameBoot_TacticalStatus();
std::string XrGameBoot_TacticalHint();
// GeneralsX @feature Ultron 15/09/2026 P21 read-only panel state: armed
// order mode, current group and waypoint queue for persistent button states.
void XrGameBoot_TacticalState(int &mode,int &group,bool &queue);
int XrGameBoot_GroupSize(int group);
void XrGameBoot_Communicator();
void XrGameBoot_SetLanguage(int language);
std::string XrGameBoot_LanguageStatus();
bool XrGameBoot_ExpandedUI();
std::string XrGameBoot_WorldHoverInfo();
// GeneralsX @feature visionOS 23/09/2026 Pinch preview: what a click at the active spatial pointer would do
// (TouchInput::TapIntent values: 0 nothing, 1 select, 2 move, 3 attack, 4 interact, 5 deselect). When the click acts on or
// selects an object, `targetRoom` gets its position in room space and `targetRadiusM` its size on the table (metres);
// both are left alone otherwise. Issues nothing.
int XrGameBoot_PointerIntent(XrVector3f *targetRoom,float *targetRadiusM);
// GeneralsX @feature visionOS 23/09/2026 Placement ghost legality from the engine's own check: -1 unknown / no placement,
// 0 illegal, 1 legal.
int XrGameBoot_PlacementLegal();
// GeneralsX @feature Muse 16/09/2026 Read-only match-result latch: poll once
// before and after the game frame; MatchResult returns None when no result
// is latched or the card was dismissed. Dismiss never touches the engine.
void XrGameBoot_PollMatchResult();
XrEndgameResult XrGameBoot_MatchResult();
void XrGameBoot_DismissMatchResult();
#if defined(RTS_DEBUG) || defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
// Debug-only end-game triggers for short controlled scenarios; absent from
// release builds unless RTS_DEBUG_CHEATS=ON is set explicitly.
enum class XrDebugEndgame { Victory, Defeat, QuickVictory, LocalDefeat };
void XrGameBoot_DebugEndgame(XrDebugEndgame action);
#endif

#include "Lib/BaseType.h" // Bool

// Game render resolution (injected as -xres/-yres): 16:9, proven panel
// aspect for this UI, sampled 1:1 by the tabletop quad.
constexpr int kXrGameWidth = 1280;
constexpr int kXrGameHeight = 720;

#ifdef __ANDROID__
// Full boot: storage env, working directory, SDL events, GL XR config,
// engine init. eglDisplay/eglContext are informational (the context must
// already be current on this thread). Returns false on failure -- the
// caller then falls back to the triangle loop and the log says why.
bool XrGameBoot_Init(JNIEnv *env, jobject activity, void *eglDisplay, void *eglContext);
#else
// GeneralsX @feature visionOS port: everything the Android front half of
// XrGameBoot_Init discovers through JNI and marker files arrives in this struct
// instead (docs/visionos-engine-host.md). Strings are copied; the pointers only
// have to stay valid during the call. Zero-initialise, then fill.
enum XrGameBootPolicy {
	XRBOOT_POLICY_SEED_OPTIONS     = 1u << 0, // copy DefaultOptions.ini to <userDataRoot>/Options.ini on first run
	XRBOOT_POLICY_LOGIC_TIME_SCALE = 1u << 1, // simulation runs at logicHz whatever the host frame rate (docs risk R1)
	XRBOOT_POLICY_RENDER_CAP       = 1u << 2, // the engine thread limits itself to renderFpsCap frames per second
	XRBOOT_POLICY_GUARD_TIME_KEYS  = 1u << 3, // keep the in-game speed keys inside the policy envelope
	XRBOOT_POLICY_DEFAULT = XRBOOT_POLICY_SEED_OPTIONS | XRBOOT_POLICY_LOGIC_TIME_SCALE |
		XRBOOT_POLICY_RENDER_CAP | XRBOOT_POLICY_GUARD_TIME_KEYS
};
struct XrGameBootHostConfig {
	const char *zhRoot = nullptr;          // Zero Hour tree: working directory, CNC_GENERALS_ZH_PATH (required)
	const char *baseRoot = nullptr;        // base Generals tree, CNC_GENERALS_PATH (optional)
	const char *userDataRoot = nullptr;    // GENERALSX_USERDATA_DIR: saves, Options.ini, maps (required)
	const char *appSupportRoot = nullptr;  // xr-layout-v2.cfg, xr-camera-v1.cfg, game_language.cfg, HOME (required)
	const char *logPath = nullptr;         // stderr sink (previous log kept as *-prev.log); null = leave stderr alone
	bool logTee = true;                    // keep the original stderr as well (console / Xcode)
	int renderWidth = 0, renderHeight = 0; // engine backbuffer (-xres/-yres); 0 = kXrGameWidth/Height
	const char *textLanguage = nullptr;    // GENERALSX_TEXT_LANGUAGE override ("english"); null = game_language.cfg / default
	void *eglDisplay = nullptr;            // informational; the context must be current on the calling thread
	void *eglContext = nullptr;
	void *(*getProcAddress)(const char *) = nullptr; // eglGetProcAddress (D3D8GLES_XRConfig::getProcAddress)
	unsigned glFlags = 0;                  // D3D8GLES_XRFLAG_*
	unsigned policy = XRBOOT_POLICY_DEFAULT; // XrGameBootPolicy
	int logicHz = 30;                      // simulation rate with XRBOOT_POLICY_LOGIC_TIME_SCALE
	int renderFpsCap = 45;                 // engine frames per second with XRBOOT_POLICY_RENDER_CAP (0 = uncapped)
};
// visionOS boot: same sequence as the Android XrGameBoot_Init after its storage discovery
// (environment, working directory, Options seeding, SDL events, critsecs, memory manager, Version,
// command line, GX_XR_OffscreenBoot, d3d8gles XR config, FramePacer, engine init). Runs on the
// engine thread with the ANGLE context current. Returns false on failure; XrGameBoot_LastError()
// then holds the reason (the log has the details).
bool XrGameBoot_InitHost(const XrGameBootHostConfig &config);
// Redirects stderr into `path` (previous log kept) and, with `tee`, keeps forwarding to the original stderr.
// Idempotent; XrGameBoot_InitHost calls it for config.logPath, a host may call it earlier.
void XrGameBoot_InstallLogSink(const char *path, bool tee);
const char *XrGameBoot_LastError();
// Pause bookkeeping for the host lifecycle (engine thread only). paused=true silences audio, releases
// the mouse and cancels edge scrolling exactly like SDL3GameEngine does on DID_ENTER_BACKGROUND;
// paused=false undoes only what the pause did.
void XrGameBoot_SetHostPaused(bool paused);
// Effective simulation rate self-check (engine thread): logic frames per wall-clock second since the
// last call, logged by the host every 10 s. Returns false until a window of at least `minSeconds` elapsed.
struct XrLogicRate { double logicHz = 0, engineFps = 0, seconds = 0; unsigned logicFrames = 0, engineFrames = 0; bool inGame = false; };
bool XrGameBoot_SampleLogicRate(XrLogicRate &out, double minSeconds);
// ---- package C2 (presentation and world); engine thread only ----
// Render frame cap of the engine: fps > 0 keeps the limiter on at that rate (inside the frame policy envelope), fps <= 0 switches the limiter off
// (the simulation still runs at the policy's logic rate). Applied at once.
void XrGameBoot_SetRenderFpsCap(int fps);
// Dynamic shadows while the stereo world is drawn: 0 off, 1 decals only (the Quest default: volumes skipped), 2 volumes + decals. The volume flag
// itself travels in XrWorldFrame::volumeShadows; this call owns the decals (XrWorldFrame has no field for "no shadows at all").
void XrGameBoot_SetShadowMode(int mode);
// Text entry without an SDL window (docs/visionos-presentation.md section 9): true while the focused game window is a text entry gadget (chat, save
// game name, lobby name); `currentText` (optional) receives its content (UTF-8).
bool XrGameBoot_TextFieldFocused(std::string *currentText);
// Types `utf8` into the focused entry gadget after `backspaces` Backspace presses, then presses Enter when `enter`. False when no entry has the focus.
bool XrGameBoot_TextInput(const char *utf8, int backspaces, bool enter);
#endif // __ANDROID__

// One game frame. Returns FALSE once the game wants to quit (or on an
// unexpected exception, logged, rather than crashing across the thread).
Bool XrGameBoot_Frame();

// GeneralsX @bugfix Codex 14/09/2026 Scoped, XR-thread-only presentation
// callback for synchronous loadscreen draws; never steps engine/simulation.
void XrGameBoot_SetLoadingPresenter(void (*present)(void *),void *context);

// True only for an interactive campaign/skirmish/network/replay match.
// Intro movies and every shell/menu state return false so the XR host can
// present them as a conventional front panel rather than a tabletop.
bool XrGameBoot_IsInteractiveGame();

// XR-thread-only pointer and keyboard bridge. UVs are converted by the host
// to engine pixels; this is a persistent tracked pointer, not touch input.
void XrGameBoot_Pointer(bool active, float x, float y, bool select, bool secondary, float wheel);
enum class XrGameKey { Back, Left, Right, Up, Down };
void XrGameBoot_Key(XrGameKey key, bool down);

// GeneralsX @feature Codex 13/09/2026 Native camera actions preserve script locks.
bool XrGameBoot_CameraPreset(int preset);
bool XrGameBoot_AdjustCamera(float yawRadians, float pitchRadians);
bool XrGameBoot_NavigateWorld(float rightSeconds,float forwardSeconds,float zoomSeconds);
int XrGameBoot_DefaultCameraPreset();
bool XrGameBoot_SaveCameraDefault();
float XrGameBoot_CameraPitchDegrees();
const char *XrGameBoot_LayoutPath();
const char *XrGameBoot_LegacyLayoutPath();
void XrGameBoot_SetSplitEnabled(bool enabled);
bool XrGameBoot_SplitReady();
XrGameRect XrGameBoot_WorldRect();
XrGameRect XrGameBoot_CommandRect();
bool XrGameBoot_HasUIAt(float x,float y);
bool XrGameBoot_CanStereoWorld();
bool XrGameBoot_CanObserveGround();
bool XrGameBoot_PickObserverGround(const XrSurface &board,const XrPosef &aim,XrVector3f &ground,XrVector3f *roomPoint=nullptr);
bool XrGameBoot_ObserverStep(XrVector3f current,XrVector3f delta,XrVector3f &next);
const char *XrGameBoot_PerformanceScene();
std::string XrGameBoot_PresentationStatus(bool stereoVisible,bool requested);
void XrGameBoot_SetWorldFrame(const XrWorldFrame &frame);
unsigned int XrGameBoot_StereoTexture(int eye);
bool XrGameBoot_StereoAtlas();
bool XrGameBoot_StereoMultiview();
void XrGameBoot_ConfigureMultiview(void *(*resolver)(const char *));
bool XrGameBoot_PickWorld(const XrSurface &board,const XrPosef &aim,XrWorldHit &hit);
void XrGameBoot_SpatialPointer(bool active);
void XrGameBoot_SpatialClick(bool cancel);
unsigned int XrGameBoot_WorldTexture();
unsigned int XrGameBoot_UITexture();
void XrGameBoot_RoutePointer(int target); // 0 composed, 1 world, 2 windows

// Current game frame texture (d3d8gles owned FBO), 0 until the first frame
// has rendered. Bottom-up GL-native: sample with flipped V.
unsigned int XrGameBoot_GameTexture();

// Actual render size (TheDisplay once booted, else the kXrGame* defaults).
int XrGameBoot_GameWidth();
int XrGameBoot_GameHeight();

// Orderly teardown (mirrors GameMain's). The process itself is ended by
// the activity (System.exit) so the next launch starts fresh.
void XrGameBoot_Shutdown();

#endif // GX_XR_HOST
