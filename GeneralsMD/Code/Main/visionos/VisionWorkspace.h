// visionOS workspace: the persisted part of the user's arrangement (XrLayout, the Quest's versioned relative layout file) and the
// anchor it is relative to. PURE apart from the file I/O of XrLayout::load / save; host tested (scripts/qa/vision-presentation-test.sh).
//
// Quest policy P20 (XrHello.cpp, "free-standing session geometry"): there is no persistent room anchor, so a pose saved at home can
// be oversized or behind the player elsewhere. The file is therefore SAVED with poses relative to the launch heading (so a future
// versions can use them) but on load only the preferences are restored (worldZoom, health bars, unit rings, board frame, resolution
// tier, ...). On visionOS the same reasoning applies (the immersive space origin is where the user was when it opened), and
// additionally the board WIDTH is restored, clamped, because it is not room relative: the user's chosen table size survives.
#pragma once

#include <cmath>
#include <string>

#include "VisionPresentationLogic.h"
#include "XrWorkspacePlacement.h"

struct VisionWorkspace {
	XrLayout layout;                 // preferences + the last saved arrangement
	XrPosef anchor = {{0, 0, 0, 1}, {0, 0, 0}};
	bool anchorKnown = false;
	bool loaded = false;             // a layout file was read (migrated included)
	bool upgraded = false;
	bool widthPending = false;       // restore the saved board width once the board exists
	float savedBoardWidth = 0;
	bool dirty = false;
	unsigned saves = 0, saveFailures = 0;

	// Reads the layout (v2 path, then the v1 legacy path) and applies the Quest P20 rule. Returns whether a file was found.
	bool loadFrom(const char *path, const char *legacyPath, int systemLanguageCode = 1)
	{
		bool restored = path != nullptr && path[0] != '\0' && layout.load(path);
		if (!restored && legacyPath != nullptr && legacyPath[0] != '\0') restored = layout.load(legacyPath);
		layout.initializeLanguage(restored, systemLanguageCode);
		upgraded = layout.upgradeDefaults();
		loaded = restored;
		const float width = layout.relative[1].width;
		// Only a saved width that the interaction layer would also accept is restored (0.45 ... 2.0 m).
		widthPending = restored && std::isfinite(width) && width >= 0.45f && width <= 2.0f && std::fabs(width - 1.0f) > 0.02f;
		savedBoardWidth = widthPending ? width : 0.0f;
		layout.applyFreeStandingStart();  // P20: never restore yesterday's room-relative geometry
		dirty = upgraded || !restored;
		return restored;
	}

	// The head heading of the first tracked frame becomes the anchor (XrWorkspacePlacement.h: "place once per process").
	void ensureAnchor(const XRFrameInfo &frame)
	{
		if (anchorKnown || !frame.head_tracked) return;
		anchor = visionHeadingAnchor(frame);
		anchorKnown = true;
	}

	// The upright screen: Quest relative[0] applied to the anchor.
	XrSurface screenSurface() const
	{
		XrSurface s = layout.relative[0];
		s.pose = xrPoseMul(anchor, s.pose);
		return s;
	}

	// Record the current arrangement (board, UI bar, screen relative to the anchor) for the next save.
	void record(const XrSurface &board, const XrSurface &uiBar, float worldZoom)
	{
		if (!anchorKnown) return;
		const XrPosef inverse = xrPoseInverse(anchor);
		layout.worldZoom = std::clamp(worldZoom, 0.5f, 3.0f);
		layout.relative[1] = board;
		layout.relative[1].pose = xrPoseMul(inverse, board.pose);
		layout.relative[2] = uiBar;
		layout.relative[2].pose = xrPoseMul(inverse, uiBar.pose);
		dirty = true;
	}

	bool save(const char *path)
	{
		if (!dirty || path == nullptr || path[0] == '\0') return false;
		if (layout.save(path)) {
			dirty = false;
			++saves;
			return true;
		}
		++saveFailures;
		return false;
	}
};
