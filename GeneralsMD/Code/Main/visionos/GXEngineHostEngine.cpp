// GXEngineHostEngine.cpp - see GXEngineHostEngine.h. Pure C++ (engine headers); runs on the engine thread.
#include "GXEngineHostEngine.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "GXGraphicsSettings.h"
#include "VisionEngineBridgeXr.h"
#include "VisionFrameDriver.h"
#include "VisionPresentation.h"
#include "XrGameBoot.h"

// The d3d8gles C entry points this file calls. Declared here rather than through d3d8gles.h, which drags in <d3d8.h> and
// needs the engine's Windows-compat preamble; the signatures are those of Core/Libraries/Source/d3d8gles/include/d3d8gles.h
// (extern "C"), and the ring target indices are checked against its enum in XrGameBoot.cpp (static_assert).
struct D3D8GLES_XRTargets;
extern "C" {
void d3d8gles_SetXRHostTargets(const struct D3D8GLES_XRTargets *targets);
void d3d8gles_InvalidateCachedState();
void d3d8gles_RequireXRFullWorld();
unsigned int d3d8gles_GetGameTexture();
unsigned int d3d8gles_GetXRWorldTexture();
unsigned int d3d8gles_GetXRUITexture();
unsigned int d3d8gles_XRStereoTexture(int eye);
}

// The 27-function forwarding surface (docs/visionos-interaction.md section 7); declared here rather than pulling in XrGameBoot.h's
// std::string overloads a second time for the two package C2 calls that are new in this wave.
extern "C" {
void GX_XR_PresentLoadingFrame();
}
extern bool XrGameBoot_TextFieldFocused(std::string *currentText);
extern bool XrGameBoot_TextInput(const char *utf8, int backspaces, bool enter);
extern void XrGameBoot_SetRenderFpsCap(int fps);
extern void XrGameBoot_SetShadowMode(int mode);

namespace {
std::unique_ptr<VisionFrameDriver> s_driver;
VisionPresentation s_presentation;
VisionPresentationOutput s_plan;
GXGraphicsSettings s_graphics = gxGraphicsDefaults();
bool s_booted = false;
bool s_atlas = false;
GXHostFrame *s_currentFrame = nullptr; // the ring slot GXEngineHostEngine_Frame is currently writing (for the nested loading presenter)

void copyError(char *error, size_t capacity, const std::string &text)
{
	if (error != nullptr && capacity > 0) snprintf(error, capacity, "%s", text.c_str());
}

VisionPresentationFacts gatherFacts()
{
	VisionPresentationFacts f;
	f.booted = s_booted;
	f.interactiveGame = XrGameBoot_IsInteractiveGame();
	f.canStereoWorld = XrGameBoot_CanStereoWorld();
	f.splitReady = XrGameBoot_SplitReady();
	f.expandedUI = XrGameBoot_ExpandedUI();
	f.canObserveGround = XrGameBoot_CanObserveGround();
	f.gameWidth = XrGameBoot_GameWidth();
	f.gameHeight = XrGameBoot_GameHeight();
	f.gameTexture = XrGameBoot_GameTexture() != 0;
	f.worldTexture = XrGameBoot_WorldTexture() != 0;
	f.uiTexture = XrGameBoot_UITexture() != 0;
	f.eyeTexture[0] = d3d8gles_XRStereoTexture(0) != 0;
	f.eyeTexture[1] = s_atlas ? f.eyeTexture[0] : d3d8gles_XRStereoTexture(1) != 0;
	f.worldRect = XrGameBoot_WorldRect();
	f.commandRect = XrGameBoot_CommandRect();
	return f;
}

VisionPresentationInput makeInput(const XRFrameInfo *head)
{
	VisionPresentationInput in;
	in.frame = head;
	in.facts = gatherFacts();
	in.atlas = s_atlas;
	in.gfx = s_graphics;
	in.layoutPath = XrGameBoot_LayoutPath();
	in.legacyLayoutPath = XrGameBoot_LegacyLayoutPath();
	return in;
}

void applyGraphicsToTextures(GXHostFrameOutput *out)
{
	GXEngineHost_SetPresentationStatus(visionModeName(VisionPresentationMode(out->presentationMode)), s_plan.stereoWorld ? s_plan.eye.width : 0,
		s_plan.stereoWorld ? s_plan.eye.height : 0, XrGameBoot_GameWidth(), XrGameBoot_GameHeight());
	std::string text;
	const bool focused = XrGameBoot_TextFieldFocused(&text);
	GXEngineHost_SetTextFieldState(focused, focused ? text.c_str() : nullptr);
}
} // namespace

void GXEngineHostEngine_InstallLogSink(const char *path) { XrGameBoot_InstallLogSink(path, true); }
const char *GXEngineHostEngine_LastError(void) { return XrGameBoot_LastError(); }

bool GXEngineHostEngine_Boot(const GXEngineHostConfig *config, const GXHostGLInfo *gl, char *error, size_t errorCapacity)
{
	XrGameBootHostConfig hc;
	hc.zhRoot = config->zhRoot;
	hc.baseRoot = (config->baseRoot != nullptr && config->baseRoot[0] != '\0') ? config->baseRoot : config->zhRoot;
	hc.userDataRoot = config->userDataRoot;
	hc.appSupportRoot = config->appSupportRoot;
	hc.logPath = config->logPath;
	hc.logTee = true;
	hc.renderWidth = config->renderWidth;
	hc.renderHeight = config->renderHeight;
	hc.textLanguage = config->textLanguage;
	hc.eglDisplay = gl->eglDisplay;
	hc.eglContext = gl->eglContext;
	hc.getProcAddress = gl->getProcAddress;
	hc.glFlags = gl->flags;
	hc.policy = config->policy != 0 ? config->policy : XRBOOT_POLICY_DEFAULT;
	hc.logicHz = config->logicHz > 0 ? config->logicHz : 30;
	hc.renderFpsCap = config->renderFpsCap > 0 ? config->renderFpsCap : (config->renderFpsCap < 0 ? 0 : 45);
	s_atlas = gl->atlas;
	if (!XrGameBoot_InitHost(hc)) {
		const char *why = XrGameBoot_LastError();
		copyError(error, errorCapacity, (why != nullptr && why[0] != '\0') ? why : "engine init failed (no reason recorded; see the log)");
		return false;
	}
	s_driver = std::make_unique<VisionFrameDriver>(VisionCreateXrGameBootBridge());
	XrGameBoot_SetLoadingPresenter([](void *) { GXEngineHostEngine_PresentLoading(); }, nullptr);
	s_booted = true;
	return true;
}

void GXEngineHostEngine_Describe(const XRFrameInfo *head, GXHostFrameRequest *request)
{
	XrGameBoot_SetRenderFpsCap(s_graphics.renderFpsCap);
	XrGameBoot_SetShadowMode(s_graphics.shadowMode);
	const VisionPresentationInput in = makeInput(head);
	s_presentation.update(in, *s_driver, s_plan);
	*request = s_plan.request;
}

void GXEngineHostEngine_ApplyGraphics(const GXGraphicsSettings *applied)
{
	if (applied != nullptr) s_graphics = *applied;
}

bool GXEngineHostEngine_TextInput(const char *utf8, bool replace, bool enter)
{
	if (!s_booted) return false;
	int backspaces = 0;
	if (replace) {
		std::string current;
		if (XrGameBoot_TextFieldFocused(&current)) {
			// One Backspace per UTF-8 codepoint (BMP; matches forwardTextInputEvent's decode).
			for (size_t i = 0; i < current.size();) {
				const unsigned char c = static_cast<unsigned char>(current[i]);
				i += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
				++backspaces;
			}
		}
	}
	return XrGameBoot_TextInput(utf8, backspaces, enter);
}

bool GXEngineHostEngine_Frame(GXHostFrame *frame, GXHostFrameOutput *output)
{
	memset(output, 0, sizeof(*output));
	s_currentFrame = frame; // valid until this function returns; the loading presenter (called from inside XrGameBoot_Frame) uses it
	// Frame protocol (docs/visionos-shell.md "Engine attach guide"), engine thread, ANGLE context current:
	d3d8gles_SetXRHostTargets(frame->targets);
	// Interaction layer: drain the input queue, apply gestures through the engine bridge, adopt the board, fill the
	// eye poses / fov of the world frame. The engine thread only runs frames while the layer is running.
	s_driver->step(frame->info, true, s_plan.plan.panels, s_plan.plan.panelCount, s_plan.world);
	XrGameBoot_SetWorldFrame(s_plan.world);
	XrGameBoot_SetSplitEnabled(s_plan.splitUI);
	XrGameBoot_PollMatchResult();
	const bool running = XrGameBoot_Frame();  // a loading presenter may call GXEngineHostEngine_PresentLoading and publish nested frames
	d3d8gles_InvalidateCachedState(); // the frame (and its cached GL state) is over
	XrGameBoot_PollMatchResult();

	const VisionPresentationFacts post = gatherFacts();
	VisionPresentationInput in = makeInput(&frame->info);
	s_presentation.finish(post, *s_driver, s_plan.world, in, *output);
	if (s_presentation.consumeRequireFullWorld()) d3d8gles_RequireXRFullWorld();
	output->atlas = s_atlas;
	applyGraphicsToTextures(output);
	s_currentFrame = nullptr;
	return running == TRUE;
}

// Installed on XrGameBoot_SetLoadingPresenter (GXEngineHostEngine_Boot): called from INSIDE the engine's frame (a synchronous
// loader is drawing frames of its own loading screen) without stepping the simulation. Describes and publishes ONE nested
// frame (the engine's loading screen on the upright panel) through GXEngineHost_PresentNested.
void GXEngineHostEngine_PresentLoading()
{
	if (!s_driver || s_currentFrame == nullptr) return;
	const VisionPresentationFacts post = gatherFacts();
	VisionPresentationInput in = makeInput(&s_currentFrame->info);
	GXHostFrameOutput out;
	s_presentation.describeLoading(post, *s_driver, in, out);
	applyGraphicsToTextures(&out);
	// Upright screen (GAME target) + the UI target, no stereo: the request the next slot is configured for.
	GXHostFrameRequest req = {};
	req.gameWidth = XrGameBoot_GameWidth();
	req.gameHeight = XrGameBoot_GameHeight();
	req.targetMask = GX_TARGET_GAME | GX_TARGET_UI;
	const int result = GXEngineHost_PresentNested(s_currentFrame, &out, &req);
	if (result == GX_NESTED_LOST) {
		d3d8gles_SetXRHostTargets(nullptr);
	} else if (result == GX_NESTED_CONTINUED) {
		d3d8gles_SetXRHostTargets(s_currentFrame->targets);
	}
	// GX_NESTED_KEPT (no fresh head): keep drawing into the current slot; s_currentFrame is unchanged.
}

void GXEngineHostEngine_SetPaused(bool paused)
{
	if (!s_booted) return;
	XrGameBoot_SetHostPaused(paused);
	if (paused && s_driver) s_driver->reset(kVisionResetFocus); // a pinch in flight must release the engine, balanced
}

void GXEngineHostEngine_Shutdown(void)
{
	// No XrGameBoot_Shutdown(): the engine is single-start and the app is ended by the user; tearing down singletons that
	// may be half broken (a fatal error, a quit in the middle of a load) would only risk a crash. Silence it instead.
	if (!s_booted) return;
	XrGameBoot_SetHostPaused(true);
}

bool GXEngineHostEngine_SampleLogicRate(double minSeconds, double *logicHz, double *engineFps, bool *inGame)
{
	XrLogicRate rate;
	if (!XrGameBoot_SampleLogicRate(rate, minSeconds)) return false;
	*logicHz = rate.logicHz;
	*engineFps = rate.engineFps;
	*inGame = rate.inGame;
	return true;
}

bool GXEngineHostEngine_IsInteractiveGame(void) { return s_booted && XrGameBoot_IsInteractiveGame(); }
