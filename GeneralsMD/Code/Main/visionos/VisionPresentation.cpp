// See VisionPresentation.h.
#include "VisionPresentation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
bool isWorkspaceMode(VisionMode m) { return m == VisionMode::WorkspaceMove || m == VisionMode::WorkspaceTwoHand; }
} // namespace

void VisionPresentation::layout(const VisionPresentationFacts &facts, const VisionFrameDriver &driver, VisionPanelPlan &plan) const
{
	const VisionHostState &host = driver.host();
	VisionLayoutInput li;
	li.mode = view_.mode;
	li.planarWorld = view_.mode == VisionPresentationMode::Tabletop && !view_.stereoVisible;
	li.boardPlaced = host.boardPlaced;
	li.board = host.board;
	li.boardAspect = host.boardAspect;
	li.screenKnown = workspace_.anchorKnown;
	li.screen = workspace_.screenSurface();
	li.expandedUI = facts.expandedUI;
	li.commandRect = facts.commandRect;
	li.worldRect = facts.worldRect;
	li.gameWidth = facts.gameWidth;
	li.gameHeight = facts.gameHeight;
	li.uiPanels = true; // the engine UI beside the board is never hidden on visionOS (the Commands console is not ported)
	visionLayoutPanels(li, plan);
}

void VisionPresentation::update(const VisionPresentationInput &in, VisionFrameDriver &driver, VisionPresentationOutput &out)
{
	out = VisionPresentationOutput();
	const XRFrameInfo &frame = *in.frame;
	const VisionPresentationFacts &facts = in.facts;
	++frameCounter_;

	// ---- workspace: anchor on the first tracked head, preferences once ----
	workspace_.ensureAnchor(frame);
	if (!layoutRead_) {
		layoutRead_ = true;
		workspace_.loadFrom(in.layoutPath, in.legacyLayoutPath);
	}
	if (!zoomApplied_ && layoutRead_) {
		zoomApplied_ = true;
		driver.setWorldZoom(std::clamp(workspace_.layout.worldZoom, 0.5f, 3.0f));
	}
	if (workspace_.widthPending && driver.boardPlaced()) {
		// The user's chosen table size survives the launch (Quest policy P20 keeps poses fresh; the width is not room relative).
		XrSurface board = driver.host().board;
		board.width = std::clamp(workspace_.savedBoardWidth, 0.45f, 2.0f);
		driver.setBoard(board);
		workspace_.widthPending = false;
	}

	// ---- presentation state machine (before the frame) ----
	const bool stereoRequested = facts.booted && visionPresentationBegin(view_, facts) && frame.eye_count > 0;
	out.stereoWorld = stereoRequested;
	out.splitUI = !view_.uprightGame;

	// ---- engine world frame ----
	VisionWorldFrameInput wi;
	wi.stereoRequested = stereoRequested;
	wi.frame = &frame;
	wi.graphics = in.gfx;
	wi.atlas = in.atlas;
	wi.healthBars = workspace_.layout.healthBars;
	wi.unitRings = workspace_.layout.unitRings;
	wi.boardFrame = workspace_.layout.boardFrame;
	out.eye = visionBuildWorldFrame(out.world, wi);

	// ---- ring targets ----
	GXHostFrameRequest &req = out.request;
	req.gameWidth = facts.gameWidth;
	req.gameHeight = facts.gameHeight;
	req.eyeCount = int(frame.eye_count < 2 ? frame.eye_count : 2);
	req.targetMask = GX_TARGET_GAME | GX_TARGET_UI;
	if (facts.interactiveGame) req.targetMask |= GX_TARGET_WORLD; // planar world / full-world recovery
	if (stereoRequested) {
		req.targetMask |= GX_TARGET_STEREO | GX_TARGET_WORLD;
		req.eyeWidth = out.eye.width;
		req.eyeHeight = out.eye.height;
	}

	// ---- panel table for the interaction step (last frame's state, last frame's board) ----
	layout(facts, driver, out.plan);
}

void VisionPresentation::fillLayers(const VisionPanelPlan &plan, const VisionPresentationFacts &facts, GXHostFrameOutput &out) const
{
	// Only targets the engine really produced (a layer whose texture does not exist would be an empty quad).
	out.layerCount = 0;
	for (int i = 0; i < plan.layerCount && out.layerCount < GX_HOST_MAX_LAYERS; ++i) {
		const GXHostLayer &l = plan.layers[i];
		const bool present = l.target == GX_XRT_UI ? facts.uiTexture : l.target == GX_XRT_WORLD ? facts.worldTexture : facts.gameTexture;
		if (present) out.layers[out.layerCount++] = l;
	}
}

void VisionPresentation::notice(VisionPresentationMode mode, GXHostFrameOutput &out) const
{
	out.noticeTitle[0] = out.noticeDetail[0] = '\0';
	if (mode == VisionPresentationMode::Recovery) {
		snprintf(out.noticeTitle, sizeof(out.noticeTitle), "Restoring the view");
		snprintf(out.noticeDetail, sizeof(out.noticeDetail), "The battlefield is being redrawn. Release any pinch.");
	} else if (mode == VisionPresentationMode::GroundView) {
		snprintf(out.noticeTitle, sizeof(out.noticeTitle), "Ground view");
		snprintf(out.noticeDetail, sizeof(out.noticeDetail), "Pinch a spot to walk there. Hold a pinch still to return to the table.");
	}
}

VisionPresentationMode VisionPresentation::finish(const VisionPresentationFacts &post, const VisionFrameDriver &driver, const XrWorldFrame &world,
	const VisionPresentationInput &in, GXHostFrameOutput &out)
{
	memset(&out, 0, sizeof(out));
	const VisionPresentationMode mode = visionPresentationEnd(view_, post, world.observer);
	out.presentationMode = int(mode);
	out.groundView = mode == VisionPresentationMode::GroundView;
	out.stereoValid = view_.stereoVisible && mode != VisionPresentationMode::Recovery;

	// Final layout: the layers published now are the panel table of the next interaction step.
	layout(post, driver, plan_);
	fillLayers(plan_, post, out);
	out.hasFocus = plan_.hasFocus;
	out.focus[0] = plan_.focus.x; out.focus[1] = plan_.focus.y; out.focus[2] = plan_.focus.z;
	notice(mode, out);

	// Feedback (Metal, over the eyes and layers). The pointer dot names a layer: the panel the interaction layer used this frame (its
	// index refers to the pre-frame table) is looked up by name among the final layers.
	const VisionInteractionOutput &o = driver.output();
	VisionPanelPlan feedbackPlan = plan_;
	feedback_.build(o, driver.host(), feedbackPlan, in.gfx, out.feedback);
	if (out.feedback.pointerLayer >= 0) {
		const int idx = out.feedback.pointerLayer;
		int found = -1;
		if (idx < plan_.layerCount) {
			for (uint32_t i = 0; i < out.layerCount; ++i)
				if (strncmp(out.layers[i].name, plan_.layers[idx].name, sizeof(out.layers[i].name)) == 0) { found = i; break; }
		}
		out.feedback.pointerLayer = found;
		if (found < 0) out.feedback.flags &= ~GX_FB_PANEL_POINTER;
	}
	// Ground View is the observer's whole view: no board cues.
	if (mode == VisionPresentationMode::GroundView) out.feedback.flags &= ~(GX_FB_BOX | GX_FB_GRAB_BAR | GX_FB_CURSOR | GX_FB_RAY | GX_FB_PANEL_POINTER | GX_FB_HOVER | GX_FB_WAYPOINT);

	// ---- workspace persistence: when a board gesture ends or the board was recentered / reset ----
	const VisionHostState &host = driver.host();
	const bool gestureEnded = isWorkspaceMode(previousInteractionMode_) && !isWorkspaceMode(o.mode);
	const bool recentered = (o.events & (kVisionEventRecentered | kVisionEventBoardMoved)) != 0 && !isWorkspaceMode(o.mode);
	previousInteractionMode_ = o.mode;
	if ((gestureEnded || recentered || o.worldZoomChanged) && host.boardPlaced && workspace_.anchorKnown && !isWorkspaceMode(o.mode)) {
		const float fullAspect = float(post.gameHeight) / float(std::max(1, post.gameWidth));
		const XrSurface bar = visionUiBarSurface(host.board, host.boardAspect, fullAspect, post.commandRect);
		workspace_.record(host.board, bar, o.worldZoom);
		if (in.layoutPath != nullptr) workspace_.save(in.layoutPath);
	}
	return mode;
}

void VisionPresentation::describeLoading(const VisionPresentationFacts &post, const VisionFrameDriver &driver, const VisionPresentationInput &in,
	GXHostFrameOutput &out)
{
	(void)in;
	memset(&out, 0, sizeof(out));
	visionPresentationLoadingBegin(view_);
	out.presentationMode = int(VisionPresentationMode::Loading);
	out.stereoValid = false;
	layout(post, driver, plan_);
	fillLayers(plan_, post, out);
	out.hasFocus = plan_.hasFocus;
	out.focus[0] = plan_.focus.x; out.focus[1] = plan_.focus.y; out.focus[2] = plan_.focus.z;
	out.feedback.pointerLayer = -1;
	out.feedback.placementLegal = -1;
}
