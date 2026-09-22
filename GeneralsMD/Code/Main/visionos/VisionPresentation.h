// visionOS host: what to show and where, per engine frame (package C2). Pure C++ apart from the file I/O of the layout; no engine
// symbols and no Apple types, so the host tests and the scripted fake engine run the very same object as the real engine.
//
// One object per engine (GXEngineHostEngine.cpp owns it), called on the engine thread in this order every frame:
//
//     update(in, driver, out)        before the frame: presentation state machine (Begin), workspace restore, XrWorldFrame, ring request,
//                                    the panel table for the interaction step (built from last frame's resolved state and board)
//     driver.step(..., out.plan.panels, out.plan.panelCount, out.world)          package E: input, gestures, board, eyes into the world frame
//     XrGameBoot_SetWorldFrame / SetSplitEnabled / XrGameBoot_Frame              the engine frame (a loader may call loadingPresent())
//     finish(post, driver, out.world, frameOutput)                               after the frame: resolve what was captured (P17), final
//                                                                                layout (= the composite layers), feedback, workspace save
//
// The panel table of step N+1 equals the composite layers published by frame N (same function, same state, same board), which is the
// contract of docs/visionos-interaction.md section 9 ("the pose given to visionMakePanel must be the pose of the layer").
#pragma once

#include "GXEngineHostServices.h"
#include "GXGraphicsSettings.h"
#include "VisionFeedback.h"
#include "VisionFrameDriver.h"
#include "VisionPresentationLogic.h"
#include "VisionWorkspace.h"

struct VisionPresentationInput {
	const XRFrameInfo *frame = nullptr;
	bool sessionRunning = true;
	VisionPresentationFacts facts;          // read before the engine frame
	bool atlas = false;                     // ring stereo layout (both eyes in STEREO_LEFT)
	GXGraphicsSettings gfx = gxGraphicsDefaults(); // the settings applied by the engine thread this frame
	const char *layoutPath = nullptr;       // XrGameBoot_LayoutPath(): read once (preferences), written when the user edits the workspace
	const char *legacyLayoutPath = nullptr; // XrGameBoot_LegacyLayoutPath()
};

struct VisionPresentationOutput {
	XrWorldFrame world;                     // enabled / sizes / toggles set here; board, coverage, observer, eyes, fov by the driver
	VisionPanelPlan plan;                   // the panel table for the interaction step (its layers are informational: finish() publishes them)
	bool stereoWorld = false;               // the world is captured per eye this frame
	bool splitUI = true;                    // XrGameBoot_SetSplitEnabled
	GXHostFrameRequest request = {};        // ring targets this frame needs
	VisionEyeExtent eye;
};

class VisionPresentation {
public:
	void update(const VisionPresentationInput &in, VisionFrameDriver &driver, VisionPresentationOutput &out);

	// After the engine frame. Fills the presentation half of `out` (layers, feedback, mode, notice, stereoValid, focus). `world` is the
	// frame's XrWorldFrame (observer flag). Returns the resolved mode; `requireFullWorld()` then says whether the caller must ask the
	// engine for a complete world next frame (d3d8gles_RequireXRFullWorld).
	VisionPresentationMode finish(const VisionPresentationFacts &post, const VisionFrameDriver &driver, const XrWorldFrame &world,
		const VisionPresentationInput &in, GXHostFrameOutput &out);

	// The synchronous loading presenter (XrGameBoot_SetLoadingPresenter): describes what the nested frame shows (the engine's loading
	// screen on the upright panel) without touching the simulation. `post` are the facts right now.
	void describeLoading(const VisionPresentationFacts &post, const VisionFrameDriver &driver, const VisionPresentationInput &in,
		GXHostFrameOutput &out);
	void loadingEnded() { visionPresentationLoadingEnd(view_); }

	VisionViewState &view() { return view_; }
	const VisionViewState &view() const { return view_; }
	VisionWorkspace &workspace() { return workspace_; }
	const VisionPanelPlan &plan() const { return plan_; }
	bool consumeRequireFullWorld() { const bool r = view_.requireFullWorld; view_.requireFullWorld = false; return r; }
	unsigned saves() const { return workspace_.saves; }

private:
	void layout(const VisionPresentationFacts &facts, const VisionFrameDriver &driver, VisionPanelPlan &plan) const;
	void fillLayers(const VisionPanelPlan &plan, const VisionPresentationFacts &facts, GXHostFrameOutput &out) const;
	void notice(VisionPresentationMode mode, GXHostFrameOutput &out) const;

	VisionViewState view_;
	VisionWorkspace workspace_;
	VisionFeedbackBuilder feedback_;
	VisionPanelPlan plan_;
	bool layoutRead_ = false;
	bool zoomApplied_ = false;
	VisionMode previousInteractionMode_ = VisionMode::Idle;
	int frameCounter_ = 0;
};
