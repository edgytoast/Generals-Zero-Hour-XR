// visionOS spatial UI: the action side of the Quest 'Commands' and 'Menu' consoles.
//
// A faithful port of the two Quest switch tables, with the Quest line references in the .cpp:
//   * XrCommandUI.h  applyCommandAction()   -> visionApplyCommandAction()
//   * XrMenuUI.h     applyMenuAction()      -> visionApplyMenuAction()
//   * XrInteraction.h controller gestures   -> visionApplyExtraAction() (camera presets, home base, language)
// Every action ends in the same XrGameBoot_* call as on the Quest. Nothing here decides a game rule: selection,
// orders, groups, formations and camera all stay inside the engine behind those calls.
//
// Engine seam. Package E's VisionEngineBridge already exports TacticalAction, CancelTarget, CanAdjustWorld, ExpandedUI,
// CanStereoWorld, CanObserveGround, IsInteractiveGame, TacticalState and the two hover queries. The consoles need a few
// more XrGameBoot_* functions the bridge does not carry yet (TacticalGroup, Bookmark, ViewBase, CameraPreset,
// SaveCameraDefault, Communicator, SetLanguage and the read-only state behind the button labels); they are the small
// companion interface VisionCommandExtras below, implemented by the engine glue in GXEnginePanelState.cpp and by
// recording fakes in scripts/qa/vision-ui-*-test.cpp. If package E later folds them into VisionEngineBridge the
// interface can be deleted and the calls redirected (one adapter class).
//
// Threading: like XrGameBoot, every call here must be made on the engine thread (GXEngineHost_Post); the functions
// are pure otherwise (no globals), so hosts and tests can call them anywhere.
#pragma once

#include <string>

#include "GXEnginePanelState.h"
#include "VisionEngineBridge.h"

class VisionCommandExtras {
public:
	virtual ~VisionCommandExtras() {}

	// ---- calls (XrGameBoot.h) ----
	virtual void TacticalGroup(int group, int operation) = 0; // 0 recall, 1 save, 2 add, 3 center
	virtual void Bookmark(int slot, bool save) = 0;           // map views A-D
	virtual bool ViewBase() = 0;                              // home base camera
	virtual bool CameraPreset(int preset) = 0;                // 0 classic, 1..3 presets, 4 favorite
	virtual bool SaveCameraDefault() = 0;
	virtual void Communicator() = 0;
	virtual void SetLanguage(int language) = 0;              // 0 German, 1 English

	// ---- read-only state behind labels and enabled states ----
	virtual std::string TacticalReason(int action) = 0;      // empty = allowed
	virtual bool FormationActive() = 0;
	virtual bool BookmarkKnown(int slot) = 0;
	virtual int GroupSize(int group) = 0;
	virtual std::string TacticalStatus() = 0;
	virtual std::string TacticalHint() = 0;
	virtual std::string LanguageStatus() = 0;
	virtual int MatchResult() = 0;                           // XrEndgameResult as int
	virtual int DefaultCameraPreset() = 0;
};

struct VisionCommandContext {
	VisionEngineBridge *bridge = nullptr;  // never null when actions are applied
	VisionCommandExtras *extras = nullptr; // never null when actions are applied
	// Presentation flags the Quest reads from XrHello (x.splitVisible / x.stereoVisible). Only the two menu actions that
	// the Quest gates on them use them.
	bool splitVisible = true;
	bool stereoVisible = true;
};

// true when `id` is a live (hittable) control of that console page: the same test the Quest hit-testing applies
// (a dead gap of the panel never reaches the switch).
bool visionPanelHasControl(int page, int id, bool help, bool tactics);

// applyCommandAction(): `id` is a control of the Commands console (33 close ... 47). Mutates the session exactly like
// x.commands.* and makes the same engine calls.
GXPanelActionResult visionApplyCommandAction(GXPanelSession &session, VisionCommandContext &ctx, int id);

// applyMenuAction() for the pages of the 'Menu' console (GX_PANEL_MENU_*): sets session.menuPage = page first, so a
// navigation control (tabs, 'Manage groups', help) leaves the new page in session.menuPage.
GXPanelActionResult visionApplyMenuAction(GXPanelSession &session, int page, VisionCommandContext &ctx, int id);

// Controller gestures of the Quest that are useful as buttons (GX_EXTRA_*).
GXPanelActionResult visionApplyExtraAction(GXPanelSession &session, int extra, int value, VisionCommandContext &ctx);

// Everything the consoles need to know from the engine, as one plain snapshot (called on the engine thread by the
// frame hook; pure against the two seams). Leaves `version`, `worldHover*` and `panelHover` untouched.
void visionCaptureSnapshot(VisionCommandContext &ctx, const GXPanelSession &session, GXPanelSnapshot &out);

// The action a Commands-console control id maps to (xrCommandAction, XrCommands.h): -1 = none.
int visionCommandTacticalAction(int id);
