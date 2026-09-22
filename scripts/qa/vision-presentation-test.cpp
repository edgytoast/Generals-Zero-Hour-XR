// Host tests of the visionOS presentation layer (package C2): graphics settings, eye sizing, the world frame, the presentation state machine
// (recorded scenarios), the panel layout and its hit-testing through the real interaction layer, the readability arithmetic, feedback,
// workspace persistence and the whole update / step / finish pipeline. No engine, no GL, no device, no game data.
// Build and run: scripts/qa/vision-presentation-test.sh
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "GXEngineHost.h"
#include "GXGraphicsSettings.h"
#include "VisionFeedback.h"
#include "VisionFrameDriver.h"
#include "VisionPresentation.h"
#include "VisionPresentationLogic.h"
#include "VisionScriptedBridge.h"
#include "VisionWorkspace.h"

static int checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { fprintf(stderr, "vision-presentation check %d failed at line %d: %s\n", checks, __LINE__, #cond); exit(1); } } while (0)
static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
#define NEAR(a, b) CHECK(near((a), (b)))
#define NEARE(a, b, e) CHECK(near((a), (b), (e)))
static bool vnear(XrVector3f a, XrVector3f b, float eps = 1e-3f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }
#define VNEAR(a, b) CHECK(vnear((a), (b)))

// ---- the shell input queue the frame driver drains (stand-ins for visionos/Input/GXXRInput.mm) ----
static std::vector<XRInteractionEvent> g_queue;
static size_t g_queueRead = 0;
static float g_shellBoard[16];
static int g_shellBoardSets = 0;
extern "C" bool XRInteraction_PollEvent(XRInteractionEvent *out) {
	if (g_queueRead >= g_queue.size()) { g_queue.clear(); g_queueRead = 0; return false; }
	*out = g_queue[g_queueRead++];
	return true;
}
extern "C" void XRInteraction_SetBoardTransform(const float m[16], float, float) { memcpy(g_shellBoard, m, sizeof(g_shellBoard)); ++g_shellBoardSets; }
extern "C" void XRInteraction_ClearBoardTransform(void) {}

// ---- helpers ----
static XRFrameInfo makeFrame(uint64_t index, double t, XrVector3f head, int vpW = 3680, int vpH = 3140) {
	XRFrameInfo f = {};
	f.frame_index = index;
	f.predicted_display_time_s = t;
	f.head_tracked = true;
	f.head_pose = {{head.x, head.y, head.z}, {0, 0, 0, 1}};
	f.eye_count = 2;
	for (int e = 0; e < 2; ++e) {
		f.eyes[e].pose = {{head.x + (e == 0 ? -0.031f : 0.031f), head.y, head.z}, {0, 0, 0, 1}};
		f.eyes[e].fov = e == 0 ? XRFov{-0.80f, 0.60f, 0.55f, -0.70f} : XRFov{-0.60f, 0.80f, 0.55f, -0.70f};
		f.eyes[e].viewport = {0, 0, vpW, vpH};
	}
	return f;
}
static const XrVector3f kHead = {0, 1.5f, 0};

// =========================================================================================== 1. graphics settings
static void testSettings() {
	GXGraphicsSettings s = {};
	s.size = sizeof(s);
	GXEngineHost_GetGraphicsDefaults(&s);
	NEAR(s.renderScale, 1.0f);
	CHECK(s.renderFpsCap == 45 && s.shadowMode == GX_SHADOWS_DECALS && s.eyeTier == GX_EYE_BALANCED && s.uiResolution == GX_UI_720P);
	CHECK(s.flags == GX_GFX_FLAGS_DEFAULT && s.size == sizeof(GXGraphicsSettings));
	GXGraphicsSettings_ResetForTest();
	GXGraphicsSettings g = {};
	g.size = sizeof(g);
	GXEngineHost_GetGraphics(&g);
	CHECK(gxGraphicsEqual(g, s) && GXEngineHost_GetGraphicsGeneration() == 0);
	// clamping
	GXGraphicsSettings w = g;
	w.renderScale = 9.0f; w.renderFpsCap = 7; w.shadowMode = 9; w.eyeTier = -3; w.uiResolution = 5; w.flags = 0xFFFF;
	CHECK(GXEngineHost_SetGraphics(&w));
	GXEngineHost_GetGraphics(&g);
	NEAR(g.renderScale, 1.5f);
	CHECK(g.renderFpsCap == 30 && g.shadowMode == GX_SHADOWS_DECALS && g.eyeTier == GX_EYE_BALANCED && g.uiResolution == GX_UI_720P && g.flags == GX_GFX_FLAGS_DEFAULT);
	CHECK(GXEngineHost_GetGraphicsGeneration() == 1);
	w.renderScale = 0.1f; w.renderFpsCap = 500; w.shadowMode = GX_SHADOWS_VOLUMES; w.eyeTier = GX_EYE_ULTRA; w.uiResolution = GX_UI_1080P;
	CHECK(GXEngineHost_SetGraphics(&w));
	GXEngineHost_GetGraphics(&g);
	NEAR(g.renderScale, 0.5f);
	CHECK(g.renderFpsCap == 120 && g.shadowMode == GX_SHADOWS_VOLUMES && g.eyeTier == GX_EYE_ULTRA && g.uiResolution == GX_UI_1080P);
	w.renderFpsCap = -5; w.renderScale = NAN;
	GXEngineHost_SetGraphics(&w);
	GXEngineHost_GetGraphics(&g);
	CHECK(g.renderFpsCap == -1);
	NEAR(g.renderScale, 1.0f);
	w.renderFpsCap = 0;
	GXEngineHost_SetGraphics(&w);
	GXEngineHost_GetGraphics(&g);
	CHECK(g.renderFpsCap == 45); // 0 = default
	// no change = no new generation
	const uint32_t gen = GXEngineHost_GetGraphicsGeneration();
	GXEngineHost_SetGraphics(&g);
	CHECK(GXEngineHost_GetGraphicsGeneration() == gen);
	CHECK(!GXEngineHost_SetGraphics(nullptr));
	GXGraphicsSettings tiny = {};
	tiny.size = 2;
	CHECK(!GXEngineHost_SetGraphics(&tiny));
	// engine-thread hand-off: applied lags until consumed
	GXGraphicsSettings applied = {};
	applied.size = sizeof(applied);
	uint32_t appliedGen = 0;
	GXEngineHost_GetAppliedGraphics(&applied); // before: defaults
	CHECK(applied.renderFpsCap == 45 && applied.shadowMode == GX_SHADOWS_DECALS);
	CHECK(GXGraphicsSettings_ConsumeForEngine(&applied, &appliedGen));
	CHECK(appliedGen == GXEngineHost_GetGraphicsGeneration() && applied.eyeTier == GX_EYE_ULTRA);
	CHECK(!GXGraphicsSettings_ConsumeForEngine(&applied, &appliedGen));
	// extendability: an OLDER caller (smaller struct) only writes the fields it has; a newer reader leaves unknown fields alone
	GXGraphicsSettings old = {};
	old.size = (uint32_t)offsetof(GXGraphicsSettings, shadowMode); // knows scale and fps only
	old.renderScale = 0.75f; old.renderFpsCap = 60; old.shadowMode = GX_SHADOWS_OFF; // shadowMode lies outside `size`
	CHECK(GXEngineHost_SetGraphics(&old));
	GXEngineHost_GetGraphics(&g);
	NEAR(g.renderScale, 0.75f);
	CHECK(g.renderFpsCap == 60 && g.shadowMode == GX_SHADOWS_VOLUMES); // untouched
	GXGraphicsSettings rd = {};
	rd.size = (uint32_t)offsetof(GXGraphicsSettings, shadowMode);
	rd.shadowMode = 77;
	GXEngineHost_GetGraphics(&rd);
	CHECK(rd.shadowMode == 77 && near(rd.renderScale, 0.75f) && rd.size == (uint32_t)offsetof(GXGraphicsSettings, shadowMode)); // getter stayed inside size
	// text form round trip
	char line[128];
	CHECK(GXEngineHost_FormatGraphics(&g, line, sizeof(line)) > 10);
	GXGraphicsSettings p = {};
	p.size = sizeof(p);
	CHECK(GXEngineHost_ParseGraphics(line, &p) && gxGraphicsEqual(p, g));
	GXGraphicsSettings keep = p;
	CHECK(!GXEngineHost_ParseGraphics("garbage line", &p) && gxGraphicsEqual(p, keep));
	CHECK(GXEngineHost_ParseGraphics("scale=2.0 fps=-1", &p) && near(p.renderScale, 1.5f) && p.renderFpsCap == -1);
	int w0 = 0, h0 = 0;
	gxUIResolutionSize(GX_UI_720P, w0, h0);
	CHECK(w0 == 1280 && h0 == 720);
	gxUIResolutionSize(GX_UI_1080P, w0, h0);
	CHECK(w0 == 1920 && h0 == 1080);
	GXGraphicsSettings_ResetForTest();
}

// =========================================================================================== 2. eye size and world frame
static void testEyeAndWorldFrame() {
	// Vision Pro class viewport (3680 x 3140): every tier is a fraction of it (Quest rule: xrStereoExtent), scale multiplies
	const VisionEyeExtent bal = visionEyeExtent(3680, 3140, GX_EYE_BALANCED, 1.0f, false);
	const VisionEyeExtent hi = visionEyeExtent(3680, 3140, GX_EYE_HIGH, 1.0f, false);
	const VisionEyeExtent ul = visionEyeExtent(3680, 3140, GX_EYE_ULTRA, 1.0f, false);
	CHECK(bal.width == 1536 && bal.height == 1310 && !bal.clamped);
	CHECK(hi.width == 1920 && ul.width == 2304);
	CHECK(bal.width < hi.width && hi.width < ul.width && bal.height < hi.height && hi.height < ul.height);
	// aspect kept (the Quest rule keeps the viewport aspect)
	NEARE(float(bal.width) / bal.height, 3680.0f / 3140.0f, 0.01f);
	const VisionEyeExtent half = visionEyeExtent(3680, 3140, GX_EYE_BALANCED, 0.5f, false);
	CHECK(half.width == 768 && half.height == 655);
	const VisionEyeExtent up = visionEyeExtent(3680, 3140, GX_EYE_BALANCED, 1.5f, false);
	CHECK(up.width == 2304 && !up.clamped);
	// ring limits: Ultra at 1.5 exceeds 8 MP and is scaled down uniformly, aspect kept
	const VisionEyeExtent big = visionEyeExtent(3680, 3140, GX_EYE_ULTRA, 1.5f, false);
	CHECK(big.clamped && (long long)big.width * big.height <= kVisionMaxEyePixels + 3000 && big.width <= kVisionMaxEyeDim);
	NEARE(float(big.width) / big.height, float(ul.width) / ul.height, 0.01f);
	// atlas: width limit is on the packed width
	const VisionEyeExtent atlas = visionEyeExtent(8000, 3000, GX_EYE_ULTRA, 1.5f, true);
	CHECK(atlas.width * 2 <= kVisionMaxEyeDim * 2 && atlas.width <= kVisionMaxEyeDim / 2 + 1);
	// degenerate viewport (no eye info): the Quest fallback square, still limited
	const VisionEyeExtent none = visionEyeExtent(0, 0, GX_EYE_BALANCED, 1.0f, false);
	CHECK(none.width == 1536 && none.height == 1536);
	const VisionEyeExtent nan = visionEyeExtent(3680, 3140, 99, NAN, false);
	CHECK(nan.width == ul.width);
	// tiny render scale never goes below the minimum
	const VisionEyeExtent tiny = visionEyeExtent(100, 100, GX_EYE_BALANCED, 0.5f, false);
	CHECK(tiny.width >= kVisionMinEyeDim && tiny.height >= kVisionMinEyeDim);

	// ---- the world frame ----
	XRFrameInfo frame = makeFrame(1, 10.0, kHead);
	VisionWorldFrameInput wi;
	wi.stereoRequested = true;
	wi.frame = &frame;
	wi.atlas = false;
	XrWorldFrame world;
	visionBuildWorldFrame(world, wi);
	CHECK(world.enabled && world.width == 1536 && world.height == 1310);
	CHECK(!world.multiviewStereo && !world.atlasStereo && world.elideWorldCopy && !world.volumeShadows);
	CHECK(world.healthBars && world.unitRings && world.boardFrame);
	wi.graphics.shadowMode = GX_SHADOWS_VOLUMES;
	wi.atlas = true;
	wi.stereoRequested = false;
	visionBuildWorldFrame(world, wi);
	CHECK(!world.enabled && world.volumeShadows && world.atlasStereo);
	wi.graphics.shadowMode = GX_SHADOWS_OFF;
	visionBuildWorldFrame(world, wi);
	CHECK(!world.volumeShadows);

	// ---- the eye clip matrices the engine derives from the frame (xrWorldEyeClip, unchanged Quest code) look at the board ----
	VisionScriptedBridge bridge;
	VisionFrameDriver d(&bridge);
	XrWorldFrame w2;
	visionBuildWorldFrame(w2, {true, &frame, gxGraphicsDefaults(), false, true, true, true});
	g_queue.clear();
	d.step(frame, true, nullptr, 0, w2);
	CHECK(d.boardPlaced() && d.host().boardPlaced);
	CHECK(vnear(w2.board.pose.position, d.host().board.pose.position));
	NEAR(w2.board.width, 1.0f);
	for (int eye = 0; eye < 2; ++eye) {
		NEAR(w2.eyes[eye].position.x, frame.eyes[eye].pose.position.x);
		NEAR(w2.fov[eye].angleLeft, frame.eyes[eye].fov.angle_left);
	}
	// game world center (the point shown at the board center) -> clip space of each eye: inside the frustum, in front of the eye
	float worldToBoard[16];
	CHECK(xrWorldToBoard(worldToBoard, {1000, 1000, 0}, {1, 0, 0}, 800.0f));
	for (int eye = 0; eye < 2; ++eye) {
		float clip[16];
		xrWorldEyeClip(clip, w2, eye, worldToBoard);
		const float x = clip[12] + 0, y = clip[13] + 0, ww = clip[15];
		// clip * (1000,1000,0,1) = column combination: the world point (1000,1000,0)
		const float px = clip[0] * 1000 + clip[4] * 1000 + clip[12], py = clip[1] * 1000 + clip[5] * 1000 + clip[13],
			pw = clip[3] * 1000 + clip[7] * 1000 + clip[15];
		(void)x; (void)y; (void)ww;
		CHECK(pw > 0.1f);
		CHECK(std::fabs(px / pw) < 1.0f && std::fabs(py / pw) < 1.0f);
	}
}

// =========================================================================================== 3. state machine scenarios
struct ScenarioFrame {
	const char *what;
	bool booted, interactive, canStereo, split, expanded, eye0, eye1, world, game, ui;
	bool loading;
	bool observer;
	VisionPresentationMode expect;
	bool expectRecovery;
	bool expectFullWorld;
};

static void runScenario(const char *name, const std::vector<ScenarioFrame> &frames, unsigned expectModeChanges, unsigned expectRecoveries) {
	VisionViewState s;
	for (const ScenarioFrame &sf : frames) {
		VisionPresentationFacts pre;
		pre.booted = sf.booted; pre.interactiveGame = sf.interactive; pre.canStereoWorld = sf.canStereo; pre.splitReady = sf.split;
		pre.expandedUI = sf.expanded;
		s.requireFullWorld = false;
		if (sf.booted) visionPresentationBegin(s, pre); // like VisionPresentation::update
		if (sf.loading) visionPresentationLoadingBegin(s); else visionPresentationLoadingEnd(s);
		VisionPresentationFacts post = pre;
		post.eyeTexture[0] = sf.eye0; post.eyeTexture[1] = sf.eye1; post.worldTexture = sf.world; post.gameTexture = sf.game; post.uiTexture = sf.ui;
		const VisionPresentationMode m = visionPresentationEnd(s, post, sf.observer);
		if (m != sf.expect || s.recoveryVisible != sf.expectRecovery || s.requireFullWorld != sf.expectFullWorld) {
			fprintf(stderr, "scenario '%s' step '%s': mode %s (want %s) recovery %d (want %d) fullWorld %d (want %d)\n", name, sf.what, visionModeName(m),
				visionModeName(sf.expect), s.recoveryVisible, sf.expectRecovery, s.requireFullWorld, sf.expectFullWorld);
			exit(1);
		}
		++checks;
	}
	CHECK(s.modeChanges == expectModeChanges);
	CHECK(s.recoveries == expectRecoveries);
}

static void testStateMachine() {
	using M = VisionPresentationMode;
	//              what                 boot  inter  st3d  split expnd e0    e1    world game  ui    load   obs    expect            rec    full
	// Scenario A (modelled on the Quest log lines "presentation mode -> shell-panel / tabletop-game", "P5 presentation -> world + detached UI"):
	// engine boots, menu, skirmish loads (blocking loader publishes), match starts in stereo, dialog opens, cinematic, back, ground view, exit.
	runScenario("skirmish", {
		{"not booted",           false, false, false, false, false, false, false, false, false, false, false, false, M::Idle,       false, false},
		{"first shell frame",    true,  false, false, false, false, false, false, false, true,  false, false, false, M::Menu,       false, false},
		{"menu",                 true,  false, false, false, false, false, false, false, true,  false, false, false, M::Menu,       false, false},
		{"map load (nested)",    true,  false, false, false, false, false, false, false, true,  false, true,  false, M::Loading,    false, false},
		{"map load (nested)",    true,  false, false, false, false, false, false, false, true,  false, true,  false, M::Loading,    false, false},
		{"match: stereo ready",  true,  true,  true,  true,  false, true,  true,  false, true,  true,  false, false, M::Tabletop,   false, false},
		{"match: steady",        true,  true,  true,  true,  false, true,  true,  false, true,  true,  false, false, M::Tabletop,   false, false},
		{"dialog: expanded UI",  true,  true,  true,  true,  true,  true,  true,  false, true,  true,  false, false, M::Tabletop,   false, false},
		{"movie starts",         true,  true,  true,  false, false, false, false, false, true,  false, false, false, M::Cinematic,  false, false},
		{"movie ends",           true,  true,  true,  true,  false, true,  true,  false, true,  true,  false, false, M::Tabletop,   false, false},
		{"ground view",          true,  true,  true,  true,  false, true,  true,  false, true,  true,  false, true,  M::GroundView, false, false},
		{"exit ground view",     true,  true,  true,  true,  false, true,  true,  false, true,  true,  false, false, M::Tabletop,   false, false},
		{"back to menu",         true,  false, false, false, false, false, false, false, true,  false, false, false, M::Menu,       false, false},
	}, 3, 0);
	// Scenario B: late stereo loss with no complete composed frame (P17: "late capture loss: suppress incomplete image, full world requested")
	runScenario("capture loss", {
		{"match",                true,  true,  true,  true,  false, true,  true,  false, true,  true,  false, false, M::Tabletop,   false, false},
		{"eyes vanish, no game", true,  true,  true,  true,  false, false, false, false, false, true,  false, false, M::Recovery,   true,  true},
		{"eyes vanish, no game", true,  true,  true,  true,  false, false, false, false, false, true,  false, false, M::Recovery,   true,  true},
		{"full world restored",  true,  true,  true,  true,  false, true,  true,  false, true,  true,  false, false, M::Tabletop,   false, false},
	}, 1, 2);
	// Scenario C: stereo lost but a complete composed frame exists: cinematic fallback, not a recovery card
	runScenario("stereo lost, composed ok", {
		{"match",                true,  true,  true,  true,  false, true,  true,  false, true,  true,  false, false, M::Tabletop,   false, false},
		{"eyes lost, game ok",   true,  true,  true,  true,  false, false, false, false, true,  true,  false, false, M::Cinematic,  false, false},
	}, 1, 0);
	// Scenario D: an observer frame whose stereo capture failed must not stay in ground view (sky-only capture): recovery + full world
	runScenario("observer capture loss", {
		{"match",                true,  true,  true,  true,  false, true,  true,  false, true,  true,  false, false, M::Tabletop,   false, false},
		{"observer, eyes lost",  true,  true,  true,  true,  false, false, false, false, true,  true,  false, true,  M::Recovery,   true,  true},
	}, 1, 1);
	// Scenario E: a network / replay match: not stereo eligible, planar world + UI (split ready, world capture present)
	runScenario("network match", {
		{"menu",                 true,  false, false, false, false, false, false, false, true,  false, false, false, M::Menu,       false, false},
		{"planar match",         true,  true,  false, true,  false, false, false, true,  true,  true,  false, false, M::Tabletop,   false, false},
		{"planar, no world tex", true,  true,  false, true,  false, false, false, false, true,  true,  false, false, M::Cinematic,  false, false},
	}, 2, 0);
	// Scenario F: nothing produced before the very first frame completes: not a recovery card in the shell (Loading = indicator)
	runScenario("cold start", {
		{"no frame yet",         true,  false, false, false, false, false, false, false, false, false, false, false, M::Loading,    true,  true},
		{"first frame",          true,  false, false, false, false, false, false, false, true,  false, false, false, M::Menu,       false, false},
	}, 1, 1);

	// ---- Quest templates behave exactly as in XrViewMode.h (re-checked here through the visionOS state) ----
	{
		VisionViewState s;
		s.interactiveGame = true; s.uprightGame = true; s.stereoWorld = false;
		xrApplyWorldStartup(s, false);
		CHECK(s.uprightGame && !s.stereoWorld);         // not eligible: unchanged
		xrApplyWorldStartup(s, true);
		CHECK(!s.uprightGame && s.stereoWorld && s.startViewApplied); // P15: gameplay is always tabletop
		s.controlsArmed = true;
		xrRequestWorldView(s, false);
		CHECK(s.stereoWorld && !s.uprightGame && !s.controlsArmed && s.grab.cancelled >= 1);
		VisionViewState h;
		h.splitVisible = true; h.stereoWorld = true;
		CHECK(xrResolveCapturedView(h, true, true, true, true) == false && h.stereoVisible && h.splitVisible);
		VisionViewState n;
		n.splitVisible = true; n.stereoWorld = true;
		CHECK(xrResolveCapturedView(n, true, false, false, false) == true && !n.stereoVisible && !n.splitVisible);
	}
	// ---- the interactive flip cancels a grab in flight and re-arms the start view (Quest: grab.cancel, controlsArmed=false) ----
	{
		VisionViewState s;
		VisionPresentationFacts f;
		f.booted = true; f.canStereoWorld = true;
		visionPresentationBegin(s, f);
		const int before = s.grab.cancelled;
		f.interactiveGame = true;
		const bool stereo = visionPresentationBegin(s, f);
		CHECK(stereo && s.grab.cancelled > before && s.startViewApplied);
		f.canStereoWorld = false; // no longer eligible: stereo request drops at once
		CHECK(!visionPresentationBegin(s, f));
	}
	// ---- the loading presenter: Ground View ends, split / stereo assumptions are dropped, Quest style ----
	{
		VisionViewState s;
		s.stereoVisible = true; s.splitVisible = true; s.observerActive = true;
		visionPresentationLoadingBegin(s);
		CHECK(s.mode == M::Loading && !s.stereoVisible && !s.splitVisible && !s.observerActive && s.loading);
	}
}

// =========================================================================================== 4. panel layout
static XrSurface flatBoard(XrVector3f pos, float width, float yaw = 0) {
	XrSurface s;
	s.width = width;
	s.pose.position = pos;
	s.pose.orientation = xrMul(xrAxisAngle({0, 1, 0}, yaw), xrAxisAngle({1, 0, 0}, -1.57079633f));
	return s;
}

static void testLayout() {
	VisionLayoutInput li;
	li.mode = VisionPresentationMode::Tabletop;
	li.boardPlaced = true;
	li.board = flatBoard({0, 0.8f, -0.9f}, 1.0f);
	li.boardAspect = 9.0f / 16.0f;
	li.commandRect = xrCommandRect(0.30f);
	li.worldRect = xrViewportRect(0, 0, 1280, 470, 1280, 720);
	li.screenKnown = true;
	li.screen = visionScreenSurface({{0, 0, 0, 1}, {0, 1.5f, 0}});
	VisionPanelPlan p;
	visionLayoutPanels(li, p);
	// compact: the bar and the HUD, two layers and two panels, the same poses
	CHECK(p.layerCount == 2 && p.panelCount == 2);
	CHECK(p.panels[0].kind == kVisionPanelGameUI && p.panels[1].kind == kVisionPanelGameHud);
	CHECK(std::strcmp(p.layers[0].name, "ui-bar") == 0 && std::strcmp(p.layers[1].name, "ui-hud") == 0);
	for (int i = 0; i < 2; ++i) {
		CHECK(p.layerOfPanel[i] == i && p.panelOfLayer[i] == i);
		const GXHostLayer &l = p.layers[i];
		const VisionPanel &pn = p.panels[i];
		VNEAR((XrVector3f{l.position[0], l.position[1], l.position[2]}), pn.surface.pose.position);
		NEAR(l.size[0], pn.surface.width);
		NEAR(l.size[1], pn.surface.width * pn.aspect);
		NEAR(l.orientation[0], pn.surface.pose.orientation.x); NEAR(l.orientation[3], pn.surface.pose.orientation.w);
		CHECK((l.flags & GX_LAYER_HAS_UVRECT) && (l.flags & GX_LAYER_FLIP_Y) && (l.flags & GX_LAYER_PREMULTIPLIED) && l.target == GX_XRT_UI);
		NEAR(l.uvRect[0], pn.rect.x); NEAR(l.uvRect[1], pn.rect.y); NEAR(l.uvRect[2], pn.rect.w); NEAR(l.uvRect[3], pn.rect.h);
	}
	// the crops are the Quest rectangles: bar = xrCommandRect band at the bottom, HUD = the complement above it
	const XrGameRect bar = li.commandRect;
	NEAR(p.layers[0].uvRect[1], 0.0f); NEAR(p.layers[0].uvRect[3], bar.h);
	NEAR(p.layers[1].uvRect[1], bar.h); NEAR(p.layers[1].uvRect[3], 1.0f - bar.h);
	// together they tile the canvas: the HUD's bottom edge is the bar's top edge, same width, same plane
	const float full = 720.0f / 1280.0f;
	const XrSurface s0 = p.panels[0].surface, s1 = p.panels[1].surface;
	const XrVector3f up0 = xrRotate(s0.pose.orientation, {0, 1, 0});
	const float barTop = xrDot(xrSub(s0.pose.position, s0.pose.position), up0) + 0.5f * p.layers[0].size[1];
	const float hudBottom = xrDot(xrSub(s1.pose.position, s0.pose.position), up0) - 0.5f * p.layers[1].size[1];
	NEAR(barTop, hudBottom);
	NEAR(p.layers[0].size[1] + p.layers[1].size[1], full * s0.width);
	CHECK(vnear(xrRotate(s0.pose.orientation, {0, 0, 1}), xrRotate(s1.pose.orientation, {0, 0, 1})));
	// the canvas leans back 24 degrees toward the player: normal points up and toward the near edge (board local -Y)
	const XrVector3f n = xrRotate(s0.pose.orientation, {0, 0, 1});
	const XrVector3f nBoard = visionBoardToLocal(li.board, xrAdd(li.board.pose.position, n));
	NEARE(nBoard.z, std::sin(kVisionUiLeanBackRad), 1e-3f);
	NEARE(nBoard.y, -std::cos(kVisionUiLeanBackRad), 1e-3f);
	// it stands behind the far edge with the configured gap and lift (bottom edge of the canvas)
	{
		const XrVector3f up = xrRotate(s0.pose.orientation, {0, 1, 0});
		const XrVector3f bottom = xrSub(s0.pose.position, xrScale(up, 0.5f * p.layers[0].size[1]));
		const XrVector3f local = visionBoardToLocal(li.board, bottom);
		NEARE(local.y, li.board.width * li.boardAspect * 0.5f + kVisionUiGapM, 1e-3f);
		NEARE(local.z, kVisionUiLiftM, 1e-3f);
		NEARE(local.x, 0.0f, 1e-3f);
	}
	// expanded (a dialog): one piece, the whole canvas, sharing the bottom edge of the compact bar
	li.expandedUI = true;
	VisionPanelPlan e;
	visionLayoutPanels(li, e);
	CHECK(e.layerCount == 1 && e.panelCount == 1 && e.panels[0].kind == kVisionPanelGameUI);
	NEAR(e.layers[0].uvRect[2], 1.0f); NEAR(e.layers[0].uvRect[3], 1.0f);
	NEAR(e.layers[0].size[1], full * e.layers[0].size[0]);
	{
		const XrSurface se = e.panels[0].surface;
		const XrVector3f upE = xrRotate(se.pose.orientation, {0, 1, 0});
		const XrVector3f bottomE = xrSub(se.pose.position, xrScale(upE, 0.5f * e.layers[0].size[1]));
		const XrVector3f bottomC = xrSub(s0.pose.position, xrScale(up0, 0.5f * p.layers[0].size[1]));
		CHECK(vnear(bottomE, bottomC));
	}
	li.expandedUI = false;
	// planar world (not stereo eligible): the world crop on the board + the UI pieces
	li.planarWorld = true;
	VisionPanelPlan pw;
	visionLayoutPanels(li, pw);
	CHECK(pw.layerCount == 3 && pw.panels[0].kind == kVisionPanelWorld2D && pw.layers[0].target == GX_XRT_WORLD);
	NEAR(pw.layers[0].uvRect[3], li.worldRect.h);
	VNEAR((XrVector3f{pw.layers[0].position[0], pw.layers[0].position[1], pw.layers[0].position[2]}), li.board.pose.position);
	li.planarWorld = false;
	// menus / loading / cinematic / recovery: one upright screen, the composed target, full frame
	for (auto m : {VisionPresentationMode::Menu, VisionPresentationMode::Loading, VisionPresentationMode::Cinematic, VisionPresentationMode::Recovery}) {
		li.mode = m;
		VisionPanelPlan u;
		visionLayoutPanels(li, u);
		CHECK(u.layerCount == 1 && u.panelCount == 1 && u.panels[0].kind == kVisionPanelGameScreen && u.layers[0].target == GX_XRT_GAME);
		NEAR(u.layers[0].size[0], kVisionScreenWidthM);
		NEAR(u.layers[0].size[1], kVisionScreenWidthM * full);
		NEAR(u.layers[0].uvRect[2], 1.0f);
		// in front of the anchor (head at 1.5 m, facing -Z): 1.1 m ahead, 2 cm below head height
		VNEAR((XrVector3f{u.layers[0].position[0], u.layers[0].position[1], u.layers[0].position[2]}), (XrVector3f{0, 1.48f, -1.1f}));
	}
	// ground view and idle: no panels; a tabletop without a placed board has none either
	li.mode = VisionPresentationMode::GroundView;
	VisionPanelPlan gv;
	visionLayoutPanels(li, gv);
	CHECK(gv.layerCount == 0 && gv.panelCount == 0);
	li.mode = VisionPresentationMode::Tabletop; li.boardPlaced = false;
	visionLayoutPanels(li, gv);
	CHECK(gv.layerCount == 0);
	li.mode = VisionPresentationMode::Menu; li.screenKnown = false;
	visionLayoutPanels(li, gv);
	CHECK(gv.layerCount == 0);

	// ---- the board moves: the UI follows it rigidly; size of the UI does not change with the board width (readability) ----
	li.mode = VisionPresentationMode::Tabletop; li.boardPlaced = true; li.screenKnown = true;
	li.board = flatBoard({0.4f, 0.9f, -1.3f}, 1.6f, 0.7f);
	VisionPanelPlan moved;
	visionLayoutPanels(li, moved);
	NEAR(moved.layers[0].size[0], kVisionUiCanvasWidthM);
	const XrVector3f ln = visionBoardToLocal(li.board, {moved.layers[0].position[0], moved.layers[0].position[1], moved.layers[0].position[2]});
	CHECK(ln.y > li.board.width * li.boardAspect * 0.5f); // behind the (larger) far edge
	NEARE(ln.x, 0.0f, 1e-3f);
	// exceptional inputs never produce NaN layers
	li.commandRect = {0, 0, 0, 0};
	VisionPanelPlan degenerate;
	visionLayoutPanels(li, degenerate);
	for (int i = 0; i < degenerate.layerCount; ++i) CHECK(std::isfinite(degenerate.layers[i].size[1]) && std::isfinite(degenerate.layers[i].position[1]));
}

// =========================================================================================== 5. hit-testing through the real interaction layer
struct Sim {
	VisionScriptedBridge bridge;
	VisionInteraction vi;
	VisionHostState host;
	VisionInteractionOutput out;
	std::vector<XRInteractionEvent> queue;
	double t = 100.0;
	Sim() : vi(&bridge, VisionConfig()) {
		host.head = {{0, 0, 0, 1}, kHead};
		host.boardPlaced = true;
		host.board = flatBoard({0, 0.8f, -0.9f}, 1.0f);
		host.boardAspect = 9.0f / 16.0f;
		host.time_s = t;
		bridge.interactive = bridge.canStereo = bridge.canAdjust = true;
		syncEngine();
	}
	void syncEngine() {
		host.engine.interactiveGame = bridge.interactive; host.engine.canStereoWorld = bridge.canStereo; host.engine.canAdjustWorld = bridge.canAdjust;
		host.engine.expandedUI = bridge.expanded; host.engine.canObserveGround = bridge.canObserve;
	}
	void frame() {
		host.time_s = t; t += 1.0 / 60.0;
		syncEngine();
		vi.update(host, queue.data(), queue.size(), out);
		queue.clear();
		if (out.boardChanged) { host.board = out.board; host.boardPlaced = true; }
	}
	void frames(int n) { for (int i = 0; i < n; ++i) frame(); }
	XRInteractionEvent base(XRInteractionType type, XrVector3f hp) {
		XRInteractionEvent e = {};
		e.type = type; e.pointer_id = 1; e.hand = XR_HAND_RIGHT; e.pointer_kind = XR_POINTER_INDIRECT_PINCH; e.timestamp_s = t;
		e.position_world = {hp.x, hp.y, hp.z}; e.has_hand_pose = true; e.hand_pose.position = {hp.x, hp.y, hp.z}; e.hand_pose.orientation = {0, 0, 0, 1};
		return e;
	}
	void begin(XrVector3f gazeTarget, XrVector3f hp) {
		XRInteractionEvent e = base(XR_EVENT_PINCH_BEGIN, hp);
		const XrVector3f d = xrSub(gazeTarget, kHead);
		const float l = xrLength(d);
		e.has_ray = true; e.ray_world.origin = {kHead.x, kHead.y, kHead.z}; e.ray_world.direction = {d.x / l, d.y / l, d.z / l};
		queue.push_back(e);
	}
	void move(XrVector3f hp) { queue.push_back(base(XR_EVENT_PINCH_DRAG, hp)); }
	void end(XrVector3f hp) { queue.push_back(base(XR_EVENT_PINCH_END, hp)); }
};
static const XrVector3f kHand = {0.1f, 1.2f, -0.4f};

static void testHitTesting() {
	VisionLayoutInput li;
	li.mode = VisionPresentationMode::Tabletop;
	li.boardPlaced = true;
	li.board = flatBoard({0, 0.8f, -0.9f}, 1.0f);
	li.commandRect = xrCommandRect(0.30f);
	li.screenKnown = true;
	li.screen = visionScreenSurface({{0, 0, 0, 1}, kHead});
	VisionPanelPlan plan;
	visionLayoutPanels(li, plan);
	const float bar = li.commandRect.h;
	// pinch the center of the control bar with a gaze ray: the engine pointer gets the pixel at the middle of the bar band
	{
		Sim s;
		for (int i = 0; i < plan.panelCount; ++i) s.host.panels[s.host.panelCount++] = plan.panels[i];
		s.begin(plan.panels[0].surface.pose.position, kHand);
		s.frames(3);
		CHECK(s.out.mode == VisionMode::PanelPointer && s.out.panelPointerPanel == 0);
		NEARE(s.out.panelU, 0.5f, 1e-3f); NEARE(s.out.panelV, 0.5f, 1e-3f);
		NEARE(s.bridge.lastPointerX, 0.5f * 1279.0f, 1.0f);
		// the bar is the bottom band of the canvas: its middle is at pixel y = 719 * (1 - bar/2)
		NEARE(s.bridge.lastPointerY, (1.0f - 0.5f * bar) * 719.0f, 1.5f);
		CHECK(s.bridge.lastRoute == kVisionRouteWindows);
		s.end(kHand);
		s.frames(3);
		CHECK(s.out.mode == VisionMode::Idle);
	}
	// a point in the upper part of the HUD panel maps above the bar band; the HUD is hit only where a window exists (HasUIAt answers true here)
	{
		Sim s;
		for (int i = 0; i < plan.panelCount; ++i) s.host.panels[s.host.panelCount++] = plan.panels[i];
		const XrSurface hud = plan.panels[1].surface;
		const XrVector3f top = xrAdd(hud.pose.position, xrRotate(hud.pose.orientation, {0, 0.45f * hud.width * plan.panels[1].aspect, 0}));
		s.begin(top, kHand);
		s.frames(3);
		CHECK(s.out.panelPointerPanel == 1);
		CHECK(s.bridge.lastPointerY < 0.5f * 719.0f); // upper half of the canvas
		NEARE(s.bridge.lastPointerY, (1.0f - (bar + (1.0f - bar) * 0.95f)) * 719.0f, 2.0f);
	}
	// upright screen: pixel = uv over the whole frame
	{
		VisionLayoutInput mi = li;
		mi.mode = VisionPresentationMode::Menu;
		VisionPanelPlan mp;
		visionLayoutPanels(mi, mp);
		Sim s;
		s.bridge.interactive = false;
		s.host.panels[s.host.panelCount++] = mp.panels[0];
		const XrSurface sc = mp.panels[0].surface;
		const XrVector3f corner = xrAdd(sc.pose.position, xrRotate(sc.pose.orientation, {0.25f * sc.width, 0.25f * sc.width * mp.panels[0].aspect, 0}));
		s.begin(corner, kHand);
		s.frames(3);
		CHECK(s.out.panelPointerPanel == 0);
		NEARE(s.bridge.lastPointerX, 0.75f * 1279.0f, 1.5f);
		NEARE(s.bridge.lastPointerY, 0.25f * 719.0f, 1.5f);
		// feedback: the pointer dot lands where the pinch is, in front of the panel
		VisionFeedbackBuilder fb;
		GXHostFeedback f;
		VisionPanelPlan plan2 = mp;
		fb.build(s.out, s.host, plan2, gxGraphicsDefaults(), f);
		CHECK((f.flags & GX_FB_PANEL_POINTER) && f.pointerLayer == 0);
		VNEAR((XrVector3f{f.pointerPos[0], f.pointerPos[1], f.pointerPos[2]}), xrAdd(corner, xrRotate(sc.pose.orientation, {0, 0, kVisionLayerLiftM})));
	}
	// text-size consistency: a pixel of the UI target has the same physical size on the bar and on the HUD piece
	{
		const float texelBar = plan.panels[0].surface.width / (plan.panels[0].rect.w * 1280.0f);
		const float texelHud = plan.panels[1].surface.width / (plan.panels[1].rect.w * 1280.0f);
		NEAR(texelBar, texelHud);
		const float vBar = plan.layers[0].size[1] / (plan.panels[0].rect.h * 720.0f);
		NEAR(vBar, texelBar);
	}
}

// =========================================================================================== 6. readability
static void testReadability() {
	// UI text of 1280x720 on the tabletop canvas seen from the default seat: head 0.9 m ahead of the board center, 0.45 m above the table
	VisionLayoutInput li;
	li.mode = VisionPresentationMode::Tabletop;
	li.boardPlaced = true;
	li.board = flatBoard({0, 0.0f, -0.9f}, 1.0f);   // table at y = 0
	li.commandRect = xrCommandRect(0.30f);
	VisionPanelPlan plan;
	visionLayoutPanels(li, plan);
	const XrVector3f head = {0, 0.45f, 0};
	const float dBar = visionDistanceToSurface(head, plan.panels[0].surface);
	const VisionReadability r10 = visionReadability(plan.panels[0].surface.width, 1280, dBar, 10.0f);
	// documented numbers (docs/visionos-presentation.md section 5): distance ~1.4 m, ~2.9 arcmin per texel, a 10 px glyph ~ 29 arcmin
	NEARE(dBar, 1.39f, 0.08f);
	NEARE(r10.arcminPerTexel, 3.0f, 0.3f);
	CHECK(r10.glyphArcmin >= 24.0f && r10.glyphArcmin <= 36.0f);
	// the smallest text (8 px) still reads at the readability floor of 20 arcmin
	const VisionReadability r8 = visionReadability(plan.panels[0].surface.width, 1280, dBar, 8.0f);
	CHECK(r8.glyphArcmin >= 20.0f);
	// the upright screen
	VisionLayoutInput mi = li;
	mi.mode = VisionPresentationMode::Menu; mi.screenKnown = true; mi.screen = visionScreenSurface({{0, 0, 0, 1}, {0, 0.45f, 0}});
	VisionPanelPlan mp;
	visionLayoutPanels(mi, mp);
	const float dScreen = visionDistanceToSurface(head, mp.panels[0].surface);
	NEARE(dScreen, 1.1f, 0.03f);
	const VisionReadability rs = visionReadability(kVisionScreenWidthM, 1280, dScreen, 10.0f);
	NEARE(rs.arcminPerTexel, 3.3f, 0.1f);
	// formulas: small-angle sanity and monotonicity
	const VisionReadability a = visionReadability(1.0f, 1000, 1.0f, 1.0f);
	NEARE(a.arcminPerTexel, 3.4377f, 0.01f);  // 1 mm at 1 m = 3.4377 arcmin
	NEARE(a.texelsPerDegree, 60.0f / a.arcminPerTexel, 1e-3f);
	const VisionReadability b = visionReadability(1.0f, 1000, 2.0f, 1.0f);
	CHECK(b.arcminPerTexel < a.arcminPerTexel);
	// 1920 wide UI on the same panel is sampled about 1:1 at 34 px/degree, 1280 is magnified
	const VisionReadability d720 = visionReadability(plan.panels[0].surface.width, 1280, dBar, 10.0f, 34.0f);
	const VisionReadability d1080 = visionReadability(plan.panels[0].surface.width, 1920, dBar, 10.0f, 34.0f);
	CHECK(d720.texelsPerDisplayPixel < 0.65f && d1080.texelsPerDisplayPixel > 0.8f && d1080.texelsPerDisplayPixel < 1.1f);
	// the audit table of docs/visionos-presentation.md section 5 (printed so the document can quote the run)
	printf("readability: tabletop UI canvas %.2f m wide at %.2f m (eye to bar center): %.2f arcmin/texel; glyph 8/10/12 px = %.1f / %.1f / %.1f arcmin; "
		"720p %.2f texel/display px, 1080p %.2f\n", plan.panels[0].surface.width, dBar, r10.arcminPerTexel, r8.glyphArcmin, r10.glyphArcmin,
		visionReadability(plan.panels[0].surface.width, 1280, dBar, 12.0f).glyphArcmin, d720.texelsPerDisplayPixel, d1080.texelsPerDisplayPixel);
	printf("readability: upright screen %.2f m wide at %.2f m: %.2f arcmin/texel; glyph 8/10/12 px = %.1f / %.1f / %.1f arcmin\n", kVisionScreenWidthM, dScreen,
		rs.arcminPerTexel, visionReadability(kVisionScreenWidthM, 1280, dScreen, 8.0f).glyphArcmin, rs.glyphArcmin,
		visionReadability(kVisionScreenWidthM, 1280, dScreen, 12.0f).glyphArcmin);
	CHECK(!std::isfinite(visionReadability(0, 1280, 1, 10).arcminPerTexel) || visionReadability(0, 1280, 1, 10).arcminPerTexel == 0.0f);
}

// =========================================================================================== 7. feedback
static void testFeedback() {
	Sim s;
	VisionPanelPlan plan;
	VisionFeedbackBuilder fb;
	GXHostFeedback f;
	const GXGraphicsSettings gfx = gxGraphicsDefaults();
	// idle: only the grab bar (visible near the board when looked at), nothing else
	s.frames(2);
	fb.build(s.out, s.host, plan, gfx, f);
	CHECK(f.pointerLayer == -1 && f.placementLegal == -1 && (f.flags & (GX_FB_BOX | GX_FB_CURSOR | GX_FB_PLACEMENT | GX_FB_GROUND_TARGET)) == 0);
	// a box drag
	s.begin(visionBoardToWorld(s.host.board, {-0.2f, -0.05f, 0}), kHand);
	s.frame();
	for (int i = 1; i <= 12; ++i) { s.move({kHand.x + 0.01f * i, kHand.y, kHand.z - 0.005f * i}); s.frame(); }
	CHECK(s.out.mode == VisionMode::BoxSelect && s.out.box.active);
	fb.build(s.out, s.host, plan, gfx, f);
	CHECK((f.flags & GX_FB_BOX) && f.boxAdditive == 0 && (f.flags & GX_FB_CURSOR));
	for (int i = 0; i < 4; ++i) VNEAR((XrVector3f{f.boxCorners[i][0], f.boxCorners[i][1], f.boxCorners[i][2]}), s.out.box.corners[i]);
	NEAR(f.boardWidth, 1.0f);
	// the focus marker switch removes cursor markers but never the box
	GXGraphicsSettings noMarker = gfx;
	noMarker.flags = GX_GFX_COMFORT_FADE;
	fb.build(s.out, s.host, plan, noMarker, f);
	CHECK((f.flags & GX_FB_BOX) && !(f.flags & (GX_FB_CURSOR | GX_FB_RAY | GX_FB_PANEL_POINTER | GX_FB_HOVER | GX_FB_WAYPOINT)));
	s.end({kHand.x + 0.12f, kHand.y, kHand.z - 0.06f});
	s.frames(2);
	CHECK(s.out.mode == VisionMode::Idle && !s.out.box.active);
	// a tap leaves a destination marker that fades after 1.2 s
	{
		Sim t;
		VisionFeedbackBuilder fb2;
		t.begin(visionBoardToWorld(t.host.board, {0.1f, 0.05f, 0}), kHand);
		XrVector3f lastCursor = {};
		for (int i = 0; i < 3; ++i) { t.frame(); fb2.build(t.out, t.host, plan, gfx, f); if (t.out.cursorVisible) lastCursor = t.out.cursorWorld; }
		t.end(kHand);
		t.frame();
		CHECK(t.out.events & kVisionEventTap);
		fb2.build(t.out, t.host, plan, gfx, f);
		CHECK((f.flags & GX_FB_WAYPOINT) && f.waypointAge < 0.05f);
		VNEAR((XrVector3f{f.waypoint[0], f.waypoint[1], f.waypoint[2]}), lastCursor);
		t.frames(30);
		fb2.build(t.out, t.host, plan, gfx, f);
		CHECK((f.flags & GX_FB_WAYPOINT) && f.waypointAge > 0.4f && f.waypointAge < 0.6f);
		t.frames(60);
		fb2.build(t.out, t.host, plan, gfx, f);
		CHECK(!(f.flags & GX_FB_WAYPOINT));
	}
	// Ground View: comfort fade veil at the switch, then ramp down; the switch turns it off
	{
		Sim g;
		g.bridge.canObserve = true;
		XRInteractionEvent c = {};
		c.type = XR_EVENT_COMMAND; c.command = XR_CMD_ENTER_GROUND_VIEW;
		g.queue.push_back(c);
		g.frames(3);
		g.begin(visionBoardToWorld(g.host.board, {0.2f, 0.1f, 0}), kHand);
		g.frame();
		VisionFeedbackBuilder fg;
		fg.build(g.out, g.host, plan, gfx, f);
		CHECK((f.flags & GX_FB_GROUND_TARGET) && f.groundTargetValid == 1);
		g.end(kHand);
		g.frame();
		CHECK(g.out.ground.mode == XrObserverMode::Active);
		fg.build(g.out, g.host, plan, gfx, f);
		NEAR(f.fadeAlpha, 1.0f);
		g.frames(6);
		fg.build(g.out, g.host, plan, gfx, f);
		CHECK(f.fadeAlpha > 0.0f && f.fadeAlpha < 1.0f);
		GXGraphicsSettings off = gfx;
		off.flags = GX_GFX_FOCUS_MARKER;
		fg.build(g.out, g.host, plan, off, f);
		NEAR(f.fadeAlpha, 0.0f);
		g.frames(20);
		fg.build(g.out, g.host, plan, gfx, f);
		NEAR(f.fadeAlpha, 0.0f);
	}
	// placement cue: legality, cancel armed and the point
	{
		Sim p;
		p.bridge.pendingPlacement = true;
		p.bridge.legal = 0;
		p.host.engine.placementPending = true; p.host.engine.canRotatePlacement = true; p.host.engine.placementLegal = 0;
		p.begin(visionBoardToWorld(p.host.board, {0.1f, 0.0f, 0}), kHand);
		p.frames(2);
		VisionFeedbackBuilder fp;
		p.host.engine.placementPending = true; p.host.engine.placementLegal = 0;
		fp.build(p.out, p.host, plan, gfx, f);
		if (p.out.placement.active) CHECK((f.flags & GX_FB_PLACEMENT) && f.placementLegal == 0);
		else CHECK(!(f.flags & GX_FB_PLACEMENT));
	}
	// hover / region: a pinch on a panel lights that panel's outline
	{
		Sim h;
		VisionLayoutInput li;
		li.mode = VisionPresentationMode::Tabletop; li.boardPlaced = true; li.board = h.host.board; li.commandRect = xrCommandRect(0.30f);
		VisionPanelPlan pp;
		visionLayoutPanels(li, pp);
		for (int i = 0; i < pp.panelCount; ++i) h.host.panels[h.host.panelCount++] = pp.panels[i];
		h.begin(pp.panels[0].surface.pose.position, kHand);
		h.frames(2);
		VisionFeedbackBuilder fh;
		fh.build(h.out, h.host, pp, gfx, f);
		CHECK((f.flags & GX_FB_PANEL_POINTER) && f.pointerLayer == 0);
		if (h.out.regionCount > 0) { bool any = false; for (int i = 0; i < h.out.regionCount; ++i) any = any || h.out.regions[i].active; if (any) CHECK(f.flags & GX_FB_HOVER); }
	}
}

// =========================================================================================== 8. workspace
static void testWorkspace() {
	char path[] = "/tmp/vision-presentation-layout.XXXXXX";
	const int fd = mkstemp(path);
	close(fd);
	VisionWorkspace w;
	CHECK(!w.loadFrom("/nonexistent/none.cfg", "", 1)); // fresh install
	CHECK(w.dirty && !w.loaded);
	const XRFrameInfo f = makeFrame(1, 1.0, {1.0f, 1.6f, -2.0f});
	CHECK(!w.anchorKnown);
	w.ensureAnchor(f);
	CHECK(w.anchorKnown);
	// the anchor is the head heading: origin at the head, facing -Z
	VNEAR(w.anchor.position, (XrVector3f{1.0f, 1.6f, -2.0f}));
	VNEAR(xrRotate(w.anchor.orientation, {0, 0, -1}), (XrVector3f{0, 0, -1}));
	XRFrameInfo untracked = makeFrame(2, 1.1, {5, 5, 5});
	untracked.head_tracked = false;
	VisionWorkspace w2;
	w2.ensureAnchor(untracked);
	CHECK(!w2.anchorKnown); // never anchored on the fallback pose
	// a head turned 90 degrees (facing +X): the screen is 1.1 m in that direction
	XRFrameInfo turned = makeFrame(3, 1.2, {0, 1.5f, 0});
	turned.head_pose.orientation = {0, -0.70710678f, 0, 0.70710678f}; // yaw -90 degrees about Y: -Z -> +X
	VisionWorkspace w3;
	w3.ensureAnchor(turned);
	const XrSurface sc = w3.screenSurface();
	VNEAR(sc.pose.position, (XrVector3f{1.1f, 1.48f, 0}));
	NEAR(sc.width, kVisionScreenWidthM);
	// record + save + load: preferences and the WIDTH come back, poses do not (Quest policy P20)
	const XrSurface board = flatBoard({1.0f, 0.8f, -2.9f}, 1.3f);
	const XrSurface bar = visionUiBarSurface(board, 9.0f / 16.0f, 9.0f / 16.0f, xrCommandRect(0.3f));
	w.layout.unitRings = false;
	w.layout.healthBars = false;
	w.record(board, bar, 1.7f);
	CHECK(w.dirty && w.save(path) && !w.dirty && w.saves == 1);
	VisionWorkspace r;
	CHECK(r.loadFrom(path, "", 1) && r.loaded);
	CHECK(!r.layout.unitRings && !r.layout.healthBars && r.layout.boardFrame); // preferences restored
	NEAR(r.layout.worldZoom, 1.7f);
	CHECK(r.widthPending);
	NEAR(r.savedBoardWidth, 1.3f);
	// P20: the arrangement itself is the free-standing default again
	VNEAR(r.layout.relative[1].pose.position, (XrVector3f{0, -0.54f, -0.55f}));
	NEAR(r.layout.relative[1].width, 1.65f);
	// saved poses are relative to the anchor: the file holds the board 1.0 m .. behind the launch heading origin
	{
		XrLayout raw;
		CHECK(raw.load(path) && raw.formatVersion == 11);
		const XrSurface saved = raw.relative[1];
		VNEAR(saved.pose.position, (XrVector3f{0.0f, -0.8f, -0.9f}));
		NEAR(saved.width, 1.3f);
		const XrVector3f up = xrRotate(saved.pose.orientation, {0, 0, 1});
		NEARE(up.y, 1.0f, 1e-3f);
		NEAR(raw.worldZoom, 1.7f);
	}
	// a default-width layout does not trigger a restore
	VisionWorkspace d;
	d.ensureAnchor(f);
	d.record(flatBoard({1.0f, 0.8f, -2.9f}, 1.0f), bar, 1.0f);
	CHECK(d.save(path));
	VisionWorkspace dr;
	dr.loadFrom(path, "", 1);
	CHECK(!dr.widthPending);
	// a corrupt file is ignored (defaults, dirty so it is rewritten)
	{
		FILE *fp = fopen(path, "w");
		fputs("this is not a layout\n", fp);
		fclose(fp);
		VisionWorkspace c;
		CHECK(!c.loadFrom(path, "", 1) && c.dirty && !c.widthPending);
	}
	// an absurd saved width is refused
	{
		XrLayout bad;
		bad.relative[1].width = 3.9f;
		CHECK(bad.save(path));
		VisionWorkspace c;
		CHECK(c.loadFrom(path, "", 1) && !c.widthPending);
	}
	unlink(path);
}

// =========================================================================================== 9. the whole pipeline
struct EngineScript {
	VisionScriptedBridge bridge;
	VisionFrameDriver driver;
	VisionPresentation pres;
	std::string layoutPath;
	int frameNo = 0;
	double t = 50.0;
	VisionPresentationFacts pre, post;
	GXHostFrameOutput out;
	VisionPresentationOutput plan;
	XrWorldFrame world;
	bool eyesProduced = true;   // the engine really rendered the stereo targets

	EngineScript() : driver(&bridge) {}
	// One engine frame in the exact order of GXEngineHostEngine.cpp.
	void frame(bool observerRendered = false) {
		const XRFrameInfo info = makeFrame(uint64_t(++frameNo), t, kHead);
		t += 1.0 / 45.0;
		VisionPresentationInput in;
		in.frame = &info;
		in.facts = pre;
		in.layoutPath = layoutPath.c_str();
		pres.update(in, driver, plan);
		world = plan.world;
		driver.step(info, true, plan.plan.panels, plan.plan.panelCount, world);
		post = pre;
		post.eyeTexture[0] = post.eyeTexture[1] = plan.stereoWorld && eyesProduced;
		pres.finish(post, driver, world, in, out);
		(void)observerRendered;
	}
};
static void setFacts(EngineScript &e, bool interactive, bool split, bool stereoEyes, bool ui, bool game, bool world = false) {
	e.pre = VisionPresentationFacts();
	e.pre.booted = true;
	e.pre.interactiveGame = interactive;
	e.pre.canStereoWorld = interactive;
	e.pre.splitReady = split;
	e.pre.gameTexture = game; e.pre.uiTexture = ui; e.pre.worldTexture = world;
	e.pre.eyeTexture[0] = e.pre.eyeTexture[1] = stereoEyes;
	e.pre.commandRect = xrCommandRect(0.30f);
	e.pre.worldRect = xrViewportRect(0, 0, 1280, 470, 1280, 720);
	e.bridge.interactive = interactive; e.bridge.canStereo = interactive; e.bridge.canAdjust = interactive;
}

static void testPipeline() {
	char path[] = "/tmp/vision-presentation-pipe.XXXXXX";
	close(mkstemp(path));
	unlink(path);
	EngineScript e;
	e.layoutPath = path;
	g_queue.clear();
	// ---- not booted: nothing but the indicator ----
	e.pre = VisionPresentationFacts();
	e.frame();
	CHECK(e.out.layerCount == 0 && !e.out.stereoValid && e.out.presentationMode == int(VisionPresentationMode::Idle));
	// ---- menu (engine booted, shell): the upright screen, the interaction table names it ----
	setFacts(e, false, false, false, false, true);
	e.frame();
	e.frame();
	CHECK(e.out.presentationMode == int(VisionPresentationMode::Menu) && e.out.layerCount == 1 && std::strcmp(e.out.layers[0].name, "screen") == 0);
	CHECK(e.plan.plan.panelCount == 1 && e.plan.plan.panels[0].kind == kVisionPanelGameScreen);
	CHECK(!e.out.stereoValid && !e.plan.world.enabled && (e.plan.request.targetMask & GX_TARGET_GAME) && !(e.plan.request.targetMask & GX_TARGET_STEREO));
	CHECK(e.out.hasFocus);
	// the published layers of frame N are the panel table of frame N+1 (contract of docs/visionos-interaction.md section 9)
	{
		e.frame();
		CHECK(uint32_t(e.plan.plan.panelCount) == e.out.layerCount);
		for (uint32_t i = 0; i < e.out.layerCount; ++i) {
			VNEAR((XrVector3f{e.out.layers[i].position[0], e.out.layers[i].position[1], e.out.layers[i].position[2]}), e.plan.plan.panels[i].surface.pose.position);
			NEAR(e.out.layers[i].size[0], e.plan.plan.panels[i].surface.width);
		}
	}
	// ---- the match starts: stereo world, UI beside it ----
	setFacts(e, true, true, true, true, true);
	e.frame();
	e.frame();
	e.frame();
	CHECK(e.out.presentationMode == int(VisionPresentationMode::Tabletop) && e.out.stereoValid);
	CHECK(e.plan.world.enabled && e.plan.world.width == 1536 && e.plan.world.height == 1310 && e.plan.stereoWorld);
	CHECK((e.plan.request.targetMask & GX_TARGET_STEREO) && e.plan.request.eyeWidth == 1536 && e.plan.request.eyeHeight == 1310);
	CHECK(e.out.layerCount == 2 && std::strcmp(e.out.layers[0].name, "ui-bar") == 0 && std::strcmp(e.out.layers[1].name, "ui-hud") == 0);
	CHECK(e.driver.boardPlaced() && e.world.board.width > 0 && vnear(e.world.board.pose.position, e.driver.host().board.pose.position));
	CHECK(e.plan.plan.panelCount == 2);
	for (int i = 0; i < 2; ++i) VNEAR((XrVector3f{e.out.layers[i].position[0], e.out.layers[i].position[1], e.out.layers[i].position[2]}), e.plan.plan.panels[i].surface.pose.position);
	// panels are hit by the interaction layer: pinch the bar
	{
		const XrVector3f target = e.plan.plan.panels[0].surface.pose.position;
		XRInteractionEvent ev = {};
		ev.type = XR_EVENT_PINCH_BEGIN; ev.pointer_id = 7; ev.hand = XR_HAND_RIGHT; ev.pointer_kind = XR_POINTER_INDIRECT_PINCH; ev.timestamp_s = e.t;
		ev.has_ray = true;
		const XrVector3f d = xrSub(target, kHead);
		const float l = xrLength(d);
		ev.ray_world.origin = {kHead.x, kHead.y, kHead.z}; ev.ray_world.direction = {d.x / l, d.y / l, d.z / l};
		ev.position_world = {0.1f, 1.2f, -0.4f}; ev.has_hand_pose = true; ev.hand_pose.position = ev.position_world; ev.hand_pose.orientation = {0, 0, 0, 1};
		g_queue.push_back(ev);
		e.frame();
		e.frame();
		CHECK(e.driver.output().mode == VisionMode::PanelPointer && e.bridge.pointerCalls > 0);
		CHECK((e.out.feedback.flags & GX_FB_PANEL_POINTER) && e.out.feedback.pointerLayer == 0);
		ev.type = XR_EVENT_PINCH_END;
		g_queue.push_back(ev);
		e.frame();
		e.frame();
		CHECK(e.driver.output().mode == VisionMode::Idle);
	}
	// box select on the board through the pipeline: feedback carries the corners
	{
		const XrSurface board = e.driver.host().board;
		XRInteractionEvent ev = {};
		ev.type = XR_EVENT_PINCH_BEGIN; ev.pointer_id = 8; ev.hand = XR_HAND_RIGHT; ev.pointer_kind = XR_POINTER_INDIRECT_PINCH; ev.timestamp_s = e.t;
		ev.has_ray = true;
		const XrVector3f d = xrSub(visionBoardToWorld(board, {-0.2f, -0.05f, 0}), kHead);
		const float l = xrLength(d);
		ev.ray_world.origin = {kHead.x, kHead.y, kHead.z}; ev.ray_world.direction = {d.x / l, d.y / l, d.z / l};
		ev.position_world = {0.1f, 1.2f, -0.4f}; ev.has_hand_pose = true; ev.hand_pose.position = ev.position_world; ev.hand_pose.orientation = {0, 0, 0, 1};
		g_queue.push_back(ev);
		e.frame();
		for (int i = 1; i <= 12; ++i) {
			ev.type = XR_EVENT_PINCH_DRAG; ev.position_world = {0.1f + 0.01f * i, 1.2f, -0.4f - 0.005f * i}; ev.hand_pose.position = ev.position_world;
			g_queue.push_back(ev);
			e.frame();
		}
		CHECK(e.driver.output().mode == VisionMode::BoxSelect && (e.out.feedback.flags & GX_FB_BOX));
		CHECK((e.out.feedback.flags & GX_FB_CURSOR) && e.bridge.triggerHeld);
		ev.type = XR_EVENT_PINCH_END;
		g_queue.push_back(ev);
		e.frame();
		e.frame();
		CHECK(!e.bridge.triggerHeld && !(e.out.feedback.flags & GX_FB_BOX));
	}
	// ---- a dialog opens: the canvas grows to full, the HUD piece disappears ----
	e.pre.expandedUI = true;
	e.bridge.expanded = true;
	e.frame();
	e.frame();
	CHECK(e.out.layerCount == 1 && e.out.layers[0].uvRect[3] == 1.0f);
	e.pre.expandedUI = false;
	e.bridge.expanded = false;
	// ---- a movie starts: split off, the composed frame on the upright panel; then back ----
	setFacts(e, true, false, false, false, true);
	e.frame();
	e.frame();
	CHECK(e.out.presentationMode == int(VisionPresentationMode::Cinematic) && e.out.layerCount == 1 && std::strcmp(e.out.layers[0].name, "screen") == 0);
	setFacts(e, true, true, true, true, true);
	e.frame();
	e.frame();
	CHECK(e.out.presentationMode == int(VisionPresentationMode::Tabletop));
	// ---- capture loss: recovery card text and the full-world request ----
	setFacts(e, true, true, false, true, false);
	e.eyesProduced = false;
	e.frame();
	CHECK(e.out.presentationMode == int(VisionPresentationMode::Recovery) && e.pres.consumeRequireFullWorld() && e.out.noticeTitle[0] != '\0');
	CHECK(!e.pres.consumeRequireFullWorld());
	CHECK(!e.out.stereoValid);
	e.eyesProduced = true;
	setFacts(e, true, true, true, true, true);
	e.frame();
	e.frame();
	CHECK(e.out.presentationMode == int(VisionPresentationMode::Tabletop) && e.out.noticeTitle[0] == '\0');
	// ---- a loading presenter frame (synchronous loader): upright panel, no simulation involved ----
	{
		GXHostFrameOutput nested;
		VisionPresentationFacts loadingFacts = e.pre;
		VisionPresentationInput in;
		XRFrameInfo info = makeFrame(999, e.t, kHead);
		in.frame = &info;
		e.pres.describeLoading(loadingFacts, e.driver, in, nested);
		CHECK(nested.presentationMode == int(VisionPresentationMode::Loading) && !nested.stereoValid && nested.layerCount == 1);
		CHECK(std::strcmp(nested.layers[0].name, "screen") == 0);
		CHECK(e.pres.view().loading);
		e.pres.loadingEnded();
		CHECK(!e.pres.view().loading);
	}
	// ---- Ground View through the pipeline ----
	setFacts(e, true, true, true, true, true);
	e.bridge.canObserve = true;
	e.pre.canObserveGround = true;
	e.frame();
	{
		XRInteractionEvent c = {};
		c.type = XR_EVENT_COMMAND; c.command = XR_CMD_ENTER_GROUND_VIEW;
		g_queue.push_back(c);
		e.frame();
	}
	for (int i = 0; i < 3; ++i) e.frame();
	{
		const XrSurface board = e.driver.host().board;
		XRInteractionEvent ev = {};
		ev.type = XR_EVENT_PINCH_BEGIN; ev.pointer_id = 9; ev.hand = XR_HAND_RIGHT; ev.pointer_kind = XR_POINTER_INDIRECT_PINCH; ev.timestamp_s = e.t;
		ev.has_ray = true;
		const XrVector3f d = xrSub(visionBoardToWorld(board, {0.2f, 0.1f, 0}), kHead);
		const float l = xrLength(d);
		ev.ray_world.origin = {kHead.x, kHead.y, kHead.z}; ev.ray_world.direction = {d.x / l, d.y / l, d.z / l};
		ev.position_world = {0.1f, 1.2f, -0.4f}; ev.has_hand_pose = true; ev.hand_pose.position = ev.position_world; ev.hand_pose.orientation = {0, 0, 0, 1};
		g_queue.push_back(ev);
		e.frame();
		ev.type = XR_EVENT_PINCH_END;
		g_queue.push_back(ev);
		e.frame();
	}
	CHECK(e.driver.output().ground.mode == XrObserverMode::Active);
		e.frame();
	CHECK(e.world.observer && e.world.observerHead.y > 1.0f);
	// the engine rendered the observer view: mode GroundView, no panels, opaque backdrop, fade at the switch was 1
	{
		e.post = e.pre;
		e.post.eyeTexture[0] = e.post.eyeTexture[1] = true;
		VisionPresentationInput in;
		XRFrameInfo info = makeFrame(1000, e.t, kHead);
		in.frame = &info;
		e.pres.finish(e.post, e.driver, e.world, in, e.out);
		CHECK(e.out.presentationMode == int(VisionPresentationMode::GroundView) && e.out.groundView && e.out.layerCount == 0 && e.out.stereoValid);
		CHECK(e.out.noticeTitle[0] != '\0');
		CHECK(!(e.out.feedback.flags & (GX_FB_BOX | GX_FB_GRAB_BAR | GX_FB_CURSOR)));
		CHECK(e.plan.plan.panelCount == 0);
	}
	// exit: back to the unchanged tabletop
	{
		XRInteractionEvent c = {};
		c.type = XR_EVENT_COMMAND; c.command = XR_CMD_EXIT_GROUND_VIEW;
		g_queue.push_back(c);
		e.frame();
		e.frame();
		e.frame();
		CHECK(e.driver.output().ground.mode == XrObserverMode::Off);
		CHECK(e.out.presentationMode == int(VisionPresentationMode::Tabletop) && e.out.layerCount == 2);
	}
	// ---- the graphics settings reach the world frame ----
	{
		GXGraphicsSettings g = gxGraphicsDefaults();
		g.eyeTier = GX_EYE_HIGH; g.renderScale = 0.75f; g.shadowMode = GX_SHADOWS_VOLUMES;
		const XRFrameInfo info = makeFrame(1200, e.t, kHead);
		VisionPresentationInput in;
		in.frame = &info; in.facts = e.pre; in.gfx = g; in.layoutPath = path;
		e.pres.update(in, e.driver, e.plan);
		CHECK(e.plan.world.width == int(1920 * 0.75f) && e.plan.world.volumeShadows);
		CHECK(e.plan.request.eyeWidth == e.plan.world.width);
	}
	// ---- workspace persistence: move the board with the grab bar (two-hand scale), the layout is written when the gesture ends ----
	unlink(path);
	{
		EngineScript w;
		w.layoutPath = path;
		setFacts(w, true, true, true, true, true);
		w.frame();
		w.frame();
		CHECK(w.pres.saves() == 0);
		XRInteractionEvent c = {};
		c.type = XR_EVENT_COMMAND; c.command = XR_CMD_RECENTER_BOARD;
		g_queue.push_back(c);
		w.frame();
		w.frame();
		CHECK(w.pres.saves() >= 1 && access(path, R_OK) == 0);
		XrLayout saved;
		CHECK(saved.load(path) && saved.formatVersion == 11 && near(saved.relative[1].width, w.driver.host().board.width));
		unlink(path);
	}
	unlink(path);
}

int main() {
	testSettings();
	testEyeAndWorldFrame();
	testStateMachine();
	testLayout();
	testHitTesting();
	testReadability();
	testFeedback();
	testWorkspace();
	testPipeline();
	printf("vision-presentation-test: %d checks passed\n", checks);
	return 0;
}
