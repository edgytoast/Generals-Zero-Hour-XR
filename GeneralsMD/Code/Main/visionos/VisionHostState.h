// visionOS interaction layer: the per-frame snapshot the host hands to VisionInteraction.
//
// Everything here is a plain value, filled by the render thread once per frame right
// before VisionInteraction::update(). No Apple or OpenXR types: poses are XrPosef (POD,
// see xr_shim/openxr/openxr.h), matching the pure Xr*.h headers.
//
// Conventions (identical to the Quest host, so the Xr*.h math is reused unchanged):
//   * World / room space: right-handed, +Y up, metres, -Z is a pose's forward axis. It is the
//     immersive-space origin used by ARKit device anchors and by LayerRenderer spatial events.
//   * A surface (XrSurface) is a quad centred on `pose`, `width` metres wide along local +X and
//     width*aspect metres tall along local +Y; the quad normal is local +Z (out of the play surface).
//     A tabletop board lies flat with the normal pointing up; local +Y is the FAR edge of the map and
//     local -Y (the "bottom" edge) is the edge nearest the player. "Board space" below always means
//     that local frame in metres: x right, y toward the far edge, z up out of the board.
#pragma once

#include "VisionEngineBridge.h"
#include "XrPlacement.h"

constexpr int kVisionMaxPanels = 6;

// Panel kinds, numbered like the Quest surface slots so route selection is identical
// (slot 3, the transparent HUD overlay, routes to the windows target).
enum VisionPanelKind {
	kVisionPanelGameScreen = 0,      // composed 1280x720 frame: shell, menus, movies
	kVisionPanelWorld2D = 1,         // planar world (only when stereo is unavailable)
	kVisionPanelGameUI = 2,          // engine control bar / build window texture
	kVisionPanelGameHud = 3,         // transparent HUD above the control bar (hit only where a window exists)
	kVisionPanelCommandsButton = 4,  // host-owned small "Commands" toggle (XrPanelLayout id 100)
	kVisionPanelCommandsConsole = 5  // host-owned army console (XrPanelLayout command table)
};

struct VisionPanel {
	bool visible = false;
	VisionPanelKind kind = kVisionPanelGameUI;
	XrSurface surface;                 // room-space quad (world locked)
	float aspect = 9.0f / 16.0f;       // height / width of the quad
	XrGameRect rect;                   // crop of the composed frame shown on the quad (game panels)
	bool help = false, tactics = false; // Commands console layout variant (xrCommandLayout)
};

// Engine state the interaction layer gates on. Fill it with visionCaptureEngineFlags(bridge)
// (VisionInteraction.h) or from your own cached values.
struct VisionEngineFlags {
	bool interactiveGame = false;   // XrGameBoot_IsInteractiveGame
	bool canStereoWorld = false;    // XrGameBoot_CanStereoWorld
	bool canAdjustWorld = false;    // XrGameBoot_CanAdjustWorld: skirmish/campaign, camera not script-locked
	bool canObserveGround = false;  // XrGameBoot_CanObserveGround
	bool expandedUI = false;        // native dialog open: world gestures are suspended, panels stay live
	bool placementPending = false;  // a building preview exists (bridge PlacementPending; falls back to CanRotatePlacement)
	bool canRotatePlacement = false; // XrGameBoot_CanRotatePlacement: the preview can be rotated
	bool armedCommand = false;      // XrGameBoot HasArmedCommand (ability/superweapon waiting for a target)
	int placementLegal = -1;        // -1 unknown, 0 illegal, 1 legal (optional engine query)
	float placementDegrees = 0;     // XrGameBoot_PlacementDegrees
	int gameWidth = 1280, gameHeight = 720; // composed frame size in engine pixels
};

struct VisionHostState {
	double time_s = 0;           // monotonic seconds (XRFrameInfo.predicted_display_time_s or the host clock)
	uint64_t frame = 0;
	bool sessionFocused = true;  // false while the layer is paused/inactive/invalidated: input is suspended
	bool headTracked = true;     // false = no device anchor: everything in flight is cancelled

	XrPosef head = {{0, 0, 0, 1}, {0, 1.5f, 0}}; // head (midpoint of the eyes) in room space

	// The physical board. The host owns the pose between frames; the interaction layer proposes changes
	// in VisionInteractionOutput::board. When boardPlaced is false the layer computes the initial
	// placement from the head pose (0.9 m ahead, table height, yaw only).
	bool boardPlaced = false;
	bool boardVisible = true;
	XrSurface board;
	float boardAspect = 9.0f / 16.0f; // height / width of the map area (tactical view h / w)
	bool tableHeightKnown = false;    // ARKit plane detection found a table (not available in the simulator)
	float tableHeight = 0;            // world Y of that table top
	float worldZoom = 1.0f;           // host-side map zoom (coverage multiplier 0.5..3, Quest semantics)

	VisionPanel panels[kVisionMaxPanels];
	int panelCount = 0;

	VisionEngineFlags engine;
};
