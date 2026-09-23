// visionOS interaction layer: forwarding VisionEngineBridge, see VisionEngineBridgeXr.h.
//
// Every method is a one-line forward to the XrGameBoot_* function of the same name; nothing here decides anything.
// The three optional queries (PlacementPending, HasArmedCommand, PlacementLegal) are the only engine reads that
// XrGameBoot.h does not export; they use the same engine objects XrGameBoot.cpp itself reads for the same purpose.
#include "VisionEngineBridgeXr.h"
#include "gx_backend.h" // GX_XR_HOST

#if defined(GX_XR_HOST)
#include "XrGameBoot.h"
#endif

// The forwarding body is compiled only where the XrGameBoot_* API is actually declared and defined: on Android, and on any
// other GX_XR_HOST platform once package C has made XrGameBoot.h host neutral and defined the functions. C signals that by
// defining GX_XRGAMEBOOT_HOST (in XrGameBoot.h or as a target compile definition). Until then the factory below returns
// nullptr, so adding this file to the engine library (visionos/*.cpp glob) can never break the build.
#if defined(GX_XR_HOST) && (defined(__ANDROID__) || defined(GX_XRGAMEBOOT_HOST))

// GX_VISION_BRIDGE_NO_ENGINE_QUERIES is defined only by scripts/qa/vision-bridge-forward-test.sh, which links the
// forwarder against recording XrGameBoot_* stubs without the engine headers.
#if !defined(GX_VISION_BRIDGE_NO_ENGINE_QUERIES)
#include "GameClient/InGameUI.h"                 // TheInGameUI->getPendingPlaceType()
#include "SDL3Device/GameClient/TouchInput.h"    // TouchInput::hasArmedCommand()
#endif

namespace {

class VisionXrGameBootBridge final : public VisionEngineBridge {
public:
	// ---- Pointer and keyboard ----
	void Pointer(bool active, float x, float y, bool select, bool secondary, float wheel) override {
		XrGameBoot_Pointer(active, x, y, select, secondary, wheel);
	}
	void Key(VisionKey key, bool down) override { XrGameBoot_Key(static_cast<XrGameKey>(key), down); }
	void RoutePointer(int target) override { XrGameBoot_RoutePointer(target); }

	// ---- Spatial (tabletop) path ----
	bool PickWorld(const XrSurface &board, const XrPosef &aim, XrWorldHit &hit) override {
		return XrGameBoot_PickWorld(board, aim, hit);
	}
	void SpatialPointer(bool active) override { XrGameBoot_SpatialPointer(active); }
	void SpatialTrigger(bool down, bool available, bool additive) override {
		XrGameBoot_SpatialTrigger(down, available, additive);
	}
	void SpatialClick(bool cancel) override { XrGameBoot_SpatialClick(cancel); }
	void CancelTarget() override { XrGameBoot_CancelTarget(); }
	void TacticalAction(int action) override { XrGameBoot_TacticalAction(action); }

	// ---- Camera ----
	bool AdjustCamera(float yawRadians, float pitchRadians) override {
		return XrGameBoot_AdjustCamera(yawRadians, pitchRadians);
	}
	bool NavigateWorld(float rightSeconds, float forwardSeconds, float zoomSeconds) override {
		return XrGameBoot_NavigateWorld(rightSeconds, forwardSeconds, zoomSeconds);
	}
	bool CanAdjustWorld() override { return XrGameBoot_CanAdjustWorld(); }

	// ---- Building placement ----
	bool PlacementPending() override {
#if !defined(GX_VISION_BRIDGE_NO_ENGINE_QUERIES)
		return TheInGameUI != nullptr && TheInGameUI->getPendingPlaceType() != nullptr;
#else
		return CanRotatePlacement();
#endif
	}
	bool CanRotatePlacement() override { return XrGameBoot_CanRotatePlacement(); }
	bool RotatePlacement(float radians) override { return XrGameBoot_RotatePlacement(radians); }
	float PlacementDegrees() override { return XrGameBoot_PlacementDegrees(); }
	// The engine's own legality check of the ghost (the one that tints it red), kept by InGameUI.
	int PlacementLegal() override { return XrGameBoot_PlacementLegal(); }
	int PointerIntent(XrVector3f &target, float &radius, bool &hasTarget) override {
		hasTarget = false;
		XrVector3f t = {};
		float r = -1.0f;
		const int intent = XrGameBoot_PointerIntent(&t, &r);
		if (r > 0.0f) { target = t; radius = r; hasTarget = true; }
		return intent;
	}
	bool HasArmedCommand() override {
#if !defined(GX_VISION_BRIDGE_NO_ENGINE_QUERIES)
		return TouchInput::hasArmedCommand() != 0;
#else
		return false;
#endif
	}

	// ---- Ground View ----
	bool CanObserveGround() override { return XrGameBoot_CanObserveGround(); }
	bool PickObserverGround(const XrSurface &board, const XrPosef &aim, XrVector3f &ground,
		XrVector3f *roomPoint) override {
		return XrGameBoot_PickObserverGround(board, aim, ground, roomPoint);
	}
	bool ObserverStep(XrVector3f current, XrVector3f delta, XrVector3f &next) override {
		return XrGameBoot_ObserverStep(current, delta, next);
	}

	// ---- Read-only state ----
	bool IsInteractiveGame() override { return XrGameBoot_IsInteractiveGame(); }
	bool CanStereoWorld() override { return XrGameBoot_CanStereoWorld(); }
	bool ExpandedUI() override { return XrGameBoot_ExpandedUI(); }
	bool HasUIAt(float x, float y) override { return XrGameBoot_HasUIAt(x, y); }
	int GameWidth() override { return XrGameBoot_GameWidth(); }
	int GameHeight() override { return XrGameBoot_GameHeight(); }
	void TacticalState(int &mode, int &group, bool &queue) override { XrGameBoot_TacticalState(mode, group, queue); }
	std::string HoverInfo(float x, float y) override { return XrGameBoot_HoverInfo(x, y); }
	std::string WorldHoverInfo() override { return XrGameBoot_WorldHoverInfo(); }
};

} // namespace

VisionEngineBridge *VisionCreateXrGameBootBridge() {
	static VisionXrGameBootBridge bridge;
	return &bridge;
}

#else // no XrGameBoot host API in this build

VisionEngineBridge *VisionCreateXrGameBootBridge() { return nullptr; }

#endif
