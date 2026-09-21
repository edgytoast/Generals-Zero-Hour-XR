// visionOS host: what to show and where, per engine frame (MINIMAL; the Quest presentation state machine is package C2).
//
// VisionPresentation::update() is the single seam for that state machine. It runs on the engine thread before
// VisionFrameDriver::step() and decides, from the engine's own state and the compositor's head/eye snapshot:
//   * whether the world is captured in stereo for the tabletop (interactive skirmish/campaign) or the engine shows
//     a conventional screen (shell, menus, movies, loading),
//   * which ring targets and sizes this frame needs (GXHostFrameRequest),
//   * the panel table the interaction layer picks against (VisionPanel) and the matching composite layers,
//   * the engine's XrWorldFrame (board, extents, toggles; eyes / fov / board pose are filled by the driver).
// What this minimal version does: stereo world when the engine reports an interactive game, plus the engine UI
// texture on one tilted panel in front of the board; otherwise the composed game texture on one upright panel above
// the far edge of the board. No layout persistence, no workspace arrangement, no Commands console, no Ground View
// veil, no result card: those are C2's job and replace the body of update(), not its callers.
#pragma once

#include "GXEngineHostServices.h"
#include "VisionFrameDriver.h"

struct VisionPresentationInput {
	const XRFrameInfo *frame = nullptr;
	bool sessionRunning = true;
	bool engineBooted = false;
	int gameWidth = 1280, gameHeight = 720; // engine backbuffer
	bool atlas = false;                     // ring stereo layout (both eyes in STEREO_LEFT)
	bool interactive = false;               // XrGameBoot_IsInteractiveGame()
	bool canStereoWorld = false;            // XrGameBoot_CanStereoWorld()
};

struct VisionPresentationOutput {
	XrWorldFrame world;                     // enabled / sizes / toggles set here; board, eyes, fov by the driver
	VisionPanel panels[kVisionMaxPanels];
	int panelCount = 0;
	bool stereoWorld = false;               // the world is captured per eye this frame
	bool splitUI = true;                    // XrGameBoot_SetSplitEnabled
	GXHostFrameRequest request = {};        // ring targets this frame needs
	GXHostLayer layers[GX_HOST_MAX_LAYERS]; // planned composite layers (targets checked after the frame)
	int layerCount = 0;
	bool hasFocus = false;                  // world point for the constant depth of the eye composite (the table)
	float focus[3] = {0, 0, 0};
};

class VisionPresentation {
public:
	void update(const VisionPresentationInput &in, const VisionFrameDriver &driver, VisionPresentationOutput &out);

private:
	void addPanel(VisionPresentationOutput &out, VisionPanelKind kind, int target, const char *name, const XrPosef &pose,
		float widthM, float aspect);
};
