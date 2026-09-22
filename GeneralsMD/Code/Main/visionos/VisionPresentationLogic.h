// visionOS presentation logic: PURE functions and value types (no engine, no Apple, no GL). Host tested by
// scripts/qa/vision-presentation-test.sh with recorded scenarios.
//
// This is the visionOS port of the per-frame presentation logic of the Quest host (XrHello.cpp runLoop):
//
//   Quest (XrHello.cpp / XrViewMode.h / XrLayers.h / XrLayout.h / XrWorld.h)     visionOS (this file)
//   ------------------------------------------------------------------------      ---------------------------------------
//   presentation mode change (interactiveGame flips)                             visionPresentationBegin()
//   split ready / lost (P5), xrApplyWorldStartup                                 visionPresentationBegin()
//   XrWorldFrame built from views + surfaces[1] + layout + performance           visionBuildWorldFrame()
//   xrStereoExtent(view size, resolution tier)                                   visionEyeExtent() (+ render scale, ring limits)
//   xrResolveCapturedView after XrGameBoot_Frame (P17 recovery)                  visionPresentationEnd()
//   activeSurface / displayedSurface / surfaceRect / surfaceAspect               visionLayoutPanels()
//   XrLayout::applyTabletopPreset (UI beside the board; visionOS moves the bar)  visionUiBarSurface() / visionUiFarSurface()
//
// The engine facts (XrGameBoot_IsInteractiveGame, SplitReady, texture names ...) arrive as a plain struct so the very
// same logic runs in the host tests, in the scripted fake engine and in the real engine (GXEngineHostEngine.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "GXEngineHostServices.h"
#include "GXGraphicsSettings.h"
#include "VisionHostState.h"
#include "XrLayers.h"
#include "XrLayout.h"
#include "XrViewMode.h"
#include "XrWorld.h"

// ============================================================================================ facts and state

// What the engine says right now. `pre` facts are read before the engine frame, `post` facts after it (textures exist only
// once the frame produced them).
struct VisionPresentationFacts {
	bool booted = false;
	bool interactiveGame = false;   // XrGameBoot_IsInteractiveGame(): a campaign / skirmish / network / replay match
	bool canStereoWorld = false;    // XrGameBoot_CanStereoWorld(): offline skirmish or campaign (Quest eligibility)
	bool splitReady = false;        // XrGameBoot_SplitReady(): world and UI are captured separately
	bool expandedUI = false;        // XrGameBoot_ExpandedUI(): a native dialog needs the whole UI canvas
	bool canObserveGround = false;  // XrGameBoot_CanObserveGround()
	bool gameTexture = false;       // XrGameBoot_GameTexture() != 0 (a complete composed frame exists)
	bool worldTexture = false;      // XrGameBoot_WorldTexture() != 0 (planar world capture)
	bool uiTexture = false;         // XrGameBoot_UITexture() != 0
	bool eyeTexture[2] = {false, false}; // XrGameBoot_StereoTexture(eye) != 0 (atlas: eye 1 mirrors eye 0)
	int gameWidth = 1280, gameHeight = 720;
	XrGameRect worldRect;           // XrGameBoot_WorldRect(): the tactical view inside the composed frame
	XrGameRect commandRect;         // XrGameBoot_CommandRect(): the control bar band (bottom)
};

enum class VisionPresentationMode {
	Idle,       // engine not booted: the compositor shows its status indicator
	Loading,    // a synchronous loader (map load, campaign movie) is presenting frames: the engine's loading screen on the upright panel
	Menu,       // shell, menus, lobby, briefing: the composed frame on the upright panel
	Cinematic,  // an interactive match whose UI split is off (movie, letterbox, camera path, shell overlay): composed frame, upright
	Tabletop,   // interactive match: stereo world on the board + the engine UI beside it (planar world when stereo is not eligible)
	GroundView, // the tabletop's observer (human scale) view
	Recovery    // the world capture failed (P17): explicit card, full world rendering requested
};

inline const char *visionModeName(VisionPresentationMode m)
{
	switch (m) {
		case VisionPresentationMode::Idle: return "";
		case VisionPresentationMode::Loading: return "loading";
		case VisionPresentationMode::Menu: return "menu";
		case VisionPresentationMode::Cinematic: return "cinematic";
		case VisionPresentationMode::Tabletop: return "tabletop";
		case VisionPresentationMode::GroundView: return "ground-view";
		case VisionPresentationMode::Recovery: return "recovery";
	}
	return "";
}

// The fields of XrHello that xrResolveCapturedView / xrApplyWorldStartup / xrRequestWorldView (XrViewMode.h) read and write, so those
// Quest templates are used UNCHANGED. `grab` and `controlsArmed` are the Quest workspace-grab bookkeeping those templates touch.
struct VisionViewState {
	bool presentationKnown = false;
	bool interactiveGame = false;
	bool stereoWorld = true;        // requested (XrLayout::startStereo defaults to true); forced off when the game cannot stereo
	bool uprightGame = false;       // legacy full-panel override (only xrRequestWorldView / xrApplyWorldStartup clear it)
	bool startViewApplied = false;
	bool splitVisible = false;      // world + UI captured separately and usable
	bool stereoVisible = false;     // the stereo eyes are complete and shown
	bool recoveryVisible = false;   // P17: nothing complete to show
	bool controlsArmed = false;
	struct Grab { int cancelled = 0; void cancel() { ++cancelled; } } grab;
	// bookkeeping the tests and logs read
	unsigned modeChanges = 0, splitChanges = 0, recoveries = 0;
	VisionPresentationMode mode = VisionPresentationMode::Idle;
	VisionPresentationMode previousMode = VisionPresentationMode::Idle;
	bool loading = false;           // inside the synchronous loading presenter
	bool observerActive = false;    // Ground View rendered last frame
	bool requireFullWorld = false;  // the host must call d3d8gles_RequireXRFullWorld() (set by visionPresentationEnd, cleared by the caller)
};

// Before the engine frame (XrHello runLoop: presentation mode change, P5 split change, xrApplyWorldStartup). Returns whether the
// stereo world is requested for this frame (XrWorldFrame::enabled).
inline bool visionPresentationBegin(VisionViewState &s, const VisionPresentationFacts &f)
{
	if (!f.canStereoWorld) s.stereoWorld = false;
	if (!s.presentationKnown || f.interactiveGame != s.interactiveGame) {
		s.presentationKnown = true;
		s.interactiveGame = f.interactiveGame;
		s.startViewApplied = false;
		s.grab.cancel();
		s.controlsArmed = false;
		s.uprightGame = false;
		++s.modeChanges;
	}
	if (f.splitReady != s.splitVisible) {
		s.splitVisible = f.splitReady;
		s.grab.cancel();
		s.controlsArmed = false;
		++s.splitChanges;
	}
	// P12.1 / P15: remember the tabletop intent for every eligible interactive game (also after a cinematic).
	xrApplyWorldStartup(s, f.canStereoWorld);
	if (!f.canStereoWorld) s.stereoWorld = false;
	return s.stereoWorld;
}

// After the engine frame (XrHello: xrResolveCapturedView + the observer rule). `observerRendered` is whether the frame was drawn
// in Ground View. Decides what is shown from what the engine really produced and the resulting mode.
inline VisionPresentationMode visionPresentationEnd(VisionViewState &s, const VisionPresentationFacts &f, bool observerRendered)
{
	if (!f.booted) {
		s.recoveryVisible = s.requireFullWorld = s.observerActive = s.stereoVisible = false;
		s.previousMode = s.mode;
		s.mode = VisionPresentationMode::Idle;
		return s.mode;
	}
	const bool eyesReady = f.eyeTexture[0] && f.eyeTexture[1];
	s.recoveryVisible = xrResolveCapturedView(s, f.splitReady, eyesReady, f.worldTexture, f.gameTexture);
	// A sky-only / failed stereo capture must never leave an active observer without an opaque scene and a way back.
	if (observerRendered && !s.stereoVisible) s.recoveryVisible = true;
	s.observerActive = observerRendered && s.stereoVisible;
	s.requireFullWorld = s.recoveryVisible;
	if (s.recoveryVisible) ++s.recoveries;

	VisionPresentationMode mode;
	if (!f.booted) mode = VisionPresentationMode::Idle;
	else if (s.loading) mode = VisionPresentationMode::Loading;
	else if (s.recoveryVisible) mode = f.interactiveGame ? VisionPresentationMode::Recovery : VisionPresentationMode::Loading;
	else if (s.stereoVisible) mode = s.observerActive ? VisionPresentationMode::GroundView : VisionPresentationMode::Tabletop;
	else if (f.interactiveGame && s.splitVisible) mode = VisionPresentationMode::Tabletop; // planar world + UI (stereo not eligible)
	else if (f.interactiveGame) mode = VisionPresentationMode::Cinematic;
	else mode = VisionPresentationMode::Menu;
	if (mode != s.mode) {
		s.previousMode = s.mode;
		s.mode = mode;
	}
	return mode;
}

// The synchronous loading presenter (XrLoadingPresenter::present): a loader is drawing frames of the engine's loading screen.
// Ends Ground View, drops every view assumption the way the Quest does (splitVisible / stereoVisible false).
inline void visionPresentationLoadingBegin(VisionViewState &s)
{
	s.loading = true;
	s.splitVisible = false;
	s.stereoVisible = false;
	s.observerActive = false;
	s.grab.cancel();
	s.controlsArmed = false;
	s.mode = VisionPresentationMode::Loading;
}
inline void visionPresentationLoadingEnd(VisionViewState &s) { s.loading = false; }

// ============================================================================================ world frame and eye size

// Ring limits (visionos/ANGLE/GXXRTargetRing.mm allocates 4 slots x (2 eye targets + depth)): one eye target at most this many
// pixels and this many pixels per side. At the cap: 8.0 MP x 4 B x 2 eyes x 4 slots = 256 MB of colour targets plus the engine's
// D24S8 depth (about 64 MB more), well inside a 16 GB device. The Ultra tier at render scale 1.5 would exceed it and is scaled down.
constexpr int kVisionMinEyeDim = 64;
constexpr int kVisionMaxEyeDim = 4096;
constexpr long long kVisionMaxEyePixels = 8000000;

struct VisionEyeExtent {
	int width = 0, height = 0;
	bool clamped = false;   // the ring limits reduced the requested size
};

// Stereo target size per eye. Quest rule (xrStereoExtent, XrWorld.h: tier budget from the eye viewport, aspect kept) times the
// user's render scale, then the ring limits. `atlas`: both eyes share one target, so the width limit applies to twice the width.
inline VisionEyeExtent visionEyeExtent(unsigned viewWidth, unsigned viewHeight, int tier, float renderScale, bool atlas)
{
	VisionEyeExtent r;
	int w = 0, h = 0;
	xrStereoExtent(viewWidth, viewHeight, w, h, std::clamp(tier, 0, 2));
	const float scale = std::isfinite(renderScale) ? std::clamp(renderScale, gxgfx::kMinRenderScale, gxgfx::kMaxRenderScale) : 1.0f;
	double dw = double(w) * scale, dh = double(h) * scale;
	const double maxWidth = atlas ? kVisionMaxEyeDim / 2 : kVisionMaxEyeDim;
	double limit = 1.0;
	limit = std::min(limit, maxWidth / dw);
	limit = std::min(limit, double(kVisionMaxEyeDim) / dh);
	limit = std::min(limit, std::sqrt(double(kVisionMaxEyePixels) / (dw * dh)));
	if (limit < 1.0) {
		dw *= limit;
		dh *= limit;
		r.clamped = true;
	}
	r.width = std::max(kVisionMinEyeDim, int(dw));
	r.height = std::max(kVisionMinEyeDim, int(dh));
	return r;
}

struct VisionWorldFrameInput {
	bool stereoRequested = false;           // visionPresentationBegin's result
	const XRFrameInfo *frame = nullptr;
	GXGraphicsSettings graphics = gxGraphicsDefaults();
	bool atlas = false;                     // ring layout
	bool healthBars = true, unitRings = true, boardFrame = true; // XrLayout preferences
};

// Everything of XrWorldFrame that XrHello fills except the board, coverage, observer and the eye poses / fov, which the interaction
// layer's frame driver (VisionFrameDriver::step: visionApplyToWorldFrame + visionFillWorldFrameEyes) owns. Returns the eye extent used.
inline VisionEyeExtent visionBuildWorldFrame(XrWorldFrame &world, const VisionWorldFrameInput &in)
{
	world.enabled = in.stereoRequested;
	VisionEyeExtent extent;
	unsigned vw = 0, vh = 0;
	if (in.frame != nullptr && in.frame->eye_count > 0) {
		vw = unsigned(std::max(0, in.frame->eyes[0].viewport.width));
		vh = unsigned(std::max(0, in.frame->eyes[0].viewport.height));
	}
	extent = visionEyeExtent(vw, vh, in.graphics.eyeTier, in.graphics.renderScale, in.atlas);
	world.width = extent.width;
	world.height = extent.height;
	world.healthBars = in.healthBars;
	world.unitRings = in.unitRings;
	world.boardFrame = in.boardFrame;
	world.volumeShadows = in.graphics.shadowMode == GX_SHADOWS_VOLUMES;
	world.multiviewStereo = false;   // ANGLE has no OVR_multiview
	world.atlasStereo = in.atlas;
	world.elideWorldCopy = true;     // P14: the composed frame's world copy is skipped while the eyes are complete
	return extent;
}

// ============================================================================================ panel geometry

// Defaults (the readability audit is in docs/visionos-presentation.md section 5):
//   * upright screen (menus, movies, loading): Quest relative[0], 1.35 m wide, 1.1 m ahead of the launch heading at eye level;
//   * tabletop mode (visionOS layout, not the Quest preset): the CONTROL BAR lies in front of the board's near edge like a
//     console, tilted 18 degrees up toward the player, 1.10 m wide; it is the part the player reads and pinches most, so it
//     is the closest one and it never stands between the player and the map. The rest of the engine canvas (the HUD band
//     with mission text, timers and messages, or the whole canvas while a dialog owns it) stands behind the far edge and
//     leans back 24 degrees, as the Quest canvas does.
//     The Quest preset (whole canvas behind the far edge) put an opaque bar across the back of the map; on Vision Pro that
//     read as a wall above the battlefield.
constexpr float kVisionScreenWidthM = 1.35f;
constexpr float kVisionScreenAheadM = 1.10f;
constexpr float kVisionScreenBelowHeadM = 0.02f;
constexpr float kVisionUiCanvasWidthM = 1.60f;       // far canvas while a dialog owns it (menus must stay readable)
constexpr float kVisionUiHudWidthM = 1.30f;          // far HUD band (mostly transparent: mission text, timers, messages)
constexpr float kVisionUiLeanBackRad = 0.41887902f;  // 24 degrees, XrLayout tabletop preset
constexpr float kVisionUiGapM = 0.10f;               // between the board's far edge and the bottom of the far canvas
constexpr float kVisionUiLiftM = 0.03f;              // above the table plane
constexpr float kVisionUiBarWidthM = 1.10f;          // near console (control bar)
constexpr float kVisionUiBarTiltRad = 0.31415927f;   // 18 degrees up from the table toward the player (low: hides little map)
constexpr float kVisionUiBarGapM = 0.09f;            // board near edge -> console top edge (clear of the 7 cm pan rim)
constexpr float kVisionUiBarLiftM = 0.01f;           // console bottom edge above the table plane
constexpr float kVisionLayerLiftM = 0.004f;          // marker quads stand this far in front of a panel

// A band of the engine canvas standing behind the far edge of `board`, leaning back toward the player: `band` (canvas UV,
// y up from the bottom) is placed so the bottom edge of `bottomRef` sits `kVisionUiGapM` behind the far edge. Returns the
// pose of the CENTER of `bottomRef` (the Quest convention; visionUiPieceSurface offsets other bands from it).
inline XrSurface visionUiFarSurface(const XrSurface &board, float boardAspect, float canvasAspect, XrGameRect bottomRef,
	float canvasWidth = kVisionUiCanvasWidthM)
{
	XrSurface ui;
	ui.width = canvasWidth;
	const float boardHeight = board.width * boardAspect;
	const float lean = kVisionUiLeanBackRad;
	// Panel axes in board space (x right, y toward the far edge, z up): up vector leans away from the player.
	ui.pose.orientation = xrNormalize(xrMul(board.pose.orientation, xrAxisAngle({1, 0, 0}, 1.57079633f - lean)));
	const XrVector3f up = {0, std::sin(lean), std::cos(lean)};
	const float centerUp = std::clamp(bottomRef.h, 0.05f, 1.0f) * 0.5f * canvasAspect * canvasWidth;
	const XrVector3f localBottom = {0, boardHeight * 0.5f + kVisionUiGapM, kVisionUiLiftM};
	const XrVector3f local = xrAdd(localBottom, xrScale(up, centerUp));
	ui.pose.position = xrAdd(board.pose.position, xrRotate(board.pose.orientation, local));
	return ui;
}

// The control bar console in front of the near edge: its TOP edge (the side of the bar nearest the map) runs along the near
// edge `kVisionUiBarGapM` out, and it tilts up toward the player by kVisionUiBarTiltRad. Returns the pose of the bar's center.
// `bar` is XrGameBoot_CommandRect(). This is also the surface the workspace records (VisionPresentation::finish).
inline XrSurface visionUiBarSurface(const XrSurface &board, float boardAspect, float canvasAspect, XrGameRect bar,
	float barWidth = kVisionUiBarWidthM)
{
	XrSurface ui;
	ui.width = barWidth;
	const float boardHeight = board.width * boardAspect;
	const float tilt = kVisionUiBarTiltRad;
	// Rotating the board frame by `tilt` about X: panel up = (0, cos, sin) (toward the map and up), normal = (0, -sin, cos)
	// (toward the player and up).
	ui.pose.orientation = xrNormalize(xrMul(board.pose.orientation, xrAxisAngle({1, 0, 0}, tilt)));
	const XrVector3f up = {0, std::cos(tilt), std::sin(tilt)};
	const float barH = std::clamp(bar.h, 0.05f, 1.0f) * canvasAspect * barWidth;
	const XrVector3f localBottom = {0, -(boardHeight * 0.5f + kVisionUiBarGapM + barH * std::cos(tilt)), kVisionUiBarLiftM};
	const XrVector3f local = xrAdd(localBottom, xrScale(up, 0.5f * barH));
	ui.pose.position = xrAdd(board.pose.position, xrRotate(board.pose.orientation, local));
	return ui;
}

// How far the console reaches toward the player from the near edge (board-plane depth). The grab bar sits beyond it.
inline float visionUiBarReachM(float canvasAspect, XrGameRect bar, float barWidth = kVisionUiBarWidthM)
{
	const float barH = std::clamp(bar.h, 0.05f, 1.0f) * canvasAspect * barWidth;
	return kVisionUiBarGapM + barH * std::cos(kVisionUiBarTiltRad);
}

// XrHello::displayedSurface for the UI pieces: the piece's band is offset along the surface's local Y (xrUIBandOffset).
inline XrSurface visionUiPieceSurface(const XrSurface &barSurface, XrGameRect piece, XrGameRect bar, float canvasAspect)
{
	XrSurface result = barSurface;
	const float offset = xrUIBandOffset(piece, bar, canvasAspect) * result.width;
	result.pose.position = xrAdd(result.pose.position, xrRotate(result.pose.orientation, {0, offset, 0}));
	return result;
}

// The upright screen, in front of the launch heading (Quest relative[0]).
inline XrSurface visionScreenSurface(const XrPosef &anchor)
{
	XrSurface s;
	s.width = kVisionScreenWidthM;
	s.pose = xrPoseMul(anchor, {{0, 0, 0, 1}, {0, -kVisionScreenBelowHeadM, -kVisionScreenAheadM}});
	return s;
}

// A head heading (yaw only) at head height: the workspace anchor (XrWorkspacePlacement.h xrWorkspaceHeading, from one head pose).
inline XrPosef visionHeadingAnchor(const XRFrameInfo &frame)
{
	float fx = 0, fz = -1;
	const XrQuaternionf q = {frame.head_pose.orientation.x, frame.head_pose.orientation.y, frame.head_pose.orientation.z, frame.head_pose.orientation.w};
	const XrVector3f fwd = xrRotate(q, {0, 0, -1});
	const float n = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
	if (n > 1e-4f) { fx = fwd.x / n; fz = fwd.z / n; }
	return {xrAxisAngle({0, 1, 0}, std::atan2(-fx, -fz)), {frame.head_pose.position.x, frame.head_pose.position.y, frame.head_pose.position.z}};
}

struct VisionPanelPlan {
	VisionPanel panels[kVisionMaxPanels];
	int panelCount = 0;
	GXHostLayer layers[GX_HOST_MAX_LAYERS];
	int layerCount = 0;
	int layerOfPanel[kVisionMaxPanels] = {-1, -1, -1, -1, -1, -1};
	int panelOfLayer[GX_HOST_MAX_LAYERS] = {-1, -1, -1, -1, -1, -1, -1, -1};
	bool hasFocus = false;
	XrVector3f focus = {};
};

struct VisionLayoutInput {
	VisionPresentationMode mode = VisionPresentationMode::Idle;
	bool planarWorld = false;         // Tabletop without stereo: the world texture is drawn planar on the board
	bool boardPlaced = false;
	XrSurface board;
	float boardAspect = 9.0f / 16.0f;
	bool screenKnown = false;         // the upright screen anchor exists (first tracked frame)
	XrSurface screen;
	bool expandedUI = false;
	XrGameRect commandRect, worldRect;
	int gameWidth = 1280, gameHeight = 720;
	bool uiPanels = true;             // XrLayout::commandsVisible-like switch for the engine UI beside the board
};

inline VisionPanel visionMakePanelForPlan(VisionPanelKind kind, const XrSurface &surface, float aspect, XrGameRect rect)
{
	VisionPanel p;
	p.visible = true;
	p.kind = kind;
	p.surface = surface;
	p.aspect = aspect;
	p.rect = rect;
	return p;
}

// Adds one textured quad to the plan. `panel` is the input table entry (nullptr: layer only, e.g. the recovery card). The panel and the
// layer share the same pose, size and crop: the pose given to the interaction layer IS the pose of the composite layer.
inline void visionAddQuad(VisionPanelPlan &plan, VisionPanelKind kind, int target, const char *name, const XrSurface &surface, float aspect,
	XrGameRect rect, bool addPanel)
{
	if (plan.layerCount >= GX_HOST_MAX_LAYERS) return;
	if (!(aspect > 0) || !std::isfinite(aspect) || !(surface.width > 0)) return;
	GXHostLayer &layer = plan.layers[plan.layerCount];
	memset(&layer, 0, sizeof(layer));
	snprintf(layer.name, sizeof(layer.name), "%s", name);
	layer.target = target;
	layer.position[0] = surface.pose.position.x; layer.position[1] = surface.pose.position.y; layer.position[2] = surface.pose.position.z;
	layer.orientation[0] = surface.pose.orientation.x; layer.orientation[1] = surface.pose.orientation.y;
	layer.orientation[2] = surface.pose.orientation.z; layer.orientation[3] = surface.pose.orientation.w;
	layer.size[0] = surface.width;
	layer.size[1] = surface.width * aspect;
	layer.uvRect[0] = rect.x; layer.uvRect[1] = rect.y; layer.uvRect[2] = rect.w; layer.uvRect[3] = rect.h;
	layer.flags = GX_LAYER_FLIP_Y | GX_LAYER_PREMULTIPLIED | GX_LAYER_HAS_UVRECT; // GL targets are bottom-up, coverage-premultiplied
	int layerIndex = plan.layerCount++;
	if (addPanel && plan.panelCount < kVisionMaxPanels) {
		plan.panels[plan.panelCount] = visionMakePanelForPlan(kind, surface, aspect, rect);
		plan.layerOfPanel[plan.panelCount] = layerIndex;
		plan.panelOfLayer[layerIndex] = plan.panelCount;
		++plan.panelCount;
	}
}


// ============================================================================================ panel layout

// The panel table the interaction layer picks against AND the composite layers the compositor draws, from one function so they can
// never differ (docs/visionos-interaction.md section 9: "the pose given to visionMakePanel must be the pose of the layer").
//
//   Menu / Cinematic / Loading / Recovery : one upright screen panel showing the composed GAME target.
//   Tabletop, stereo                      : the engine UI around the board, cropped from the UI target with the Quest rectangles
//                                           (xrUIPieceRect): piece 2 = the control bar on the near console (visionUiBarSurface), or
//                                           the whole canvas behind the far edge while a dialog is open; piece 3 = the transparent HUD
//                                           band behind the far edge (visionUiFarSurface).
//   Tabletop, planar world (no stereo)    : the WORLD target cropped to the tactical view, flat on the board, plus the same UI pieces.
//   GroundView                            : no panels (the observer has the whole view; Quest hides the surfaces the same way).
inline void visionLayoutPanels(const VisionLayoutInput &in, VisionPanelPlan &plan)
{
	plan = VisionPanelPlan();
	const float fullAspect = float(in.gameHeight) / float(std::max(1, in.gameWidth));
	const bool upright = in.mode == VisionPresentationMode::Menu || in.mode == VisionPresentationMode::Cinematic ||
		in.mode == VisionPresentationMode::Loading || in.mode == VisionPresentationMode::Recovery;
	if (upright) {
		if (in.screenKnown) {
			visionAddQuad(plan, kVisionPanelGameScreen, GX_XRT_GAME, "screen", in.screen, fullAspect, XrGameRect(), true);
			plan.hasFocus = true;
			plan.focus = in.screen.pose.position;
		}
		return;
	}
	if (in.mode != VisionPresentationMode::Tabletop || !in.boardPlaced) return;
	plan.hasFocus = true;
	plan.focus = in.board.pose.position;
	if (in.planarWorld) {
		const XrGameRect r = in.worldRect;
		const float aspect = (r.w > 0.0f) ? fullAspect * r.h / r.w : in.boardAspect;
		XrSurface world = in.board;
		visionAddQuad(plan, kVisionPanelWorld2D, GX_XRT_WORLD, "world", world, aspect, r, true);
	}
	if (!in.uiPanels) return;
	const XrGameRect bar = in.commandRect;
	// Compact: the bar on the near console, the HUD band behind the far edge (its bottom edge where the far canvas starts).
	// Expanded (a dialog owns the canvas): the whole canvas behind the far edge, where there is room for it.
	static const char *const names[4] = {"", "", "ui-bar", "ui-hud"};
	for (int piece = 2; piece <= 3; ++piece) {
		const XrGameRect rect = xrUIPieceRect(piece, bar, in.expandedUI);
		if (!(rect.w > 0.0f) || !(rect.h > 0.0f)) continue; // the HUD piece is empty while a dialog owns the canvas
		XrSurface surface;
		if (piece == 2 && !in.expandedUI) surface = visionUiBarSurface(in.board, in.boardAspect, fullAspect, bar);
		else surface = visionUiFarSurface(in.board, in.boardAspect, fullAspect, rect, in.expandedUI ? kVisionUiCanvasWidthM : kVisionUiHudWidthM);
		visionAddQuad(plan, piece == 2 ? kVisionPanelGameUI : kVisionPanelGameHud, GX_XRT_UI, names[piece], surface,
			fullAspect * rect.h / rect.w, rect, true);
	}
}

// ============================================================================================ readability

// Angular size of UI text on a flat panel. `texelsAcross` is the engine backbuffer width (1280), `distanceM` the eye to the text.
struct VisionReadability {
	float arcminPerTexel = 0;        // visual angle of one UI pixel (small-angle exact: 2 atan)
	float glyphArcmin = 0;           // visual angle of a glyph `glyphTexels` tall
	float texelsPerDegree = 0;       // UI texel density on the panel
	float texelsPerDisplayPixel = 0; // texels per display pixel at `displayPixelsPerDegree` (1 = text is sampled 1:1; < 1: magnified, soft)
};
inline VisionReadability visionReadability(float panelWidthM, int texelsAcross, float distanceM, float glyphTexels,
	float displayPixelsPerDegree = 34.0f)
{
	VisionReadability r;
	if (!(panelWidthM > 0) || texelsAcross <= 0 || !(distanceM > 0)) return r;
	const double texelM = double(panelWidthM) / texelsAcross;
	const double radians = 2.0 * std::atan(texelM / (2.0 * distanceM));
	const double arcmin = radians * 180.0 / 3.14159265358979 * 60.0;
	r.arcminPerTexel = float(arcmin);
	r.glyphArcmin = float(arcmin * glyphTexels);
	r.texelsPerDegree = float(60.0 / arcmin);
	r.texelsPerDisplayPixel = r.texelsPerDegree / displayPixelsPerDegree;
	return r;
}

// Distance from the head to the center of a panel band (for the readability audit and tests).
inline float visionDistanceToSurface(const XrVector3f &head, const XrSurface &surface)
{
	return xrLength(xrSub(surface.pose.position, head));
}
