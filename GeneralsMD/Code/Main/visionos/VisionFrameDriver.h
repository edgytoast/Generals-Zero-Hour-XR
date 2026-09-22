// visionOS interaction layer: the per-frame glue between the Compositor Services frame and the engine host.
//
// Header-only and pure (no Apple types, no engine symbols beyond the VisionEngineBridge seam), so it is host-tested
// (scripts/qa/vision-interaction-test.sh) and the Objective-C++ engine host (package C) makes ONE call per rendered
// frame instead of re-deriving the sequence in docs/visionos-interaction.md section 9:
//
//     // render thread, inside the XRPresentation frame callback, before the engine frame
//     const VisionInteractionOutput &out = driver.step(*frame, sessionRunning, panels, panelCount, worldFrame);
//     // ... fill the rest of worldFrame (sizes, toggles), XrGameBoot_SetWorldFrame(worldFrame)
//     // ... XrGameBoot_Frame(), render, submit; draw the overlays described by `out`
//
// What step() does, in the Quest order (XrHello.cpp runLoop: input step, SetWorldFrame, Frame):
//   1. fills VisionHostState from the XRFrameInfo (time, head pose, tracking) and the renderer's panel table;
//   2. captures the engine flags through the bridge (visionCaptureEngineFlags);
//   3. drains XRInteraction_PollEvent (the queue GXXRInput.mm fills from the main thread);
//   4. VisionInteraction::update;
//   5. adopts the proposed board / zoom for the next frame and tells the shell input layer where the board is
//      (XRInteraction_SetBoardTransform);
//   6. copies board, coverage and Ground View into the XrWorldFrame and the eye poses / FOV from the compositor frame.
#pragma once

#include "VisionInteraction.h"

// XRPose (visionos/Platform/XRPresentation.h) -> XrPosef (the pure Xr*.h headers).
inline XrPosef visionPoseFromXR(const XRPose &p) {
	return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, {p.position.x, p.position.y, p.position.z}};
}

// Time, head pose and tracking state of a compositor frame. `sessionRunning` is false while the layer is paused,
// idle or invalidated (XRPresentation_GetSessionState() != XR_SESSION_RUNNING): input is suspended and everything in
// flight is cancelled by VisionInteraction. Leaves the board, panels and engine flags alone.
inline void visionFillHostFromFrame(const XRFrameInfo &f, bool sessionRunning, VisionHostState &host) {
	host.time_s = f.predicted_display_time_s;
	host.frame = f.frame_index;
	host.sessionFocused = sessionRunning;
	host.headTracked = f.head_tracked;
	host.head = visionPoseFromXR(f.head_pose);
}

// Eye poses and FOV of a compositor frame into the engine frame description (a mono frame mirrors eye 0).
// XRFov is OpenXR style (left/down negative), the same as XrFovf.
inline void visionFillWorldFrameEyes(const XRFrameInfo &f, XrWorldFrame &world) {
	for (int i = 0; i < 2; ++i) {
		const XREyeView &e = f.eyes[(i < int(f.eye_count)) ? i : 0];
		world.eyes[i] = visionPoseFromXR(e.pose);
		world.fov[i] = {e.fov.angle_left, e.fov.angle_right, e.fov.angle_up, e.fov.angle_down};
	}
}

// One entry of the panel table (VisionHostState::panels): a world-locked textured quad that the renderer draws.
// `widthM` is its width in metres, `aspect` height / width, `rect` the crop of the composed engine frame (GL bottom-up
// UVs; the default is the whole frame). Host-owned panels (Commands button / console) ignore `rect`.
inline VisionPanel visionMakePanel(VisionPanelKind kind, const XrPosef &pose, float widthM, float aspect,
	XrGameRect rect = XrGameRect()) {
	VisionPanel p;
	p.visible = true;
	p.kind = kind;
	p.surface.pose = pose;
	p.surface.width = widthM;
	p.aspect = aspect;
	p.rect = rect;
	return p;
}

class VisionFrameDriver {
public:
	static constexpr size_t kMaxEventsPerFrame = 256;

	explicit VisionFrameDriver(VisionEngineBridge *bridge, const VisionConfig &config = VisionConfig())
		: bridge_(bridge), interaction_(bridge, config) {
		host_.boardAspect = 9.0f / 16.0f;
	}

	VisionInteraction &interaction() { return interaction_; }
	const VisionHostState &host() const { return host_; }
	const VisionInteractionOutput &output() const { return output_; }

	// Optional inputs the renderer/shell may know. Sticky until changed.
	void setBoardAspect(float heightOverWidth) { host_.boardAspect = heightOverWidth; }      // tactical view h / w
	void setTableHeight(bool known, float worldY = 0) { host_.tableHeightKnown = known; host_.tableHeight = worldY; }
	void setBoardVisible(bool visible) { host_.boardVisible = visible; }
	// A board the shell already placed (for example restored from a previous session). Without it the layer proposes the
	// initial placement (0.9 m ahead of the head) on the first frame with a tracked head.
	void adoptBoard(const XrSurface &board) { host_.board = board; host_.boardPlaced = true; }
	bool boardPlaced() const { return host_.boardPlaced; }
	// A board change that comes from the host (package C2: the restored board width). Like adoptBoard, and it also tells the shell
	// input layer where the board now is, so pinch picking follows it on the very next event.
	void setBoard(const XrSurface &board) {
		host_.board = board;
		host_.boardPlaced = true;
		float m[16], hx = 0, hz = 0;
		visionBoardTransformForShell(board, host_.boardAspect, m, hx, hz);
		XRInteraction_SetBoardTransform(m, hx, hz);
	}
	// The stored map zoom (Quest: XrLayout::worldZoom restored at start; the interaction layer clamps it to 0.5 ... 3).
	void setWorldZoom(float zoom) { host_.worldZoom = zoom; }

	// One call per rendered frame, render thread, before XrGameBoot_SetWorldFrame / XrGameBoot_Frame. `panels` is the
	// renderer's table for THIS frame (visible textured quads: engine UI, HUD, Commands button/console).
	const VisionInteractionOutput &step(const XRFrameInfo &frame, bool sessionRunning, const VisionPanel *panels,
		int panelCount, XrWorldFrame &world) {
		visionFillHostFromFrame(frame, sessionRunning, host_);
		host_.panelCount = 0;
		for (int i = 0; panels && i < panelCount && i < kVisionMaxPanels; ++i) host_.panels[host_.panelCount++] = panels[i];
		if (bridge_) visionCaptureEngineFlags(*bridge_, host_.engine);

		XRInteractionEvent events[kMaxEventsPerFrame];
		const size_t n = visionDrainEvents(events, kMaxEventsPerFrame);
		interaction_.update(host_, events, n, output_);

		if (output_.boardChanged) {
			host_.board = output_.board;
			host_.boardPlaced = true;
			float m[16], hx = 0, hz = 0;
			visionBoardTransformForShell(output_.board, host_.boardAspect, m, hx, hz);
			XRInteraction_SetBoardTransform(m, hx, hz);
		}
		if (output_.worldZoomChanged) host_.worldZoom = output_.worldZoom;

		visionApplyToWorldFrame(output_, world);
		visionFillWorldFrameEyes(frame, world);
		return output_;
	}

	// Layer paused / invalidated / app inactive: release everything in flight (balanced). Call from the session state
	// callback (XRPresentation_SetSessionStateCallback) for every state except XR_SESSION_RUNNING.
	void reset(VisionResetReason reason) { interaction_.reset(reason); }

private:
	VisionEngineBridge *bridge_;
	VisionInteraction interaction_;
	VisionHostState host_;
	VisionInteractionOutput output_;
};
