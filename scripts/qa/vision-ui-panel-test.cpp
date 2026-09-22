// Host test for the visionOS spatial-UI panel model and actions (VisionPanelModel.cpp, VisionCommandActions.cpp).
//
//   scripts/qa/vision-ui-panel-test.sh
//
// Drives the pure model and the ported Quest switch tables against recording fakes of the two engine seams
// (VisionEngineBridge from package E, VisionCommandExtras for the XrGameBoot_* calls the bridge does not carry):
//   * every control id of every page maps to the same engine call as the Quest (XrCommandUI.h applyCommandAction,
//     XrMenuUI.h applyMenuAction), including the cases where the Quest gates the call away;
//   * state derivation (armed / on / pending / disabled), labels in both languages, group count badges;
//   * page switching, group operations, bookmarks, extra (controller gesture) actions, the snapshot capture.
// The expected tables below are transcribed from the Quest sources by hand (the comments name the Quest lines), not
// generated from the code under test.
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "GXEnginePanelState.h"
#include "VisionCommandActions.h"
#include "VisionPanelModel.h"
#include "XrPanelLayout.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                                    \
	do {                                                                    \
		if (cond) ++g_pass;                                                 \
		else {                                                              \
			++g_fail;                                                       \
			std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);   \
			std::printf(__VA_ARGS__);                                       \
			std::printf("\n");                                              \
		}                                                                   \
	} while (0)

// ---------------------------------------------------------------------------------------------------------------------
// Recording fakes
// ---------------------------------------------------------------------------------------------------------------------
struct Fake : VisionEngineBridge, VisionCommandExtras {
	std::vector<std::string> log;
	bool canAdjust = true, expanded = false, canStereo = true, canObserve = true, interactive = true;
	int mode = 0, group = 0;
	bool queue = false;
	std::map<int, std::string> reasons; // by tactical action
	bool formation = false;
	bool bookmarks[4] = {false, false, false, false};
	int sizes[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	std::string status = "Kontextbefehl · 0 gewählt · Gruppe 1", hint;
	bool cameraOk = true;

	void rec(const std::string &s) { log.push_back(s); }
	std::string calls() const
	{
		std::string out;
		for (size_t i = 0; i < log.size(); ++i) out += (i ? ";" : "") + log[i];
		return out;
	}

	// ---- VisionEngineBridge (only what the panels use records; everything else must never be called) ----
	void Pointer(bool, float, float, bool, bool, float) override { rec("Pointer"); }
	void Key(VisionKey, bool) override { rec("Key"); }
	void RoutePointer(int) override { rec("RoutePointer"); }
	bool PickWorld(const XrSurface &, const XrPosef &, XrWorldHit &) override { rec("PickWorld"); return false; }
	void SpatialPointer(bool) override { rec("SpatialPointer"); }
	void SpatialTrigger(bool, bool, bool) override { rec("SpatialTrigger"); }
	void SpatialClick(bool) override { rec("SpatialClick"); }
	void CancelTarget() override { rec("CANCEL"); }
	void TacticalAction(int a) override { rec("TA(" + std::to_string(a) + ")"); }
	bool AdjustCamera(float, float) override { rec("AdjustCamera"); return false; }
	bool NavigateWorld(float, float, float) override { rec("NavigateWorld"); return false; }
	bool CanAdjustWorld() override { return canAdjust; }
	bool CanRotatePlacement() override { return false; }
	bool RotatePlacement(float) override { rec("RotatePlacement"); return false; }
	float PlacementDegrees() override { return 0; }
	bool CanObserveGround() override { return canObserve; }
	bool PickObserverGround(const XrSurface &, const XrPosef &, XrVector3f &, XrVector3f *) override { return false; }
	bool ObserverStep(XrVector3f, XrVector3f, XrVector3f &) override { return false; }
	bool IsInteractiveGame() override { return interactive; }
	bool CanStereoWorld() override { return canStereo; }
	bool ExpandedUI() override { return expanded; }
	bool HasUIAt(float, float) override { return false; }
	int GameWidth() override { return 1280; }
	int GameHeight() override { return 720; }
	void TacticalState(int &m, int &g, bool &q) override { m = mode; g = group; q = queue; }
	std::string HoverInfo(float, float) override { return ""; }
	std::string WorldHoverInfo() override { return ""; }

	// ---- VisionCommandExtras ----
	void TacticalGroup(int g, int op) override { rec("TG(" + std::to_string(g) + "," + std::to_string(op) + ")"); }
	void Bookmark(int slot, bool save) override { rec("BM(" + std::to_string(slot) + "," + std::to_string(save ? 1 : 0) + ")"); }
	bool ViewBase() override { rec("BASE"); return cameraOk; }
	bool CameraPreset(int p) override { rec("CAM(" + std::to_string(p) + ")"); return cameraOk; }
	bool SaveCameraDefault() override { rec("CAMSAVE"); return cameraOk; }
	void Communicator() override { rec("COMM"); }
	void SetLanguage(int l) override { rec("LANG(" + std::to_string(l) + ")"); }
	std::string TacticalReason(int a) override
	{
		auto it = reasons.find(a);
		return it == reasons.end() ? std::string() : it->second;
	}
	bool FormationActive() override { return formation; }
	bool BookmarkKnown(int s) override { return s >= 0 && s < 4 && bookmarks[s]; }
	int GroupSize(int g) override { return g >= 0 && g < 10 ? sizes[g] : 0; }
	std::string TacticalStatus() override { return status; }
	std::string TacticalHint() override { return hint; }
	std::string LanguageStatus() override { return ""; }
	int MatchResult() override { return 0; }
	int DefaultCameraPreset() override { return 1; }
};

static VisionCommandContext ctxOf(Fake &f, bool split = true, bool stereo = true)
{
	VisionCommandContext c;
	c.bridge = &f;
	c.extras = &f;
	c.splitVisible = split;
	c.stereoVisible = stereo;
	return c;
}
static GXPanelSession freshSession(int language = 0)
{
	GXPanelSession s;
	std::memset(&s, 0, sizeof(s));
	visionPanelPrefsDefaults(s.prefs, language);
	return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// Expected mappings, transcribed from the Quest sources
// ---------------------------------------------------------------------------------------------------------------------
struct Expect {
	int id;
	const char *calls; // recorded engine calls in order, ';' separated
	bool stereo = false, close = false;
	int host = GX_HOST_NONE, hostValue = 0;
};

// XrCommandUI.h:40 xrCommandAction(hit) + :41-43 (action<=9 sets stereoWorld) for the compact console. Ids >= 16 are not
// in the action table: 20-29 groups (:37-39), 30-32 group operations (:36), 33 close (:31), 34 help (:32), 35 Communicator
// (:35), 37 tactics (:22), 40-42 tactics (:23-28), 43-47 bookmarks (:25-26).
static const Expect kCommands[] = {
	{1, "TA(5)", true},   {2, "TA(6)", true},    {3, "TA(7)", true},  {4, "TA(8)", true},   {8, "TA(9)", true},
	{0, "TA(0)", true},   {5, "TA(10)"},         {6, "TA(11)"},       {15, "TA(13)"},       {7, "TA(12)"},
	{13, "TA(26)"},       {14, "TA(27)"},        {11, "TA(28)"},      {12, "TA(29)"},       {9, "TA(30)"},
	{10, "TA(31)"},       {35, "COMM"},
	{20, "TG(0,0)"},      {21, "TG(1,0)"},       {22, "TG(2,0)"},     {23, "TG(3,0)"},      {24, "TG(4,0)"},
	{25, "TG(5,0)"},      {26, "TG(6,0)"},       {27, "TG(7,0)"},     {28, "TG(8,0)"},      {29, "TG(9,0)"},
	{30, ""},             {31, ""},              {32, ""},            {33, "", false, false, GX_HOST_HIDE_COMMANDS},
	{34, ""},             {37, ""},
	{40, "TA(40)", true}, {41, "TA(41)", true},  {42, "TA(42)", true}, {43, ""},
	{44, "BM(0,0)"},      {45, "BM(1,0)"},       {46, "BM(2,0)"},     {47, "BM(3,0)"},
};

// XrMenuUI.h:72-83 (page 1, 'Einheiten'): ids 0-9 are TacticalAction(id) after the tabletop gate (stereoWorld), 16 is
// TacticalAction(31), the menu closes for <=8, 12, 13, 16; 14 opens page 2; 15 closes; 17 finishes.
static const Expect kMenuUnits[] = {
	{5, "TA(5)", true, true},   {6, "TA(6)", true, true},   {7, "TA(7)", true, true},   {8, "TA(8)", true, true},
	{9, "TA(9)", true, false},  {0, "TA(0)", true, true},   {1, "TA(1)", true, true},   {2, "TA(2)", true, true},
	{3, "TA(3)", true, true},   {4, "TA(4)", true, true},   {16, "TA(31)", false, true}, {12, "TA(12)", false, true},
	{10, "TA(10)"},             {11, "TA(11)"},             {13, "TA(13)", false, true}, {14, ""},
	{15, "", false, true},      {17, "", false, true, GX_HOST_FINISH_ARRANGEMENT},
};
// XrMenuUI.h:85-93 (page 2, 'Gruppen'): ids < 12 are TacticalAction(id + 20); 12 -> 12; 13 -> 10; 16 -> 13; 14 / 15
// navigate; close for 3..11 and 12.
static const Expect kMenuGroups[] = {
	{0, "TA(20)"},              {1, "TA(21)"},              {2, "TA(22)"},              {3, "TA(23)", false, true},
	{4, "TA(24)", false, true}, {5, "TA(25)", false, true}, {6, "TA(26)", false, true}, {7, "TA(27)", false, true},
	{8, "TA(28)", false, true}, {9, "TA(29)", false, true}, {10, "TA(30)", false, true}, {11, "TA(31)", false, true},
	{12, "TA(12)", false, true}, {13, "TA(10)"},            {14, ""},                   {15, ""},
	{16, "TA(13)"},             {17, "", false, true, GX_HOST_FINISH_ARRANGEMENT},
};

static std::string hostOf(const GXPanelActionResult &r)
{
	return std::to_string(r.host) + ":" + std::to_string(r.hostValue);
}

static void runExpect(const char *name, int page, const Expect *table, size_t n, bool commands)
{
	for (size_t i = 0; i < n; ++i) {
		const Expect &e = table[i];
		Fake f;
		GXPanelSession s = freshSession();
		s.tactics = true; // the foldout ids exist only then
		VisionCommandContext c = ctxOf(f);
		GXPanelActionResult r = commands ? visionApplyCommandAction(s, c, e.id) : visionApplyMenuAction(s, page, c, e.id);
		CHECK(r.handled, "%s id %d not handled", name, e.id);
		CHECK(f.calls() == e.calls, "%s id %d: engine calls '%s', expected '%s'", name, e.id, f.calls().c_str(), e.calls);
		CHECK(r.stereoWorld == e.stereo, "%s id %d: stereoWorld %d, expected %d", name, e.id, r.stereoWorld, e.stereo);
		CHECK(r.closeMenu == e.close, "%s id %d: closeMenu %d, expected %d", name, e.id, r.closeMenu, e.close);
		CHECK(r.host == e.host && r.hostValue == e.hostValue, "%s id %d: host %s, expected %d:%d", name, e.id, hostOf(r).c_str(), e.host,
			e.hostValue);
		CHECK(r.engineCalled == !f.log.empty(), "%s id %d: engineCalled flag %d vs %zu calls", name, e.id, r.engineCalled, f.log.size());
	}
}

// Every hittable id of a page's table must be covered by an expectation (or be a known navigation / host id).
static void coverage(const char *name, int page, bool help, bool tactics, const std::set<int> &covered)
{
	XrPanelControl table[80];
	const int count = page == GX_PANEL_COMMANDS ? xrCommandLayout(help, tactics, table, 80) : xrMenuLayout(page, table, 80);
	for (int i = 0; i < count; ++i) {
		if (!xrPanelRoleHittable(table[i].role)) continue;
		CHECK(covered.count(table[i].id) == 1, "%s: hittable id %d has no expectation", name, table[i].id);
	}
}

static std::set<int> idsOf(const Expect *t, size_t n, std::initializer_list<int> extra)
{
	std::set<int> s(extra);
	for (size_t i = 0; i < n; ++i) s.insert(t[i].id);
	return s;
}

// ---------------------------------------------------------------------------------------------------------------------
static bool validUtf8(const char *s)
{
	const unsigned char *p = reinterpret_cast<const unsigned char *>(s);
	while (*p) {
		int n = *p < 0x80 ? 0 : (*p >> 5) == 6 ? 1 : (*p >> 4) == 14 ? 2 : (*p >> 3) == 30 ? 3 : -1;
		if (n < 0) return false;
		++p;
		for (int i = 0; i < n; ++i, ++p)
			if ((*p & 0xC0) != 0x80) return false;
	}
	return true;
}

static const GXPanelControl *find(const std::vector<GXPanelControl> &v, int n, int id)
{
	for (int i = 0; i < n; ++i)
		if (v[size_t(i)].id == id) return &v[size_t(i)];
	return nullptr;
}

struct Built {
	GXPanelView view;
	std::vector<GXPanelControl> controls;
	int count = 0;
	const GXPanelControl *get(int id) const { return find(controls, count, id); }
};
static Built build(int page, const GXPanelSnapshot &s, int lang)
{
	Built b;
	b.controls.resize(GX_PANEL_MAX_CONTROLS);
	b.count = visionPanelBuild(page, s, lang, b.view, b.controls.data(), GX_PANEL_MAX_CONTROLS);
	return b;
}
static GXPanelSnapshot liveSnapshot()
{
	GXPanelSnapshot s;
	visionPanelSnapshotDefaults(s);
	s.engineKind = GX_ENGINE_KIND_REAL;
	s.interactiveGame = s.canAdjustWorld = s.canStereoWorld = s.canObserveGround = true;
	s.splitVisible = s.stereoVisible = true;
	std::snprintf(s.status, sizeof(s.status), "Kontextbefehl · 3 gewählt · Gruppe 1");
	return s;
}

int main()
{
	// ---------------------------------------------------------------- 1. actions: Commands console
	runExpect("commands", GX_PANEL_COMMANDS, kCommands, sizeof(kCommands) / sizeof(kCommands[0]), true);
	{
		// The tactics foldout exists in the table only when open; compact ids are covered by the same run.
		std::set<int> covered = idsOf(kCommands, sizeof(kCommands) / sizeof(kCommands[0]), {});
		coverage("commands tactics", GX_PANEL_COMMANDS, false, true, covered);
		coverage("commands compact", GX_PANEL_COMMANDS, false, false, covered);
		coverage("commands help", GX_PANEL_COMMANDS, true, false, {33, 34, 36});
	}
	// group operations: pending state, second tap cancels, number applies it and clears (:36-39)
	for (int op = 1; op <= 3; ++op) {
		Fake f;
		GXPanelSession s = freshSession();
		VisionCommandContext c = ctxOf(f);
		visionApplyCommandAction(s, c, 29 + op);
		CHECK(s.groupOperation == op && f.log.empty(), "op %d arms without a call", op);
		visionApplyCommandAction(s, c, 29 + op);
		CHECK(s.groupOperation == 0, "op %d second tap cancels", op);
		visionApplyCommandAction(s, c, 29 + op);
		visionApplyCommandAction(s, c, 24); // group number 5
		CHECK(f.calls() == "TG(4," + std::to_string(op) + ")" && s.groupOperation == 0, "op %d then number: %s", op, f.calls().c_str());
		visionApplyCommandAction(s, c, 30 + (op % 3));
		CHECK(s.groupOperation == ((op % 3) + 1), "switching operation replaces the pending one");
	}
	{ // an order clears a pending group operation (:41), Communicator too (:35)
		Fake f;
		GXPanelSession s = freshSession();
		VisionCommandContext c = ctxOf(f);
		visionApplyCommandAction(s, c, 31);
		visionApplyCommandAction(s, c, 5);
		CHECK(s.groupOperation == 0 && f.calls() == "TA(10)", "STOP clears the pending operation: %s", f.calls().c_str());
		visionApplyCommandAction(s, c, 32);
		visionApplyCommandAction(s, c, 35);
		CHECK(s.groupOperation == 0, "Communicator clears the pending operation");
	}
	{ // bookmarks: recall vs save (:25-26), the armed flag is consumed even by a recall
		Fake f;
		GXPanelSession s = freshSession();
		s.tactics = true;
		VisionCommandContext c = ctxOf(f);
		visionApplyCommandAction(s, c, 43);
		CHECK(s.bookmarkSave, "'Save view' arms");
		visionApplyCommandAction(s, c, 46);
		CHECK(f.calls() == "BM(2,1)" && !s.bookmarkSave, "save into C: %s", f.calls().c_str());
		f.log.clear();
		visionApplyCommandAction(s, c, 43);
		visionApplyCommandAction(s, c, 43);
		CHECK(!s.bookmarkSave, "'Save view' twice disarms");
		visionApplyCommandAction(s, c, 43);
		visionApplyCommandAction(s, c, 5); // any other control consumes the armed flag (:31)
		CHECK(!s.bookmarkSave && f.calls() == "TA(10)", "another control disarms 'Save view': %s", f.calls().c_str());
	}
	{ // tactics foldout toggles and resets transient state (:22)
		Fake f;
		GXPanelSession s = freshSession();
		VisionCommandContext c = ctxOf(f);
		s.groupOperation = 2;
		visionApplyCommandAction(s, c, 37);
		CHECK(s.tactics && s.groupOperation == 0 && !s.bookmarkSave, "foldout opens and clears");
		visionApplyCommandAction(s, c, 37);
		CHECK(!s.tactics, "foldout closes");
		// with the foldout closed the tactics ids do not exist: a stray press is ignored, no engine call
		visionApplyCommandAction(s, c, 40);
		CHECK(f.log.empty(), "40 with a closed foldout is ignored");
	}
	{ // help toggle and pages (:32-33)
		Fake f;
		GXPanelSession s = freshSession();
		VisionCommandContext c = ctxOf(f);
		visionApplyCommandAction(s, c, 34);
		CHECK(s.help, "help on");
		for (int i = 1; i <= 4; ++i) {
			visionApplyCommandAction(s, c, 36);
			CHECK(s.helpPage == i % 4, "help page %d", s.helpPage);
		}
		visionApplyCommandAction(s, c, 5); // not a help control: ignored
		CHECK(f.log.empty(), "orders are dead while the help is shown");
		visionApplyCommandAction(s, c, 34);
		CHECK(!s.help, "help off");
	}
	{ // gating: no adjustable world -> nothing reaches the engine except the pure UI ids (:24, :34)
		Fake f;
		f.canAdjust = false;
		GXPanelSession s = freshSession();
		s.tactics = true;
		VisionCommandContext c = ctxOf(f);
		for (const Expect &e : kCommands) visionApplyCommandAction(s, c, e.id);
		CHECK(f.log.empty(), "no engine call while the world is not adjustable, got '%s'", f.calls().c_str());
	}
	{ // disabled tactics: a non-empty reason swallows the press (:27)
		Fake f;
		f.reasons[40] = "Formation benötigt mindestens zwei Einheiten";
		f.reasons[41] = "Zuerst eigene bewegliche Einheiten auswählen";
		GXPanelSession s = freshSession();
		s.tactics = true;
		s.bookmarkSave = true;
		VisionCommandContext c = ctxOf(f);
		GXPanelActionResult r = visionApplyCommandAction(s, c, 40);
		CHECK(f.log.empty() && !r.engineCalled && !r.stereoWorld, "formation with a reason sends nothing");
		CHECK(s.bookmarkSave, "a swallowed press keeps the armed 'Save view' (Quest returns before :28)");
		visionApplyCommandAction(s, c, 42);
		CHECK(f.calls() == "TA(42)", "42 has no reason and goes through: %s", f.calls().c_str());
	}
	{ // ids that are not controls of the page never reach the engine
		Fake f;
		GXPanelSession s = freshSession();
		VisionCommandContext c = ctxOf(f);
		for (int id : {-1, -10, 16, 17, 18, 19, 36, 38, 39, 48, 100, 101, 1000}) visionApplyCommandAction(s, c, id);
		CHECK(f.log.empty(), "dead ids: '%s'", f.calls().c_str());
	}

	// ---------------------------------------------------------------- 2. actions: Menu console
	runExpect("menu units", GX_PANEL_MENU_UNITS, kMenuUnits, sizeof(kMenuUnits) / sizeof(kMenuUnits[0]), false);
	runExpect("menu groups", GX_PANEL_MENU_GROUPS, kMenuGroups, sizeof(kMenuGroups) / sizeof(kMenuGroups[0]), false);
	coverage("menu units", GX_PANEL_MENU_UNITS, false, false, idsOf(kMenuUnits, sizeof(kMenuUnits) / sizeof(kMenuUnits[0]), {20, 21, 22, 23, 24}));
	coverage("menu groups", GX_PANEL_MENU_GROUPS, false, false, idsOf(kMenuGroups, sizeof(kMenuGroups) / sizeof(kMenuGroups[0]), {20, 21, 22, 23, 24}));
	{ // navigation results (session.menuPage)
		Fake f;
		GXPanelSession s = freshSession();
		VisionCommandContext c = ctxOf(f);
		visionApplyMenuAction(s, GX_PANEL_MENU_UNITS, c, 14);
		CHECK(s.menuPage == GX_PANEL_MENU_GROUPS, "units -> groups");
		visionApplyMenuAction(s, GX_PANEL_MENU_GROUPS, c, 14);
		CHECK(s.menuPage == GX_PANEL_MENU_UNITS, "groups -> units");
		visionApplyMenuAction(s, GX_PANEL_MENU_GROUPS, c, 15);
		CHECK(s.menuPage == GX_PANEL_MENU_WINDOWS, "groups -> windows");
		for (int tab = 0; tab < 4; ++tab) {
			visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 20 + tab);
			CHECK(s.menuPage == tab, "tab %d", tab);
		}
		visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 24);
		CHECK(s.menuPage == GX_PANEL_MENU_HELP && s.menuHelpPage == 0, "controller guide from a tab");
		for (int i = 1; i <= 6; ++i) {
			visionApplyMenuAction(s, GX_PANEL_MENU_HELP, c, 36);
			CHECK(s.menuHelpPage == i % 5, "controller guide page %d", s.menuHelpPage);
		}
		visionApplyMenuAction(s, GX_PANEL_MENU_HELP, c, 34);
		CHECK(s.menuPage == GX_PANEL_MENU_WINDOWS, "guide -> windows");
		GXPanelActionResult r = visionApplyMenuAction(s, GX_PANEL_MENU_HELP, c, 33);
		CHECK(r.closeMenu && r.host == GX_HOST_FINISH_ARRANGEMENT, "guide close");
		CHECK(f.log.empty(), "navigation never reaches the engine");
	}
	{ // page 1 gate: needs the tabletop and an adjustable world (:78-81)
		for (int mode = 0; mode < 2; ++mode) {
			Fake f;
			f.canAdjust = mode != 0;
			GXPanelSession s = freshSession();
			VisionCommandContext c = ctxOf(f, /*split=*/mode == 1 ? false : true);
			for (int id : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) visionApplyMenuAction(s, GX_PANEL_MENU_UNITS, c, id);
			CHECK(f.log.empty(), "targeted tactics gated (canAdjust=%d split=%d): '%s'", f.canAdjust, mode != 1, f.calls().c_str());
		}
	}
	{ // page 3 ('Ansicht'): engine relevant ids
		Fake f;
		GXPanelSession s = freshSession(0);
		VisionCommandContext c = ctxOf(f);
		GXPanelActionResult r = visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 11);
		CHECK(f.calls() == "LANG(1)" && s.prefs.language == 1 && r.handled, "language German -> English: %s", f.calls().c_str());
		f.log.clear();
		visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 11);
		CHECK(f.calls() == "LANG(0)" && s.prefs.language == 0, "language back: %s", f.calls().c_str());
		f.log.clear();
		r = visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 16);
		CHECK(f.calls() == "CANCEL" && r.host == GX_HOST_ARM_GROUND_VIEW && r.closeMenu, "Ground View: %s", f.calls().c_str());
		f.log.clear();
		f.canObserve = false;
		r = visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 16);
		CHECK(f.log.empty() && r.host == GX_HOST_NONE && !r.closeMenu, "Ground View refused when the ground cannot be observed");
		f.canObserve = true;
		VisionCommandContext noStereo = ctxOf(f, true, false);
		r = visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, noStereo, 16);
		CHECK(f.log.empty() && r.host == GX_HOST_NONE, "Ground View refused without the stereo world");
		// toggles (:117-137)
		const bool h0 = s.prefs.healthBars, u0 = s.prefs.unitRings, b0 = s.prefs.boardFrame;
		visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 4);
		visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 5);
		visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 6);
		CHECK(s.prefs.healthBars == !h0 && s.prefs.unitRings == !u0 && s.prefs.boardFrame == !b0, "layout toggles");
		for (int i = 0; i < 3; ++i) visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 10);
		CHECK(s.prefs.resolutionTier == 0, "resolution cycles 0-1-2-0");
		visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 10);
		CHECK(s.prefs.resolutionTier == 1, "resolution tier 1");
		// stereo path cycle (:108-112) starting from multiview=false, atlas=false
		visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 14);
		CHECK(s.prefs.atlasStereo && !s.prefs.multiviewStereo, "stereo: reference -> compact");
		visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 14);
		CHECK(!s.prefs.atlasStereo && s.prefs.multiviewStereo, "stereo: compact -> multiview");
		visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 14);
		CHECK(!s.prefs.atlasStereo && !s.prefs.multiviewStereo, "stereo: multiview -> reference");
		visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 12);
		visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 13);
		visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 15);
		CHECK(s.prefs.volumeShadows && s.prefs.measurement && !s.prefs.elideWorldCopy, "shadow / measurement / extra world toggles");
		r = visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 8);
		CHECK(s.prefs.leftHanded && r.closeMenu, "left-handed closes the menu (:120)");
		r = visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 9);
		CHECK(r.host == GX_HOST_PHOTO_ARRANGEMENT && r.stereoWorld && r.closeMenu, "photo arrangement (:121-128)");
		f.canStereo = false;
		r = visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, 9);
		CHECK(!r.stereoWorld, "photo arrangement asks for the world view only when it can be stereo");
		// reserved status slots 0-3 and out of range ids are dead (:96-97)
		for (int id : {0, 1, 2, 3, 17, 18, 19}) {
			r = visionApplyMenuAction(s, GX_PANEL_MENU_VIEW, c, id);
			CHECK(!r.handled || id == 17, "view id %d", id);
		}
		std::set<int> covered = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20, 21, 22, 23, 24};
		coverage("menu view", GX_PANEL_MENU_VIEW, false, false, covered);
	}
	{ // page 0 ('Fenster'): every action is host side; 12/13 need an adjustable world (:150)
		Fake f;
		GXPanelSession s = freshSession();
		VisionCommandContext c = ctxOf(f);
		XrPanelControl table[80];
		const int n = xrMenuLayout(GX_PANEL_MENU_WINDOWS, table, 80);
		for (int i = 0; i < n; ++i) {
			if (!xrPanelRoleHittable(table[i].role)) continue;
			const int id = table[i].id;
			if (id == 24 || (id >= 20 && id <= 23)) continue;
			GXPanelActionResult r = visionApplyMenuAction(s, GX_PANEL_MENU_WINDOWS, c, id);
			CHECK(r.handled && !r.engineCalled, "windows id %d", id);
			if (id == 18) CHECK(r.host == GX_HOST_RECENTER_WORKSPACE, "windows 18 recenters");
			else if (id == 17) CHECK(r.host == GX_HOST_FINISH_ARRANGEMENT && r.closeMenu, "windows 17 finishes");
			else CHECK(r.host == GX_HOST_WORKSPACE_EDIT && r.hostValue == id, "windows id %d host effect %s", id, hostOf(r).c_str());
		}
		CHECK(f.log.empty(), "the workspace page never calls the engine");
		f.canAdjust = false;
		GXPanelActionResult r = visionApplyMenuAction(s, GX_PANEL_MENU_WINDOWS, c, 12);
		CHECK(r.host == GX_HOST_NONE, "map coverage needs an adjustable world");
	}

	// ---------------------------------------------------------------- 3. extra actions (controller gestures)
	{
		Fake f;
		GXPanelSession s = freshSession();
		VisionCommandContext c = ctxOf(f);
		for (int p = 0; p <= 4; ++p) visionApplyExtraAction(s, GX_EXTRA_CAMERA_PRESET, p, c);
		CHECK(f.calls() == "CAM(0);CAM(1);CAM(2);CAM(3);CAM(4)", "camera presets: %s", f.calls().c_str());
		f.log.clear();
		GXPanelActionResult r = visionApplyExtraAction(s, GX_EXTRA_CAMERA_PRESET, 9, c);
		CHECK(!r.handled && f.log.empty(), "camera preset out of range");
		visionApplyExtraAction(s, GX_EXTRA_SAVE_CAMERA_DEFAULT, 0, c);
		visionApplyExtraAction(s, GX_EXTRA_VIEW_BASE, 0, c);
		visionApplyExtraAction(s, GX_EXTRA_CANCEL_TARGET, 0, c);
		visionApplyExtraAction(s, GX_EXTRA_SET_LANGUAGE, 1, c);
		visionApplyExtraAction(s, GX_EXTRA_TACTICAL_ACTION, 13, c);
		CHECK(f.calls() == "CAMSAVE;BASE;CANCEL;LANG(1);TA(13)", "extras: %s", f.calls().c_str());
		CHECK(s.prefs.language == 1, "language pref");
		f.cameraOk = false;
		r = visionApplyExtraAction(s, GX_EXTRA_CAMERA_PRESET, 1, c);
		CHECK(r.hostValue == 0, "camera refused is reported");
	}

	// ---------------------------------------------------------------- 4. model: state derivation, labels, badges
	{
		GXPanelSnapshot s = liveSnapshot();
		s.session.tactics = true;
		s.mode = 5;
		s.queue = true;
		s.group = 3;
		s.session.groupOperation = 2;
		s.formationActive = true;
		s.bookmarkKnown[1] = true;
		for (int i = 0; i < 10; ++i) s.groupSize[i] = i * 3;
		std::snprintf(s.reason[2], sizeof(s.reason[2]), "Zuerst eigene bewegliche Einheiten auswählen");
		Built b = build(GX_PANEL_COMMANDS, s, 1);
		CHECK(b.count > 40, "commands console has %d controls", b.count);
		CHECK((b.get(1)->state & GX_STATE_ARMED) != 0, "Move armed for mode 5");
		CHECK((b.get(2)->state & GX_STATE_ARMED) == 0, "Attack march not armed");
		CHECK((b.get(8)->state & GX_STATE_ON) != 0 && std::string(b.get(8)->value) == "ON", "waypoints on: %s", b.get(8)->value);
		CHECK((b.get(23)->state & GX_STATE_ON) != 0 && (b.get(22)->state & GX_STATE_ON) == 0, "group 4 is the current one");
		CHECK((b.get(31)->state & GX_STATE_PENDING) != 0 && (b.get(30)->state & GX_STATE_PENDING) == 0, "operation 2 pending");
		CHECK((b.get(40)->state & GX_STATE_ON) != 0 && std::string(b.get(40)->label) == "Release formation", "formation on: '%s'", b.get(40)->label);
		CHECK((b.get(41)->state & GX_STATE_DISABLED) != 0, "force move disabled by its reason");
		CHECK(std::string(b.get(41)->explain) == s.reason[2], "disabled explanation is the engine reason: '%s'", b.get(41)->explain);
		CHECK((b.get(42)->state & GX_STATE_DISABLED) == 0, "no-pursuit enabled");
		CHECK(b.get(24)->badge == 12 && b.get(20)->badge == 0 && b.get(29)->badge == 27, "group count badges %d %d %d", b.get(24)->badge,
			b.get(20)->badge, b.get(29)->badge);
		CHECK(std::string(b.get(45)->value) == "●" && b.get(45)->badge == 1, "bookmark B known");
		CHECK(std::string(b.get(44)->value) == "○" && b.get(44)->badge == 0, "bookmark A unknown");
		CHECK(std::string(b.view.status) == s.status, "status card");
		CHECK(std::string(b.view.hint) == "New / Extend: choose a number now", "hint shows the pending operation: '%s'", b.view.hint);
		CHECK(b.view.sectionCount == 5, "5 sections (orders, instant, groups, tactics, map views), got %d", b.view.sectionCount);
		// mode 9 / 10 arm the two tactics buttons (:167-168)
		s.mode = 9;
		b = build(GX_PANEL_COMMANDS, s, 0);
		CHECK((b.get(41)->state & GX_STATE_ARMED) != 0 && (b.get(42)->state & GX_STATE_ARMED) == 0, "force move armed by mode 9");
		s.mode = 10;
		b = build(GX_PANEL_COMMANDS, s, 0);
		CHECK((b.get(42)->state & GX_STATE_ARMED) != 0, "guard without pursuit armed by mode 10");
		// save view armed
		s.session.groupOperation = 0;
		s.session.bookmarkSave = true;
		b = build(GX_PANEL_COMMANDS, s, 1);
		CHECK((b.get(43)->state & GX_STATE_ON) != 0 && std::string(b.get(43)->value) == "ON", "'Save view' on");
		CHECK(std::string(b.view.hint) == "Save view: now choose A–D", "bookmark hint: '%s'", b.view.hint);
		// engine hint shown when no operation is pending
		s.session.bookmarkSave = false;
		std::snprintf(s.hint, sizeof(s.hint), "Move: click destination on the table");
		b = build(GX_PANEL_COMMANDS, s, 1);
		CHECK(std::string(b.view.hint) == s.hint, "engine hint: '%s'", b.view.hint);
		// guard disabled by the reason of action 8
		std::snprintf(s.reason[0], sizeof(s.reason[0]), "First select your own mobile units");
		b = build(GX_PANEL_COMMANDS, s, 1);
		CHECK((b.get(4)->state & GX_STATE_DISABLED) != 0 && std::string(b.get(4)->explain) == s.reason[0], "guard disabled with its reason");
		// no adjustable world: everything that acts is disabled, the four UI controls are not
		s.canAdjustWorld = false;
		b = build(GX_PANEL_COMMANDS, s, 1);
		int disabled = 0, enabled = 0;
		for (int i = 0; i < b.count; ++i) (b.controls[size_t(i)].state & GX_STATE_DISABLED) ? ++disabled : ++enabled;
		CHECK(disabled > 35, "%d controls disabled without an adjustable world", disabled);
		for (int id : {33, 34, 37}) CHECK((b.get(id)->state & GX_STATE_DISABLED) == 0, "UI control %d stays enabled", id);
		CHECK(std::string(b.get(20)->explain) == "Unavailable in a dialog or while camera is locked", "reason: '%s'", b.get(20)->explain);
		// the compact console has no tactics controls
		s.session.tactics = false;
		b = build(GX_PANEL_COMMANDS, s, 1);
		CHECK(b.get(40) == nullptr && b.get(44) == nullptr && b.view.sectionCount == 3, "compact console: %d sections", b.view.sectionCount);
		CHECK(std::string(b.get(37)->label) == "Tactics +", "foldout label '%s'", b.get(37)->label);
		// help variant
		s.session.help = true;
		s.session.helpPage = 2;
		b = build(GX_PANEL_COMMANDS, s, 1);
		CHECK(std::string(b.view.title) == "Commands · 3/4" && b.get(34) && b.get(36) && b.get(1) == nullptr, "help variant '%s'", b.view.title);
	}
	{ // menu pages
		GXPanelSnapshot s = liveSnapshot();
		s.mode = 7;
		s.queue = true;
		Built u = build(GX_PANEL_MENU_UNITS, s, 0);
		CHECK((u.get(7)->state & GX_STATE_ARMED) != 0 && (u.get(9)->state & GX_STATE_ON) != 0, "units page armed / waypoint states");
		CHECK((u.get(20)->state & GX_STATE_ACTIVE) == 0 && (u.get(21)->state & GX_STATE_ACTIVE) != 0, "units tab active");
		s.splitVisible = false;
		u = build(GX_PANEL_MENU_UNITS, s, 0);
		CHECK((u.get(5)->state & GX_STATE_DISABLED) != 0 && (u.get(10)->state & GX_STATE_DISABLED) == 0, "targeted tactics need the tabletop");
		Built g = build(GX_PANEL_MENU_GROUPS, s, 0);
		CHECK(g.get(2) != nullptr && (g.get(2)->state & GX_STATE_DISABLED) == 0, "groups page enabled");
		s.canAdjustWorld = false;
		g = build(GX_PANEL_MENU_GROUPS, s, 0);
		CHECK((g.get(2)->state & GX_STATE_DISABLED) != 0 && (g.get(14)->state & GX_STATE_DISABLED) == 0, "groups page gated, navigation not");
		s = liveSnapshot();
		s.session.prefs.healthBars = false;
		s.session.prefs.resolutionTier = 2;
		s.session.prefs.language = 1;
		Built v = build(GX_PANEL_MENU_VIEW, s, 1);
		CHECK((v.get(4)->state & GX_STATE_ON) == 0 && std::string(v.get(4)->value) == "OFF" && std::string(v.get(4)->label) == "Health bars", "health bars off: '%s' '%s'",
			v.get(4)->label, v.get(4)->value);
		CHECK((v.get(5)->state & GX_STATE_ON) != 0 && std::string(v.get(5)->value) == "ON", "unit rings on");
		CHECK((v.get(10)->state & GX_STATE_ON) != 0 && std::string(v.get(10)->value) == "Ultra+", "resolution Ultra+: '%s'", v.get(10)->value);
		CHECK(std::string(v.get(11)->value) == "English", "language chip '%s'", v.get(11)->value);
		CHECK((v.get(16)->state & GX_STATE_DISABLED) == 0, "Ground View available");
		s.canObserveGround = false;
		v = build(GX_PANEL_MENU_VIEW, s, 1);
		CHECK((v.get(16)->state & GX_STATE_DISABLED) != 0, "Ground View disabled when the ground cannot be observed");
		Built w = build(GX_PANEL_MENU_WINDOWS, s, 1);
		CHECK(w.get(18) != nullptr && w.get(18)->role == GX_ROLE_IMMEDIATE, "windows page has 'align everything'");
		Built h = build(GX_PANEL_MENU_HELP, s, 1);
		CHECK(std::string(h.view.title) == "Controller guide · 1/5", "help title '%s'", h.view.title);
		Built none = build(99, s, 0);
		CHECK(none.count == 0, "unknown page builds nothing");
	}
	{ // page switching: build reflects the session flags (help / tactics variants are different tables)
		GXPanelSnapshot s = liveSnapshot();
		Built compact = build(GX_PANEL_COMMANDS, s, 0);
		s.session.tactics = true;
		Built tactics = build(GX_PANEL_COMMANDS, s, 0);
		s.session.help = true;
		Built help = build(GX_PANEL_COMMANDS, s, 0);
		CHECK(compact.count < tactics.count && help.count < compact.count, "table variants: %d < %d, help %d", compact.count, tactics.count, help.count);
	}

	// ---------------------------------------------------------------- 5. labels: both languages, every hittable control labelled
	{
		// Words that are the same in both languages or names.
		const std::set<std::string> same = {"Communicator", "Stereo", "Multiview", "Original", "Auto", "English", "Ultra+", "✕", "?", "Ultra", "Team"};
		int labels = 0, translated = 0;
		std::set<std::string> untranslated;
		for (int page : {GX_PANEL_COMMANDS, GX_PANEL_MENU_WINDOWS, GX_PANEL_MENU_UNITS, GX_PANEL_MENU_GROUPS, GX_PANEL_MENU_VIEW, GX_PANEL_MENU_HELP}) {
			for (int variant = 0; variant < (page == GX_PANEL_COMMANDS ? 3 : 1); ++variant) {
				GXPanelSnapshot s = liveSnapshot();
				s.session.tactics = variant >= 1;
				s.session.help = variant == 2;
				s.formationActive = false;
				XrPanelControl table[80];
				const int count = page == GX_PANEL_COMMANDS ? xrCommandLayout(s.session.help, s.session.tactics, table, 80) :
					xrMenuLayout(page, table, 80);
				Built de = build(page, s, 0), en = build(page, s, 1);
				CHECK(de.count == en.count, "page %d: same controls in both languages", page);
				for (int i = 0; i < count; ++i) {
					if (!xrPanelRoleHittable(table[i].role)) continue;
					const GXPanelControl *cd = de.get(table[i].id), *ce = en.get(table[i].id);
					CHECK(cd && ce, "page %d variant %d: hittable id %d has no control", page, variant, table[i].id);
					if (!cd || !ce) continue;
					CHECK(cd->label[0] != '\0' && ce->label[0] != '\0', "page %d id %d has a label", page, table[i].id);
					CHECK(cd->role == table[i].role, "role of %d is the Quest's", table[i].id);
					++labels;
					if (std::string(cd->label) != ce->label || same.count(cd->label) || std::isdigit(static_cast<unsigned char>(cd->label[0])) || std::strlen(cd->label) == 1) ++translated;
					else untranslated.insert(std::to_string(page) + ":" + std::to_string(table[i].id) + ":" + cd->label);
				}
				for (int i = 0; i < de.count; ++i) {
					CHECK(validUtf8(de.controls[size_t(i)].label) && validUtf8(de.controls[size_t(i)].explain) && validUtf8(de.controls[size_t(i)].value),
						"valid UTF-8 (de) id %d", de.controls[size_t(i)].id);
					CHECK(validUtf8(en.controls[size_t(i)].label) && validUtf8(en.controls[size_t(i)].explain), "valid UTF-8 (en) id %d", en.controls[size_t(i)].id);
				}
				CHECK(validUtf8(de.view.title) && validUtf8(en.view.title) && validUtf8(en.view.hint), "valid UTF-8 titles");
				for (int i = 0; i < de.view.sectionCount; ++i)
					CHECK(std::string(en.view.sections[i].title) != "" && validUtf8(en.view.sections[i].title), "section %d english", i);
			}
		}
		for (const auto &u : untranslated) std::printf("  NOTE untranslated label %s\n", u.c_str());
		CHECK(untranslated.empty(), "%zu labels have no English translation", untranslated.size());
		std::printf("labels checked: %d (%d translated or language neutral)\n", labels, translated);
		// specific translations from the Quest table
		CHECK(std::string(visionTr("Bewegen", 1)) == "Move" && std::string(visionTr("Bewegen", 0)) == "Bewegen", "visionTr");
		CHECK(std::string(visionTr("nicht in der Tabelle", 1)) == "nicht in der Tabelle", "unknown strings pass through");
		CHECK(std::string(GXPanelModel_Translate("Schließen", 1)) == "Close", "C translate");
	}

	// ---------------------------------------------------------------- 6. snapshot capture through the seams
	{
		Fake f;
		f.mode = 8;
		f.group = 2;
		f.queue = true;
		f.formation = true;
		f.bookmarks[3] = true;
		f.sizes[5] = 11;
		f.canAdjust = false;
		f.expanded = true;
		f.reasons[8] = "r8";
		f.reasons[42] = "r42";
		f.status = "S";
		f.hint = "H";
		GXPanelSession s = freshSession(1);
		s.help = true;
		VisionCommandContext c = ctxOf(f, false, true);
		GXPanelSnapshot snap;
		visionPanelSnapshotDefaults(snap);
		visionCaptureSnapshot(c, s, snap);
		CHECK(snap.mode == 8 && snap.group == 2 && snap.queue && snap.formationActive && snap.bookmarkKnown[3] && !snap.bookmarkKnown[0] &&
				snap.groupSize[5] == 11 && !snap.canAdjustWorld && snap.expandedUI && snap.interactiveGame && snap.canStereoWorld,
			"snapshot fields");
		CHECK(std::string(snap.reason[0]) == "r8" && snap.reason[1][0] == '\0' && std::string(snap.reason[3]) == "r42", "reasons for actions 8/40/41/42");
		CHECK(std::string(snap.status) == "S" && std::string(snap.hint) == "H" && !snap.splitVisible && snap.stereoVisible, "strings and presentation flags");
		CHECK(snap.session.help && snap.session.prefs.language == 1, "session copied");
		CHECK(f.log.empty(), "capturing a snapshot never issues engine calls");
		// truncation keeps UTF-8 intact
		f.status.assign(400, 'x');
		f.status += "";
		f.hint.assign(315, 'a');
		f.hint += "ä"; // two bytes straddling the 320 limit
		f.hint += "ää";
		visionCaptureSnapshot(c, s, snap);
		CHECK(validUtf8(snap.hint) && validUtf8(snap.status) && std::strlen(snap.status) == 319, "truncation is UTF-8 safe (%zu)", std::strlen(snap.status));
	}
	// ---------------------------------------------------------------- 7. the C entry points refuse bad input
	{
		GXPanelView v;
		GXPanelControl c[4];
		GXPanelSnapshot s;
		visionPanelSnapshotDefaults(s);
		CHECK(GXPanelModel_Build(GX_PANEL_COMMANDS, nullptr, 0, &v, c, 4) == 0, "null snapshot");
		CHECK(GXPanelModel_Build(GX_PANEL_COMMANDS, &s, 0, &v, c, 0) == 0, "no room");
		int n = GXPanelModel_Build(GX_PANEL_COMMANDS, &s, 0, &v, c, 4);
		CHECK(n == 4, "output is bounded by maxControls (%d)", n);
	}

	std::printf("vision-ui-panel-test: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
