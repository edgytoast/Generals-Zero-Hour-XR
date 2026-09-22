// A scripted, engine-free VisionEngineBridge: the engine answers the interaction layer needs (flags, a realistic PickWorld against the
// board plane, Ground View terrain checks) come from plain fields, and every call is counted. Used by
//   * scripts/qa/vision-presentation-test.cpp (host tests of the presentation pipeline), and
//   * GXPresentationScript.cpp (the fake engine's scripted mode sequence in the simulator, package C2).
// It contains no game rules, exactly like the recording fake of vision-interaction-test.cpp it is modelled on.
#pragma once

#include <cmath>

#include "VisionInteraction.h"

class VisionScriptedBridge : public VisionEngineBridge {
public:
	// configuration (the scenario sets these)
	bool interactive = false, canStereo = false, expanded = false, canAdjust = false, canObserve = false;
	bool armed = false, pendingPlacement = false;
	int legal = -1;
	int gameW = 1280, gameH = 720;
	float mapAspect = 9.0f / 16.0f;
	float walkableRadius = 1e9f; // observer picks / steps beyond this board-space radius are refused
	// counters
	int pointerCalls = 0, triggerCalls = 0, clickCalls = 0, pickCalls = 0, keyCalls = 0, cancelCalls = 0, adjustCalls = 0, navigateCalls = 0;
	int observerPicks = 0, observerSteps = 0;
	int lastRoute = -1;
	bool lastKeyDown = false;
	VisionKey lastKey = VisionKey::Back;
	float lastPointerX = 0, lastPointerY = 0;
	bool triggerHeld = false;

	void Pointer(bool active, float x, float y, bool select, bool, float) override {
		++pointerCalls; lastPointerX = x; lastPointerY = y; (void)active; (void)select;
	}
	void Key(VisionKey key, bool down) override { ++keyCalls; lastKey = key; lastKeyDown = down; }
	void RoutePointer(int target) override { lastRoute = target; }
	bool PickWorld(const XrSurface &board, const XrPosef &aim, XrWorldHit &hit) override {
		++pickCalls;
		const XrVector3f dir = xrRotate(aim.orientation, {0, 0, -1});
		XrVector3f local;
		float t;
		if (!interactive || !visionRayBoardPlane(board, aim.position, dir, local, t)) return false;
		if (std::fabs(local.x) > board.width * 0.5f || std::fabs(local.y) > board.width * mapAspect * 0.5f) return false;
		hit.room = visionBoardToWorld(board, local);
		hit.x = (local.x / board.width + 0.5f) * float(gameW - 1);
		hit.y = (0.5f - local.y / (board.width * mapAspect)) * float(gameH - 1);
		hit.distance = t;
		return true;
	}
	void SpatialPointer(bool) override {}
	void SpatialTrigger(bool down, bool available, bool) override { ++triggerCalls; triggerHeld = down && available; }
	void SpatialClick(bool) override { ++clickCalls; }
	void CancelTarget() override { ++cancelCalls; }
	void TacticalAction(int) override {}
	bool AdjustCamera(float, float) override { ++adjustCalls; return canAdjust; }
	bool NavigateWorld(float, float, float) override { ++navigateCalls; return canAdjust; }
	bool CanAdjustWorld() override { return canAdjust; }
	bool PlacementPending() override { return pendingPlacement; }
	bool CanRotatePlacement() override { return pendingPlacement; }
	bool RotatePlacement(float) override { return pendingPlacement; }
	float PlacementDegrees() override { return 0; }
	int PlacementLegal() override { return legal; }
	bool HasArmedCommand() override { return armed; }
	bool CanObserveGround() override { return canObserve; }
	bool PickObserverGround(const XrSurface &board, const XrPosef &aim, XrVector3f &ground, XrVector3f *roomPoint) override {
		++observerPicks;
		XrWorldHit hit;
		if (!PickWorld(board, aim, hit)) return false;
		XrVector3f local = visionBoardToLocal(board, hit.room);
		if (std::sqrt(local.x * local.x + local.y * local.y) > walkableRadius) return false;
		ground = {local.x * 2000.0f, local.y * 2000.0f, 0}; // game units: any consistent scale
		if (roomPoint) *roomPoint = hit.room;
		return true;
	}
	bool ObserverStep(XrVector3f current, XrVector3f delta, XrVector3f &next) override {
		++observerSteps;
		next = xrAdd(current, delta);
		return true;
	}
	bool IsInteractiveGame() override { return interactive; }
	bool CanStereoWorld() override { return canStereo; }
	bool ExpandedUI() override { return expanded; }
	bool HasUIAt(float, float) override { return true; }
	int GameWidth() override { return gameW; }
	int GameHeight() override { return gameH; }
	void TacticalState(int &mode, int &group, bool &queue) override { mode = 0; group = 0; queue = false; }
	std::string HoverInfo(float, float) override { return std::string(); }
	std::string WorldHoverInfo() override { return std::string(); }
};
