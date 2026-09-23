// Host test for GeneralsMD/Code/Main/visionos/VisionEngineBridgeXr.cpp (the forwarding VisionEngineBridge).
//
// The forwarder is linked against RECORDING stand-ins for the XrGameBoot_* functions. Their signatures come from the real
// XrGameBoot.h (scripts/qa/vision-bridge-forward-test.sh feeds this file a copy with the Android/JNI parts removed), so a
// signature drift in the engine host breaks this test at compile/link time. Each bridge method is called with distinctive
// arguments and must reach the XrGameBoot function of the same name with the same arguments and return value.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "VisionEngineBridgeXr.h"
#include "XrGameBoot.h" // neutralised copy (see the .sh)

static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- recorder ---------------------------------------------------------------------------------------------------
static std::vector<std::string> g_log;
static void rec(const char *name) { g_log.push_back(name); }
static void recf(const char *name, double a, double b = 0, double c = 0, double d = 0, double e = 0, double f = 0) {
	char buf[256];
	std::snprintf(buf, sizeof buf, "%s(%g,%g,%g,%g,%g,%g)", name, a, b, c, d, e, f);
	g_log.push_back(buf);
}
static bool g_ret = true; // value the boolean stand-ins return

// ---- XrGameBoot_* stand-ins (exact signatures of XrGameBoot.h) ----------------------------------------------------
void XrGameBoot_Pointer(bool active, float x, float y, bool select, bool secondary, float wheel) { recf("Pointer", active, x, y, select, secondary, wheel); }
void XrGameBoot_Key(XrGameKey key, bool down) { recf("Key", int(key), down); }
void XrGameBoot_RoutePointer(int target) { recf("RoutePointer", target); }
bool XrGameBoot_PickWorld(const XrSurface &board, const XrPosef &aim, XrWorldHit &hit) {
	recf("PickWorld", board.width, aim.position.x, aim.position.y, aim.position.z);
	hit.x = 11; hit.y = 22; hit.distance = 3; hit.room = {4, 5, 6};
	return g_ret;
}
void XrGameBoot_SpatialPointer(bool active) { recf("SpatialPointer", active); }
void XrGameBoot_SpatialTrigger(bool down, bool available, bool additive) { recf("SpatialTrigger", down, available, additive); }
void XrGameBoot_SpatialClick(bool cancel) { recf("SpatialClick", cancel); }
void XrGameBoot_CancelTarget() { rec("CancelTarget"); }
void XrGameBoot_TacticalAction(int action) { recf("TacticalAction", action); }
bool XrGameBoot_AdjustCamera(float yaw, float pitch) { recf("AdjustCamera", yaw, pitch); return g_ret; }
bool XrGameBoot_NavigateWorld(float r, float f, float z) { recf("NavigateWorld", r, f, z); return g_ret; }
bool XrGameBoot_CanAdjustWorld() { rec("CanAdjustWorld"); return g_ret; }
bool XrGameBoot_CanRotatePlacement() { rec("CanRotatePlacement"); return g_ret; }
bool XrGameBoot_RotatePlacement(float radians) { recf("RotatePlacement", radians); return g_ret; }
float XrGameBoot_PlacementDegrees() { rec("PlacementDegrees"); return 42.5f; }
int XrGameBoot_PlacementLegal() { rec("PlacementLegal"); return 1; }
int XrGameBoot_PointerIntent(XrVector3f *targetRoom, float *targetRadiusM) {
	rec("PointerIntent");
	if (targetRoom && targetRadiusM) { *targetRoom = {0.25f, 0.8f, -0.9f}; *targetRadiusM = 0.03f; }
	return 3;
}
bool XrGameBoot_CanObserveGround() { rec("CanObserveGround"); return g_ret; }
bool XrGameBoot_PickObserverGround(const XrSurface &board, const XrPosef &aim, XrVector3f &ground, XrVector3f *roomPoint) {
	recf("PickObserverGround", board.width, aim.position.x, roomPoint != nullptr);
	ground = {7, 8, 9};
	if (roomPoint) *roomPoint = {1, 2, 3};
	return g_ret;
}
bool XrGameBoot_ObserverStep(XrVector3f cur, XrVector3f delta, XrVector3f &next) {
	recf("ObserverStep", cur.x, cur.y, cur.z, delta.x, delta.y, delta.z);
	next = {cur.x + delta.x, cur.y + delta.y, cur.z + delta.z};
	return g_ret;
}
bool XrGameBoot_IsInteractiveGame() { rec("IsInteractiveGame"); return g_ret; }
bool XrGameBoot_CanStereoWorld() { rec("CanStereoWorld"); return g_ret; }
bool XrGameBoot_ExpandedUI() { rec("ExpandedUI"); return g_ret; }
bool XrGameBoot_HasUIAt(float x, float y) { recf("HasUIAt", x, y); return g_ret; }
int XrGameBoot_GameWidth() { rec("GameWidth"); return 1920; }
int XrGameBoot_GameHeight() { rec("GameHeight"); return 1080; }
void XrGameBoot_TacticalState(int &mode, int &group, bool &queue) { rec("TacticalState"); mode = 3; group = 5; queue = true; }
std::string XrGameBoot_HoverInfo(float x, float y) { recf("HoverInfo", x, y); return "hover"; }
std::string XrGameBoot_WorldHoverInfo() { rec("WorldHoverInfo"); return "world"; }

static void expectLast(const char *s) {
	++g_checks;
	if (g_log.empty() || g_log.back() != s) {
		++g_fail;
		std::printf("FAIL expected last call %s, got %s\n", s, g_log.empty() ? "(none)" : g_log.back().c_str());
	}
	g_log.clear();
}
static void expectOnly(const char *s) { CHECK(g_log.size() == 1); expectLast(s); }

int main() {
	VisionEngineBridge *b = VisionCreateXrGameBootBridge();
	CHECK(b != nullptr);
	CHECK(b == VisionCreateXrGameBootBridge()); // singleton

	b->Pointer(true, 10.5f, 20.25f, true, false, -1.0f); expectOnly("Pointer(1,10.5,20.25,1,0,-1)");
	b->Pointer(false, 0, 0, false, true, 2.0f);          expectOnly("Pointer(0,0,0,0,1,2)");
	b->Key(VisionKey::Back, true);                       expectOnly("Key(0,1,0,0,0,0)");
	b->Key(VisionKey::Right, false);                     expectOnly("Key(2,0,0,0,0,0)");
	b->Key(VisionKey::Down, true);                       expectOnly("Key(4,1,0,0,0,0)");
	b->RoutePointer(kVisionRouteWindows);                expectOnly("RoutePointer(2,0,0,0,0,0)");

	XrSurface board;
	board.width = 1.25f;
	XrPosef aim = {{0, 0, 0, 1}, {0.5f, 1.5f, -0.25f}};
	XrWorldHit hit;
	g_ret = true;
	CHECK(b->PickWorld(board, aim, hit));
	CHECK(hit.x == 11 && hit.y == 22 && hit.distance == 3 && hit.room.x == 4 && hit.room.y == 5 && hit.room.z == 6);
	expectOnly("PickWorld(1.25,0.5,1.5,-0.25,0,0)");
	g_ret = false;
	CHECK(!b->PickWorld(board, aim, hit)); g_log.clear();
	g_ret = true;

	b->SpatialPointer(true);                 expectOnly("SpatialPointer(1,0,0,0,0,0)");
	b->SpatialTrigger(true, false, true);    expectOnly("SpatialTrigger(1,0,1,0,0,0)");
	b->SpatialTrigger(false, true, false);   expectOnly("SpatialTrigger(0,1,0,0,0,0)");
	b->SpatialClick(true);                   expectOnly("SpatialClick(1,0,0,0,0,0)");
	b->CancelTarget();                       expectOnly("CancelTarget");
	b->TacticalAction(13);                   expectOnly("TacticalAction(13,0,0,0,0,0)");

	CHECK(b->AdjustCamera(0.5f, -0.25f));    expectOnly("AdjustCamera(0.5,-0.25,0,0,0,0)");
	CHECK(b->NavigateWorld(0.05f, -0.05f, 0)); expectOnly("NavigateWorld(0.05,-0.05,0,0,0,0)");
	CHECK(b->CanAdjustWorld());              expectOnly("CanAdjustWorld");
	g_ret = false;
	CHECK(!b->AdjustCamera(0, 0)); CHECK(!b->NavigateWorld(0, 0, 0)); CHECK(!b->CanAdjustWorld());
	CHECK(!b->CanRotatePlacement()); CHECK(!b->RotatePlacement(0.1f)); CHECK(!b->CanObserveGround());
	CHECK(!b->IsInteractiveGame()); CHECK(!b->CanStereoWorld()); CHECK(!b->ExpandedUI()); CHECK(!b->HasUIAt(1, 2));
	{
		XrVector3f g, n; XrVector3f rp;
		CHECK(!b->PickObserverGround(board, aim, g, &rp)); CHECK(!b->ObserverStep({0, 0, 0}, {1, 0, 0}, n));
	}
	g_ret = true;
	g_log.clear();

	// Placement. With the engine queries compiled out (this test) the optional ones use the interface defaults.
	CHECK(b->CanRotatePlacement());          expectOnly("CanRotatePlacement");
	CHECK(b->PlacementPending());            expectOnly("CanRotatePlacement"); // default: same as CanRotatePlacement
	CHECK(b->RotatePlacement(0.75f));        expectOnly("RotatePlacement(0.75,0,0,0,0,0)");
	CHECK(b->PlacementDegrees() == 42.5f);   expectOnly("PlacementDegrees");
	CHECK(b->PlacementLegal() == 1);         expectOnly("PlacementLegal");
	{
		XrVector3f t = {};
		float r = 0;
		bool has = false;
		CHECK(b->PointerIntent(t, r, has) == 3); expectOnly("PointerIntent");
		CHECK(has && t.x == 0.25f && t.y == 0.8f && t.z == -0.9f && r == 0.03f);
	}
	CHECK(!b->HasArmedCommand());            CHECK(g_log.empty());

	// Ground View.
	CHECK(b->CanObserveGround());            expectOnly("CanObserveGround");
	{
		XrVector3f ground, room, next;
		CHECK(b->PickObserverGround(board, aim, ground, &room));
		CHECK(ground.x == 7 && ground.y == 8 && ground.z == 9 && room.x == 1 && room.y == 2 && room.z == 3);
		expectOnly("PickObserverGround(1.25,0.5,1,0,0,0)");
		CHECK(b->PickObserverGround(board, aim, ground, nullptr));
		expectOnly("PickObserverGround(1.25,0.5,0,0,0,0)");
		CHECK(b->ObserverStep({1, 2, 3}, {0.5f, 0, -1}, next));
		CHECK(next.x == 1.5f && next.y == 2 && next.z == 2);
		expectOnly("ObserverStep(1,2,3,0.5,0,-1)");
	}

	// Read-only state.
	CHECK(b->IsInteractiveGame());           expectOnly("IsInteractiveGame");
	CHECK(b->CanStereoWorld());              expectOnly("CanStereoWorld");
	CHECK(b->ExpandedUI());                  expectOnly("ExpandedUI");
	CHECK(b->HasUIAt(640.5f, 360.25f));      expectOnly("HasUIAt(640.5,360.25,0,0,0,0)");
	CHECK(b->GameWidth() == 1920);           expectOnly("GameWidth");
	CHECK(b->GameHeight() == 1080);          expectOnly("GameHeight");
	{
		int mode = 0, group = 0; bool queue = false;
		b->TacticalState(mode, group, queue);
		CHECK(mode == 3 && group == 5 && queue);
		expectOnly("TacticalState");
	}
	CHECK(b->HoverInfo(3, 4) == "hover");    expectOnly("HoverInfo(3,4,0,0,0,0)");
	CHECK(b->WorldHoverInfo() == "world");   expectOnly("WorldHoverInfo");

	std::printf("%s %d vision bridge forwarding checks (%d failed)\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
