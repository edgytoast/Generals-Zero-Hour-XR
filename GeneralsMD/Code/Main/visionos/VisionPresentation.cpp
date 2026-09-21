// See VisionPresentation.h.
#include "VisionPresentation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
constexpr float kStereoTierScale = 1.0f;
constexpr float kUiPanelWidthM = 0.90f;
constexpr float kUiPanelTiltRad = 0.96f;   // ~55 degrees back from vertical: the control bar faces the player
constexpr float kUiPanelForwardM = 0.20f;  // in front of the near edge of the board
constexpr float kUiPanelHeightM = 0.14f;   // above the table
constexpr float kScreenWidthM = 1.28f;     // upright composed screen
constexpr float kScreenLiftM = 0.02f;
}

void VisionPresentation::addPanel(VisionPresentationOutput &out, VisionPanelKind kind, int target, const char *name,
	const XrPosef &pose, float widthM, float aspect)
{
	if (out.panelCount < kVisionMaxPanels)
		out.panels[out.panelCount++] = visionMakePanel(kind, pose, widthM, aspect);
	if (out.layerCount < GX_HOST_MAX_LAYERS) {
		GXHostLayer &layer = out.layers[out.layerCount++];
		memset(&layer, 0, sizeof(layer));
		snprintf(layer.name, sizeof(layer.name), "%s", name);
		layer.target = target;
		layer.position[0] = pose.position.x; layer.position[1] = pose.position.y; layer.position[2] = pose.position.z;
		layer.orientation[0] = pose.orientation.x; layer.orientation[1] = pose.orientation.y;
		layer.orientation[2] = pose.orientation.z; layer.orientation[3] = pose.orientation.w;
		layer.size[0] = widthM; layer.size[1] = widthM * aspect;
		layer.flags = GX_LAYER_FLIP_Y | GX_LAYER_PREMULTIPLIED; // GL targets are bottom-up, coverage-premultiplied
	}
}

void VisionPresentation::update(const VisionPresentationInput &in, const VisionFrameDriver &driver, VisionPresentationOutput &out)
{
	out = VisionPresentationOutput();
	const XRFrameInfo &frame = *in.frame;
	const bool stereo = in.engineBooted && in.interactive && in.canStereoWorld && frame.eye_count > 0;
	out.stereoWorld = stereo;
	out.splitUI = true;

	// ---- engine world frame ----
	XrWorldFrame &world = out.world;
	world.enabled = stereo;
	const XRRect &vp = frame.eyes[0].viewport;
	xrStereoExtent(unsigned(std::max(0, vp.width)), unsigned(std::max(0, vp.height)), world.width, world.height, 1);
	world.multiviewStereo = false; // ANGLE has no OVR_multiview
	world.atlasStereo = in.atlas;
	world.elideWorldCopy = true;   // the world is drawn once per eye, not a third time for the composed frame

	// ---- ring targets ----
	GXHostFrameRequest &req = out.request;
	req.gameWidth = in.gameWidth;
	req.gameHeight = in.gameHeight;
	req.eyeCount = int(frame.eye_count < 2 ? frame.eye_count : 2);
	req.targetMask = GX_TARGET_GAME | GX_TARGET_UI;
	if (stereo) {
		req.targetMask |= GX_TARGET_STEREO | GX_TARGET_WORLD;
		req.eyeWidth = int(world.width * kStereoTierScale);
		req.eyeHeight = int(world.height * kStereoTierScale);
	}

	// ---- panels and layers (need the board the driver placed) ----
	const VisionHostState &host = driver.host();
	if (!host.boardPlaced) return;
	const XrSurface &board = host.board;
	out.hasFocus = true;
	out.focus[0] = board.pose.position.x; out.focus[1] = board.pose.position.y; out.focus[2] = board.pose.position.z;
	const float boardHeight = board.width * host.boardAspect;
	if (stereo) {
		// Engine UI (control bar) on a panel leaning toward the player, in front of the board's near edge.
		const XrQuaternionf tilt = xrAxisAngle({1, 0, 0}, kUiPanelTiltRad);
		XrPosef pose;
		pose.orientation = xrNormalize(xrMul(board.pose.orientation, tilt));
		pose.position = xrAdd(board.pose.position,
			xrRotate(board.pose.orientation, {0, -(boardHeight * 0.5f + kUiPanelForwardM), kUiPanelHeightM}));
		addPanel(out, kVisionPanelGameUI, GX_XRT_UI, "engine-ui", pose, kUiPanelWidthM,
			float(in.gameHeight) / float(std::max(1, in.gameWidth)));
	} else {
		// Composed screen (shell, menus, movies) standing behind the far edge of the board, facing the player.
		const XrQuaternionf up = xrAxisAngle({1, 0, 0}, 1.5707963f);
		const float aspect = float(in.gameHeight) / float(std::max(1, in.gameWidth));
		XrPosef pose;
		pose.orientation = xrNormalize(xrMul(board.pose.orientation, up));
		pose.position = xrAdd(board.pose.position,
			xrRotate(board.pose.orientation, {0, boardHeight * 0.5f + 0.05f, kScreenWidthM * aspect * 0.5f + kScreenLiftM}));
		addPanel(out, kVisionPanelGameScreen, GX_XRT_GAME, "engine-screen", pose, kScreenWidthM, aspect);
	}
}
