// Host unit test for the visionOS spatial audio model (no simulator, no OpenAL, no game data).
// Covers visionos/Audio/GXAudioListenerMath.h (board -> world listener transform, tabletop attenuation) and the state holders
// in GXAudioHostState.h that back the GXAudio_* C API. When GX_TEST_XRWORLD is defined the transform is also checked against the
// real xrWorldToBoard() of the Quest/visionOS board mapping (GeneralsMD/Code/Main/XrWorld.h).
#include "../../visionos/Audio/GXAudioHostState.h"
#include "../../visionos/Audio/GXAudioListener.h"

#ifdef GX_TEST_XRWORLD
#include "XrWorld.h"
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace gxaudio;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                                                        \
	do {                                                                                        \
		if (cond) { ++g_pass; }                                                                 \
		else { ++g_fail; printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); }              \
	} while (0)
static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps * (1.0f + std::fabs(b)); }
static bool nearV(Vec3 a, Vec3 b, float eps = 1e-3f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }
static float rnd(float lo, float hi) { return lo + (hi - lo) * (float)rand() / (float)RAND_MAX; }

// A world point in the listener's own basis: (right, forward, up) components.
static Vec3 toListenerLocal(const ListenerWorld &l, Vec3 world) {
	const Vec3 right = cross(l.forward, l.up);
	const Vec3 d = sub(world, l.position);
	return make(dot(d, right), dot(d, l.forward), dot(d, l.up));
}
static Vec3 rotateZ(Vec3 v, float a) { return make(v.x * cosf(a) - v.y * sinf(a), v.x * sinf(a) + v.y * cosf(a), v.z); }

static void testFrameBasics() {
	printf("board frame\n");
	BoardFrame f = defaultBoardFrame();
	f.rightX = 3.0f; f.rightY = 4.0f;   // deliberately not normalised
	Vec3 ax, ay;
	CHECK(boardAxes(f, &ax, &ay), "axes from a normal frame");
	CHECK(nearV(ax, make(0.6f, 0.8f, 0)) && nearV(ay, make(-0.8f, 0.6f, 0)), "axes are right and right rotated +90 degrees");
	CHECK(nearV(cross(ax, ay), make(0, 0, 1)), "board X x Y = world +Z: proper rotation, no mirroring");
	BoardFrame bad = defaultBoardFrame(); bad.rightX = 0; bad.rightY = 0;
	CHECK(!boardAxes(bad, &ax, &ay) && nearV(ax, make(1, 0, 0)) && nearV(ay, make(0, 1, 0)), "degenerate right vector falls back to identity axes");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	bad.rightX = nan;
	CHECK(!boardAxes(bad, &ax, &ay), "NaN right vector rejected");

	// Round trip and a hand-checkable case: right = +Y (map rotated 90 degrees).
	f.centerWorld = make(500, -200, 30); f.rightX = 0; f.rightY = 1;
	const float mpu = 0.0004f;
	const Vec3 w = boardPointToWorld(f, mpu, make(0.1f, 0.2f, 0.3f));
	// board +X (0.1 m = 250 u) -> world +Y; board +Y (0.2 m = 500 u) -> world -X; board +Z 0.3 m = 750 u.
	CHECK(nearV(w, make(500 - 500, -200 + 250, 30 + 750)), "hand-computed mapping for a 90 degree frame: got (%.1f, %.1f, %.1f)", w.x, w.y, w.z);
	CHECK(nearV(worldPointToBoard(f, mpu, w), make(0.1f, 0.2f, 0.3f), 1e-4f), "board->world->board round trip");
	srand(7);
	bool ok = true;
	for (int i = 0; i < 200; ++i) {
		BoardFrame g; g.centerWorld = make(rnd(-3000, 3000), rnd(-3000, 3000), rnd(-50, 200));
		const float a = rnd(-3.14f, 3.14f); g.rightX = cosf(a) * rnd(0.5f, 3); g.rightY = sinf(a) * rnd(0.5f, 3);
		const float s = rnd(0.0001f, 0.002f); const Vec3 p = make(rnd(-1, 1), rnd(-1, 1), rnd(-0.5f, 1.5f));
		ok = ok && nearV(worldPointToBoard(g, s, boardPointToWorld(g, s, p)), p, 1e-3f);
	}
	CHECK(ok, "200 random frames round-trip");
}

#ifdef GX_TEST_XRWORLD
static void testAgainstRealBoardMapping() {
	printf("against the real xrWorldToBoard()\n");
	srand(11);
	bool ok = true;
	for (int i = 0; i < 300; ++i) {
		const XrVector3f center = {rnd(-2000, 2000), rnd(-2000, 2000), rnd(0, 100)};
		const float a = rnd(-3.14f, 3.14f);
		const XrVector3f right = {cosf(a), sinf(a), 0};
		const float span = rnd(400, 3000);
		float m[16];
		if (!xrWorldToBoard(m, center, right, span)) { ok = false; break; }
		const XrVector3f w = {rnd(-3000, 3000), rnd(-3000, 3000), rnd(0, 300)};
		const XrVector3f bn = xrTransformPoint(m, w);   // board widths, centred, x right, y "up the map"
		BoardFrame f; f.centerWorld = make(center.x, center.y, center.z); f.rightX = right.x; f.rightY = right.y;
		const float boardWidthMeters = rnd(0.5f, 1.5f);
		const float mpu = boardWidthMeters / span;
		const Vec3 mine = worldPointToBoard(f, mpu, make(w.x, w.y, w.z));
		ok = ok && near(mine.x, bn.x * boardWidthMeters, 1e-3f) && near(mine.y, bn.y * boardWidthMeters, 1e-3f) && near(mine.z, bn.z * boardWidthMeters, 1e-3f);
	}
	CHECK(ok, "worldPointToBoard equals xrWorldToBoard scaled by the board width for 300 random frames (same axes, same handedness)");
}
#endif

static void testListenerTransform() {
	printf("listener transform\n");
	const float mpu = 0.0004f;   // 0.8 m board / 2000 units
	BoardFrame f; f.centerWorld = make(1000, 1000, 15); f.rightX = 1; f.rightY = 0;
	ListenerWorld l;
	// Head 0.9 m in front of the board (board -Y), 0.55 m up, looking at the board centre.
	CHECK(computeListener(f, mpu, make(0, -0.9f, 0.55f), make(0, 0.9f, -0.55f), make(0, 0, 1), &l), "computeListener accepts a normal pose");
	CHECK(nearV(l.position, make(1000, 1000 - 2250, 15 + 1375)), "head position in game units: (%.1f, %.1f, %.1f)", l.position.x, l.position.y, l.position.z);
	CHECK(near(length(l.forward), 1) && near(length(l.up), 1) && near(dot(l.forward, l.up), 0, 1e-4f), "forward/up are orthonormal");
	CHECK(near(l.metersPerUnit, mpu), "AL_METERS_PER_UNIT carries the board scale");
	// The board centre must be dead ahead of the head: its listener-local coordinates are (0, +d, <0).
	const Vec3 c = toListenerLocal(l, f.centerWorld);
	CHECK(near(c.x, 0, 1e-3f) && c.y > 0 && c.z < 0, "board centre is straight ahead and below (%.1f, %.1f, %.1f)", c.x, c.y, c.z);
	// A source on the board's left half is to the listener's left (negative local X); right half -> positive.
	const Vec3 left = toListenerLocal(l, boardPointToWorld(f, mpu, make(-0.3f, 0, 0)));
	const Vec3 right = toListenerLocal(l, boardPointToWorld(f, mpu, make(0.3f, 0, 0)));
	CHECK(left.x < 0 && right.x > 0 && near(left.x, -right.x, 1e-3f), "board left is listener left (%.1f), board right is listener right (%.1f)", left.x, right.x);

	// Same pose but the head turned 90 degrees to the right (yaw about board +Z): forward = +X.
	CHECK(computeListener(f, mpu, make(0, -0.9f, 0.55f), make(1, 0, 0), make(0, 0, 1), &l), "turned head");
	const Vec3 ahead = toListenerLocal(l, boardPointToWorld(f, mpu, make(0.9f, -0.9f, 0.55f)));   // 0.9 m along board +X from the head
	CHECK(near(ahead.x, 0, 1e-3f) && ahead.y > 0, "after turning right, a point on board +X is ahead");
	const Vec3 behindLeft = toListenerLocal(l, boardPointToWorld(f, mpu, make(0.0f, -0.0f, 0.55f)));   // above the centre: now to the LEFT (+Y is left of a +X facing head)
	CHECK(behindLeft.x < 0, "after turning right, the board's far side (+Y) is on the listener's left (%.1f)", behindLeft.x);

	// Robustness.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	ListenerWorld keep = l;
	CHECK(!computeListener(f, 0.0f, make(0, 0, 1), make(0, 1, 0), make(0, 0, 1), &l), "zero scale rejected");
	CHECK(!computeListener(f, -1.0f, make(0, 0, 1), make(0, 1, 0), make(0, 0, 1), &l), "negative scale rejected");
	CHECK(!computeListener(f, mpu, make(nan, 0, 1), make(0, 1, 0), make(0, 0, 1), &l), "NaN head position rejected");
	CHECK(!computeListener(f, mpu, make(0, 0, 1), make(0, 0, 0), make(0, 0, 1), &l), "zero forward rejected");
	CHECK(nearV(l.position, keep.position) && nearV(l.forward, keep.forward), "rejected inputs leave the output untouched");
	// Looking straight down with up == forward (degenerate): must repair to an orthonormal basis, never NaN.
	CHECK(computeListener(f, mpu, make(0, 0, 0.6f), make(0, 0, -1), make(0, 0, -1), &l), "forward parallel to up accepted");
	CHECK(finite(l.up) && near(length(l.up), 1) && near(dot(l.forward, l.up), 0, 1e-4f), "degenerate up repaired to an orthonormal basis");
	CHECK(computeListener(f, mpu, make(0, 0, 0.6f), make(0, 0, -1), make(0, 1, 0), &l) && near(dot(l.forward, l.up), 0, 1e-4f), "looking straight down with up = board +Y");
}

static void testYawInvariance() {
	printf("yaw invariance\n");
	srand(3);
	bool worldRotation = true, headRotation = true;
	for (int i = 0; i < 300; ++i) {
		const float mpu = rnd(0.0002f, 0.001f);
		BoardFrame f; f.centerWorld = make(rnd(-2000, 2000), rnd(-2000, 2000), rnd(0, 60));
		const float a = rnd(-3.14f, 3.14f); f.rightX = cosf(a); f.rightY = sinf(a);
		const Vec3 head = make(rnd(-0.6f, 0.6f), rnd(-1.2f, -0.3f), rnd(0.2f, 0.9f));
		const Vec3 fwd = make(rnd(-0.5f, 0.5f), rnd(0.2f, 1), rnd(-1, -0.1f)), up = make(0, 0, 1);
		const Vec3 src = make(rnd(-0.4f, 0.4f), rnd(-0.4f, 0.4f), rnd(0, 0.05f));   // a board point
		ListenerWorld l0, l1;
		computeListener(f, mpu, head, fwd, up, &l0);
		const Vec3 w0 = boardPointToWorld(f, mpu, src);
		const Vec3 local0 = toListenerLocal(l0, w0);

		// (a) Rotate the whole world (frame right vector, centre and the source's world position) about the world Z axis:
		//     the listener-relative geometry must not change.
		const float psi = rnd(-3.14f, 3.14f);
		BoardFrame g = f;
		g.centerWorld = rotateZ(f.centerWorld, psi);
		g.rightX = f.rightX * cosf(psi) - f.rightY * sinf(psi);
		g.rightY = f.rightX * sinf(psi) + f.rightY * cosf(psi);
		computeListener(g, mpu, head, fwd, up, &l1);
		worldRotation = worldRotation && nearV(toListenerLocal(l1, rotateZ(w0, psi)), local0, 2e-3f);

		// (b) Turning the head about board Z by theta while the source stays put rotates the listener-local coordinates by -theta.
		const float th = rnd(-3.14f, 3.14f);
		computeListener(f, mpu, head, rotateZ(fwd, th), up, &l1);
		const Vec3 local1 = toListenerLocal(l1, w0);
		// Rotating the head rigidly about +Z by th is equivalent to rotating the source about the head by -th and keeping the
		// original head basis, whatever the head pitch.
		const Vec3 rel = sub(w0, l0.position);
		headRotation = headRotation && nearV(local1, toListenerLocal(l0, add(l0.position, rotateZ(rel, -th))), 2e-3f);
	}
	CHECK(worldRotation, "rotating the world and the map frame together leaves head-relative geometry identical (300 random poses)");
	CHECK(headRotation, "turning the head by theta rotates head-relative coordinates by -theta (300 random poses)");
}

static void testAttenuation() {
	printf("attenuation\n");
	CHECK(near(inverseDistanceClamped(1, 1, 100, 1), 1) && near(inverseDistanceClamped(2, 1, 100, 1), 0.5f) && near(inverseDistanceClamped(4, 1, 100, 1), 0.25f), "1/d for rolloff 1");
	CHECK(near(inverseDistanceClamped(0.1f, 1, 100, 1), 1), "inside the reference distance the level is 1 (clamped)");
	CHECK(near(inverseDistanceClamped(500, 1, 100, 1), inverseDistanceClamped(100, 1, 100, 1)), "beyond the maximum distance the level stops falling");
	CHECK(near(inverseDistanceClamped(600, 200, 600, 0.5f), 0.5f), "the engine's own defaults (ref 200, max 600, rolloff 0.5) reach 0.5 at max range");
	CHECK(near(inverseDistanceClamped(50, 0, 100, 1), 1), "zero reference distance cannot divide by zero");

	const TabletopTuning t = defaultTabletopTuning();
	const float mpu = 0.0004f;
	const SourceAttenuation nominal = tabletopAttenuation(t, mpu, 600.0f);
	CHECK(near(nominal.refDistance, 2500) && near(nominal.maxDistance, 20000) && near(nominal.rolloff, 0.35f), "nominal event: ref %.0f u (1.0 m), max %.0f u (8 m), rolloff %.2f", nominal.refDistance, nominal.maxDistance, nominal.rolloff);
	const SourceAttenuation big = tabletopAttenuation(t, mpu, 1200.0f), quiet = tabletopAttenuation(t, mpu, 100.0f), huge = tabletopAttenuation(t, mpu, 90000.0f), unknown = tabletopAttenuation(t, mpu, 0.0f);
	CHECK(near(big.refDistance, 5000) && near(quiet.refDistance, 1250) && near(huge.refDistance, 5000) && near(unknown.refDistance, 2500), "event range scales ref/max by 0.5..2 (big %.0f, quiet %.0f, clamped %.0f, unknown %.0f)", big.refDistance, quiet.refDistance, huge.refDistance, unknown.refDistance);
	// The same physical distance must give the same level whatever the board scale (units are converted, not the geometry).
	const SourceAttenuation s2 = tabletopAttenuation(t, 0.0008f, 600.0f);
	CHECK(near(inverseDistanceClamped(1.6f / 0.0004f, nominal.refDistance, nominal.maxDistance, nominal.rolloff),
	           inverseDistanceClamped(1.6f / 0.0008f, s2.refDistance, s2.maxDistance, s2.rolloff)), "level for a given physical distance is independent of the board scale");

	// The table: head 0.4 to 1.6 m from any table point. Compare to the original camera-relative parameters seen from the head.
	float lo = 1e9f, hi = 0;
	for (float d = 0.3f; d <= 1.6f; d += 0.05f) {
		const float g = inverseDistanceClamped(d / mpu, nominal.refDistance, nominal.maxDistance, nominal.rolloff);
		lo = std::fmin(lo, g); hi = std::fmax(hi, g);
	}
	printf("  tabletop model: gain across 0.3..1.6 m = %.3f .. %.3f (%.2f dB spread)\n", lo, hi, 20 * log10f(hi / lo));
	CHECK(hi / lo < 1.5f && lo > 0.7f, "a battle across the table stays within 3.5 dB (spread %.2f dB)", 20 * log10f(hi / lo));
	// Original parameters (min 200, max 600 units, rolloff 0.5) with a head ~1400 units above the table: every source is past max
	// range, so all of them collapse to the same clamped level (no distance information) and the whole table is at half level.
	float olo = 1e9f, ohi = 0;
	for (float d = 1400; d <= 1400 + 1300; d += 50) {
		const float g = inverseDistanceClamped(d, 200, 600, 0.5f);
		olo = std::fmin(olo, g); ohi = std::fmax(ohi, g);
	}
	printf("  original parameters from the head: gain %.3f .. %.3f (flat at half level: %.1f dB below the tabletop level)\n", olo, ohi, 20 * log10f(1.0f / olo));
	CHECK(near(olo, ohi) && olo <= 0.5f + 1e-6f, "original per-source ranges applied to a head-height listener are flat and at most half level (%.3f)", olo);
	// Never silent, never above unity.
	bool sane = true;
	for (float d = 0; d < 1e6f; d = d * 1.5f + 1) { const float g = inverseDistanceClamped(d, nominal.refDistance, nominal.maxDistance, nominal.rolloff); sane = sane && g > 0.05f && g <= 1.0f; }
	CHECK(sane, "attenuation stays within (0.05, 1] for every distance (audible even when the user walks away)");
}

static void testHostState() {
	printf("host state\n");
	HostListenerState s;
	const float pos[3] = {0, -0.9f, 0.5f}, fwd[3] = {0, 0.9f, -0.5f}, up[3] = {0, 0, 1};
	HostListenerSnapshot a = s.snapshot();
	CHECK(!a.active && a.spatialSetting && !a.hostEnabled && !a.poseValid, "defaults: spatial switch ON (visionOS default), host off, no pose");
	s.setPose(0.0004f, pos, fwd, up);
	CHECK(!s.active(), "a pose alone does not activate the host listener while the host has not enabled it");
	s.setHostEnabled(true);
	CHECK(!s.active(), "enabling resets the pose: no stale pose can drive the listener");
	CHECK(s.setPose(0.0004f, pos, fwd, up) && s.active(), "enabled + pose -> active");
	const unsigned g1 = s.snapshot().generation;
	s.setSpatial(false);
	CHECK(!s.active() && s.snapshot().generation != g1, "switch OFF -> original camera-relative behaviour, generation bumps so sources are retuned");
	s.setSpatial(true);
	CHECK(s.active(), "switch back ON reactivates without a new pose");
	const float badPos[3] = {std::numeric_limits<float>::quiet_NaN(), 0, 0};
	const HostListenerSnapshot before = s.snapshot();
	CHECK(!s.setPose(0.0004f, badPos, fwd, up) && near(s.snapshot().listener.position.x, before.listener.position.x), "invalid pose ignored, previous pose kept");
	CHECK(!s.setPose(0.0f, pos, fwd, up), "zero scale ignored");
	CHECK(!s.setPose(0.0004f, nullptr, fwd, up), "null pointer ignored");
	s.setHostEnabled(false);
	CHECK(!s.active(), "host disabled -> inactive");

	// Board frame is applied to the next pose.
	HostListenerState f;
	f.setHostEnabled(true);
	const float centre[3] = {1000, 1000, 0};
	f.setBoardFrame(centre, 0, 1);
	f.setPose(0.0004f, pos, fwd, up);
	const ListenerWorld w = f.snapshot().listener;
	CHECK(nearV(w.position, make(1000 + 2250, 1000, 1250)), "board frame with right=+Y places the head 2250 u along world +X: (%.0f, %.0f, %.0f)", w.position.x, w.position.y, w.position.z);
	const float degenerate[3] = {0, 0, 0};
	f.setBoardFrame(degenerate, 0, 0);   // zero right vector: ignored
	f.setPose(0.0004f, pos, fwd, up);
	CHECK(nearV(f.snapshot().listener.position, w.position), "degenerate board frame ignored");

	// Tuning.
	HostListenerState t;
	t.setTuning(2.0f, 0.5f, 10.0f);
	CHECK(near(t.snapshot().tuning.refDistanceMeters, 2.0f) && near(t.snapshot().tuning.rolloff, 0.5f) && near(t.snapshot().tuning.maxDistanceMeters, 10.0f), "tuning stored");
	t.setTuning(0, -1, 0);
	CHECK(near(t.snapshot().tuning.refDistanceMeters, 1.0f) && near(t.snapshot().tuning.rolloff, 0.35f) && near(t.snapshot().tuning.maxDistanceMeters, 8.0f), "values <= 0 restore the defaults");
	t.setTuning(5.0f, 0.3f, 2.0f);
	CHECK(t.snapshot().tuning.maxDistanceMeters >= t.snapshot().tuning.refDistanceMeters, "max distance never below the reference distance");
	const unsigned bg = t.snapshot().binauralGeneration;
	t.setBinaural(1); t.setBinaural(1);
	CHECK(t.snapshot().binauralMode == 1 && t.snapshot().binauralGeneration == bg + 1, "binaural mode change bumps its generation once");
	t.setBinaural(9);
	CHECK(t.snapshot().binauralMode == 0, "out-of-range binaural mode -> automatic");

	// Volumes.
	HostMixState m;
	float v = -1;
	CHECK(!m.takeMasterDirty() && !m.takeCategoryDirty(0, &v), "nothing dirty initially");
	m.setMaster(1.7f);
	CHECK(near(m.master(), 1.0f) && m.takeMasterDirty() && !m.takeMasterDirty(), "master clamps to 1 and is delivered once");
	m.setCategory(9, 0.5f);
	m.setCategory(2, std::numeric_limits<float>::quiet_NaN());
	CHECK(near(m.category(2), 0.0f) && m.categoryWasSet(2) && m.takeCategoryDirty(2, &v) && near(v, 0.0f), "NaN volume becomes 0");
	m.setCategory(1, 0.25f); m.setCategory(1, 0.75f);
	CHECK(m.takeCategoryDirty(1, &v) && near(v, 0.75f) && !m.takeCategoryDirty(1, &v), "latest value wins, delivered once");
	CHECK(!m.categoryWasSet(0) && !m.categoryWasSet(9), "unset category reports the engine's value (was not set by the host)");

	// Pause state machine.
	HostPauseState p;
	CHECK(p.nextAction() == HostPauseState::None, "idle");
	CHECK(p.request(true) && !p.request(true), "repeated pause requests are idempotent");
	CHECK(p.nextAction() == HostPauseState::ApplyPause && p.nextAction() == HostPauseState::None, "engine thread applies the pause once");
	CHECK(p.request(false) && p.nextAction() == HostPauseState::ApplyResume && p.nextAction() == HostPauseState::None, "resume applied once");
	p.request(true); p.request(false);
	CHECK(p.nextAction() == HostPauseState::None, "pause+resume before the engine thread ran cancel out (no list-level pause happens)");
	CHECK(!p.request(false), "resume without pause is a no-op");
}

int main() {
	testFrameBasics();
#ifdef GX_TEST_XRWORLD
	testAgainstRealBoardMapping();
#else
	printf("(xrWorldToBoard cross-check not compiled: build with -DGX_TEST_XRWORLD and the Quest headers on the include path)\n");
#endif
	testListenerTransform();
	testYawInvariance();
	testAttenuation();
	testHostState();
	printf("\nvision-audio-listener-test: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
