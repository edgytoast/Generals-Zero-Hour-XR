// visionOS interaction layer: the engine-facing seam.
//
// VisionEngineBridge mirrors the part of the Quest host API (XrGameBoot.h) that
// spatial interaction needs, with the SAME names, argument lists and semantics, so
// the visionOS interaction layer makes exactly the engine calls the Quest host
// makes (XrGameBoot_Pointer / _SpatialPointer / _SpatialTrigger / _SpatialClick /
// _PickWorld / _RotatePlacement / _NavigateWorld ...). Game rules are never
// re-implemented here: selection, contextual commands (move, attack, capture,
// enter, guard, gather, deploy ...), building placement and the observer terrain
// checks all stay inside the engine behind these calls.
//
// XrGameBoot.h itself cannot be included off Android (the whole file is inside
// `#ifdef __ANDROID__` and pulls in <jni.h>), so this header re-declares the
// surface as an abstract C++ interface instead of editing the Quest header.
//
// Implementations:
//   * package C (engine bridge) provides VisionXrGameBootBridge, a forwarding
//     implementation where each virtual calls the XrGameBoot_* free function of
//     the same name (the table in docs/visionos-interaction.md lists them 1:1);
//   * scripts/qa/vision-interaction-test.cpp provides a recording fake;
//   * VisionNullEngineBridge (below) is an inert implementation for hosts that run
//     the shell without the engine (the interaction layer still moves the board).
//
// Threading: like XrGameBoot, every method must be called from the single render
// thread that owns the engine.
#pragma once

#include <string>
#include "XrLayers.h" // XrGameRect
#include "XrWorld.h"  // XrSurface, XrPosef, XrWorldHit, XrVector3f

// Same values and order as XrGameKey in XrGameBoot.h.
enum class VisionKey { Back, Left, Right, Up, Down };

// Pointer route targets for RoutePointer (Mouse::PointerTarget in the engine).
constexpr int kVisionRouteComposed = 0; // composed 1280x720 frame (shell, menus, movies)
constexpr int kVisionRouteWorld = 1;    // tabletop world (spatial ray owns picking)
constexpr int kVisionRouteWindows = 2;  // 2D UI windows (control bar, dialogs)

class VisionEngineBridge {
public:
	virtual ~VisionEngineBridge() {}

	// ---- Pointer and keyboard (XrGameBoot_Pointer / _Key / _RoutePointer) ----
	// x,y are engine pixels of the composed frame (0..GameWidth-1, 0..GameHeight-1).
	virtual void Pointer(bool active, float x, float y, bool select, bool secondary, float wheel) = 0;
	virtual void Key(VisionKey key, bool down) = 0;
	virtual void RoutePointer(int target) = 0;

	// ---- Spatial (tabletop) path ----
	// Pick the board volume with a world-space ray. Fills the engine "token" pixel and the hit point
	// in room space. Returns false when the ray misses the board volume or the engine is not ready.
	virtual bool PickWorld(const XrSurface &board, const XrPosef &aim, XrWorldHit &hit) = 0;
	// PickWorld with gaze aim assist: snap onto a selectable object within `assistRadians` (XrGameBoot_PickWorldAssisted).
	// The pick for a pinch the system attributed to an object's tracking area (XrGameBoot_PickObject): the object, no ray.
	virtual bool PickObject(unsigned objectID, const XrPosef &aim, XrWorldHit &hit) { (void)objectID; (void)aim; (void)hit; return false; }
	virtual bool PickWorldAssisted(const XrSurface &board, const XrPosef &aim, XrWorldHit &hit, float assistRadians) {
		(void)assistRadians;
		return PickWorld(board, aim, hit);
	}
	// Snapshot the last PickWorld result as the active spatial pointer (or clear it).
	virtual void SpatialPointer(bool active) = 0;
	// Deferred trigger: press does nothing, release before ~2 cm ray drag = click (select or
	// contextual command), beyond = box. `available` false cancels a pending gesture. `additive`
	// is latched at the press.
	virtual void SpatialTrigger(bool down, bool available, bool additive) = 0;
	// Commit at the active pointer (armed command / placement / explicit mode); cancel=true is the
	// engine's own "cancel or deselect".
	virtual void SpatialClick(bool cancel) = 0;
	// Focus/tracking cancellation: clears armed targets, tactics and gesture; never deselects or orders.
	virtual void CancelTarget() = 0;
	// Tactical palette (XrGameBoot_TacticalAction ids: 0..8 order modes, 10 stop, 13 cancel/deselect ...).
	virtual void TacticalAction(int action) = 0;

	// ---- Camera ----
	// Radians added to the tactical view angle / pitch (script locks respected by the engine).
	virtual bool AdjustCamera(float yawRadians, float pitchRadians) = 0;
	// Camera pan in board basis; |right|,|forward| are clamped to 0.05 per call by the engine
	// (xrWorldPan), 0.05 = 2.5 % of the board width. zoomSeconds is the alternate engine zoom path.
	virtual bool NavigateWorld(float rightSeconds, float forwardSeconds, float zoomSeconds) = 0;
	virtual bool CanAdjustWorld() = 0;

	// ---- Building placement ----
	// True while a building preview exists at all (TheInGameUI->getPendingPlaceType() != nullptr), even for
	// placements that cannot be rotated (walls, anchored line builds). Default: same as CanRotatePlacement.
	virtual bool PlacementPending() { return CanRotatePlacement(); }
	virtual bool CanRotatePlacement() = 0;
	virtual bool RotatePlacement(float radians) = 0;
	virtual float PlacementDegrees() = 0;
	// Optional: -1 unknown, 0 illegal location, 1 legal (XrGameBoot_PlacementLegal: the engine's own ghost check).
	virtual int PlacementLegal() { return -1; }
	// Optional pinch preview: what a click at the active spatial pointer would do (kVisionIntent*); `target`/`radius` get the
	// room-space position and table size of the object it would select or act on, if any. Issues nothing.
	virtual int PointerIntent(XrVector3f &target, float &radius, bool &hasTarget) { (void)target; (void)radius; hasTarget = false; return 0; }
	// Optional: TouchInput::hasArmedCommand() (superweapon / ability waiting for a target).
	virtual bool HasArmedCommand() { return false; }

	// ---- Ground View (observer) ----
	virtual bool CanObserveGround() = 0;
	virtual bool PickObserverGround(const XrSurface &board, const XrPosef &aim, XrVector3f &ground,
		XrVector3f *roomPoint) = 0;
	virtual bool ObserverStep(XrVector3f current, XrVector3f delta, XrVector3f &next) = 0;

	// ---- Read-only state used to gate gestures and route panels ----
	virtual bool IsInteractiveGame() = 0;
	virtual bool CanStereoWorld() = 0;
	virtual bool ExpandedUI() = 0;
	virtual bool HasUIAt(float x, float y) = 0;
	virtual int GameWidth() = 0;
	virtual int GameHeight() = 0;
	virtual void TacticalState(int &mode, int &group, bool &queue) = 0;
	virtual std::string HoverInfo(float x, float y) = 0;
	virtual std::string WorldHoverInfo() = 0;
};

// Inert implementation: nothing is picked, nothing is sent. Board/workspace gestures still work.
class VisionNullEngineBridge : public VisionEngineBridge {
public:
	void Pointer(bool, float, float, bool, bool, float) override {}
	void Key(VisionKey, bool) override {}
	void RoutePointer(int) override {}
	bool PickWorld(const XrSurface &, const XrPosef &, XrWorldHit &) override { return false; }
	void SpatialPointer(bool) override {}
	void SpatialTrigger(bool, bool, bool) override {}
	void SpatialClick(bool) override {}
	void CancelTarget() override {}
	void TacticalAction(int) override {}
	bool AdjustCamera(float, float) override { return false; }
	bool NavigateWorld(float, float, float) override { return false; }
	bool CanAdjustWorld() override { return false; }
	bool CanRotatePlacement() override { return false; }
	bool RotatePlacement(float) override { return false; }
	float PlacementDegrees() override { return 0; }
	bool CanObserveGround() override { return false; }
	bool PickObserverGround(const XrSurface &, const XrPosef &, XrVector3f &, XrVector3f *) override { return false; }
	bool ObserverStep(XrVector3f, XrVector3f, XrVector3f &) override { return false; }
	bool IsInteractiveGame() override { return false; }
	bool CanStereoWorld() override { return false; }
	bool ExpandedUI() override { return false; }
	bool HasUIAt(float, float) override { return false; }
	int GameWidth() override { return 1280; }
	int GameHeight() override { return 720; }
	void TacticalState(int &mode, int &group, bool &queue) override { mode = 0; group = 0; queue = false; }
	std::string HoverInfo(float, float) override { return std::string(); }
	std::string WorldHoverInfo() override { return std::string(); }
};
