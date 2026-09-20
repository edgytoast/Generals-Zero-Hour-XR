// visionOS interaction layer host tests: scenarios against a recording fake engine bridge.
//
// Build/run: scripts/qa/vision-interaction-test.sh  (macOS clang++; no OpenXR SDK, no device, no game data).
// Every scenario drives VisionInteraction exactly like the render thread does: one update() per frame with the
// XRInteraction events collected since the last frame, and checks (a) the calls the engine bridge received (the
// same XrGameBoot_* surface the Quest host drives) and (b) the VisionInteractionOutput consumed by the renderer.
#include "visionos/VisionInteraction.h"
#include "../../visionos/Input/GXXRInput.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { fprintf(stderr, "vision-interaction check %d failed at line %d: %s\n", checks, __LINE__, #cond); exit(1); } } while (0)
static bool near(float a, float b, float eps = 1e-3f) { return std::isfinite(a) && std::fabs(a - b) <= eps; }
#define NEAR(a, b) CHECK(near((a), (b)))
#define NEAREPS(a, b, e) CHECK(near((a), (b), (e)))
static bool finite3f(XrVector3f v) { return std::isfinite(v.x + v.y + v.z); }
static bool vnear(XrVector3f a, XrVector3f b, float eps = 1e-3f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }
#define VNEAR(a, b) CHECK(vnear((a), (b)))

// ------------------------------------------------------------------ recording fake bridge

enum class C { Pointer, Key, Route, Pick, SpatialPointer, Trigger, Click, Cancel, Tactical, Adjust, Navigate, Rotate,
	ObsPick, ObsStep };
struct Call {
	C c;
	float a = 0, b = 0, d = 0;      // numeric args (see below)
	bool f1 = false, f2 = false, f3 = false;
	XrPosef aim = {{0, 0, 0, 1}, {0, 0, 0}};
	int i = 0;
};

struct FakeBridge : VisionEngineBridge {
	std::vector<Call> calls;
	// configuration
	bool pickOk = true, canAdjust = true, canRotate = false, canObserve = true, interactive = true, stereo = true;
	bool armed = false, expanded = false, hudHasUI = true, obsPickOk = true;
	int legal = -1;
	float placementDeg = 0;
	float wallX = 1e9f;      // observer steps beyond this x are refused
	bool navigateOk = true, adjustOk = true;
	int gameW = 1280, gameH = 720;
	float aspect = 9.0f / 16;
	// state mirrors for balance checks
	int triggerBalance = 0, buttonBalance = 0;
	bool pressed = false;
	XrSurface lastBoard;

	int count(C c) const { int n = 0; for (auto &k : calls) n += k.c == c; return n; }
	std::vector<Call> of(C c) const { std::vector<Call> v; for (auto &k : calls) if (k.c == c) v.push_back(k); return v; }
	void clear() { calls.clear(); }

	void Pointer(bool active, float x, float y, bool select, bool secondary, float wheel) override {
		Call k{C::Pointer};
		k.a = x; k.b = y; k.f1 = active; k.f2 = select; k.f3 = secondary; k.d = wheel;
		calls.push_back(k);
		const bool now = active && select;
		if (now && !pressed) ++buttonBalance;
		if (!now && pressed) --buttonBalance;
		pressed = now;
	}
	void Key(VisionKey key, bool down) override { Call k{C::Key}; k.i = int(key); k.f1 = down; calls.push_back(k); }
	void RoutePointer(int target) override { Call k{C::Route}; k.i = target; calls.push_back(k); }
	bool PickWorld(const XrSurface &board, const XrPosef &aim, XrWorldHit &hit) override {
		Call k{C::Pick}; k.aim = aim; calls.push_back(k);
		lastBoard = board;
		if (!pickOk) return false;
		const XrVector3f dir = xrRotate(aim.orientation, {0, 0, -1});
		XrVector3f local;
		float t;
		if (!visionRayBoardPlane(board, aim.position, dir, local, t)) return false;
		if (std::fabs(local.x) > board.width * 0.5f || std::fabs(local.y) > board.width * aspect * 0.5f) return false;
		hit.room = visionBoardToWorld(board, local);
		hit.x = (local.x / board.width + 0.5f) * float(gameW - 1);
		hit.y = (0.5f - local.y / (board.width * aspect)) * float(gameH - 1);
		hit.distance = t;
		return true;
	}
	void SpatialPointer(bool active) override { Call k{C::SpatialPointer}; k.f1 = active; calls.push_back(k); }
	void SpatialTrigger(bool down, bool available, bool additive) override {
		Call k{C::Trigger}; k.f1 = down; k.f2 = available; k.f3 = additive; calls.push_back(k);
		if (down && available && !trigDown) { trigDown = true; ++triggerBalance; }
		else if ((!down || !available) && trigDown) { trigDown = false; --triggerBalance; }
	}
	bool trigDown = false;
	void SpatialClick(bool cancel) override { Call k{C::Click}; k.f1 = cancel; calls.push_back(k); }
	void CancelTarget() override { calls.push_back(Call{C::Cancel}); }
	void TacticalAction(int action) override { Call k{C::Tactical}; k.i = action; calls.push_back(k); }
	bool AdjustCamera(float yaw, float pitch) override { Call k{C::Adjust}; k.a = yaw; k.b = pitch; calls.push_back(k); return adjustOk; }
	bool NavigateWorld(float r, float f, float z) override { Call k{C::Navigate}; k.a = r; k.b = f; k.d = z; calls.push_back(k); return navigateOk; }
	bool CanAdjustWorld() override { return canAdjust; }
	bool CanRotatePlacement() override { return canRotate; }
	bool RotatePlacement(float radians) override { Call k{C::Rotate}; k.a = radians; calls.push_back(k); return canRotate; }
	float PlacementDegrees() override { return placementDeg; }
	int PlacementLegal() override { return legal; }
	bool HasArmedCommand() override { return armed; }
	bool CanObserveGround() override { return canObserve; }
	bool PickObserverGround(const XrSurface &board, const XrPosef &aim, XrVector3f &ground, XrVector3f *room) override {
		Call k{C::ObsPick}; k.aim = aim; calls.push_back(k);
		if (!obsPickOk) return false;
		const XrVector3f dir = xrRotate(aim.orientation, {0, 0, -1});
		XrVector3f local;
		float t;
		if (!visionRayBoardPlane(board, aim.position, dir, local, t)) return false;
		ground = {local.x * 100.0f, local.y * 100.0f, 5.0f}; // game units: 100 per metre of board, terrain 5 high
		if (room) *room = visionBoardToWorld(board, local);
		return true;
	}
	bool ObserverStep(XrVector3f current, XrVector3f delta, XrVector3f &next) override {
		Call k{C::ObsStep}; k.a = delta.x; k.b = delta.y; calls.push_back(k);
		if (std::fabs(delta.x) > 2 || std::fabs(delta.y) > 2) return false;
		if (current.x + delta.x > wallX) return false;
		next = {current.x + delta.x, current.y + delta.y, 5.0f};
		return true;
	}
	bool IsInteractiveGame() override { return interactive; }
	bool CanStereoWorld() override { return stereo; }
	bool ExpandedUI() override { return expanded; }
	bool HasUIAt(float, float) override { return hudHasUI; }
	int GameWidth() override { return gameW; }
	int GameHeight() override { return gameH; }
	void TacticalState(int &m, int &g, bool &q) override { m = 0; g = 0; q = false; }
	std::string HoverInfo(float, float) override { return "hover"; }
	std::string WorldHoverInfo() override { return "world"; }
};

// ------------------------------------------------------------------ simulation harness

static const XrVector3f kHead = {0, 1.5f, 0};

static XrSurface flatBoard(XrVector3f pos, float width, float yaw = 0) {
	XrSurface s;
	s.width = width;
	s.pose.position = pos;
	s.pose.orientation = xrMul(xrAxisAngle({0, 1, 0}, yaw), xrAxisAngle({1, 0, 0}, -1.57079633f));
	return s;
}

struct Sim {
	FakeBridge bridge;
	VisionConfig cfg;
	VisionInteraction vi;
	VisionHostState host;
	VisionInteractionOutput out;
	std::vector<XRInteractionEvent> queue;
	double t = 100.0;
	uint64_t frameNo = 0;

	explicit Sim(bool place = true) : vi(&bridge, VisionConfig()) {
		host.head = {{0, 0, 0, 1}, kHead};
		host.boardPlaced = place;
		host.board = flatBoard({0, 0.8f, -0.9f}, 1.0f);
		host.boardAspect = 9.0f / 16;
		host.engine.interactiveGame = host.engine.canStereoWorld = host.engine.canAdjustWorld = true;
		host.engine.canObserveGround = true;
		host.time_s = t;
	}
	void syncEngine() {
		host.engine.canAdjustWorld = bridge.canAdjust;
		host.engine.canObserveGround = bridge.canObserve;
		host.engine.expandedUI = bridge.expanded;
		host.engine.placementPending = bridge.canRotate;
		host.engine.armedCommand = bridge.armed;
		host.engine.placementLegal = bridge.legal;
		host.engine.placementDegrees = bridge.placementDeg;
	}
	// One rendered frame: hands the queued events to the layer and applies its proposals like the host does.
	void frame() {
		++frameNo;
		host.frame = frameNo;
		host.time_s = t;
		t += 1.0 / 60.0;
		syncEngine();
		vi.update(host, queue.data(), queue.size(), out);
		queue.clear();
		if (out.boardChanged) { host.board = out.board; host.boardPlaced = true; }
		if (out.worldZoomChanged) host.worldZoom = out.worldZoom;
	}
	void frames(int n) { for (int i = 0; i < n; ++i) frame(); }
	void seconds(double s) { frames(int(s * 60.0 + 0.5)); }

	XRInteractionEvent base(XRInteractionType type, uint32_t id, XRHand hand, XrVector3f hp) {
		XRInteractionEvent e = {};
		e.type = type;
		e.pointer_id = id;
		e.hand = hand;
		e.pointer_kind = XR_POINTER_INDIRECT_PINCH;
		e.timestamp_s = t;
		e.position_world = {hp.x, hp.y, hp.z};
		e.has_hand_pose = true;
		e.hand_pose.position = {hp.x, hp.y, hp.z};
		e.hand_pose.orientation = {0, 0, 0, 1};
		return e;
	}
	void begin(uint32_t id, XRHand hand, XrVector3f gazeTarget, XrVector3f hp) {
		XRInteractionEvent e = base(XR_EVENT_PINCH_BEGIN, id, hand, hp);
		const XrVector3f d = xrSub(gazeTarget, kHead);
		const float l = xrLength(d);
		e.has_ray = true;
		e.ray_world.origin = {kHead.x, kHead.y, kHead.z};
		e.ray_world.direction = {d.x / l, d.y / l, d.z / l};
		queue.push_back(e);
	}
	void beginAtBoard(uint32_t id, XRHand hand, float lx, float ly, XrVector3f hp) {
		begin(id, hand, visionBoardToWorld(host.board, {lx, ly, 0}), hp);
	}
	void move(uint32_t id, XRHand hand, XrVector3f hp, XrQuaternionf q = {0, 0, 0, 1}) {
		XRInteractionEvent e = base(XR_EVENT_PINCH_DRAG, id, hand, hp);
		e.hand_pose.orientation = {q.x, q.y, q.z, q.w};
		queue.push_back(e);
	}
	void end(uint32_t id, XRHand hand, XrVector3f hp) { queue.push_back(base(XR_EVENT_PINCH_END, id, hand, hp)); }
	void cancel(uint32_t id, XRHand hand, XrVector3f hp) { queue.push_back(base(XR_EVENT_PINCH_CANCEL, id, hand, hp)); }
	void command(XRInteractionCommand c, int v = 0) {
		XRInteractionEvent e = {};
		e.type = XR_EVENT_COMMAND;
		e.command = c;
		e.command_value = v;
		queue.push_back(e);
	}
	void raw(XRInteractionType type) { XRInteractionEvent e = {}; e.type = type; queue.push_back(e); }
};

static XrVector3f H0() { return {0.10f, 1.20f, -0.35f}; } // resting pinch point of a hand
static XrVector3f plus(XrVector3f a, float x, float y, float z) { return {a.x + x, a.y + y, a.z + z}; }

// Independent model of the head->hand ray amplification used for expectations.
static XrVector3f planeHit(XrVector3f hand, float planeY) {
	const float s = (planeY - kHead.y) / (hand.y - kHead.y);
	return {kHead.x + (hand.x - kHead.x) * s, planeY, kHead.z + (hand.z - kHead.z) * s};
}

static std::vector<Call> triggers(const FakeBridge &b) { return b.of(C::Trigger); }

// =============================================================== tests

static void testHelpers() {
	const XrSurface b = flatBoard({1, 0.8f, -2}, 1.2f, 0.6f);
	const XrVector3f w = visionBoardToWorld(b, {0.3f, -0.2f, 0.05f});
	VNEAR(visionBoardToLocal(b, w), (XrVector3f{0.3f, -0.2f, 0.05f}));
	// flat board: local +z is up, local +y is away from a viewer facing -Z
	const XrSurface f = flatBoard({0, 0.8f, -0.9f}, 1.0f);
	VNEAR(visionBoardToWorld(f, {0, 0, 1}), (XrVector3f{0, 1.8f, -0.9f}));
	VNEAR(visionBoardToWorld(f, {0, 0.25f, 0}), (XrVector3f{0, 0.8f, -1.15f}));
	VNEAR(visionBoardToWorld(f, {0.25f, 0, 0}), (XrVector3f{0.25f, 0.8f, -0.9f}));
	// ray/plane
	XrVector3f local;
	float t;
	const XrVector3f target = visionBoardToWorld(f, {0.2f, 0.1f, 0});
	const XrVector3f d = xrSub(target, kHead);
	CHECK(visionRayBoardPlane(f, kHead, {d.x, d.y, d.z}, local, t));
	NEAR(local.x, 0.2f);
	NEAR(local.y, 0.1f);
	CHECK(!visionRayBoardPlane(f, kHead, {0, 1, 0}, local, t));             // pointing up
	CHECK(!visionRayBoardPlane(f, {0, 0.3f, 0}, {0, -1, 0}, local, t));     // viewer below the board
	// aim from ray: -Z looks along the direction
	const XrVector3f dir = {0.6f, -0.64f, 0.48f};
	const XrPosef aim = visionAimFromRay({1, 2, 3}, dir);
	VNEAR(xrRotate(aim.orientation, {0, 0, -1}), dir);
	VNEAR(aim.position, (XrVector3f{1, 2, 3}));
	// shell transform: X right, Y up, Z toward the player, half extents
	float m[16], hx, hz;
	visionBoardTransformForShell(f, 0.6f, m, hx, hz);
	NEAR(m[0], 1); NEAR(m[5], 1); NEAR(m[10], 1);              // columns are the world axes for a heading-0 board
	NEAR(m[12], 0); NEAR(m[13], 0.8f); NEAR(m[14], -0.9f);
	NEAR(hx, 0.5f); NEAR(hz, 0.3f);
	// initial placement: floor-origin device, head yaw 0
	VisionConfig cfg;
	VisionHostState host;
	host.head.position = {0, 1.6f, 0};
	XrSurface s = visionInitialBoard(cfg, host, 1.0f);
	VNEAR(s.pose.position, (XrVector3f{0, 0.8f, -0.9f}));
	NEAR(s.width, 1.0f);
	VNEAR(xrRotate(s.pose.orientation, {0, 0, 1}), (XrVector3f{0, 1, 0})); // flat, normal up
	// head turned 90 degrees to face +X: the board is ahead, yaw only, still flat
	host.head.orientation = xrAxisAngle({0, 1, 0}, -1.57079633f);
	s = visionInitialBoard(cfg, host, 1.0f);
	VNEAR(s.pose.position, (XrVector3f{0.9f, 0.8f, 0}));
	VNEAR(xrRotate(s.pose.orientation, {0, 0, 1}), (XrVector3f{0, 1, 0}));
	VNEAR(xrRotate(s.pose.orientation, {0, 1, 0}), (XrVector3f{1, 0, 0})); // far edge points away from the viewer
	// head pitched down 40 degrees: heading ignores pitch
	host.head.orientation = xrMul(xrAxisAngle({0, 1, 0}, 0.3f), xrAxisAngle({1, 0, 0}, -0.7f));
	s = visionInitialBoard(cfg, host, 1.0f);
	NEAR(xrLength(xrSub({s.pose.position.x, 0, s.pose.position.z}, {0, 0, 0})), 0.9f);
	// simulator style head-relative origin
	host.head = {{0, 0, 0, 1}, {0, 0, 0}};
	s = visionInitialBoard(cfg, host, 1.0f);
	VNEAR(s.pose.position, (XrVector3f{0, -0.45f, -1.25f}));
	// ARKit table wins
	host.tableHeightKnown = true;
	host.tableHeight = 0.72f;
	s = visionInitialBoard(cfg, host, 1.0f);
	NEAR(s.pose.position.y, 0.72f);
	NEAR(s.pose.position.z, -0.9f);
	// width clamp
	NEAR(visionInitialBoard(cfg, host, 9.0f).width, cfg.boardMaxWidthM);
	NEAR(visionInitialBoard(cfg, host, 0.01f).width, cfg.boardMinWidthM);
	// comfort clamp: too far, too close, above the eyes
	VisionHostState h2;
	h2.head.position = {0, 1.6f, 0};
	XrSurface far = flatBoard({10, 0.8f, 0}, 1.0f);
	visionClampBoard(cfg, h2, far);
	NEAR(far.pose.position.x, cfg.boardMaxHeadDistanceM);
	XrSurface near_ = flatBoard({0.05f, 0.8f, 0}, 1.0f);
	visionClampBoard(cfg, h2, near_);
	NEAR(near_.pose.position.x, cfg.boardMinHeadDistanceM);
	XrSurface high = flatBoard({0, 2.0f, -1}, 1.0f);
	visionClampBoard(cfg, h2, high);
	NEAR(high.pose.position.y, 1.6f - cfg.boardMinBelowHeadM);
	// fade ramp
	NEAR(visionFadeAlpha(cfg, 10, 10), 1);
	NEAR(visionFadeAlpha(cfg, 10, 10 + cfg.comfortFadeSeconds * 0.5), 0.5f);
	NEAR(visionFadeAlpha(cfg, 10, 11), 0);
	// twist about hand-local Z
	const XrQuaternionf z30 = xrAxisAngle({0, 0, 1}, 0.5235988f);
	NEAR(visionTwistAbout({0, 0, 0, 1}, z30, 2), 0.5235988f);
	NEAR(visionTwistAbout(xrAxisAngle({0, 1, 0}, 0.4f), xrMul(xrAxisAngle({0, 1, 0}, 0.4f), xrAxisAngle({0, 0, 1}, -0.3f)), 2), -0.3f);
	NEAR(visionTwistAbout({0, 0, 0, 1}, xrAxisAngle({1, 0, 0}, 0.5f), 2), 0);
	// box membership uses the shared xrSelectionContains rule
	VisionBoxRect box;
	box.minX = -0.1f; box.maxX = 0.2f; box.minY = 0; box.maxY = 0.3f;
	CHECK(visionBoxContains(box, {0, 0.1f, 0}));
	CHECK(!visionBoxContains(box, {0.3f, 0.1f, 0}));
	CHECK(visionBoxContains(box, {0.2f, 0.3f, 0}));
}

static void testInitialPlacementProposal() {
	Sim s(false);
	s.frame();
	CHECK(s.out.boardChanged);
	VNEAR(s.out.board.pose.position, (XrVector3f{0, 0.8f, -0.9f}));
	CHECK(s.out.grabBar.visible);
	CHECK(s.out.regionCount >= 6);
	// applying the proposal makes the next frame stable
	s.frame();
	CHECK(!s.out.boardChanged);
	// untracked head: no proposal until the fallback frame count
	Sim u(false);
	u.host.headTracked = false;
	u.frame();
	CHECK(!u.out.boardChanged);
}

static void testSelect() {
	Sim s;
	s.beginAtBoard(1, XR_HAND_RIGHT, 0.2f, 0.1f, H0());
	s.frame();
	CHECK(s.out.mode == VisionMode::Select);
	// pointer routed to the world, pick used the gaze ray exactly, token pixel forwarded
	CHECK(s.bridge.count(C::Route) == 1 && s.bridge.of(C::Route)[0].i == kVisionRouteWorld);
	const auto picks = s.bridge.of(C::Pick);
	CHECK(picks.size() >= 1);
	const XrVector3f expectDir = [&] { XrVector3f d = xrSub(visionBoardToWorld(s.host.board, {0.2f, 0.1f, 0}), kHead); float l = xrLength(d); return XrVector3f{d.x / l, d.y / l, d.z / l}; }();
	VNEAR(xrRotate(picks[0].aim.orientation, {0, 0, -1}), expectDir);
	VNEAR(picks[0].aim.position, kHead);
	const auto ptrs = s.bridge.of(C::Pointer);
	CHECK(ptrs.size() >= 1 && ptrs[0].f1 && !ptrs[0].f2);
	NEAREPS(ptrs[0].a, (0.2f / 1.0f + 0.5f) * 1279.0f, 0.5f);
	NEAREPS(ptrs[0].b, (0.5f - 0.1f / 0.5625f) * 719.0f, 0.5f);
	// the press must not happen before release: a tap is a deferred click
	CHECK(s.bridge.count(C::Click) == 0);
	CHECK(s.bridge.triggerBalance == 0);
	s.seconds(0.1);
	s.end(1, XR_HAND_RIGHT, H0());
	s.frame();
	const auto tr = triggers(s.bridge);
	// arm (released sample), press, release; never additive; the last call is a release
	CHECK(tr.size() == 3 && !tr[0].f1 && tr[0].f2 && tr[1].f1 && tr[1].f2 && !tr[2].f1);
	CHECK(!tr.back().f1 && tr.back().f2 && !tr.back().f3);
	int presses = 0;
	for (auto &k : tr) presses += k.f1 && k.f2;
	CHECK(presses == 1);
	CHECK(s.bridge.triggerBalance == 0);
	CHECK(s.out.events & kVisionEventTap);
	CHECK(s.out.mode == VisionMode::Idle);
	// the click is decided by the engine (trigger), not a layer-issued SpatialClick or TacticalAction
	CHECK(s.bridge.count(C::Click) == 0 && s.bridge.count(C::Tactical) == 0);
	// spatial pointer is deactivated after the commit
	CHECK(!s.bridge.of(C::SpatialPointer).back().f1);
	CHECK(s.vi.stats().taps == 1);
	// tap on a pick miss does nothing to the engine trigger
	Sim m;
	m.bridge.pickOk = false;
	m.beginAtBoard(1, XR_HAND_RIGHT, 0.2f, 0.1f, H0());
	m.frame();
	m.end(1, XR_HAND_RIGHT, H0());
	m.frame();
	CHECK(m.bridge.count(C::Trigger) == 0 && m.bridge.count(C::Click) == 0);
	// gaze away from the board and every panel: nothing is sent
	Sim g;
	g.begin(1, XR_HAND_RIGHT, {0, 1.7f, -2}, H0());
	g.frame();
	g.end(1, XR_HAND_RIGHT, H0());
	g.frame();
	CHECK(g.bridge.count(C::Trigger) == 0 && g.bridge.count(C::Pointer) == 0);
	// camera locked (script) or not interactive: swallowed, never a stale click
	Sim l;
	l.bridge.canAdjust = false;
	l.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, 0.0f, H0());
	l.frame();
	l.end(1, XR_HAND_RIGHT, H0());
	l.frame();
	CHECK(l.bridge.count(C::Trigger) == 0 && l.bridge.count(C::Pick) == 0);
}

static void testAdditive() {
	// persistent toggle command
	{
		Sim s;
		s.command(XR_CMD_SET_ADDITIVE, 1);
		s.frame();
		CHECK(s.out.additive && s.vi.additiveToggle());
		s.beginAtBoard(1, XR_HAND_RIGHT, 0.1f, 0.0f, H0());
		s.frame();
		s.end(1, XR_HAND_RIGHT, H0());
		s.frame();
		for (auto &k : triggers(s.bridge)) CHECK(k.f3);
		s.command(XR_CMD_SET_ADDITIVE, 0);
		s.frame();
		CHECK(!s.out.additive);
	}
	// keyboard Shift (simulator) rides on the pinch events
	{
		Sim s;
		s.beginAtBoard(1, XR_HAND_RIGHT, 0.1f, 0.0f, H0());
		s.queue.back().modifiers_valid = true;
		s.queue.back().modifiers = XR_MOD_SHIFT;
		s.frame();
		CHECK(s.out.additive);
		s.end(1, XR_HAND_RIGHT, H0());
		s.queue.back().modifiers_valid = true;
		s.queue.back().modifiers = XR_MOD_SHIFT;
		s.frame();
		const auto tr = triggers(s.bridge);
		CHECK(tr.back().f3);
		s.raw(XR_EVENT_MODIFIERS);
		s.queue.back().modifiers = 0;
		s.frame();
		CHECK(!s.out.additive);
	}
	// second-hand tap toggles additive for the first hand's release
	{
		Sim s;
		s.beginAtBoard(1, XR_HAND_RIGHT, 0.1f, 0.0f, H0());
		s.frames(6);
		const XrVector3f left = {-0.2f, 1.2f, -0.35f};
		s.beginAtBoard(2, XR_HAND_LEFT, -0.3f, 0.0f, left);
		s.frames(3);
		s.end(2, XR_HAND_LEFT, left);
		s.frame();
		CHECK(s.out.events & kVisionEventSecondHandTap);
		CHECK(s.out.additive);
		s.end(1, XR_HAND_RIGHT, H0());
		s.frame();
		const auto tr = triggers(s.bridge);
		CHECK(tr.back().f3 && s.out.events & kVisionEventTap);
		CHECK(!s.out.additive); // latch is per gesture
		// two second-hand taps cancel each other
		Sim d;
		d.beginAtBoard(1, XR_HAND_RIGHT, 0.1f, 0.0f, H0());
		d.frames(4);
		for (int i = 0; i < 2; ++i) {
			d.beginAtBoard(2 + i, XR_HAND_LEFT, -0.3f, 0.0f, left);
			d.frames(2);
			d.end(2 + i, XR_HAND_LEFT, left);
			d.frames(2);
		}
		CHECK(!d.out.additive);
	}
}

static void testContextCommand() {
	// A tap on terrain with units selected is the SAME call sequence as a tap on a unit: the engine's deferred
	// trigger (TouchInput::tap -> evaluateContextCommand) decides. The layer only supplies the ray and pixel.
	Sim s;
	s.beginAtBoard(1, XR_HAND_RIGHT, -0.35f, -0.2f, H0());
	s.frames(3);
	s.end(1, XR_HAND_RIGHT, H0());
	s.frame();
	const auto picks = s.bridge.of(C::Pick);
	CHECK(picks.size() >= 2);
	// the final press/release used the press ray again (release jitter never changes the target)
	VNEAR(xrRotate(picks.back().aim.orientation, {0, 0, -1}), xrRotate(picks.front().aim.orientation, {0, 0, -1}));
	CHECK(s.bridge.triggerBalance == 0 && s.out.events & kVisionEventTap);
	// moving the hand a little (below 2 cm) during the pinch is still a tap at the original target
	Sim j;
	j.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, 0.0f, H0());
	j.frame();
	j.move(1, XR_HAND_RIGHT, plus(H0(), 0.008f, 0.004f, 0));
	j.frame();
	j.end(1, XR_HAND_RIGHT, plus(H0(), 0.008f, 0.004f, 0));
	j.frame();
	CHECK(j.out.events & kVisionEventTap);
	CHECK(j.bridge.count(C::Tactical) == 0);
}

static void testBoxSelect() {
	// non-trivial board: translated and yawed, so any confusion between world and board coordinates would fail
	Sim s;
	s.host.board = flatBoard({0.3f, 0.8f, -1.1f}, 1.2f, 0.5f);
	const float aspect = s.host.boardAspect;
	const float bw = 1.2f;
	(void)aspect;
	const XrVector3f hand0 = H0();
	s.beginAtBoard(1, XR_HAND_RIGHT, -0.20f, -0.05f, hand0);
	s.frames(2);
	CHECK(s.bridge.triggerBalance == 0 && !s.out.box.active); // no press yet: still undecided
	// drag the hand 12 cm along world +X then 6 cm toward -Z over 12 frames
	XrVector3f hand = hand0;
	for (int i = 1; i <= 12; ++i) {
		hand = plus(hand0, 0.01f * i, 0, -0.005f * i);
		s.move(1, XR_HAND_RIGHT, hand);
		s.frame();
	}
	CHECK(s.out.mode == VisionMode::BoxSelect);
	CHECK(s.out.box.active && s.out.box.additive == false);
	CHECK(s.bridge.triggerBalance == 1); // engine trigger pressed exactly once
	// the first press happens at the frozen START ray (before the cursor moved): the box corner is what was gazed at
	int firstDown = -1, idx = 0;
	std::vector<Call> seq;
	for (auto &k : s.bridge.calls) {
		if (k.c == C::Trigger && k.f1 && firstDown < 0) firstDown = idx;
		++idx;
	}
	CHECK(firstDown > 0);
	// find the pick right before that press
	Call pickBefore;
	for (int i = firstDown; i >= 0; --i) if (s.bridge.calls[i].c == C::Pick) { pickBefore = s.bridge.calls[i]; break; }
	const XrVector3f startDir = [&] { XrVector3f d = xrSub(visionBoardToWorld(s.host.board, {-0.20f, -0.05f, 0}), kHead); float l = xrLength(d); return XrVector3f{d.x / l, d.y / l, d.z / l}; }();
	VNEAR(xrRotate(pickBefore.aim.orientation, {0, 0, -1}), startDir);
	// expected cursor: gaze hit + (hand ray hit - hand0 ray hit) on the board plane
	const XrVector3f startWorld = visionBoardToWorld(s.host.board, {-0.20f, -0.05f, 0});
	const XrVector3f cursor = xrAdd(startWorld, xrSub(planeHit(hand, 0.8f), planeHit(hand0, 0.8f)));
	const XrVector3f cl = visionBoardToLocal(s.host.board, cursor);
	const auto &box = s.out.box;
	NEAREPS(box.minX, std::min(-0.20f, cl.x), 2e-3f);
	NEAREPS(box.maxX, std::max(-0.20f, cl.x), 2e-3f);
	NEAREPS(box.minY, std::min(-0.05f, cl.y), 2e-3f);
	NEAREPS(box.maxY, std::max(-0.05f, cl.y), 2e-3f);
	CHECK(box.maxX - box.minX > 0.15f);
	// four world-space corners lie on the board (slightly above) and are the board-space rectangle
	for (int i = 0; i < 4; ++i) {
		const XrVector3f l = visionBoardToLocal(s.host.board, box.corners[i]);
		NEAREPS(l.z, 0.002f, 1e-4f);
		CHECK((near(l.x, box.minX, 1e-3f) || near(l.x, box.maxX, 1e-3f)) && (near(l.y, box.minY, 1e-3f) || near(l.y, box.maxY, 1e-3f)));
	}
	CHECK(visionBoxContains(box, {(box.minX + box.maxX) * 0.5f, (box.minY + box.maxY) * 0.5f, 0}));
	CHECK(!visionBoxContains(box, {box.maxX + 0.05f, box.minY, 0}));
	(void)bw;
	// while dragging the engine pointer follows the cursor pixel
	CHECK(s.bridge.count(C::SpatialPointer) >= 2);
	// release commits: trigger released (Drop), box cleared, event raised, no click/tap event
	s.end(1, XR_HAND_RIGHT, hand);
	s.frame();
	CHECK(s.bridge.triggerBalance == 0 && !s.out.box.active);
	CHECK(s.out.events & kVisionEventBoxCommitted && !(s.out.events & kVisionEventTap));
	CHECK(s.vi.stats().boxes == 1);
	// second frame: spatial pointer released after the engine consumed the drop
	s.frame();
	CHECK(!s.bridge.of(C::SpatialPointer).back().f1);
}

static void testBoxAdditiveAndCancel() {
	Sim s;
	s.command(XR_CMD_SET_ADDITIVE, 1);
	s.frame();
	s.beginAtBoard(1, XR_HAND_RIGHT, -0.2f, 0, H0());
	s.frame();
	for (int i = 1; i <= 5; ++i) { s.move(1, XR_HAND_RIGHT, plus(H0(), 0.02f * i, 0, 0)); s.frame(); }
	CHECK(s.out.box.active && s.out.box.additive);
	bool pressAdd = false;
	for (auto &k : triggers(s.bridge)) if (k.f1 && k.f2) pressAdd = k.f3;
	CHECK(pressAdd);
	// second-hand tap while boxing cancels the rubber band without selecting
	const XrVector3f left = {-0.2f, 1.2f, -0.35f};
	s.beginAtBoard(2, XR_HAND_LEFT, -0.3f, 0, left);
	s.frames(2);
	s.end(2, XR_HAND_LEFT, left);
	s.frame();
	CHECK(s.out.events & kVisionEventBoxCancelled);
	CHECK(!s.out.box.active && s.bridge.triggerBalance == 0);
	const auto tr = triggers(s.bridge);
	CHECK(!tr.back().f2 || !tr.back().f1); // cancel = unavailable/up, never a Drop
	// the remaining hand is swallowed: releasing it must not click
	const int before = s.bridge.count(C::Trigger);
	s.end(1, XR_HAND_RIGHT, plus(H0(), 0.1f, 0, 0));
	s.frame();
	CHECK(s.bridge.count(C::Trigger) == before);
}

static void testBoxClampedToBoard() {
	// dragging far off the map keeps the engine pointer on the board (an off-board pick would cancel the box)
	Sim s;
	s.beginAtBoard(1, XR_HAND_RIGHT, 0.3f, 0.0f, H0());
	s.frame();
	for (int i = 1; i <= 20; ++i) { s.move(1, XR_HAND_RIGHT, plus(H0(), 0.03f * i, 0, 0)); s.frame(); }
	CHECK(s.out.box.active && s.bridge.triggerBalance == 1);
	CHECK(s.out.box.maxX <= 0.5f + 1e-3f);
	// every recorded pick after the press hit the board
	for (auto &k : s.bridge.of(C::Pick)) { (void)k; }
	CHECK(s.bridge.count(C::Cancel) == 0);
	// trigger is never sent unavailable while the cursor is outside
	for (auto &k : triggers(s.bridge)) CHECK(k.f2 || !k.f1);
}

static float sumNavigate(const FakeBridge &b, float &fwd, int &calls) {
	float r = 0;
	fwd = 0;
	calls = 0;
	for (auto &k : b.calls) if (k.c == C::Navigate) { r += k.a; fwd += k.b; ++calls; }
	return r;
}

static void testRimPan() {
	Sim s;
	// begin in the rim band just outside the far edge (y = hy + 0.03)
	const float hy = 0.5625f * 0.5f;
	s.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, hy + 0.03f, H0());
	s.frame();
	CHECK(s.out.mode == VisionMode::CameraPan);
	CHECK(s.bridge.count(C::Trigger) == 0 && s.bridge.count(C::Pick) == 0); // camera only: no engine pointer
	const XrVector3f hand1 = plus(H0(), 0.03f, 0, 0);
	s.move(1, XR_HAND_RIGHT, hand1);
	s.frame();
	// expected board-plane displacement of the cursor
	const XrVector3f d = xrSub(planeHit(hand1, 0.8f), planeHit(H0(), 0.8f));
	const XrVector3f dl = xrSub(visionBoardToLocal(s.host.board, xrAdd(visionBoardToWorld(s.host.board, {0, 0, 0}), d)), {0, 0, 0});
	float fwd;
	int calls;
	const float right = sumNavigate(s.bridge, fwd, calls);
	// content follows the hand: camera moves opposite; right = -2 * board fraction, in chunks of at most 0.05
	NEAREPS(right, -2.0f * dl.x / 1.0f, 2e-3f);
	NEAREPS(fwd, -2.0f * dl.y / 1.0f, 2e-3f);
	CHECK(calls >= 2);
	for (auto &k : s.bridge.of(C::Navigate)) {
		CHECK(std::fabs(k.a) <= 0.05f + 1e-6f && std::fabs(k.b) <= 0.05f + 1e-6f);
		CHECK(std::sqrt(k.a * k.a + k.b * k.b) <= 0.05f + 1e-5f); // the diagonal is capped like xrWorldPan
		CHECK(k.d == 0);
	}
	// dragging toward the far edge (hand away from the player) moves the camera toward the near edge (forward < 0)
	Sim f;
	f.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, hy + 0.03f, H0());
	f.frame();
	f.move(1, XR_HAND_RIGHT, plus(H0(), 0, 0, -0.03f));
	f.frame();
	sumNavigate(f.bridge, fwd, calls);
	CHECK(fwd < -0.05f); // hand toward -Z = board +y = content moves toward the far edge = camera moves toward near = forward<0
	// a refused pan (script lock) drops the residual instead of piling it up
	Sim r;
	r.bridge.navigateOk = false;
	r.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, hy + 0.03f, H0());
	r.frame();
	r.move(1, XR_HAND_RIGHT, plus(H0(), 0.2f, 0, 0));
	r.frame();
	CHECK(r.bridge.count(C::Navigate) == 1);
	r.frame();
	CHECK(r.bridge.count(C::Navigate) == 1); // nothing more without further hand motion
	// no drift when the hand is stationary
	Sim q;
	q.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, hy + 0.03f, H0());
	q.frames(10);
	CHECK(q.bridge.count(C::Navigate) == 0);
	// camera locked: swallowed
	Sim k;
	k.bridge.canAdjust = false;
	k.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, hy + 0.03f, H0());
	k.frame();
	k.move(1, XR_HAND_RIGHT, plus(H0(), 0.1f, 0, 0));
	k.frame();
	CHECK(k.bridge.count(C::Navigate) == 0);
	// the rim is outside the map: releasing it never clicks the engine
	CHECK(s.bridge.count(C::Click) == 0);
	s.end(1, XR_HAND_RIGHT, hand1);
	s.frame();
	CHECK(s.out.mode == VisionMode::Idle && s.bridge.count(C::Trigger) == 0);
}

static void twoHands(Sim &s, XrVector3f l0, XrVector3f r0) {
	s.beginAtBoard(1, XR_HAND_RIGHT, 0.05f, 0.0f, r0);
	s.frame();
	s.beginAtBoard(2, XR_HAND_LEFT, -0.05f, 0.0f, l0);
	s.frames(2);
	s.seconds(0.4); // longer than the second-hand tap window: this is a two-hand manipulation
}

static void testTwoHandCamera() {
	const XrVector3f L = {-0.20f, 1.20f, -0.40f}, R = {0.20f, 1.20f, -0.40f};
	// ---- rotation: hands rotate 30 degrees counter-clockwise (seen from above) about their midpoint
	{
		Sim s;
		twoHands(s, L, R);
		CHECK(s.out.mode == VisionMode::CameraTwoHand);
		CHECK(s.bridge.triggerBalance == 0);
		const float ang = 30.0f * 3.14159265f / 180.0f;
		// CCW seen from above: axis (from L to R) = +X rotates toward -Z (board +y)
		for (int i = 1; i <= 10; ++i) {
			const float a = ang * i / 10;
			const XrVector3f half = {0.20f * std::cos(a), 0, -0.20f * std::sin(a)};
			const XrVector3f mid = {0, 1.20f, -0.40f};
			s.move(2, XR_HAND_LEFT, {mid.x - half.x, mid.y, mid.z - half.z});
			s.move(1, XR_HAND_RIGHT, {mid.x + half.x, mid.y, mid.z + half.z});
			s.frame();
		}
		float yaw = 0;
		for (auto &k : s.bridge.of(C::Adjust)) { yaw += k.a; CHECK(k.b == 0); }
		// dead zone of 3 degrees is subtracted (no snap); content follows the hands: sign from xrWorldToBoard
		NEAREPS(yaw, s.cfg.rotateSign * (ang - s.vi.config().rotateDeadzoneRad), 2e-3f);
		CHECK(std::fabs(s.out.worldZoom - 1.0f) < 1e-4f && !s.out.worldZoomChanged);
		// releasing the first hand ends the gesture and swallows the second
		s.end(2, XR_HAND_LEFT, L);
		s.frame();
		const int adjustCalls = s.bridge.count(C::Adjust);
		s.move(1, XR_HAND_RIGHT, plus(R, 0.1f, 0, 0));
		s.frame();
		CHECK(s.bridge.count(C::Adjust) == adjustCalls); // the surviving hand is swallowed: no camera motion
		s.end(1, XR_HAND_RIGHT, R);
		s.frame();
		CHECK(s.bridge.count(C::Trigger) == 0 && s.out.mode == VisionMode::Idle);
	}
	// ---- small rotation stays inside the dead zone
	{
		Sim s;
		twoHands(s, L, R);
		for (int i = 1; i <= 5; ++i) {
			const float a = 0.008f * i; // 2.3 degrees total, inside the 3 degree dead zone
			s.move(1, XR_HAND_RIGHT, {0.20f * std::cos(a), 1.20f, -0.40f - 0.20f * std::sin(a)});
			s.move(2, XR_HAND_LEFT, {-0.20f * std::cos(a), 1.20f, -0.40f + 0.20f * std::sin(a)});
			s.frame();
		}
		CHECK(s.bridge.count(C::Adjust) == 0);
	}
	// ---- zoom: spread 1.5x (zoom in = smaller coverage multiplier), then clamps
	{
		Sim s;
		twoHands(s, L, R);
		const float ratio = 1.5f;
		for (int i = 1; i <= 8; ++i) {
			const float r = 1.0f + (ratio - 1.0f) * i / 8;
			s.move(2, XR_HAND_LEFT, {-0.20f * r, 1.20f, -0.40f});
			s.move(1, XR_HAND_RIGHT, {0.20f * r, 1.20f, -0.40f});
			s.frame();
		}
		const float expect = 1.0f / std::exp(std::log(ratio) - s.vi.config().zoomDeadzone);
		NEAREPS(s.out.worldZoom, expect, 2e-3f);
		CHECK(s.host.worldZoom == s.out.worldZoom);
		// spread far beyond: min clamp; pinch together: max clamp
		for (int i = 0; i < 30; ++i) {
			s.move(2, XR_HAND_LEFT, {-1.5f, 1.20f, -0.40f});
			s.move(1, XR_HAND_RIGHT, {1.5f, 1.20f, -0.40f});
			s.frame();
		}
		NEAR(s.out.worldZoom, s.vi.config().zoomMin);
		for (int i = 0; i < 30; ++i) {
			s.move(2, XR_HAND_LEFT, {-0.03f, 1.20f, -0.40f});
			s.move(1, XR_HAND_RIGHT, {0.03f, 1.20f, -0.40f});
			s.frame();
		}
		NEAR(s.out.worldZoom, s.vi.config().zoomMax);
		CHECK(s.bridge.count(C::Adjust) == 0); // pure scale never rotates the camera
	}
	// ---- midpoint translation pans, content follows
	{
		Sim s;
		twoHands(s, L, R);
		for (int i = 1; i <= 6; ++i) {
			s.move(2, XR_HAND_LEFT, plus(L, 0.01f * i, 0, 0));
			s.move(1, XR_HAND_RIGHT, plus(R, 0.01f * i, 0, 0));
			s.frame();
		}
		float fwd;
		int calls;
		const float right = sumNavigate(s.bridge, fwd, calls);
		const XrVector3f mid0 = {0, 1.20f, -0.40f}, mid1 = {0.06f, 1.20f, -0.40f};
		const float dx = planeHit(mid1, 0.8f).x - planeHit(mid0, 0.8f).x;
		NEAREPS(right, -2.0f * dx, 3e-3f);
		NEAREPS(fwd, 0.0f, 3e-3f);
		CHECK(s.bridge.count(C::Adjust) == 0);
		CHECK(std::fabs(s.out.worldZoom - 1) < 1e-4f);
	}
	// ---- a quick second-hand tap is NOT a two-hand gesture (already covered by additive); locked camera swallows
	{
		Sim s;
		s.bridge.canAdjust = true;
		twoHands(s, L, R);
		s.bridge.canAdjust = false;
		s.move(2, XR_HAND_LEFT, plus(L, 0.1f, 0, 0));
		s.frame();
		const int n = s.bridge.count(C::Navigate);
		s.move(2, XR_HAND_LEFT, plus(L, 0.2f, 0, 0));
		s.frame();
		CHECK(s.bridge.count(C::Navigate) == n);
	}
}

static void testWorkspace() {
	const float hy = 0.5625f * 0.5f;
	const float barY = -(hy + 0.11f);
	// ---- one hand on the grab bar moves the board (translation only, gain 1.5)
	{
		Sim s;
		const XrSurface before = s.host.board;
		s.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, barY, H0());
		s.frame();
		CHECK(s.out.mode == VisionMode::WorkspaceMove && s.out.grabBar.active);
		CHECK(s.bridge.count(C::Trigger) == 0 && s.bridge.count(C::Pick) == 0); // never reaches the game
		s.move(1, XR_HAND_RIGHT, plus(H0(), 0.10f, 0, 0));
		s.frame();
		CHECK(s.out.boardChanged && s.out.events & kVisionEventBoardMoved);
		s.frame(); // hand at rest: the proposal is stable and not re-raised
		CHECK(!s.out.boardChanged);
		const XrVector3f moved = xrSub(s.out.board.pose.position, before.pose.position);
		VNEAR(moved, (XrVector3f{0.15f, 0, 0}));
		// orientation and size unchanged, still level
		NEAR(s.out.board.width, 1.0f);
		VNEAR(xrRotate(s.out.board.pose.orientation, {0, 0, 1}), (XrVector3f{0, 1, 0}));
		VNEAR(xrRotate(s.out.board.pose.orientation, {0, 1, 0}), (XrVector3f{0, 0, -1}));
		// wrist rotation of the pinch pose does NOT tilt the board
		s.move(1, XR_HAND_RIGHT, plus(H0(), 0.10f, 0, 0), xrAxisAngle({1, 0, 1}, 0.8f));
		s.frame();
		VNEAR(xrRotate(s.out.board.pose.orientation, {0, 0, 1}), (XrVector3f{0, 1, 0}));
		s.end(1, XR_HAND_RIGHT, H0());
		s.frame();
		CHECK(s.out.mode == VisionMode::Idle && !s.out.grabBar.active);
	}
	// ---- clamps: distance from the head, height below the eyes
	{
		Sim s;
		s.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, barY, H0());
		s.frame();
		for (int i = 1; i <= 10; ++i) { s.move(1, XR_HAND_RIGHT, plus(H0(), 1.0f * i, 0, 0)); s.frame(); }
		const XrVector3f p = s.out.board.pose.position;
		CHECK(std::sqrt(p.x * p.x + p.z * p.z) <= s.vi.config().boardMaxHeadDistanceM + 1e-3f);
		for (int i = 1; i <= 10; ++i) { s.move(1, XR_HAND_RIGHT, plus(H0(), 0, 3.0f * i, 0)); s.frame(); }
		CHECK(s.out.board.pose.position.y <= kHead.y - s.vi.config().boardMinBelowHeadM + 1e-3f);
		for (int i = 1; i <= 10; ++i) { s.move(1, XR_HAND_RIGHT, plus(H0(), 0, -3.0f * i, 0)); s.frame(); }
		CHECK(s.out.board.pose.position.y >= kHead.y - s.vi.config().boardMaxBelowHeadM - 1e-3f);
	}
	// ---- two hands: move + yaw + uniform scale, level, clamped
	{
		Sim s;
		const XrVector3f L = {-0.15f, 1.2f, -0.4f}, R = {0.15f, 1.2f, -0.4f};
		s.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, barY, R);
		s.frame();
		s.beginAtBoard(2, XR_HAND_LEFT, 0.0f, barY, L);
		s.frames(2);
		CHECK(s.out.mode == VisionMode::WorkspaceTwoHand);
		const XrSurface start = s.host.board;
		// spread to 2x and rotate 40 degrees about the vertical axis
		const float ang = 40.0f * 3.14159265f / 180.0f;
		for (int i = 1; i <= 10; ++i) {
			const float f = float(i) / 10, r = 1 + f, a = ang * f;
			const XrVector3f half = {0.15f * r * std::cos(a), 0, -0.15f * r * std::sin(a)};
			s.move(2, XR_HAND_LEFT, {-half.x, 1.2f, -0.4f - half.z});
			s.move(1, XR_HAND_RIGHT, {half.x, 1.2f, -0.4f + half.z});
			s.frame();
		}
		NEAREPS(s.out.board.width, 2.0f, 1e-3f); // scale 2x, exactly the max width
		// level, and yawed by +40 degrees CCW about world up
		VNEAR(xrRotate(s.out.board.pose.orientation, {0, 0, 1}), (XrVector3f{0, 1, 0}));
		const XrVector3f xAxis = xrRotate(s.out.board.pose.orientation, {1, 0, 0});
		NEAREPS(std::atan2(-xAxis.z, xAxis.x), ang, 5e-3f);
		(void)start;
		// spreading beyond the max keeps the width at the clamp; shrinking stops at the min
		for (int i = 0; i < 20; ++i) { s.move(2, XR_HAND_LEFT, {-1.2f, 1.2f, -0.4f}); s.move(1, XR_HAND_RIGHT, {1.2f, 1.2f, -0.4f}); s.frame(); }
		NEAR(s.out.board.width, s.vi.config().boardMaxWidthM);
		for (int i = 0; i < 20; ++i) { s.move(2, XR_HAND_LEFT, {-0.05f, 1.2f, -0.4f}); s.move(1, XR_HAND_RIGHT, {0.05f, 1.2f, -0.4f}); s.frame(); }
		NEAR(s.out.board.width, s.vi.config().boardMinWidthM);
		VNEAR(xrRotate(s.out.board.pose.orientation, {0, 0, 1}), (XrVector3f{0, 1, 0}));
		// releasing one hand: the other continues as a one-hand move, no jump
		const XrSurface before = s.out.board;
		s.end(2, XR_HAND_LEFT, {-0.05f, 1.2f, -0.4f});
		s.frame();
		VNEAR(s.out.board.pose.position, before.pose.position);
		NEAR(s.out.board.width, before.width);
		s.end(1, XR_HAND_RIGHT, {0.05f, 1.2f, -0.4f});
		s.frame();
		CHECK(s.out.mode == VisionMode::Idle);
	}
	// ---- recenter puts the board back in front of the head; reset also restores the size
	{
		Sim s;
		s.host.board = flatBoard({1.2f, 0.7f, 1.5f}, 1.7f, 2.0f);
		s.command(XR_CMD_RECENTER_BOARD);
		s.frame();
		CHECK(s.out.boardChanged && s.out.events & kVisionEventRecentered);
		VNEAR(s.out.board.pose.position, (XrVector3f{0, 0.8f, -0.9f}));
		NEAR(s.out.board.width, 1.7f);
		s.host.board = flatBoard({1.2f, 0.7f, 1.5f}, 1.7f, 2.0f);
		s.host.worldZoom = 2.2f;
		s.command(XR_CMD_RESET_WORKSPACE);
		s.frame();
		NEAR(s.out.board.width, 1.0f);
		NEAR(s.out.worldZoom, 1.0f);
	}
	// ---- gaze tolerance: a bar hit within the pad, and just outside the pad it is the rim / nothing
	{
		Sim s;
		s.beginAtBoard(1, XR_HAND_RIGHT, 0.15f, barY - 0.04f, H0());
		s.frame();
		CHECK(s.out.mode == VisionMode::WorkspaceMove);
		Sim n;
		n.beginAtBoard(1, XR_HAND_RIGHT, 0.45f, barY, H0()); // beside the bar (bar half length is 0.15)
		n.frame();
		CHECK(n.out.mode == VisionMode::Idle);
	}
}

static XrQuaternionf twistQ(float a) { return xrAxisAngle({0, 0, 1}, a); }

static void testPlacement() {
	// ---- drag adjusts, twist rotates, release confirms through SpatialClick (never the deferred trigger)
	{
		Sim s;
		s.bridge.canRotate = true;
		s.bridge.legal = 1;
		s.beginAtBoard(1, XR_HAND_RIGHT, -0.1f, 0.0f, H0());
		s.frame();
		CHECK(s.out.mode == VisionMode::Placement && s.out.placement.active && s.out.placement.ghostFollowing);
		CHECK(s.out.placement.legal == 1 && s.out.placement.hasPoint);
		// the ghost appears at the gaze target immediately (engine pointer moved there)
		auto ptrs = s.bridge.of(C::Pointer);
		CHECK(ptrs.size() >= 1);
		NEAREPS(ptrs[0].a, (-0.1f + 0.5f) * 1279.0f, 0.5f);
		CHECK(s.bridge.of(C::Route)[0].i == kVisionRouteWorld);
		// drag: the ghost follows (pointer pixels change)
		const size_t n0 = ptrs.size();
		for (int i = 1; i <= 6; ++i) { s.move(1, XR_HAND_RIGHT, plus(H0(), 0.02f * i, 0, 0)); s.frame(); }
		ptrs = s.bridge.of(C::Pointer);
		CHECK(ptrs.size() >= n0 + 6);
		CHECK(ptrs.back().a > ptrs[0].a + 20.0f);
		// twist 30 degrees about the hand's local Z: dead zone (8 deg) subtracted, then RotatePlacement in radians
		const float tw = 30.0f * 3.14159265f / 180.0f;
		for (int i = 1; i <= 6; ++i) { s.move(1, XR_HAND_RIGHT, plus(H0(), 0.12f, 0, 0), twistQ(tw * i / 6)); s.frame(); }
		float total = 0;
		for (auto &k : s.bridge.of(C::Rotate)) total += k.a;
		NEAREPS(total, s.vi.config().twistSign * (tw - s.vi.config().twistDeadzoneRad), 2e-3f);
		CHECK(s.out.placement.rotating);
		// twist back: ghost rotates back by the same amount
		for (int i = 6; i >= 0; --i) { s.move(1, XR_HAND_RIGHT, plus(H0(), 0.12f, 0, 0), twistQ(tw * i / 6)); s.frame(); }
		total = 0;
		for (auto &k : s.bridge.of(C::Rotate)) total += k.a;
		NEAREPS(total, 0.0f, 2.5e-2f); // returns to (near) the dead-zone edge
		// release confirms
		s.bridge.clear();
		s.end(1, XR_HAND_RIGHT, plus(H0(), 0.12f, 0, 0));
		s.frame();
		CHECK(s.bridge.count(C::Click) == 1 && !s.bridge.of(C::Click)[0].f1);
		CHECK(s.bridge.count(C::Trigger) == 0);
		// the commit order is: pick -> spatial pointer -> click
		int ip = -1, isp = -1, ic = -1, idx = 0;
		for (auto &k : s.bridge.calls) {
			if (k.c == C::Pick) ip = idx;
			if (k.c == C::SpatialPointer && k.f1) isp = idx;
			if (k.c == C::Click) ic = idx;
			++idx;
		}
		CHECK(ip >= 0 && ip < ic && isp < ic && isp > ip);
		CHECK(s.out.events & kVisionEventPlacementConfirmed);
		s.frame(); // deferred cleanup
		CHECK(!s.bridge.of(C::SpatialPointer).back().f1);
	}
	// ---- twist beyond the dead zone required: small wobble does not rotate
	{
		Sim s;
		s.bridge.canRotate = true;
		s.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, 0.0f, H0());
		s.frame();
		s.move(1, XR_HAND_RIGHT, H0(), twistQ(0.10f));
		s.frame();
		CHECK(s.bridge.count(C::Rotate) == 0);
	}
	// ---- second-hand tap cancels
	{
		Sim s;
		s.bridge.canRotate = true;
		s.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, 0.0f, H0());
		s.frames(5);
		const XrVector3f left = {-0.2f, 1.2f, -0.35f};
		s.beginAtBoard(2, XR_HAND_LEFT, -0.3f, 0, left);
		s.frames(2);
		s.end(2, XR_HAND_LEFT, left);
		s.frame();
		CHECK(s.bridge.count(C::Cancel) == 1 && s.out.events & kVisionEventPlacementCancelled);
		// releasing the first hand then must not confirm
		s.end(1, XR_HAND_RIGHT, H0());
		s.frame();
		CHECK(s.bridge.count(C::Click) == 0);
		CHECK(s.bridge.count(C::Pointer) > 0 && !s.bridge.of(C::Pointer).back().f1); // engine pointer released
	}
	// ---- SwiftUI cancel button
	{
		Sim s;
		s.bridge.canRotate = true;
		s.command(XR_CMD_CANCEL_PLACEMENT);
		s.frame();
		CHECK(s.bridge.count(C::Cancel) == 1 && s.out.events & kVisionEventPlacementCancelled);
	}
	// ---- look away + pinch cancels; a drag that started away does not
	{
		Sim s;
		s.bridge.canRotate = true;
		s.begin(1, XR_HAND_RIGHT, {0, 1.9f, -2.0f}, H0()); // gaze at the sky
		s.frame();
		CHECK(s.out.placement.cancelArmed && s.out.mode == VisionMode::Placement);
		s.end(1, XR_HAND_RIGHT, H0());
		s.frame();
		CHECK(s.bridge.count(C::Cancel) == 1);
		Sim d;
		d.bridge.canRotate = true;
		d.begin(1, XR_HAND_RIGHT, {0, 1.9f, -2.0f}, H0());
		d.frame();
		d.move(1, XR_HAND_RIGHT, plus(H0(), 0.1f, 0, 0));
		d.frame();
		d.end(1, XR_HAND_RIGHT, plus(H0(), 0.1f, 0, 0));
		d.frame();
		CHECK(d.bridge.count(C::Cancel) == 0);
		// with the policy off, looking away is harmless
		Sim o;
		o.bridge.canRotate = true;
		o.vi.config().missCancelsPlacement = false;
		o.begin(1, XR_HAND_RIGHT, {0, 1.9f, -2.0f}, H0());
		o.frame();
		o.end(1, XR_HAND_RIGHT, H0());
		o.frame();
		CHECK(o.bridge.count(C::Cancel) == 0);
	}
	// ---- illegal ground: first confirm is forwarded (engine explains), the second consecutive one cancels
	{
		Sim s;
		s.bridge.canRotate = true;
		s.bridge.legal = 0;
		for (int i = 0; i < 2; ++i) {
			s.beginAtBoard(1 + i, XR_HAND_RIGHT, 0.0f, 0.0f, H0());
			s.frames(2);
			s.end(1 + i, XR_HAND_RIGHT, H0());
			s.frames(2);
			if (i == 0) CHECK(s.bridge.count(C::Cancel) == 0);
		}
		CHECK(s.bridge.count(C::Click) == 2 && s.bridge.count(C::Cancel) == 1);
		CHECK(s.out.placement.legal == 0);
	}
	// ---- pick miss at release keeps the placement pending (dragged off the map)
	{
		Sim s;
		s.bridge.canRotate = true;
		s.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, 0.0f, H0());
		s.frame();
		s.bridge.pickOk = false;
		s.move(1, XR_HAND_RIGHT, plus(H0(), 0.3f, 0, 0));
		s.frame();
		s.end(1, XR_HAND_RIGHT, plus(H0(), 0.3f, 0, 0));
		s.frame();
		CHECK(s.bridge.count(C::Click) == 0 && s.bridge.count(C::Cancel) == 0);
	}
	// ---- armed command (superweapon) uses the same aim flow without rotation
	{
		Sim s;
		s.bridge.armed = true;
		s.beginAtBoard(1, XR_HAND_RIGHT, 0.1f, 0.1f, H0());
		s.frame();
		CHECK(s.out.mode == VisionMode::Placement);
		s.move(1, XR_HAND_RIGHT, plus(H0(), 0, 0, 0.0f), twistQ(1.0f));
		s.frame();
		CHECK(s.bridge.count(C::Rotate) == 0);
		s.end(1, XR_HAND_RIGHT, H0());
		s.frame();
		CHECK(s.bridge.count(C::Click) == 1);
	}
	// ---- button rotation steps
	{
		Sim s;
		s.bridge.canRotate = true;
		s.command(XR_CMD_ROTATE_PLACEMENT_STEP, 15);
		s.frame();
		const auto r = s.bridge.of(C::Rotate);
		CHECK(r.size() == 1);
		NEAREPS(r[0].a, 15.0f * 3.14159265f / 180.0f, 1e-4f);
		// spatial pointer was active around it, then released
		CHECK(s.bridge.of(C::SpatialPointer).front().f1 && !s.bridge.of(C::SpatialPointer).back().f1);
	}
	// ---- the placement ends by itself: an aim gesture whose ghost vanished is swallowed
	{
		Sim s;
		s.bridge.canRotate = true;
		s.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, 0.0f, H0());
		s.frame();
		s.bridge.canRotate = false;
		s.frame();
		s.end(1, XR_HAND_RIGHT, H0());
		s.frame();
		CHECK(s.bridge.count(C::Click) == 0 && s.bridge.count(C::Cancel) == 0);
	}
}

static void testGroundView() {
	// ---- enter: command arms; a tap on visible ground chooses the location and enters
	Sim s;
	const XrSurface boardBefore = s.host.board;
	s.command(XR_CMD_ENTER_GROUND_VIEW);
	s.frame();
	CHECK(s.out.ground.mode == XrObserverMode::Armed && s.out.mode == VisionMode::GroundView);
	CHECK(s.out.ground.fadeAlpha == 0);
	// the arming pinch was never released before: requireRelease clears with a neutral frame
	s.frames(2);
	const XrVector3f hand = H0();
	s.beginAtBoard(1, XR_HAND_RIGHT, 0.2f, 0.1f, hand);
	s.frames(3);
	// during arming the tabletop gestures are off: nothing goes to the game
	CHECK(s.bridge.count(C::Trigger) == 0 && s.bridge.count(C::Pointer) == 0);
	s.end(1, XR_HAND_RIGHT, hand);
	s.frame();
	CHECK(s.out.ground.mode == XrObserverMode::Active);
	CHECK(s.out.events & kVisionEventGroundEntered);
	CHECK(s.bridge.count(C::ObsPick) == 1);
	// PickObserverGround got the tabletop board and the gaze ray
	{
		const auto p = s.bridge.of(C::ObsPick)[0];
		const XrVector3f d = xrSub(visionBoardToWorld(boardBefore, {0.2f, 0.1f, 0}), kHead);
		const float l = xrLength(d);
		VNEAR(xrRotate(p.aim.orientation, {0, 0, -1}), (XrVector3f{d.x / l, d.y / l, d.z / l}));
	}
	// the observer is anchored at the head (player height comes from the head pose), forward = head yaw
	VNEAR(s.out.ground.head, kHead);
	VNEAR(s.out.ground.forward, (XrVector3f{0, 0, -1}));
	NEAR(s.out.ground.ground.x, 20.0f);
	NEAR(s.out.ground.ground.y, 10.0f);
	// comfort fade: 1 at the switch, ramps to 0 in 0.18 s
	NEAR(s.out.ground.fadeAlpha, 1.0f);
	s.frames(6);
	CHECK(s.out.ground.fadeAlpha > 0 && s.out.ground.fadeAlpha < 1);
	s.frames(10);
	NEAR(s.out.ground.fadeAlpha, 0.0f);
	// ---- teleport: gaze at the virtual ground 2 m ahead and 1 m right, tap
	{
		const XrVector3f headNow = kHead;
		const float groundY = headNow.y - kXrObserverEyeHeightMetres;
		const XrVector3f target = {1.0f, groundY, -2.0f};
		s.bridge.clear();
		s.begin(2, XR_HAND_RIGHT, target, hand);
		s.frames(3);
		s.end(2, XR_HAND_RIGHT, hand);
		s.frame();
		CHECK(s.out.events & kVisionEventTeleported);
		const auto steps = s.bridge.of(C::ObsStep);
		CHECK(steps.size() >= 5);
		for (auto &k : steps) CHECK(std::fabs(k.a) <= 2 && std::fabs(k.b) <= 2); // engine step limit respected
		// observer ground moved by ~(1 m right, 2 m forward) = (10, 20) game units (forward maps to +y)
		NEAREPS(s.out.ground.ground.x, 20.0f + 10.0f, 0.5f);
		NEAREPS(s.out.ground.ground.y, 10.0f + 20.0f, 0.5f);
		NEAR(s.out.ground.fadeAlpha, 1.0f); // comfort fade on every teleport
		CHECK(s.vi.stats().teleports == 1);
		// no smooth locomotion: without a pinch nothing moves
		const int stepCount = s.bridge.count(C::ObsStep);
		s.frames(30);
		CHECK(s.bridge.count(C::ObsStep) == stepCount);
	}
	// ---- teleport blocked by a wall stops at the last accepted step
	{
		s.bridge.wallX = s.out.ground.ground.x + 3.0f;
		const float groundY = kHead.y - kXrObserverEyeHeightMetres;
		s.begin(3, XR_HAND_RIGHT, {3.0f, groundY, -2.0f}, hand);
		s.frames(2);
		s.end(3, XR_HAND_RIGHT, hand);
		s.frame();
		CHECK(s.out.ground.ground.x <= s.bridge.wallX + 1e-3f);
		s.bridge.wallX = 1e9f;
	}
	// ---- head height at teleport time is re-derived from the head pose
	{
		s.host.head.position = {0, 1.2f, 0};
		const float groundY = 1.2f - kXrObserverEyeHeightMetres;
		XRInteractionEvent e = {};
		s.begin(4, XR_HAND_RIGHT, {0, groundY, -1.0f}, hand);
		s.queue.back().ray_world.origin = {0, 1.2f, 0};
		{ XrVector3f d = xrSub({0, groundY, -1.0f}, {0, 1.2f, 0}); float l = xrLength(d); s.queue.back().ray_world.direction = {d.x / l, d.y / l, d.z / l}; }
		(void)e;
		s.frames(2);
		s.end(4, XR_HAND_RIGHT, hand);
		s.frame();
		NEAR(s.out.ground.head.y, 1.2f);
		s.host.head.position = kHead;
	}
	// ---- exit by long stationary hold, with progress
	{
		s.begin(5, XR_HAND_RIGHT, {0, 0, -2.0f}, hand);
		s.seconds(0.6);
		CHECK(s.out.ground.holdProgress > 0.3f && s.out.ground.holdProgress < 0.7f);
		s.seconds(0.7);
		CHECK(s.out.ground.mode == XrObserverMode::Off);
		s.end(5, XR_HAND_RIGHT, hand); // swallowed
		s.frame();
		// exit returns to the unchanged tabletop: board pose and size untouched, tabletop gestures live again
		VNEAR(s.host.board.pose.position, boardBefore.pose.position);
		NEAR(s.host.board.width, boardBefore.width);
		s.beginAtBoard(6, XR_HAND_RIGHT, 0.0f, 0.0f, hand);
		s.frame();
		CHECK(s.out.mode == VisionMode::Select);
		s.end(6, XR_HAND_RIGHT, hand);
		s.frame();
	}
	// ---- exit by command, cancel while armed, forced exit when the engine withdraws permission
	{
		Sim e;
		e.command(XR_CMD_ENTER_GROUND_VIEW);
		e.frames(3);
		CHECK(e.out.ground.mode == XrObserverMode::Armed);
		e.command(XR_CMD_EXIT_GROUND_VIEW);
		e.frame();
		CHECK(e.out.ground.mode == XrObserverMode::Off);
		Sim f;
		f.command(XR_CMD_ENTER_GROUND_VIEW);
		f.frames(3);
		f.beginAtBoard(1, XR_HAND_RIGHT, 0, 0, H0());
		f.frames(2);
		f.end(1, XR_HAND_RIGHT, H0());
		f.frame();
		CHECK(f.out.ground.mode == XrObserverMode::Active);
		f.bridge.canObserve = false; // e.g. a native dialog or movie
		f.frame();
		CHECK(f.out.ground.mode == XrObserverMode::Off && f.out.events & kVisionEventGroundExited);
		// not eligible: the command does nothing
		Sim g;
		g.bridge.canObserve = false;
		g.command(XR_CMD_ENTER_GROUND_VIEW);
		g.frame();
		CHECK(g.out.ground.mode == XrObserverMode::Off);
		// invalid ground: stays armed, reports it
		Sim h;
		h.bridge.obsPickOk = false;
		h.command(XR_CMD_ENTER_GROUND_VIEW);
		h.frames(3);
		h.beginAtBoard(1, XR_HAND_RIGHT, 0, 0, H0());
		h.frames(2);
		h.end(1, XR_HAND_RIGHT, H0());
		h.frame();
		CHECK(h.out.ground.mode == XrObserverMode::Armed && h.out.events & kVisionEventGroundInvalid);
		// tracking loss while active leaves Ground View
		Sim t;
		t.command(XR_CMD_ENTER_GROUND_VIEW);
		t.frames(3);
		t.beginAtBoard(1, XR_HAND_RIGHT, 0, 0, H0());
		t.frames(2);
		t.end(1, XR_HAND_RIGHT, H0());
		t.frame();
		CHECK(t.out.ground.mode == XrObserverMode::Active);
		t.host.headTracked = false;
		t.frame();
		CHECK(t.out.ground.mode == XrObserverMode::Off);
	}
}

static void testTrackingLossAndFlush() {
	// ---- box in progress, then tracking lost: engine trigger cancelled, pointer released, targets cancelled
	{
		Sim s;
		s.beginAtBoard(1, XR_HAND_RIGHT, -0.2f, 0, H0());
		s.frame();
		for (int i = 1; i <= 5; ++i) { s.move(1, XR_HAND_RIGHT, plus(H0(), 0.03f * i, 0, 0)); s.frame(); }
		CHECK(s.bridge.triggerBalance == 1 && s.out.box.active);
		s.host.headTracked = false;
		s.frame();
		CHECK(s.bridge.triggerBalance == 0 && !s.out.box.active);
		CHECK(s.out.events & kVisionEventCancelledAll && s.out.events & kVisionEventBoxCancelled);
		CHECK(s.bridge.count(C::Cancel) == 1);          // XrGameBoot_CancelTarget, never a deselect or an order
		CHECK(s.bridge.buttonBalance == 0);
		// the old pinch keeps producing events until the system ends it: none of them starts a gesture
		s.move(1, XR_HAND_RIGHT, plus(H0(), 0.2f, 0, 0));
		s.frame();
		s.host.headTracked = true;
		s.move(1, XR_HAND_RIGHT, plus(H0(), 0.25f, 0, 0));
		s.frame();
		CHECK(s.out.mode == VisionMode::Idle && s.vi.activePointerCount() == 0);
		const int trig = s.bridge.count(C::Trigger);
		s.end(1, XR_HAND_RIGHT, plus(H0(), 0.25f, 0, 0));
		s.frame();
		CHECK(s.bridge.count(C::Trigger) == trig && !(s.out.events & kVisionEventTap));
		// after the stale pinch ended, a fresh pinch works again
		s.beginAtBoard(2, XR_HAND_RIGHT, 0.0f, 0.0f, H0());
		s.frame();
		CHECK(s.out.mode == VisionMode::Select);
		s.end(2, XR_HAND_RIGHT, H0());
		s.frame();
		CHECK(s.out.events & kVisionEventTap);
	}
	// ---- a pinch begun while tracking is lost is ignored until it ends
	{
		Sim s;
		s.host.headTracked = false;
		s.frame();
		s.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, 0.0f, H0());
		s.frame();
		s.host.headTracked = true;
		s.frame();
		s.move(1, XR_HAND_RIGHT, H0());
		s.frame();
		CHECK(s.vi.activePointerCount() == 0 && s.bridge.count(C::Trigger) == 0);
		s.end(1, XR_HAND_RIGHT, H0());
		s.frame();
		CHECK(s.bridge.count(C::Trigger) == 0);
	}
	// ---- focus loss cancels placement and all pointers
	{
		Sim s;
		s.bridge.canRotate = true;
		s.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, 0.0f, H0());
		s.frame();
		s.host.sessionFocused = false;
		s.frame();
		CHECK(s.bridge.count(C::Cancel) == 1 && s.vi.activePointerCount() == 0);
		CHECK(s.bridge.buttonBalance == 0);
		s.host.sessionFocused = true;
		s.end(1, XR_HAND_RIGHT, H0());
		s.frame();
		CHECK(s.bridge.count(C::Click) == 0);
	}
	// ---- explicit tracking-lost event and flush event
	{
		Sim s;
		s.beginAtBoard(1, XR_HAND_RIGHT, -0.2f, 0, H0());
		s.frame();
		for (int i = 1; i <= 4; ++i) { s.move(1, XR_HAND_RIGHT, plus(H0(), 0.03f * i, 0, 0)); s.frame(); }
		s.raw(XR_EVENT_FLUSH);
		s.frame();
		CHECK(s.bridge.triggerBalance == 0 && !s.out.box.active && s.vi.activePointerCount() == 0);
		CHECK(s.bridge.count(C::Cancel) == 0); // a plain flush does not cancel engine targets
		s.raw(XR_EVENT_TRACKING_LOST);
		s.frame();
		CHECK(s.bridge.count(C::Cancel) == 1);
	}
	// ---- a hand that ARKit reports as lost cancels its pinch
	{
		Sim s;
		s.beginAtBoard(1, XR_HAND_RIGHT, -0.2f, 0, H0());
		s.frames(2);
		s.seconds(0.4);
		XRInteractionEvent e = {};
		e.type = XR_EVENT_HAND_UPDATE;
		e.hand = XR_HAND_RIGHT;
		e.hand_tracked = false;
		s.queue.push_back(e);
		s.frame();
		CHECK(s.vi.activePointerCount() == 0);
		s.end(1, XR_HAND_RIGHT, H0());
		s.frame();
		CHECK(s.bridge.triggerBalance == 0);
	}
}

// Deterministic PRNG for the fuzz test.
static uint32_t rng = 12345;
static uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void testBalancedUnderFlush() {
	// Random gesture soup with flushes, tracking losses, commands and placement toggles. After every frame the engine
	// must never hold a trigger or button that the layer cannot release, and after a final flush nothing is held.
	int flushes = 0;
	for (int run = 0; run < 400; ++run) {
		Sim s;
		s.bridge.canRotate = (rnd() % 3) == 0;
		s.bridge.legal = int(rnd() % 3) - 1;
		bool held[2] = {false, false};
		uint32_t ids[2] = {0, 0};
		uint32_t nextId = 1;
		const XRHand hands[2] = {XR_HAND_RIGHT, XR_HAND_LEFT};
		XrVector3f pos[2] = {H0(), {-0.2f, 1.2f, -0.35f}};
		for (int step = 0; step < 90; ++step) {
			const uint32_t r = rnd() % 100;
			const int h = int(rnd() % 2);
			if (r < 14 && !held[h]) {
				ids[h] = nextId++;
				const float lx = (float(rnd() % 100) / 100.0f - 0.5f) * 1.3f, ly = (float(rnd() % 100) / 100.0f - 0.5f) * 0.9f;
				s.beginAtBoard(ids[h], hands[h], lx, ly, pos[h]);
				held[h] = true;
			} else if (r < 45 && held[h]) {
				pos[h] = plus(pos[h], (float(rnd() % 100) / 100.0f - 0.5f) * 0.06f, 0, (float(rnd() % 100) / 100.0f - 0.5f) * 0.06f);
				s.move(ids[h], hands[h], pos[h]);
			} else if (r < 58 && held[h]) {
				if (rnd() % 4 == 0) s.cancel(ids[h], hands[h], pos[h]); else s.end(ids[h], hands[h], pos[h]);
				held[h] = false;
			} else if (r < 62) {
				s.raw(XR_EVENT_FLUSH);
				++flushes;
			} else if (r < 64) {
				s.raw(XR_EVENT_TRACKING_LOST);
			} else if (r < 66) {
				s.host.headTracked = !s.host.headTracked;
			} else if (r < 68) {
				s.host.sessionFocused = !s.host.sessionFocused;
			} else if (r < 71) {
				s.command(XR_CMD_CANCEL_ALL);
			} else if (r < 73) {
				s.command(XR_CMD_SET_ADDITIVE, int(rnd() % 2));
			} else if (r < 75) {
				s.command(rnd() % 2 ? XR_CMD_ENTER_GROUND_VIEW : XR_CMD_EXIT_GROUND_VIEW);
			} else if (r < 77) {
				s.command(XR_CMD_RECENTER_BOARD);
			} else if (r < 79) {
				s.bridge.canRotate = !s.bridge.canRotate;
			} else if (r < 81) {
				XRInteractionEvent e = {};
				e.type = XR_EVENT_MODIFIERS;
				e.modifiers = rnd() % 16;
				s.queue.push_back(e);
			}
			s.frame();
			// invariants
			CHECK(s.bridge.triggerBalance == 0 || s.bridge.triggerBalance == 1);
			CHECK(s.bridge.buttonBalance == 0 || s.bridge.buttonBalance == 1);
			CHECK(std::isfinite(s.out.board.width) && s.out.board.width >= 0.44f && s.out.board.width <= 2.01f);
			CHECK(s.out.worldZoom >= 0.5f - 1e-4f && s.out.worldZoom <= 3.0f + 1e-4f);
			CHECK(finite3f(s.out.board.pose.position));
			// a held trigger requires an active gesture that can still release it
			if (s.bridge.triggerBalance == 1) CHECK(s.vi.activePointerCount() >= 1);
		}
		// final flush + release of every physical pinch the system still reports
		s.host.headTracked = true;
		s.host.sessionFocused = true;
		s.raw(XR_EVENT_FLUSH);
		s.frames(3);
		for (int h = 0; h < 2; ++h) if (held[h]) s.end(ids[h], hands[h], pos[h]);
		s.frames(3);
		CHECK(s.bridge.triggerBalance == 0 && s.bridge.buttonBalance == 0);
		CHECK(s.vi.activePointerCount() == 0);
		CHECK(!s.bridge.trigDown && !s.bridge.pressed);
	}
	CHECK(flushes > 100);
}

static void testPanels() {
	// UI panel (engine build window) 1.8 m wide, 1.2 m ahead, tilted like the Quest build window; crop = control bar band.
	Sim s;
	VisionPanel ui;
	ui.visible = true;
	ui.kind = kVisionPanelGameUI;
	ui.surface.width = 0.9f;
	ui.surface.pose.position = {0, 1.0f, -0.8f};
	ui.surface.pose.orientation = xrAxisAngle({1, 0, 0}, -0.42f);
	ui.aspect = 0.32f * 9.0f / 16 * 1.0f;                // band aspect: crop height * frame aspect
	ui.rect = {0, 0, 1, 0.32f};
	s.host.panels[0] = ui;
	s.host.panelCount = 1;
	auto panelPoint = [&](float u, float v) {
		return xrAdd(ui.surface.pose.position, xrRotate(ui.surface.pose.orientation,
			{(u - 0.5f) * ui.surface.width, (v - 0.5f) * ui.surface.width * ui.aspect, 0}));
	};
	const XrVector3f target = panelPoint(0.25f, 0.6f);
	s.begin(1, XR_HAND_RIGHT, target, H0());
	s.frame();
	CHECK(s.out.mode == VisionMode::PanelPointer && s.out.panelPointerPanel == 0);
	NEAREPS(s.out.panelU, 0.25f, 5e-3f);
	NEAREPS(s.out.panelV, 0.6f, 5e-3f);
	// route to the windows target (UI slot), hover only (no press) in the first frame
	CHECK(s.bridge.of(C::Route).size() == 1 && s.bridge.of(C::Route)[0].i == kVisionRouteWindows);
	auto ptrs = s.bridge.of(C::Pointer);
	CHECK(ptrs.size() == 1 && ptrs[0].f1 && !ptrs[0].f2);
	// engine pixels: x = (rect.x + u*rect.w)*(W-1); y = (1 - rect.y - v*rect.h)*(H-1)
	NEAREPS(ptrs[0].a, 0.25f * 1279.0f, 1.0f);
	NEAREPS(ptrs[0].b, (1.0f - 0.6f * 0.32f) * 719.0f, 1.0f);
	s.end(1, XR_HAND_RIGHT, H0());
	s.frame(); // press one frame after the hover, release one frame after the press
	ptrs = s.bridge.of(C::Pointer);
	CHECK(ptrs.size() >= 2 && ptrs.back().f2);
	s.frame();
	ptrs = s.bridge.of(C::Pointer);
	// release, then inactive: a balanced click
	bool sawUp = false;
	for (size_t i = 1; i < ptrs.size(); ++i) if (ptrs[i - 1].f2 && !ptrs[i].f2) sawUp = true;
	CHECK(sawUp && s.bridge.buttonBalance == 0);
	CHECK(s.vi.stats().panelClicks == 1);
	s.frames(3);
	CHECK(s.out.mode == VisionMode::Idle && s.vi.activePointerCount() == 0);
	// every press is preceded by a move in an EARLIER frame and followed by a release in a LATER one
	// ---- dragging on a panel moves the pointer with the button held
	Sim d;
	d.host.panels[0] = ui;
	d.host.panelCount = 1;
	d.begin(1, XR_HAND_RIGHT, target, H0());
	d.frames(3);
	d.bridge.clear();
	d.move(1, XR_HAND_RIGHT, plus(H0(), 0.05f, 0, 0));
	d.frame();
	ptrs = d.bridge.of(C::Pointer);
	CHECK(!ptrs.empty() && ptrs.back().f2 && ptrs.back().a > 0.25f * 1279.0f + 5.0f);
	d.cancel(1, XR_HAND_RIGHT, H0());
	d.frame();
	CHECK(d.bridge.buttonBalance == 0); // cancel releases the button
	// ---- HUD panel passes through where no window exists
	Sim h;
	VisionPanel hud = ui;
	hud.kind = kVisionPanelGameHud;
	h.host.panels[0] = hud;
	h.host.panelCount = 1;
	h.bridge.hudHasUI = false;
	h.begin(1, XR_HAND_RIGHT, target, H0());
	h.frame();
	CHECK(h.out.mode != VisionMode::PanelPointer); // the ray continues to whatever is behind the empty HUD area
	Sim h2;
	h2.host.panels[0] = hud;
	h2.host.panelCount = 1;
	h2.begin(1, XR_HAND_RIGHT, target, H0());
	h2.frame();
	CHECK(h2.out.mode == VisionMode::PanelPointer && h2.bridge.of(C::Route)[0].i == kVisionRouteWindows);
	// ---- a panel in front of the board wins over the board behind it (nearest hit); behind it does not
	Sim n;
	VisionPanel front = ui;
	front.surface.pose = {{0, 0, 0, 1}, {0, 1.1f, -0.5f}};
	front.surface.pose.orientation = {0, 0, 0, 1};
	front.surface.width = 1.0f;
	front.aspect = 0.6f;
	front.rect = {0, 0, 1, 1};
	front.kind = kVisionPanelGameScreen;
	n.host.panels[0] = front;
	n.host.panelCount = 1;
	n.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, 0.0f, H0()); // gaze passes through the panel to the board
	n.frame();
	CHECK(n.out.mode == VisionMode::PanelPointer);
	CHECK(n.bridge.of(C::Route)[0].i == kVisionRouteComposed);
	// ---- Commands console: hit-test through the shared XrPanelLayout table; activation applies the tactic action
	Sim c;
	VisionPanel console;
	console.visible = true;
	console.kind = kVisionPanelCommandsConsole;
	console.surface.width = 0.72f;
	console.surface.pose.position = {-0.6f, 1.1f, -0.7f};
	console.aspect = float(kXrPanelHeight) / kXrPanelWidth;
	c.host.panels[0] = console;
	c.host.panelCount = 1;
	// control id 2 ("orders" row 1 col 2) center pixel (392+172, 220+34) in the 768x1024 canvas
	XrPanelControl table[64];
	const int cnt = xrCommandLayout(false, false, table, 64);
	const XrPanelControl *ctl = xrFindControl(table, cnt, 2);
	CHECK(ctl != nullptr);
	const float u = (ctl->x + ctl->w * 0.5f) / kXrPanelWidth, v = 1.0f - (ctl->y + ctl->h * 0.5f) / kXrPanelHeight;
	CHECK(xrCommandHit(u, v) == 2);
	const XrVector3f cp = xrAdd(console.surface.pose.position, {(u - 0.5f) * console.surface.width, (v - 0.5f) * console.surface.width * console.aspect, 0});
	c.begin(1, XR_HAND_RIGHT, cp, H0());
	c.frame();
	CHECK(c.out.mode == VisionMode::PanelPointer);
	CHECK(c.bridge.count(C::Pointer) == 0 && c.bridge.count(C::Route) == 0); // host panel: the game sees nothing
	c.end(1, XR_HAND_RIGHT, H0());
	c.frames(2);
	CHECK(c.bridge.count(C::Tactical) == 1 && c.bridge.of(C::Tactical)[0].i == xrCommandAction(2));
	// press on one control, release on another: nothing fires
	Sim m;
	m.host.panels[0] = console;
	m.host.panelCount = 1;
	m.begin(1, XR_HAND_RIGHT, cp, H0());
	m.frame();
	m.move(1, XR_HAND_RIGHT, plus(H0(), 0.4f, 0.0f, 0));
	m.frame();
	m.end(1, XR_HAND_RIGHT, plus(H0(), 0.4f, 0.0f, 0));
	m.frames(2);
	CHECK(m.bridge.count(C::Tactical) == 0);
	// dead space between controls is consumed but inert
	Sim g;
	g.host.panels[0] = console;
	g.host.panelCount = 1;
	const XrVector3f gap = xrAdd(console.surface.pose.position, {(0.01f - 0.5f) * console.surface.width, (0.99f - 0.5f) * console.surface.width * console.aspect, 0});
	g.begin(1, XR_HAND_RIGHT, gap, H0());
	g.frame();
	CHECK(g.out.mode == VisionMode::PanelPointer);
	g.end(1, XR_HAND_RIGHT, H0());
	g.frames(2);
	CHECK(g.bridge.count(C::Tactical) == 0 && g.out.activationCount == 0);
	// applyCommandActions=false only reports
	Sim r;
	r.vi.config().applyCommandActions = false;
	r.host.panels[0] = console;
	r.host.panelCount = 1;
	r.begin(1, XR_HAND_RIGHT, cp, H0());
	r.frame();
	r.end(1, XR_HAND_RIGHT, H0());
	bool sawActivation = false;
	for (int i = 0; i < 3; ++i) { r.frame(); if (r.out.activationCount == 1 && r.out.activations[0].control == 2 && r.out.activations[0].tacticId == xrCommandAction(2)) sawActivation = true; }
	CHECK(sawActivation && r.bridge.count(C::Tactical) == 0);
}

static void testSimulatorFallback() {
	// Option emulates the second hand (mirror about the pivot): moving the mouse rotates/scales the two-hand gesture.
	Sim s;
	s.beginAtBoard(1, XR_HAND_RIGHT, 0.0f, 0.0f, H0());
	s.queue.back().pointer_kind = XR_POINTER_DEVICE;
	s.frame();
	CHECK(s.out.mode == VisionMode::Select);
	XRInteractionEvent m = {};
	m.type = XR_EVENT_MODIFIERS;
	m.modifiers = XR_MOD_OPTION;
	s.queue.push_back(m);
	s.frame();
	// a second pointer exists; moving the first hand makes it a two-hand camera gesture
	for (int i = 1; i <= 12; ++i) { s.move(1, XR_HAND_RIGHT, plus(H0(), 0.01f * i, 0, 0)); s.frame(); }
	CHECK(s.out.mode == VisionMode::CameraTwoHand);
	CHECK(s.bridge.triggerBalance == 0);
	// the emulated hand moves the opposite way about the pivot 15 cm to the right: moving toward it closes the hands
	CHECK(s.out.worldZoom > 1.0f);
	// releasing Option (or the real hand) ends the emulated hand and swallows the rest
	m.modifiers = 0;
	s.queue.push_back(m);
	s.frame();
	s.end(1, XR_HAND_RIGHT, plus(H0(), 0.12f, 0, 0));
	s.frame();
	CHECK(s.vi.activePointerCount() == 0 && s.bridge.triggerBalance == 0);
	// a device pointer with an absolute ray drives drags even without a hand pose (mouse in the simulator)
	Sim d;
	d.beginAtBoard(1, XR_HAND_RIGHT, -0.2f, 0.0f, H0());
	d.queue.back().pointer_kind = XR_POINTER_DEVICE;
	d.queue.back().has_hand_pose = false;
	d.frame();
	for (int i = 1; i <= 8; ++i) {
		XRInteractionEvent e = d.base(XR_EVENT_PINCH_DRAG, 1, XR_HAND_RIGHT, plus(H0(), 0.01f * i, 0, 0));
		e.pointer_kind = XR_POINTER_DEVICE;
		e.has_hand_pose = false;
		e.position_world = {H0().x + 0.01f * i, H0().y, H0().z};
		e.has_current_ray = true;
		const XrVector3f tgt = visionBoardToWorld(d.host.board, {-0.2f + 0.05f * i, 0.0f, 0});
		const XrVector3f dir = xrSub(tgt, kHead);
		const float l = xrLength(dir);
		e.current_ray.origin = {kHead.x, kHead.y, kHead.z};
		e.current_ray.direction = {dir.x / l, dir.y / l, dir.z / l};
		d.queue.push_back(e);
		d.frame();
	}
	CHECK(d.out.box.active);
	NEAREPS(d.out.box.maxX, -0.2f + 0.05f * 8, 5e-3f); // absolute pointing ray, not hand amplification
	// direct pinch (fingers on the board): 1:1 projection, no gaze ray needed
	Sim p;
	XRInteractionEvent e = p.base(XR_EVENT_PINCH_BEGIN, 1, XR_HAND_RIGHT, {0.1f, 0.82f, -0.9f});
	e.pointer_kind = XR_POINTER_DIRECT_PINCH;
	p.queue.push_back(e);
	p.frame();
	CHECK(p.out.mode == VisionMode::Select);
	for (int i = 1; i <= 6; ++i) {
		p.move(1, XR_HAND_RIGHT, {0.1f + 0.03f * i, 0.82f, -0.9f});
		p.queue.back().pointer_kind = XR_POINTER_DIRECT_PINCH;
		p.frame();
	}
	CHECK(p.out.box.active);
	NEAREPS(p.out.box.maxX - p.out.box.minX, 0.18f, 6e-3f);
}

static void testRobustness() {
	// DRAG or END without BEGIN never creates a gesture
	Sim s;
	s.move(9, XR_HAND_RIGHT, H0());
	s.end(9, XR_HAND_RIGHT, H0());
	s.frame();
	CHECK(s.vi.activePointerCount() == 0 && s.bridge.calls.empty());
	CHECK(s.vi.stats().droppedEvents >= 1);
	// duplicate BEGIN for the same id
	s.beginAtBoard(1, XR_HAND_RIGHT, 0, 0, H0());
	s.beginAtBoard(1, XR_HAND_RIGHT, 0, 0, H0());
	s.frame();
	CHECK(s.vi.activePointerCount() == 1);
	// the same hand pinching again (lost END) replaces the stale pinch
	s.beginAtBoard(2, XR_HAND_RIGHT, 0.1f, 0, H0());
	s.frame();
	CHECK(s.vi.activePointerCount() == 1);
	s.end(2, XR_HAND_RIGHT, H0());
	s.frame();
	CHECK(s.out.events & kVisionEventTap);
	// a third simultaneous pinch is swallowed, never a crash
	Sim t;
	t.beginAtBoard(1, XR_HAND_RIGHT, 0, 0, H0());
	t.beginAtBoard(2, XR_HAND_LEFT, 0, 0, plus(H0(), -0.3f, 0, 0));
	t.begin(3, XR_HAND_UNKNOWN, {0, 0.8f, -0.9f}, plus(H0(), 0.5f, 0, 0));
	t.frame();
	CHECK(t.vi.activePointerCount() == 2);
	t.end(3, XR_HAND_UNKNOWN, H0());
	t.frame();
	// event queue overflow cancels safely instead of desyncing
	Sim o;
	o.beginAtBoard(1, XR_HAND_RIGHT, 0, 0, H0());
	o.frame();
	for (int i = 0; i < 700; ++i) o.move(1, XR_HAND_RIGHT, H0());
	o.frame();
	CHECK(o.vi.activePointerCount() == 0 && o.bridge.triggerBalance == 0);
	// NaN input is ignored
	Sim n;
	n.beginAtBoard(1, XR_HAND_RIGHT, 0, 0, H0());
	n.frame();
	n.move(1, XR_HAND_RIGHT, {NAN, NAN, NAN});
	n.frame();
	n.end(1, XR_HAND_RIGHT, H0());
	n.frame();
	CHECK(std::isfinite(n.out.board.width) && n.out.events & kVisionEventTap);
	// engine bridge missing: the layer still moves the board
	VisionInteraction bare(nullptr);
	VisionHostState hs;
	hs.headTracked = true;
	hs.boardPlaced = true;
	hs.board = flatBoard({0, 0.8f, -0.9f}, 1.0f);
	VisionInteractionOutput out;
	bare.update(hs, nullptr, 0, out);
	CHECK(out.grabBar.visible);
	// null bridge object: inert
	VisionNullEngineBridge nullBridge;
	VisionInteraction inert(&nullBridge);
	inert.update(hs, nullptr, 0, out);
	CHECK(out.mode == VisionMode::Idle);
	// expanded UI (native dialog): world gestures are off, the panel path stays
	Sim x;
	x.bridge.expanded = true;
	x.beginAtBoard(1, XR_HAND_RIGHT, 0, 0, H0());
	x.frame();
	x.end(1, XR_HAND_RIGHT, H0());
	x.frame();
	CHECK(x.bridge.count(C::Trigger) == 0);
}

static void testRegionsAndHover() {
	Sim s;
	s.frame();
	CHECK(s.out.regionCount >= 6);
	bool hasBoard = false, hasBar = false;
	int rim = 0;
	for (int i = 0; i < s.out.regionCount; ++i) {
		const auto &r = s.out.regions[i];
		hasBoard |= r.id == kVisionRegionBoard;
		hasBar |= r.id == kVisionRegionGrabBar;
		rim += r.id == kVisionRegionPanHandle;
		CHECK(r.corners == 4);
		for (int k = 0; k < 4; ++k) CHECK(std::isfinite(r.quad[k].x + r.quad[k].y + r.quad[k].z));
	}
	CHECK(hasBoard && hasBar && rim == 4);
	// board region corners are the map rectangle (1.0 x 0.5625 m)
	for (int i = 0; i < s.out.regionCount; ++i) if (s.out.regions[i].id == kVisionRegionBoard) {
		const auto &q = s.out.regions[i].quad;
		NEAR(xrLength(xrSub(q[0], q[1])), 1.0f);
		NEAR(xrLength(xrSub(q[1], q[2])), 0.5625f);
	}
	// grab bar centre lies in front of the near edge, on the player's side
	NEAR(s.out.grabBar.pose.position.z, -0.9f + (0.5625f * 0.5f + 0.11f));
	// the system's tracking-area hint routes a pinch whose ray missed everything
	Sim h;
	h.begin(1, XR_HAND_RIGHT, {0, 1.9f, -2.0f}, H0());
	h.queue.back().tracking_area_id = kVisionRegionGrabBar;
	h.frame();
	CHECK(h.out.mode == VisionMode::WorkspaceMove);
	// while the bar is used its region is flagged active
	bool active = false;
	for (int i = 0; i < h.out.regionCount; ++i) active |= h.out.regions[i].id == kVisionRegionGrabBar && h.out.regions[i].active;
	h.move(1, XR_HAND_RIGHT, plus(H0(), 0.05f, 0, 0));
	h.frame();
	for (int i = 0; i < h.out.regionCount; ++i) active |= h.out.regions[i].id == kVisionRegionGrabBar && h.out.regions[i].active;
	CHECK(active);
}


// =============================================================== Obj-C++ glue (visionos/Input/GXXRInput.mm)

static std::vector<XRInteractionEvent> drainGlue() {
	std::vector<XRInteractionEvent> v;
	XRInteractionEvent e;
	while (XRInteraction_PollEvent(&e)) v.push_back(e);
	return v;
}
static GXXRRawSpatialEvent rawEvent(uint64_t id, int phase, float x, float y, float z, bool ray = false) {
	GXXRRawSpatialEvent r = {};
	r.event_id = id;
	r.timestamp = 5.0;
	r.kind = GXXRRawKindIndirectPinch;
	r.phase = phase;
	r.chirality = GXXRRawChiralityRight;
	r.has_pose = true;
	r.pose_position[0] = x; r.pose_position[1] = y; r.pose_position[2] = z;
	r.pose_rotation[3] = 1;
	if (ray) { r.has_ray = true; r.ray_origin[1] = 1.5f; r.ray_direction[2] = -1; }
	return r;
}
static int countType(const std::vector<XRInteractionEvent> &v, XRInteractionType t) {
	int n = 0;
	for (auto &e : v) n += e.type == t;
	return n;
}

static void testGlue() {
	drainGlue();
	// ---- normalization: ids, ray at begin, pose, modifiers, tracking area
	{
		GXXRRawSpatialEvent b = rawEvent(0xABCDEF, GXXRRawPhaseActive, 0.1f, 1.2f, -0.3f, true);
		b.modifiers_valid = true; b.modifiers = GXXRModShift; b.tracking_area_id = 7;
		GXXRInputPushRawSpatialEvent(&b);
		GXXRRawSpatialEvent d = rawEvent(0xABCDEF, GXXRRawPhaseActive, 0.2f, 1.2f, -0.3f);
		GXXRInputPushRawSpatialEvent(&d);
		GXXRRawSpatialEvent e = rawEvent(0xABCDEF, GXXRRawPhaseEnded, 0.2f, 1.2f, -0.3f);
		GXXRInputPushRawSpatialEvent(&e);
		const auto ev = drainGlue();
		CHECK(ev.size() == 3 && ev[0].type == XR_EVENT_PINCH_BEGIN && ev[1].type == XR_EVENT_PINCH_DRAG && ev[2].type == XR_EVENT_PINCH_END);
		CHECK(ev[0].pointer_id == ev[1].pointer_id && ev[1].pointer_id == ev[2].pointer_id && ev[0].pointer_id != 0);
		CHECK(ev[0].has_ray && near(ev[0].ray_world.origin.y, 1.5f) && ev[0].has_hand_pose && near(ev[1].hand_pose.position.x, 0.2f));
		CHECK(ev[0].modifiers_valid && ev[0].modifiers == XR_MOD_SHIFT && ev[0].tracking_area_id == 7);
		CHECK(ev[0].hand == XR_HAND_RIGHT && ev[0].pointer_kind == XR_POINTER_INDIRECT_PINCH);
	}
	// ---- drag storms coalesce; begin/end are never dropped
	{
		GXXRRawSpatialEvent b = rawEvent(1, GXXRRawPhaseActive, 0, 1, 0, true);
		GXXRInputPushRawSpatialEvent(&b);
		for (int i = 0; i < 100; ++i) { GXXRRawSpatialEvent d = rawEvent(1, GXXRRawPhaseActive, 0.001f * i, 1, 0); GXXRInputPushRawSpatialEvent(&d); }
		GXXRRawSpatialEvent e = rawEvent(1, GXXRRawPhaseEnded, 0.1f, 1, 0);
		GXXRInputPushRawSpatialEvent(&e);
		const auto ev = drainGlue();
		CHECK(ev.size() == 3 && near(ev[1].hand_pose.position.x, 0.099f)); // newest sample of the run survives
	}
	// ---- flush: active pinch is delivered as CANCEL, then FLUSH; its remaining events are swallowed
	{
		GXXRRawSpatialEvent b = rawEvent(2, GXXRRawPhaseActive, 0, 1, 0, true);
		GXXRInputPushRawSpatialEvent(&b);
		XRInteraction_Flush(GXXRFlushReasonFlush);
		auto ev = drainGlue();
		CHECK(ev.size() == 3 && ev[1].type == XR_EVENT_PINCH_CANCEL && ev[2].type == XR_EVENT_FLUSH);
		GXXRRawSpatialEvent d = rawEvent(2, GXXRRawPhaseActive, 0.3f, 1, 0);
		GXXRInputPushRawSpatialEvent(&d);
		GXXRInputPushRawSpatialEvent(&d);
		GXXRRawSpatialEvent e = rawEvent(2, GXXRRawPhaseEnded, 0.3f, 1, 0);
		GXXRInputPushRawSpatialEvent(&e);
		CHECK(drainGlue().empty()); // a stale drag can never restart as a new pinch
		GXXRInputStats st;
		GXXRInputGetStats(&st);
		CHECK(st.swallowed_events >= 3 && st.active_pointers == 0);
		// the id may be reused by the system afterwards: it is a fresh pinch again
		GXXRRawSpatialEvent nb = rawEvent(2, GXXRRawPhaseActive, 0, 1, 0, true);
		GXXRInputPushRawSpatialEvent(&nb);
		ev = drainGlue();
		CHECK(ev.size() == 1 && ev[0].type == XR_EVENT_PINCH_BEGIN);
		XRInteraction_Flush(GXXRFlushReasonTrackingLost);
		ev = drainGlue();
		CHECK(countType(ev, XR_EVENT_PINCH_CANCEL) == 1 && countType(ev, XR_EVENT_TRACKING_LOST) == 1);
		GXXRRawSpatialEvent ne = rawEvent(2, GXXRRawPhaseCancelled, 0, 1, 0);
		GXXRInputPushRawSpatialEvent(&ne);
		CHECK(drainGlue().empty());
	}
	// ---- commands and modifiers keep their order relative to pointer events
	{
		GXXRRawSpatialEvent b = rawEvent(3, GXXRRawPhaseActive, 0, 1, 0, true);
		GXXRInputPushRawSpatialEvent(&b);
		GXXRInputPostCommand(GXXRCommandSetAdditive, 1);
		GXXRInputSetModifiers(GXXRModOption);
		GXXRInputSetModifiers(GXXRModOption); // unchanged: not repeated
		GXXRRawSpatialEvent e = rawEvent(3, GXXRRawPhaseEnded, 0, 1, 0);
		GXXRInputPushRawSpatialEvent(&e);
		const auto ev = drainGlue();
		CHECK(ev.size() == 4 && ev[0].type == XR_EVENT_PINCH_BEGIN && ev[1].type == XR_EVENT_COMMAND && ev[1].command == XR_CMD_SET_ADDITIVE &&
			ev[1].command_value == 1 && ev[2].type == XR_EVENT_MODIFIERS && ev[2].modifiers == XR_MOD_OPTION && ev[3].type == XR_EVENT_PINCH_END);
		GXXRInputSetModifiers(0);
		drainGlue();
	}
	// ---- hand samples coalesce per hand
	{
		for (int i = 0; i < 20; ++i) {
			GXXRRawHandSample h = {};
			h.chirality = i % 2 ? GXXRRawChiralityLeft : GXXRRawChiralityRight;
			h.tracked = true; h.pinching = i > 10; h.pinch_position[0] = 0.01f * i;
			GXXRInputPushHandSample(&h);
		}
		const auto ev = drainGlue();
		CHECK(!ev.empty() && countType(ev, XR_EVENT_HAND_UPDATE) == int(ev.size()) && ev.size() <= 20);
	}
	// ---- two simultaneous pinches produce the advisory two-hand stream; releasing one ends it
	{
		GXXRRawSpatialEvent a = rawEvent(10, GXXRRawPhaseActive, -0.1f, 1, 0, true);
		GXXRRawSpatialEvent b = rawEvent(11, GXXRRawPhaseActive, 0.1f, 1, 0, true);
		b.chirality = GXXRRawChiralityLeft;
		GXXRInputPushRawSpatialEvent(&a);
		GXXRInputPushRawSpatialEvent(&b);
		GXXRRawSpatialEvent e = rawEvent(10, GXXRRawPhaseEnded, -0.1f, 1, 0);
		GXXRInputPushRawSpatialEvent(&e);
		GXXRRawSpatialEvent e2 = rawEvent(11, GXXRRawPhaseEnded, 0.1f, 1, 0);
		GXXRInputPushRawSpatialEvent(&e2);
		const auto ev = drainGlue();
		CHECK(countType(ev, XR_EVENT_TWO_HAND_BEGIN) == 1 && countType(ev, XR_EVENT_TWO_HAND_END) == 1);
		CHECK(countType(ev, XR_EVENT_PINCH_BEGIN) == 2 && countType(ev, XR_EVENT_PINCH_END) == 2);
	}
	// ---- overflow: a FLUSH is raised so the consumer cannot be left with an unbalanced press
	{
		for (int i = 0; i < 300; ++i) { GXXRRawSpatialEvent b = rawEvent(1000 + i, GXXRRawPhaseActive, 0, 1, 0, true); GXXRInputPushRawSpatialEvent(&b); }
		for (int i = 0; i < 300; ++i) { GXXRRawSpatialEvent e = rawEvent(1000 + i, GXXRRawPhaseEnded, 0, 1, 0); GXXRInputPushRawSpatialEvent(&e); }
		GXXRInputStats st;
		GXXRInputGetStats(&st);
		CHECK(st.dropped_events > 0 && st.queue_depth <= 260);
		const auto ev = drainGlue();
		CHECK(countType(ev, XR_EVENT_FLUSH) >= 1);
		// integration: feed everything through VisionInteraction; the engine is idle at the end whatever was dropped
		Sim s;
		s.queue = ev;
		s.frames(2);
		CHECK(s.bridge.triggerBalance == 0 && s.bridge.buttonBalance == 0 && s.vi.activePointerCount() == 0);
	}
}

static void testInteractionApiCompat() {
	// v1 event layout is a prefix of v2: zero-initialised v1-style producers stay valid
	XRInteractionEvent e = {};
	e.type = XR_EVENT_PINCH_BEGIN;
	CHECK(e.modifiers == 0 && !e.modifiers_valid && !e.has_hand_pose && e.command == 0 && e.tracking_area_id == 0);
	CHECK(XR_EVENT_TWO_HAND_END == 7 && XR_EVENT_TRACKING_LOST == 8 && XR_EVENT_HAND_UPDATE == 11);
	CHECK(offsetof(XRInteractionEvent, type) == 0);
	CHECK(offsetof(XRInteractionEvent, midpoint_world) < offsetof(XRInteractionEvent, modifiers));
}

int main() {
	testHelpers();
	testInitialPlacementProposal();
	testSelect();
	testAdditive();
	testContextCommand();
	testBoxSelect();
	testBoxAdditiveAndCancel();
	testBoxClampedToBoard();
	testRimPan();
	testTwoHandCamera();
	testWorkspace();
	testPlacement();
	testGroundView();
	testTrackingLossAndFlush();
	testBalancedUnderFlush();
	testPanels();
	testSimulatorFallback();
	testRobustness();
	testRegionsAndHover();
	testInteractionApiCompat();
	testGlue();
	printf("PASS %d vision interaction checks\n", checks);
	return 0;
}
