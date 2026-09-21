// GXEngineHostEngine.cpp - see GXEngineHostEngine.h. Pure C++ (engine headers); runs on the engine thread.
#include "GXEngineHostEngine.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

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
unsigned int d3d8gles_GetGameTexture();
unsigned int d3d8gles_GetXRWorldTexture();
unsigned int d3d8gles_GetXRUITexture();
unsigned int d3d8gles_XRStereoTexture(int eye);
}

namespace {
std::unique_ptr<VisionFrameDriver> s_driver;
VisionPresentation s_presentation;
VisionPresentationOutput s_plan;
bool s_booted = false;
bool s_atlas = false;

void copyError(char *error, size_t capacity, const std::string &text)
{
	if (error != nullptr && capacity > 0) snprintf(error, capacity, "%s", text.c_str());
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
	s_booted = true;
	return true;
}

void GXEngineHostEngine_Describe(const XRFrameInfo *head, GXHostFrameRequest *request)
{
	VisionPresentationInput in;
	in.frame = head;
	in.engineBooted = s_booted;
	in.gameWidth = XrGameBoot_GameWidth();
	in.gameHeight = XrGameBoot_GameHeight();
	in.atlas = s_atlas;
	in.interactive = XrGameBoot_IsInteractiveGame();
	in.canStereoWorld = XrGameBoot_CanStereoWorld();
	s_presentation.update(in, *s_driver, s_plan);
	*request = s_plan.request;
}

bool GXEngineHostEngine_Frame(GXHostFrame *frame, GXHostFrameOutput *output)
{
	memset(output, 0, sizeof(*output));
	// Frame protocol (docs/visionos-shell.md "Engine attach guide"), engine thread, ANGLE context current:
	d3d8gles_SetXRHostTargets(frame->targets);
	// Interaction layer: drain the input queue, apply gestures through the engine bridge, adopt the board, fill the
	// eye poses / fov of the world frame. The engine thread only runs frames while the layer is running.
	s_driver->step(frame->info, true, s_plan.panels, s_plan.panelCount, s_plan.world);
	XrGameBoot_SetWorldFrame(s_plan.world);
	XrGameBoot_SetSplitEnabled(s_plan.splitUI);
	XrGameBoot_PollMatchResult();
	const bool running = XrGameBoot_Frame();
	d3d8gles_InvalidateCachedState(); // the frame (and its cached GL state) is over
	XrGameBoot_PollMatchResult();

	// What the compositor may draw from this slot: only targets the engine really produced.
	const bool stereoTextures = d3d8gles_XRStereoTexture(0) != 0 && (s_atlas || d3d8gles_XRStereoTexture(1) != 0);
	output->stereoValid = s_plan.stereoWorld && stereoTextures;
	output->atlas = s_atlas;
	output->hasFocus = s_plan.hasFocus;
	memcpy(output->focus, s_plan.focus, sizeof(output->focus));
	for (int i = 0; i < s_plan.layerCount && output->layerCount < GX_HOST_MAX_LAYERS; ++i) {
		const GXHostLayer &layer = s_plan.layers[i];
		const unsigned texture = layer.target == GX_XRT_UI ? d3d8gles_GetXRUITexture()
			: layer.target == GX_XRT_WORLD ? d3d8gles_GetXRWorldTexture() : d3d8gles_GetGameTexture();
		if (texture != 0) output->layers[output->layerCount++] = layer;
	}
	return running == TRUE;
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
