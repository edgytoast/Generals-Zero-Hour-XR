// visionOS spatial UI: the snapshot store, the engine-thread frame hook, the XrGameBoot-backed engine seams and the
// scripted engine. See GXEnginePanelState.h. Compiled into the engine library (visionos/*.cpp glob).
//
// Threads:
//   engine thread : the GXEngineHost frame hook captures the snapshot (~15 Hz, immediately after an action) and executes
//                   posted actions (GXEngineHost_Post) - the only thread that touches XrGameBoot state;
//   any thread    : GXEnginePanelState_Get copies the latest snapshot under a lock; GXPanelAction_Perform posts.
#include "GXEnginePanelState.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include "GXEngineHost.h"
#include "VisionCommandActions.h"
#include "VisionEngineBridgeXr.h"
#include "VisionPanelModel.h"
#include "gx_backend.h"

#if defined(GX_XR_HOST)
#include "XrGameBoot.h"
#endif
#include "XrStrings.h"

#if defined(GX_XR_HOST) && defined(GX_XRGAMEBOOT_HOST)
#define GX_PANEL_HAVE_XRGAMEBOOT 1
#endif

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// The real engine: the calls the VisionEngineBridge does not carry, straight into XrGameBoot_* (one line each).
// ---------------------------------------------------------------------------------------------------------------------
#if defined(GX_PANEL_HAVE_XRGAMEBOOT)
class XrGameBootExtras final : public VisionCommandExtras {
public:
	void TacticalGroup(int group, int operation) override { XrGameBoot_TacticalGroup(group, operation); }
	void Bookmark(int slot, bool save) override { XrGameBoot_Bookmark(slot, save); }
	bool ViewBase() override { return XrGameBoot_ViewBase(); }
	bool CameraPreset(int preset) override { return XrGameBoot_CameraPreset(preset); }
	bool SaveCameraDefault() override { return XrGameBoot_SaveCameraDefault(); }
	void Communicator() override { XrGameBoot_Communicator(); }
	void SetLanguage(int language) override { XrGameBoot_SetLanguage(language); }
	std::string TacticalReason(int action) override { return XrGameBoot_TacticalReason(action); }
	bool FormationActive() override { return XrGameBoot_FormationActive(); }
	bool BookmarkKnown(int slot) override { return XrGameBoot_BookmarkKnown(slot); }
	int GroupSize(int group) override { return XrGameBoot_GroupSize(group); }
	std::string TacticalStatus() override { return XrGameBoot_TacticalStatus(); }
	std::string TacticalHint() override { return XrGameBoot_TacticalHint(); }
	std::string LanguageStatus() override { return XrGameBoot_LanguageStatus(); }
	int MatchResult() override { return static_cast<int>(XrGameBoot_MatchResult()); }
	int DefaultCameraPreset() override { return XrGameBoot_DefaultCameraPreset(); }
};
#endif

// ---------------------------------------------------------------------------------------------------------------------
// The scripted engine (-uiFakeSnapshot): a small in-process model of the tactical state, enough to make every state of
// the consoles visible and every button do something visible. It is NOT the engine and decides no game rule: it mirrors
// the state transitions of XrGameBoot_TacticalAction / TacticalGroup / Bookmark that the panels display.
// ---------------------------------------------------------------------------------------------------------------------
class ScriptedEngine final : public VisionEngineBridge, public VisionCommandExtras {
public:
	int language = 0;
	int selection = 0;
	int mode = 0, group = 0;
	bool queue = false, formation = false, canAdjust = true, expanded = false;
	bool bookmarks[4] = {false, false, false, false};
	int sizes[10] = {0};
	std::string notice; // German source, like s_groupNotice
	std::string worldHoverName;

	void load(int scenario)
	{
		const int keepLanguage = language;
		*this = ScriptedEngine();
		language = keepLanguage;
		switch (scenario) {
		case 1: { // a skirmish in progress
			selection = 5;
			mode = 5;
			queue = true;
			group = 2;
			const int s[10] = {4, 0, 6, 0, 0, 3, 0, 0, 0, 12};
			std::memcpy(sizes, s, sizeof(sizes));
			bookmarks[0] = true;
			bookmarks[2] = true;
			worldHoverName = "Beispieleinheit";
			break;
		}
		case 2: // nothing selected: the tactics that need a selection are disabled with a reason
			selection = 0;
			group = 0;
			bookmarks[1] = true;
			break;
		case 3: // a native dialog is open: the world cannot be adjusted
			selection = 3;
			canAdjust = false;
			expanded = true;
			break;
		default: break;
		}
	}

	std::string tr(const char *german) const { return visionTr(german, language); }

	// ---- VisionEngineBridge ----
	void Pointer(bool, float, float, bool, bool, float) override {}
	void Key(VisionKey, bool) override {}
	void RoutePointer(int) override {}
	bool PickWorld(const XrSurface &, const XrPosef &, XrWorldHit &) override { return false; }
	void SpatialPointer(bool) override {}
	void SpatialTrigger(bool, bool, bool) override {}
	void SpatialClick(bool) override {}
	void CancelTarget() override { mode = 0; queue = false; notice.clear(); }
	void TacticalAction(int action) override
	{
		if (!canAdjust) return;
		notice.clear();
		if (action >= 40 && action <= 42) {
			if (!reason(action).empty()) return;
			cancelTactics();
			if (action == 40) { formation = !formation; return; }
			mode = action == 41 ? 9 : 10;
			return;
		}
		if (action >= 0 && action <= 8) {
			if (action == 8 && !reason(8).empty()) return;
			mode = action;
			if (action != 5) queue = false;
			return;
		}
		if (action == 9) { queue = !queue; if (queue) mode = 5; return; }
		if (action == 10) { cancelTactics(); return; }
		if (action == 13) { cancelTactics(); selection = 0; return; }
		if (action == 20 || action == 21) { group = (group + (action == 20 ? 9 : 1)) % 10; return; }
		if (action >= 22 && action <= 25) { const int op[] = {1, 0, 2, 3}; TacticalGroup(group, op[action - 22]); return; }
		if (action >= 26 && action <= 31) { selection = action == 26 || action == 27 || action == 28 ? 1 : action == 29 ? 4 : action == 30 ? 7 : 24; mode = 0; return; }
	}
	bool AdjustCamera(float, float) override { return false; }
	bool NavigateWorld(float, float, float) override { return false; }
	bool CanAdjustWorld() override { return canAdjust; }
	bool CanRotatePlacement() override { return false; }
	bool RotatePlacement(float) override { return false; }
	float PlacementDegrees() override { return 0; }
	bool CanObserveGround() override { return canAdjust; }
	bool PickObserverGround(const XrSurface &, const XrPosef &, XrVector3f &, XrVector3f *) override { return false; }
	bool ObserverStep(XrVector3f, XrVector3f, XrVector3f &) override { return false; }
	bool IsInteractiveGame() override { return true; }
	bool CanStereoWorld() override { return !expanded; }
	bool ExpandedUI() override { return expanded; }
	bool HasUIAt(float, float) override { return false; }
	int GameWidth() override { return 1280; }
	int GameHeight() override { return 720; }
	void TacticalState(int &m, int &g, bool &q) override { m = mode; g = group; q = queue; }
	std::string HoverInfo(float, float) override { return std::string(); }
	std::string WorldHoverInfo() override
	{
		if (worldHoverName.empty()) return std::string();
		return tr(worldHoverName.c_str()) + "\n" + TacticalStatus();
	}

	// ---- VisionCommandExtras ----
	void TacticalGroup(int g, int operation) override
	{
		if (g < 0 || g >= 10 || operation < 0 || operation > 3 || !canAdjust) return;
		group = g;
		if ((operation == 1 || operation == 2) && selection == 0) { notice = "Keine Auswahl: zuerst eigene Einheiten markieren"; return; }
		if ((operation == 0 || operation == 3) && sizes[g] == 0) { notice = "Gruppe leer: Einheiten wählen → Speichern → Zahl"; return; }
		mode = 0;
		if (operation == 1) sizes[g] = selection;
		else if (operation == 2) sizes[g] += selection;
		else if (operation == 0) selection = sizes[g];
		const char *notices[] = {"Gruppe ausgewählt", "Gruppe gespeichert", "Auswahl zur Gruppe hinzugefügt", "Gruppenansicht zentriert"};
		notice = notices[operation];
	}
	void Bookmark(int slot, bool save) override
	{
		if (slot < 0 || slot >= 4 || !canAdjust) return;
		if (!save && !bookmarks[slot]) { notice = "Kartenplatz leer: Ansicht merken → A–D"; return; }
		cancelTactics();
		if (save) bookmarks[slot] = true;
		notice = save ? "Kartenansicht gespeichert (nur diese Partie)" : "Kartenansicht aufgerufen; Tisch bleibt unverändert";
	}
	bool ViewBase() override { return canAdjust; }
	bool CameraPreset(int) override { return canAdjust; }
	bool SaveCameraDefault() override { return canAdjust; }
	void Communicator() override { cancelTactics(); }
	void SetLanguage(int l) override { language = l == 1 ? 1 : 0; }
	std::string reason(int action)
	{
		if (!canAdjust) return tr("Im Dialog oder bei gesperrter Kamera nicht verfügbar");
		if (selection == 0) return tr("Zuerst eigene bewegliche Einheiten auswählen");
		if (action == 40 && selection < 2 && !formation) return tr("Formation benötigt mindestens zwei Einheiten");
		return std::string();
	}
	std::string TacticalReason(int action) override { return reason(action); }
	bool FormationActive() override { return formation; }
	bool BookmarkKnown(int s) override { return s >= 0 && s < 4 && bookmarks[s]; }
	int GroupSize(int g) override { return g >= 0 && g < 10 ? sizes[g] : 0; }
	std::string TacticalStatus() override
	{
		static const char *names[] = {"Kontextbefehl", "Einheit wählen", "Auswahl +/-", "Bereich: zwei Ecken", "Bereich hinzufügen", "Bewegen",
			"Angriffsmarsch", "Erzwungener Angriff", "Position bewachen", "Zwangsbewegung", "Ohne Verfolgung"};
		char text[320];
		std::snprintf(text, sizeof(text), tr("%s%s · %d gewählt · Gruppe %d%s").c_str(), tr(names[mode]).c_str(), "", selection, group + 1,
			tr(queue ? " · Wegpunkte AN" : "").c_str());
		return text;
	}
	std::string TacticalHint() override
	{
		if (!notice.empty()) return tr(notice.c_str());
		if (queue) return tr(selection > 0 ? "Wegpunkte aktiv: Ziele nacheinander anklicken; erneut Wegpunkte tippen beendet" :
			"Zuerst eine eigene Einheit auswählen");
		switch (mode) {
		case 5: return selection > 0 ? tr("Bewegen: Ziel auf dem Tisch anklicken") : tr("Zuerst eine eigene Einheit auswählen");
		case 9: return tr("Zwangsbewegung: Bodenposition anklicken");
		case 10: return tr("Ohne Verfolgung: Bodenposition anklicken");
		default: return std::string();
		}
	}
	std::string LanguageStatus() override { return std::string(); }
	int MatchResult() override { return 0; }
	int DefaultCameraPreset() override { return 1; }

private:
	void cancelTactics() { mode = 0; queue = false; }
};

// ---------------------------------------------------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

struct Store {
	std::mutex snapMutex;
	GXPanelSnapshot snap;         // latest published snapshot (guarded by snapMutex)
	bool snapValid = false;

	std::mutex sessionMutex;      // guards session, scripted, scriptedScenario (held while an action runs)
	GXPanelSession session;
	ScriptedEngine scripted;
	std::atomic<int> scriptedScenario{0};

	std::atomic<bool> realSeen{false};
	std::atomic<bool> installed{false};
	std::atomic<int> language{0};
	std::atomic<bool> presentationSet{false};
	std::atomic<bool> presentationSplit{false}, presentationStereo{false};
	std::atomic<float> probeX{-1}, probeY{-1};
	std::atomic<bool> dirty{true};
	std::atomic<int> groundView{0};
	std::mutex resultMutex;
	std::deque<GXPanelActionResult> results; // effects for the app (bounded)

	// engine thread only
	Clock::time_point lastCapture{};
	Clock::time_point lastPanelHoverAt{};
	std::string lastWorldHover;
	uint32_t worldHoverSeq = 0;
	std::string lastPanelHover;
	int appliedLanguage = -1;

	Store()
	{
		visionPanelSnapshotDefaults(snap);
		std::memset(&session, 0, sizeof(session));
		visionPanelPrefsDefaults(session.prefs, 0);
	}
};

Store &S()
{
	static Store s;
	return s;
}

// Publishes `fresh` when anything but the version differs. Returns true when it changed.
bool publish(Store &st, GXPanelSnapshot &fresh)
{
	std::lock_guard<std::mutex> lock(st.snapMutex);
	fresh.version = st.snap.version;
	if (st.snapValid && std::memcmp(&fresh, &st.snap, sizeof(fresh)) == 0) return false;
	fresh.version = st.snap.version + 1;
	st.snap = fresh;
	st.snapValid = true;
	return true;
}

void copyText(char *dst, size_t capacity, const std::string &text)
{
	if (capacity == 0) return;
	std::strncpy(dst, text.c_str(), capacity - 1);
	dst[capacity - 1] = '\0';
	const size_t n = std::strlen(dst);
	if (n == 0) return;
	size_t lead = n - 1;
	while (lead > 0 && (static_cast<unsigned char>(dst[lead]) & 0xC0) == 0x80) --lead;
	const unsigned char b = static_cast<unsigned char>(dst[lead]);
	const size_t need = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
	if (lead + need > n) dst[lead] = '\0';
}

void captureScripted(Store &st)
{
	std::lock_guard<std::mutex> lock(st.sessionMutex);
	ScriptedEngine &e = st.scripted;
	e.language = st.session.prefs.language;
	VisionCommandContext c;
	c.bridge = &e;
	c.extras = &e;
	c.splitVisible = !e.expanded;
	c.stereoVisible = !e.expanded;
	GXPanelSnapshot fresh;
	std::memset(&fresh, 0, sizeof(fresh));
	visionCaptureSnapshot(c, st.session, fresh);
	fresh.engineKind = GX_ENGINE_KIND_SCRIPTED;
	fresh.groundViewMode = st.groundView.load();
	const std::string hover = e.WorldHoverInfo();
	copyText(fresh.worldHover, sizeof(fresh.worldHover), hover);
	fresh.worldHoverSeq = hover.empty() ? 0 : 1;
	publish(st, fresh);
}

#if defined(GX_PANEL_HAVE_XRGAMEBOOT)
XrGameBootExtras &extras()
{
	static XrGameBootExtras e;
	return e;
}

// Engine thread. Captures the snapshot when it is time (or an action just ran).
void captureReal(Store &st)
{
	const auto now = Clock::now();
	const bool dirty = st.dirty.exchange(false);
	if (!dirty && now - st.lastCapture < std::chrono::milliseconds(66)) return;
	st.lastCapture = now;

	// The engine's own XR strings (status, hint, reasons) follow the UI language (the Quest sets g_xrLanguage from XrLayout).
	const int lang = st.language.load();
	if (lang != st.appliedLanguage) {
		st.appliedLanguage = lang;
		g_xrLanguage = lang == 1 ? XrLanguage::English : XrLanguage::German;
	}
	VisionEngineBridge *bridge = VisionCreateXrGameBootBridge();
	if (!bridge) return;
	VisionCommandContext c;
	c.bridge = bridge;
	c.extras = &extras();
	GXPanelSession session;
	{
		std::lock_guard<std::mutex> lock(st.sessionMutex);
		session = st.session;
	}
	const bool canStereo = bridge->CanStereoWorld();
	c.splitVisible = st.presentationSet.load() ? st.presentationSplit.load() : (bridge->IsInteractiveGame() && canStereo);
	c.stereoVisible = st.presentationSet.load() ? st.presentationStereo.load() : canStereo;
	GXPanelSnapshot fresh;
	std::memset(&fresh, 0, sizeof(fresh));
	visionCaptureSnapshot(c, session, fresh);
	fresh.engineKind = GX_ENGINE_KIND_REAL;
	fresh.groundViewMode = st.groundView.load();

	// Last pinched / hand-pointed target: XrGameBoot_WorldHoverInfo is non-empty only while a spatial pointer is active.
	const std::string hover = bridge->WorldHoverInfo();
	if (!hover.empty() && hover != st.lastWorldHover) {
		st.lastWorldHover = hover;
		++st.worldHoverSeq;
	}
	copyText(fresh.worldHover, sizeof(fresh.worldHover), st.lastWorldHover);
	fresh.worldHoverSeq = st.worldHoverSeq;

	const float px = st.probeX.load(), py = st.probeY.load();
	if (px >= 0 && py >= 0) {
		if (now - st.lastPanelHoverAt >= std::chrono::milliseconds(200)) {
			st.lastPanelHoverAt = now;
			st.lastPanelHover = bridge->HoverInfo(px, py);
		}
	} else st.lastPanelHover.clear();
	copyText(fresh.panelHover, sizeof(fresh.panelHover), st.lastPanelHover);
	publish(st, fresh);
}
#endif

// GXEngineHost frame hook (engine thread).
void frameHook(void *, bool realEngine)
{
	Store &st = S();
	if (st.scriptedScenario != 0) return; // scripted mode owns the snapshot (captured on demand)
#if defined(GX_PANEL_HAVE_XRGAMEBOOT)
	if (realEngine) {
		st.realSeen.store(true);
		captureReal(st);
		return;
	}
#else
	(void)realEngine;
#endif
	if (st.dirty.exchange(false)) {
		GXPanelSnapshot fresh;
		std::memset(&fresh, 0, sizeof(fresh));
		{
			std::lock_guard<std::mutex> lock(st.sessionMutex);
			fresh.session = st.session;
		}
		fresh.engineKind = GX_ENGINE_KIND_NONE;
		publish(st, fresh);
	}
}

// Runs one action against the engine behind `bridge`/`extras` (engine thread, or the caller's thread when scripted).
GXPanelActionResult perform(Store &st, VisionEngineBridge &bridge, VisionCommandExtras &extras, bool splitVisible, bool stereoVisible, int page,
	int id, int extra, int value, bool isExtra)
{
	std::lock_guard<std::mutex> lock(st.sessionMutex);
	VisionCommandContext c;
	c.bridge = &bridge;
	c.extras = &extras;
	c.splitVisible = splitVisible;
	c.stereoVisible = stereoVisible;
	GXPanelActionResult r;
	if (isExtra) r = visionApplyExtraAction(st.session, extra, value, c);
	else if (page == GX_PANEL_COMMANDS) r = visionApplyCommandAction(st.session, c, id);
	else r = visionApplyMenuAction(st.session, page, c, id);
	if (st.session.prefs.language != st.language.load()) st.language.store(st.session.prefs.language);
	r.page = isExtra ? -1 : page;
	r.controlId = isExtra ? extra : id;
	if (r.handled && (r.host != GX_HOST_NONE || r.stereoWorld || r.closeMenu || (isExtra && r.hostValue == 0))) {
		std::lock_guard<std::mutex> rl(st.resultMutex);
		if (st.results.size() >= 32) st.results.pop_front();
		st.results.push_back(r);
	}
	return r;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------------------------------------------------
extern "C" {

void GXEnginePanelState_Install(void)
{
	Store &st = S();
	if (st.installed.exchange(true)) return;
	GXEngineHost_SetFrameHook(&frameHook, nullptr);
}

void GXEnginePanelState_Get(GXPanelSnapshot *out)
{
	if (!out) return;
	Store &st = S();
	if (st.scriptedScenario != 0) captureScripted(st);
	std::lock_guard<std::mutex> lock(st.snapMutex);
	if (!st.snapValid) {
		GXPanelSnapshot blank;
		visionPanelSnapshotDefaults(blank);
		std::lock_guard<std::mutex> sl(st.sessionMutex);
		blank.session = st.session;
		*out = blank;
		return;
	}
	*out = st.snap;
}

void GXEnginePanelState_SetLanguage(int language, bool persistGameText)
{
	Store &st = S();
	language = language == 1 ? 1 : 0;
	{
		std::lock_guard<std::mutex> lock(st.sessionMutex);
		st.session.prefs.language = language;
		st.scripted.language = language;
	}
	st.language.store(language);
	st.dirty.store(true);
#if defined(GX_PANEL_HAVE_XRGAMEBOOT) && defined(__BLOCKS__)
	if (persistGameText && st.scriptedScenario == 0 && st.realSeen.load()) {
		GXEngineHost_Post(^{
			XrGameBoot_SetLanguage(language);
		});
	}
#else
	(void)persistGameText;
#endif
}

void GXEnginePanelState_SetPrefs(const GXPanelPrefs *prefs)
{
	if (!prefs) return;
	Store &st = S();
	{
		std::lock_guard<std::mutex> lock(st.sessionMutex);
		st.session.prefs = *prefs;
	}
	st.language.store(prefs->language == 1 ? 1 : 0);
	st.dirty.store(true);
}

void GXEnginePanelState_GetPrefs(GXPanelPrefs *out)
{
	if (!out) return;
	Store &st = S();
	std::lock_guard<std::mutex> lock(st.sessionMutex);
	*out = st.session.prefs;
}

void GXEnginePanelState_SetPresentation(bool splitVisible, bool stereoVisible)
{
	Store &st = S();
	st.presentationSplit.store(splitVisible);
	st.presentationStereo.store(stereoVisible);
	st.presentationSet.store(true);
}

void GXEnginePanelState_SetHoverProbe(float x, float y)
{
	Store &st = S();
	st.probeX.store(x);
	st.probeY.store(y);
}

void GXEnginePanelState_SetScripted(int scenario)
{
	Store &st = S();
	std::lock_guard<std::mutex> lock(st.sessionMutex);
	st.scriptedScenario.store(scenario < 0 ? 0 : scenario);
	if (scenario > 0) {
		st.scripted.language = st.session.prefs.language;
		st.scripted.load(scenario);
	}
	st.dirty.store(true);
}

void GXEnginePanelState_SetGroundView(int mode) { S().groundView.store(mode < 0 ? 0 : mode > 2 ? 2 : mode); }

bool GXPanelAction_PopResult(GXPanelActionResult *out)
{
	if (!out) return false;
	Store &st = S();
	std::lock_guard<std::mutex> lock(st.resultMutex);
	if (st.results.empty()) return false;
	*out = st.results.front();
	st.results.pop_front();
	return true;
}

bool GXEnginePanelState_IsScripted(void) { return S().scriptedScenario != 0; }

bool GXPanelAction_Perform(int page, int controlId)
{
	Store &st = S();
	if (st.scriptedScenario != 0) {
		ScriptedEngine &e = st.scripted;
		const bool split = !e.expanded;
		perform(st, e, e, split, split, page, controlId, 0, 0, false);
		return true;
	}
#if defined(GX_PANEL_HAVE_XRGAMEBOOT) && defined(__BLOCKS__)
	if (!st.realSeen.load()) return false;
	return GXEngineHost_Post(^{
		Store &s = S();
		VisionEngineBridge *bridge = VisionCreateXrGameBootBridge();
		if (!bridge) return;
		GXPanelSnapshot snap;
		{
			std::lock_guard<std::mutex> lock(s.snapMutex);
			snap = s.snap;
		}
		perform(s, *bridge, extras(), snap.splitVisible, snap.stereoVisible, page, controlId, 0, 0, false);
		s.dirty.store(true);
	});
#else
	(void)page;
	(void)controlId;
	return false;
#endif
}

bool GXPanelAction_PerformExtra(int extra, int value)
{
	Store &st = S();
	if (st.scriptedScenario != 0) {
		ScriptedEngine &e = st.scripted;
		perform(st, e, e, true, true, 0, 0, extra, value, true);
		return true;
	}
#if defined(GX_PANEL_HAVE_XRGAMEBOOT) && defined(__BLOCKS__)
	if (!st.realSeen.load()) return false;
	return GXEngineHost_Post(^{
		Store &s = S();
		VisionEngineBridge *bridge = VisionCreateXrGameBootBridge();
		if (!bridge) return;
		perform(s, *bridge, extras(), true, true, 0, 0, extra, value, true);
		s.dirty.store(true);
	});
#else
	(void)extra;
	(void)value;
	return false;
#endif
}

} // extern "C"
