// visionOS spatial UI: the action side of the Quest consoles. See VisionCommandActions.h.
//
// Quest references are to the sources under GeneralsMD/Code/Main (line numbers of the commit this port started from):
//   XrCommandUI.h   applyCommandAction  lines 21-45
//   XrMenuUI.h      applyMenuAction     lines 52-152
//   XrCommands.h    xrCommandAction     line 15 (the id -> tactical action table)
//   XrInteraction.h camera / home base  lines 125, 163, 207-216
// Each block below says which lines it ports. The order of the checks is the Quest's order: a change of order changes
// which press is swallowed, so keep it.
#include "VisionCommandActions.h"

#include <cstring>

#include "XrCommands.h"    // xrCommandAction, xrCommandHit (XrPanelLayout.h tables)
#include "XrPanelLayout.h"
#include "XrStrings.h"

namespace {

void copyText(char *dst, size_t capacity, const std::string &text)
{
	if (capacity == 0) return;
	std::strncpy(dst, text.c_str(), capacity - 1);
	dst[capacity - 1] = '\0';
	// Never leave half a UTF-8 sequence at the cut (the umlauts and arrows of the labels are multi-byte).
	const size_t n = std::strlen(dst);
	if (n == 0) return;
	size_t lead = n - 1;
	while (lead > 0 && (static_cast<unsigned char>(dst[lead]) & 0xC0) == 0x80) --lead;
	const unsigned char b = static_cast<unsigned char>(dst[lead]);
	const size_t need = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
	if (lead + need > n) dst[lead] = '\0';
}

GXPanelActionResult handled(bool engine = false)
{
	GXPanelActionResult r = {};
	r.handled = true;
	r.engineCalled = engine;
	return r;
}
GXPanelActionResult ignored() { return GXPanelActionResult{}; }

} // namespace

int visionCommandTacticalAction(int id) { return xrCommandAction(id); }

bool visionPanelHasControl(int page, int id, bool help, bool tactics)
{
	XrPanelControl table[80];
	int count = 0;
	if (page == GX_PANEL_COMMANDS) count = xrCommandLayout(help, tactics, table, 80);
	else if (page >= GX_PANEL_MENU_WINDOWS && page <= GX_PANEL_MENU_HELP) count = xrMenuLayout(page, table, 80);
	for (int i = 0; i < count; ++i)
		if (table[i].id == id && xrPanelRoleHittable(table[i].role)) return true;
	return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Commands console (XrCommandUI.h:21-45)
// ---------------------------------------------------------------------------------------------------------------------
GXPanelActionResult visionApplyCommandAction(GXPanelSession &s, VisionCommandContext &c, int hit)
{
	VisionEngineBridge &bridge = *c.bridge;
	VisionCommandExtras &extras = *c.extras;
	if (!visionPanelHasControl(GX_PANEL_COMMANDS, hit, s.help, s.tactics)) return ignored(); // a gap never reaches the switch

	// :22 'Taktik +/-' foldout.
	if (hit == 37) {
		s.tactics = !s.tactics;
		s.bookmarkSave = false;
		s.groupOperation = 0;
		return handled();
	}
	// :23-30 tactics foldout: 40 formation, 41 force move, 42 guard without pursuit, 43 'Save view', 44-47 map views A-D.
	if (hit >= 40 && hit <= 47) {
		if (!s.tactics || s.help || !bridge.CanAdjustWorld()) return handled(); // :24
		if (hit == 43) { // :25
			s.bookmarkSave = !s.bookmarkSave;
			s.groupOperation = 0;
			return handled();
		}
		if (hit >= 44) { // :26 recall, or store when 'Save view' was armed; the flag is consumed either way
			extras.Bookmark(hit - 44, s.bookmarkSave);
			s.bookmarkSave = false;
			return handled(true);
		}
		if (!extras.TacticalReason(hit).empty()) return handled(); // :27 disabled: the reason is shown, nothing is sent
		s.bookmarkSave = false; // :28
		s.groupOperation = 0;
		GXPanelActionResult r = handled(true);
		r.stereoWorld = true; // x.stereoWorld = true
		bridge.TacticalAction(hit);
		return r;
	}
	s.bookmarkSave = false; // :31
	// :31 close: hides the console (x.layout.commandsVisible = false); the host decides what that means.
	if (hit == 33) {
		s.groupOperation = 0;
		GXPanelActionResult r = handled();
		r.host = GX_HOST_HIDE_COMMANDS;
		return r;
	}
	// :32 'Help' toggle.
	if (hit == 34) {
		s.help = !s.help;
		s.groupOperation = 0;
		return handled();
	}
	// :33 help 'Next'.
	if (hit == 36 && s.help) {
		s.helpPage = (s.helpPage + 1) % 4;
		return handled();
	}
	if (!bridge.CanAdjustWorld()) return handled(); // :34
	// :35 Communicator.
	if (hit == 35) {
		s.groupOperation = 0;
		extras.Communicator();
		return handled(true);
	}
	// :36 group operations: tapping the pending one again cancels it.
	if (hit >= 30 && hit <= 32) {
		s.groupOperation = s.groupOperation == hit - 29 ? 0 : hit - 29;
		return handled();
	}
	// :37-39 group number: recall (operation 0) or the pending save / add / center.
	if (hit >= 20 && hit < 30) {
		extras.TacticalGroup(hit - 20, s.groupOperation);
		s.groupOperation = 0;
		return handled(true);
	}
	// :40-44 everything else through the id -> tactical action table.
	const int action = xrCommandAction(hit);
	if (action < 0) return handled();
	s.groupOperation = 0;
	GXPanelActionResult r = handled(true);
	if (action <= 9) r.stereoWorld = true;
	bridge.TacticalAction(action);
	return r;
}

// ---------------------------------------------------------------------------------------------------------------------
// Menu console (XrMenuUI.h:52-152)
// ---------------------------------------------------------------------------------------------------------------------
GXPanelActionResult visionApplyMenuAction(GXPanelSession &s, int page, VisionCommandContext &c, int action)
{
	VisionEngineBridge &bridge = *c.bridge;
	VisionCommandExtras &extras = *c.extras;
	s.menuPage = page;
	if (!visionPanelHasControl(page, action, false, false)) return ignored();

	// :63 the controller guide is reachable from every tab; navigation never dispatches gameplay actions.
	if (action == 24) {
		s.menuPage = GX_PANEL_MENU_HELP;
		s.menuHelpPage = 0;
		return handled();
	}
	// :64-69 help page: 33 close, 34 back to the windows, 36 next (kXrControllerHelpPages = 5).
	if (page == GX_PANEL_MENU_HELP) {
		GXPanelActionResult r = handled();
		if (action == 33) {
			r.host = GX_HOST_FINISH_ARRANGEMENT;
			r.closeMenu = true;
		} else if (action == 34) s.menuPage = GX_PANEL_MENU_WINDOWS;
		else if (action == 36) s.menuHelpPage = (s.menuHelpPage + 1) % 5;
		return r;
	}
	// :70 'Close' on every tab: finishArrangement (closes the menu, ends window editing).
	if (action == 17) {
		GXPanelActionResult r = handled();
		r.host = GX_HOST_FINISH_ARRANGEMENT;
		r.closeMenu = true;
		return r;
	}
	// :71 tabs.
	if (action >= 20 && action <= 23) {
		s.menuPage = action - 20;
		return handled();
	}
	// :72-94 the two order pages.
	if (page == GX_PANEL_MENU_UNITS || page == GX_PANEL_MENU_GROUPS) {
		if (page == GX_PANEL_MENU_UNITS) {
			if (action == 14) { s.menuPage = GX_PANEL_MENU_GROUPS; return handled(); } // :75
			if (action == 15) { // :76 back to the game
				GXPanelActionResult r = handled();
				r.closeMenu = true;
				return r;
			}
			GXPanelActionResult r = handled(true);
			// :78-81 targeted tactics use the real world ray: they need the tabletop and an adjustable world.
			if (action >= 0 && action <= 9) {
				if (!c.splitVisible || !bridge.CanAdjustWorld()) return handled();
				r.stereoWorld = true;
			}
			bridge.TacticalAction(action == 16 ? 31 : action); // :82
			if (action <= 8 || action == 12 || action == 13 || action == 16) r.closeMenu = true; // :83
			return r;
		}
		GXPanelActionResult r = handled(); // GX_PANEL_MENU_GROUPS, :85-93
		if (action < 12) { bridge.TacticalAction(action + 20); r.engineCalled = true; }
		if (action == 12) { bridge.TacticalAction(12); r.engineCalled = true; }
		if (action == 13) { bridge.TacticalAction(10); r.engineCalled = true; }
		if (action == 14) { s.menuPage = GX_PANEL_MENU_UNITS; return r; }
		if (action == 15) { s.menuPage = GX_PANEL_MENU_WINDOWS; return r; }
		if (action == 16) { bridge.TacticalAction(13); r.engineCalled = true; }
		if ((action >= 3 && action <= 11) || action == 12) r.closeMenu = true; // :92
		return r;
	}
	// :95-141 presentation page ('Ansicht').
	if (page == GX_PANEL_MENU_VIEW) {
		if (action < 0 || action > 16) return ignored(); // :96
		if (action <= 3) return ignored();                // :97 reserved status / help slots, no flat mode
		GXPanelPrefs &p = s.prefs;
		if (action == 16) { // :98-101 armGroundView: needs the stereo world and an observable ground
			GXPanelActionResult r = handled();
			if (!c.stereoVisible || !bridge.CanObserveGround()) return r;
			bridge.CancelTarget(); // :47-49 armGroundView also cancels the armed target
			r.engineCalled = true;
			r.host = GX_HOST_ARM_GROUND_VIEW;
			r.closeMenu = true;
			return r;
		}
		// :104-116 session-only experiments: shadows A/B, timing, stereo path, extra world; nothing is saved.
		if (action >= 12 && action <= 15) {
			if (action == 12) p.volumeShadows = !p.volumeShadows;
			else if (action == 13) p.measurement = !p.measurement;
			else if (action == 14) {
				if (p.multiviewStereo) { p.multiviewStereo = false; p.atlasStereo = false; }
				else if (p.atlasStereo) { p.atlasStereo = false; p.multiviewStereo = true; }
				else p.atlasStereo = true;
			} else p.elideWorldCopy = !p.elideWorldCopy;
			GXPanelActionResult r = handled();
			r.host = GX_HOST_TOGGLE_PREF;
			r.hostValue = action;
			return r;
		}
		GXPanelActionResult r = handled();
		r.host = GX_HOST_TOGGLE_PREF;
		r.hostValue = action;
		if (action == 4) p.healthBars = !p.healthBars;   // :117
		if (action == 5) p.unitRings = !p.unitRings;     // :118
		if (action == 6) p.boardFrame = !p.boardFrame;   // :119
		if (action == 7) { r.closeMenu = true; r.host = GX_HOST_FINISH_ARRANGEMENT; } // :119 close
		if (action == 8) { p.leftHanded = !p.leftHanded; r.closeMenu = true; }        // :120
		if (action == 9) { // :121-128 photo arrangement: the whole tabletop layout again
			r.host = GX_HOST_PHOTO_ARRANGEMENT;
			if (bridge.CanStereoWorld()) r.stereoWorld = true; // xrRequestWorldView
			r.closeMenu = true;
		}
		if (action == 10) p.resolutionTier = (p.resolutionTier + 1) % 3; // :129
		if (action == 11) { // :130-137 XR text switches at once, the game text is staged for the next start
			p.language = p.language == 1 ? 0 : 1;
			extras.SetLanguage(p.language);
			r.engineCalled = true;
		}
		return r;
	}
	// :138-151 the workspace page: every action moves a window or the board: host side, not the engine.
	GXPanelActionResult r = handled();
	if (action == 18) { // :55-56 / :58 'Align everything in front of me'
		r.host = GX_HOST_RECENTER_WORKSPACE;
		return r;
	}
	if (action == 12 || action == 13) { // :150 map coverage: only while the world may be adjusted
		if (!bridge.CanAdjustWorld()) return r;
	}
	r.host = GX_HOST_WORKSPACE_EDIT;
	r.hostValue = action;
	return r;
}

// ---------------------------------------------------------------------------------------------------------------------
// Controller gestures as buttons (XrInteraction.h)
// ---------------------------------------------------------------------------------------------------------------------
GXPanelActionResult visionApplyExtraAction(GXPanelSession &s, int extra, int value, VisionCommandContext &c)
{
	VisionEngineBridge &bridge = *c.bridge;
	VisionCommandExtras &extras = *c.extras;
	GXPanelActionResult r = handled(true);
	switch (extra) {
	case GX_EXTRA_CAMERA_PRESET: // :216 XrGameBoot_CameraPreset(x.cameraPreset)
		if (value < 0 || value > 4) return ignored();
		r.hostValue = extras.CameraPreset(value) ? 1 : 0; // 0: not adjustable right now (menu, cinematic, dialog)
		return r;
	case GX_EXTRA_SAVE_CAMERA_DEFAULT: // :207
		r.hostValue = extras.SaveCameraDefault() ? 1 : 0;
		return r;
	case GX_EXTRA_VIEW_BASE: // :125
		r.hostValue = extras.ViewBase() ? 1 : 0;
		return r;
	case GX_EXTRA_CANCEL_TARGET: // XrGameBoot_CancelTarget: arming, tactics and gesture only, never deselects or orders
		bridge.CancelTarget();
		return r;
	case GX_EXTRA_SET_LANGUAGE: // XrMenuUI.h:130-137
		if (value < 0 || value > 1) return ignored();
		s.prefs.language = value;
		extras.SetLanguage(value);
		return r;
	case GX_EXTRA_TACTICAL_ACTION:
		bridge.TacticalAction(value);
		return r;
	default:
		return ignored();
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------------------------------------------------
void visionCaptureSnapshot(VisionCommandContext &c, const GXPanelSession &session, GXPanelSnapshot &out)
{
	VisionEngineBridge &bridge = *c.bridge;
	VisionCommandExtras &extras = *c.extras;
	out.interactiveGame = bridge.IsInteractiveGame();
	out.canAdjustWorld = bridge.CanAdjustWorld();
	out.canStereoWorld = bridge.CanStereoWorld();
	out.canObserveGround = bridge.CanObserveGround();
	out.expandedUI = bridge.ExpandedUI();
	out.splitVisible = c.splitVisible;
	out.stereoVisible = c.stereoVisible;
	int mode = 0, group = 0;
	bool queue = false;
	bridge.TacticalState(mode, group, queue);
	out.mode = mode;
	out.group = group;
	out.queue = queue;
	for (int i = 0; i < 10; ++i) out.groupSize[i] = extras.GroupSize(i);
	out.formationActive = extras.FormationActive();
	for (int i = 0; i < 4; ++i) out.bookmarkKnown[i] = extras.BookmarkKnown(i);
	out.matchResult = extras.MatchResult();
	out.defaultCameraPreset = extras.DefaultCameraPreset();
	const int reasonActions[4] = {8, 40, 41, 42};
	for (int i = 0; i < 4; ++i) copyText(out.reason[i], sizeof(out.reason[i]), extras.TacticalReason(reasonActions[i]));
	copyText(out.status, sizeof(out.status), extras.TacticalStatus());
	copyText(out.hint, sizeof(out.hint), extras.TacticalHint());
	copyText(out.languageStatus, sizeof(out.languageStatus), extras.LanguageStatus());
	out.session = session;
}
