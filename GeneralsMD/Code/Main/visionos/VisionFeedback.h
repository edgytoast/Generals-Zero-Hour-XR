// visionOS feedback: turns the interaction layer's per-frame output (VisionInteractionOutput) into the flat, compositor-facing
// GXHostFeedback (GXEngineHostServices.h). PURE (no engine, no Apple, no GL); host tested by scripts/qa/vision-presentation-test.sh.
//
// What is drawn where (docs/visionos-presentation.md section 7):
//   * in the ENGINE's stereo pass (depth-correct, terrain following): unit rings, health bars, board frame and the box-select outline
//     of the engine's own tactics (XrGameBoot.cpp drawXrWorldDecorations -> d3d8gles_DrawXRDecorations), exactly as on the Quest;
//   * in METAL over the composited eyes (this file): the translucent box fill + outline from VisionInteractionOutput::box (visible
//     also when the engine decorations are off), the board grab bar, the pinch cursor, the panel pointer dot and hover outline,
//     the placement legality cue, the Ground View reticle and hold ring, the eye ray, the destination marker, and the comfort veil.
#pragma once

#include <cstring>

#include "GXEngineHostServices.h"
#include "GXGraphicsSettings.h"
#include "VisionInteraction.h"
#include "VisionPresentationLogic.h"

class VisionFeedbackBuilder {
public:
	static constexpr float kWaypointSeconds = 1.2f;

	// Builds `fb` from one interaction step. `time` is the host clock (VisionHostState::time_s).
	void build(const VisionInteractionOutput &o, const VisionHostState &host, const VisionPanelPlan &plan, const GXGraphicsSettings &gfx,
		GXHostFeedback &fb)
	{
		memset(&fb, 0, sizeof(fb));
		fb.pointerLayer = -1;
		fb.placementLegal = -1;
		const bool marker = (gfx.flags & GX_GFX_FOCUS_MARKER) != 0;
		const bool fade = (gfx.flags & GX_GFX_COMFORT_FADE) != 0;
		const XrQuaternionf bq = host.boardPlaced ? host.board.pose.orientation : XrQuaternionf{0, 0, 0, 1};
		fb.boardOrientation[0] = bq.x; fb.boardOrientation[1] = bq.y; fb.boardOrientation[2] = bq.z; fb.boardOrientation[3] = bq.w;
		fb.boardWidth = host.boardPlaced ? host.board.width : 1.0f;

		if (o.box.active) {
			fb.flags |= GX_FB_BOX;
			fb.boxAdditive = o.box.additive ? 1 : 0;
			for (int i = 0; i < 4; ++i) put(fb.boxCorners[i], o.box.corners[i]);
		}
		if (o.grabBar.visible) {
			fb.flags |= GX_FB_GRAB_BAR;
			put(fb.grabPosition, o.grabBar.pose.position);
			fb.grabOrientation[0] = o.grabBar.pose.orientation.x; fb.grabOrientation[1] = o.grabBar.pose.orientation.y;
			fb.grabOrientation[2] = o.grabBar.pose.orientation.z; fb.grabOrientation[3] = o.grabBar.pose.orientation.w;
			fb.grabLength = o.grabBar.length;
			fb.grabThickness = o.grabBar.thickness;
			fb.grabActive = o.grabBar.active ? 1 : 0;
		}
		if (marker && o.cursorVisible && o.cursorOnBoard) {
			fb.flags |= GX_FB_CURSOR;
			put(fb.cursor, o.cursorWorld);
			fb.cursorOnBoard = 1;
		}
		if (marker && o.rayVisible) {
			fb.flags |= GX_FB_RAY;
			put(fb.rayStart, o.rayStart);
			put(fb.rayEnd, o.rayEnd);
		}
		// Pointer dot: the pinch is driving a panel. UV is panel-local (u right, v up), 0..1 across the quad.
		if (marker && o.panelPointerPanel >= 0 && o.panelPointerPanel < plan.panelCount) {
			const int layer = plan.layerOfPanel[o.panelPointerPanel];
			if (layer >= 0) {
				const VisionPanel &p = plan.panels[o.panelPointerPanel];
				const XrVector3f local = {(o.panelU - 0.5f) * p.surface.width, (o.panelV - 0.5f) * p.surface.width * p.aspect, kVisionLayerLiftM};
				put(fb.pointerPos, xrAdd(p.surface.pose.position, xrRotate(p.surface.pose.orientation, local)));
				fb.pointerLayer = layer;
				fb.flags |= GX_FB_PANEL_POINTER;
			}
		}
		// Hover / focus outline: a region the pinch is using (a panel or the grab bar). The grab bar already lights up through grabActive.
		if (marker) {
			for (int i = 0; i < o.regionCount; ++i) {
				const VisionRegion &r = o.regions[i];
				if (!r.active || r.corners != 4 || r.id < kVisionRegionPanelBase) continue;
				for (int c = 0; c < 4; ++c) put(fb.hoverQuad[c], r.quad[c]);
				fb.hoverActive = 1;
				fb.flags |= GX_FB_HOVER;
				break;
			}
		}
		if (o.placement.active) {
			fb.flags |= GX_FB_PLACEMENT;
			fb.placementLegal = o.placement.legal;
			fb.placementCancelArmed = o.placement.cancelArmed ? 1 : 0;
			if (o.placement.hasPoint) put(fb.placementPoint, o.placement.roomPoint);
			else if (o.cursorVisible) put(fb.placementPoint, o.cursorWorld);
			else fb.flags &= ~GX_FB_PLACEMENT;
		}
		if (marker && o.ground.hasTarget) {
			fb.flags |= GX_FB_GROUND_TARGET;
			put(fb.groundTarget, o.ground.targetRoom);
			fb.groundTargetValid = o.ground.targetValid ? 1 : 0;
		}
		fb.groundHold = o.ground.holdProgress;
		// Destination marker: a world tap was just sent to the engine (select or contextual command). The cursor is already gone in the
		// frame that releases the pinch, so the last cursor seen on the board (at most 0.25 s old) is the tap point.
		if (o.cursorVisible && o.cursorOnBoard) {
			lastCursor_ = o.cursorWorld;
			lastCursorTime_ = host.time_s;
			haveLastCursor_ = true;
		}
		if (marker && (o.events & kVisionEventTap) != 0) {
			if (o.cursorVisible && o.cursorOnBoard) {
				waypoint_ = o.cursorWorld;
				waypointTime_ = host.time_s;
				haveWaypoint_ = true;
			} else if (haveLastCursor_ && host.time_s - lastCursorTime_ <= 0.25) {
				waypoint_ = lastCursor_;
				waypointTime_ = host.time_s;
				haveWaypoint_ = true;
			}
		}
		if (haveWaypoint_) {
			const float age = float(host.time_s - waypointTime_);
			if (age >= 0.0f && age < kWaypointSeconds) {
				fb.flags |= GX_FB_WAYPOINT;
				put(fb.waypoint, waypoint_);
				fb.waypointAge = age;
			} else if (age >= kWaypointSeconds || age < 0.0f) {
				haveWaypoint_ = false;
			}
		}
		fb.fadeAlpha = fade ? std::clamp(o.ground.fadeAlpha, 0.0f, 1.0f) : 0.0f;
	}

	void reset() { haveWaypoint_ = haveLastCursor_ = false; }

private:
	static void put(float *dst, const XrVector3f &v) { dst[0] = v.x; dst[1] = v.y; dst[2] = v.z; }
	bool haveWaypoint_ = false, haveLastCursor_ = false;
	XrVector3f lastCursor_ = {};
	double lastCursorTime_ = 0;
	XrVector3f waypoint_ = {};
	double waypointTime_ = 0;
};
