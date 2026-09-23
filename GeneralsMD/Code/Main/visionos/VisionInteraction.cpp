// visionOS spatial interaction layer: implementation. See VisionInteraction.h and
// docs/visionos-interaction.md. Pure C++17, no platform or engine dependencies.
//
// Build note (package A / CMake): this file is self-contained but its includes need
//   GeneralsMD/Code/Main                                  (Xr*.h, the pure Quest headers)
//   GeneralsMD/Code/Main/visionos/xr_shim                 (<openxr/openxr.h> stand-in; NOT with real OpenXR)
//   Core/Libraries/Source/d3d8gles/include                (XRBoardBounds.h, included by XrWorld.h)
// XRInteraction.h is found through a relative include, no extra directory is needed for it.
#include "VisionInteraction.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace {
// GX_DEBUG_INPUT=1: one stderr line per pinch start and end (role, target, ray). Off by default.
bool debugInput() {
	static const bool on = [] { const char *v = getenv("GX_DEBUG_INPUT"); return v && v[0] == '1'; }();
	return on;
}
} // namespace

namespace {

constexpr float kPi = 3.14159265358979f;

inline bool finite3(XrVector3f v) { return std::isfinite(v.x + v.y + v.z); }
inline XrVector3f norm3(XrVector3f v) {
	const float l = xrLength(v);
	return l > 1e-9f && std::isfinite(l) ? xrScale(v, 1.0f / l) : XrVector3f{0, 0, 0};
}
inline XrVector3f fromXR(XRVec3 v) { return {v.x, v.y, v.z}; }
inline float wrapPi(float a) {
	while (a > kPi) a -= 2 * kPi;
	while (a < -kPi) a += 2 * kPi;
	return a;
}
inline float clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
// Continuous dead zone: zero inside +-dz, linear outside (no snap when it engages, exact undo when it returns).
inline float deadZone(float v, float dz) { return v > dz ? v - dz : v < -dz ? v + dz : 0.0f; }

// Ray / plane intersection in world space. t > 0 only.
bool rayPlane(XrVector3f o, XrVector3f d, XrVector3f p, XrVector3f n, XrVector3f &hit, float &t) {
	const float den = xrDot(d, n);
	if (!std::isfinite(den) || std::fabs(den) < 1e-5f) return false;
	t = xrDot(xrSub(p, o), n) / den;
	if (!std::isfinite(t) || t <= 0) return false;
	hit = xrAdd(o, xrScale(d, t));
	return true;
}

// Half extents of the map area and derived regions in board space.
struct BoardGeometry {
	float hx, hy;
};
inline BoardGeometry geometry(const XrSurface &b, float aspect) { return {b.width * 0.5f, b.width * aspect * 0.5f}; }

} // namespace

// =============================================================== pure helpers

XrVector3f visionBoardToLocal(const XrSurface &board, XrVector3f world) {
	return xrRotate(xrConjugate(board.pose.orientation), xrSub(world, board.pose.position));
}
XrVector3f visionBoardToWorld(const XrSurface &board, XrVector3f local) {
	return xrAdd(board.pose.position, xrRotate(board.pose.orientation, local));
}
bool visionRayBoardPlane(const XrSurface &board, XrVector3f origin, XrVector3f direction, XrVector3f &local, float &t) {
	const XrVector3f o = visionBoardToLocal(board, origin);
	const XrVector3f d = xrRotate(xrConjugate(board.pose.orientation), direction);
	if (!finite3(o) || !finite3(d) || d.z >= -1e-6f || o.z < 0) return false;
	t = -o.z / d.z;
	local = {o.x + d.x * t, o.y + d.y * t, 0};
	return std::isfinite(local.x + local.y + t) && t > 0;
}
XrPosef visionAimFromRay(XrVector3f origin, XrVector3f direction) {
	return {xrFromTo({0, 0, -1}, direction), origin};
}
XrSurface visionInitialBoard(const VisionConfig &cfg, const VisionHostState &host, float width) {
	XrSurface s;
	s.width = clampf(width, cfg.boardMinWidthM, cfg.boardMaxWidthM);
	float fx = 0, fz = -1;
	yawForwardFromQuat(host.head.orientation, &fx, &fz);
	const XrQuaternionf heading = xrAxisAngle({0, 1, 0}, atan2f(-fx, -fz));
	s.pose.orientation = xrMul(heading, xrAxisAngle({1, 0, 0}, -kPi * 0.5f)); // lie flat, far edge away
	const bool floorOrigin = host.head.position.y > 1.0f && host.head.position.y < 2.3f;
	const bool anchored = host.tableHeightKnown || floorOrigin;
	const float distance = anchored ? cfg.initialDistanceM : cfg.headRelativeDistanceM;
	const float y = host.tableHeightKnown ? host.tableHeight
		: floorOrigin ? cfg.floorOriginTableHeightM : host.head.position.y - cfg.headRelativeDropM;
	s.pose.position = {host.head.position.x + fx * distance, y, host.head.position.z + fz * distance};
	return s;
}
void visionClampBoard(const VisionConfig &cfg, const VisionHostState &host, XrSurface &board) {
	board.width = clampf(board.width, cfg.boardMinWidthM, cfg.boardMaxWidthM);
	const XrVector3f head = host.head.position;
	if (!finite3(board.pose.position)) return;
	float dx = board.pose.position.x - head.x, dz = board.pose.position.z - head.z;
	const float d = sqrtf(dx * dx + dz * dz);
	if (d > 1e-4f) {
		const float c = clampf(d, cfg.boardMinHeadDistanceM, cfg.boardMaxHeadDistanceM);
		if (c != d) {
			board.pose.position.x = head.x + dx / d * c;
			board.pose.position.z = head.z + dz / d * c;
		}
	}
	const float below = clampf(head.y - board.pose.position.y, cfg.boardMinBelowHeadM, cfg.boardMaxBelowHeadM);
	board.pose.position.y = head.y - below;
}
void visionBoardTransformForShell(const XrSurface &board, float aspect, float m[16], float &halfX, float &halfZ) {
	const XrVector3f x = xrRotate(board.pose.orientation, {1, 0, 0});
	const XrVector3f up = xrRotate(board.pose.orientation, {0, 0, 1});      // quad normal = shell board +Y
	const XrVector3f toward = xrRotate(board.pose.orientation, {0, -1, 0});  // near edge = shell board +Z
	const float v[16] = {x.x, x.y, x.z, 0, up.x, up.y, up.z, 0, toward.x, toward.y, toward.z, 0,
		board.pose.position.x, board.pose.position.y, board.pose.position.z, 1};
	memcpy(m, v, sizeof(v));
	halfX = board.width * 0.5f;
	halfZ = board.width * aspect * 0.5f;
}
void visionCaptureEngineFlags(VisionEngineBridge &b, VisionEngineFlags &f) {
	f.interactiveGame = b.IsInteractiveGame();
	f.canStereoWorld = b.CanStereoWorld();
	f.canAdjustWorld = b.CanAdjustWorld();
	f.canObserveGround = b.CanObserveGround();
	f.expandedUI = b.ExpandedUI();
	f.canRotatePlacement = b.CanRotatePlacement();
	f.placementPending = b.PlacementPending() || f.canRotatePlacement;
	f.armedCommand = b.HasArmedCommand();
	f.placementLegal = b.PlacementLegal();
	f.placementDegrees = f.canRotatePlacement ? b.PlacementDegrees() : 0;
	f.gameWidth = b.GameWidth();
	f.gameHeight = b.GameHeight();
}
bool visionBoxContains(const VisionBoxRect &box, XrVector3f p) {
	return xrSelectionContains({box.minX, box.minY, 0}, {box.maxX, box.maxY, 0}, p);
}
float visionFadeAlpha(const VisionConfig &cfg, double start, double now) {
	if (cfg.comfortFadeSeconds <= 0) return 0;
	const float a = 1.0f - float((now - start) / cfg.comfortFadeSeconds);
	return clampf(a, 0.0f, 1.0f);
}
float visionTwistAbout(XrQuaternionf s, XrQuaternionf n, int axis) {
	const XrQuaternionf r = xrMul(xrConjugate(xrNormalize(s)), xrNormalize(n));
	const float c = axis == 0 ? r.x : axis == 1 ? r.y : r.z;
	float w = r.w, cc = c;
	if (w < 0) { w = -w; cc = -cc; } // shortest representation
	return 2.0f * atan2f(cc, w);
}

XrVector3f VisionInteraction::PlaneCursor::at(XrVector3f hand, float minHeadHand) const {
	if (!valid) return start;
	if (direct) return xrSub(hand, xrScale(planeN, xrDot(xrSub(hand, planeP), planeN)));
	XrVector3f dir = xrSub(hand, origin);
	const float len = xrLength(dir);
	if (hit0Valid && len >= minHeadHand && std::isfinite(len)) {
		dir = xrScale(dir, 1.0f / len);
		XrVector3f hit;
		float t;
		if (rayPlane(origin, dir, planeP, planeN, hit, t)) {
			const XrVector3f delta = xrSub(hit, hit0);
			if (xrLength(delta) < 10.0f) return xrAdd(start, delta);
		}
	}
	XrVector3f d = xrSub(hand, hand0);
	d = xrSub(d, xrScale(planeN, xrDot(d, planeN)));
	return xrAdd(start, xrScale(d, gain));
}

// =============================================================== construction / small accessors

VisionInteraction::VisionInteraction(VisionEngineBridge *bridge, const VisionConfig &config)
	: bridge_(bridge), cfg_(config) {
	additiveToggle_ = cfg_.additiveToggle;
	observer_ = XrObserverState{};
}

int VisionInteraction::activePointerCount() const {
	int n = 0;
	for (const auto &p : ptr_) n += p.used && !p.released;
	return n;
}
void VisionInteraction::command(XRInteractionCommand c, int value) {
	XRInteractionEvent ev = {};
	ev.type = XR_EVENT_COMMAND;
	ev.command = c;
	ev.command_value = value;
	pending_.push_back(ev);
}

VisionInteraction::Ptr *VisionInteraction::find(uint32_t id) {
	for (auto &p : ptr_) if (p.used && p.id == id) return &p;
	return nullptr;
}
VisionInteraction::Ptr *VisionInteraction::allocate() {
	for (auto &p : ptr_) if (!p.used) { p = Ptr(); return &p; }
	return nullptr;
}
VisionInteraction::Ptr *VisionInteraction::other(const Ptr *p) {
	for (auto &q : ptr_) if (&q != p && q.used && !q.released) return &q;
	return nullptr;
}

// =============================================================== engine call wrappers

VisionIntentPreview VisionInteraction::engineIntent() {
	VisionIntentPreview v;
	if (!bridge_) return v;
	engineTouched_ = true;
	v.intent = bridge_->PointerIntent(v.target, v.targetRadius, v.hasTarget);
	return v;
}
bool VisionInteraction::enginePick(const XrPosef &aim, XrWorldHit &hit) {
	if (!bridge_) return false;
	engineTouched_ = true;
	return bridge_->PickWorld(board_, aim, hit);
}
void VisionInteraction::enginePointer(bool active, float x, float y, bool select) {
	if (!bridge_) return;
	engineTouched_ = true;
	bridge_->Pointer(active, x, y, select, false, 0);
	pointerActive_ = active;
	pointerSelect_ = active && select;
	if (active) { lastPx_ = x; lastPy_ = y; }
}
void VisionInteraction::engineSpatialPointer(bool active) {
	if (!bridge_) return;
	engineTouched_ = true;
	bridge_->SpatialPointer(active);
	spatialActive_ = active;
}
void VisionInteraction::engineTrigger(bool down, bool available, bool additive) {
	if (!bridge_) return;
	engineTouched_ = true;
	bridge_->SpatialTrigger(down, available, additive);
	triggerDown_ = down && available;
}
void VisionInteraction::engineRoute(int target) {
	if (!bridge_ || target < 0 || target == route_) return;
	engineTouched_ = true;
	bridge_->RoutePointer(target);
	route_ = target;
}
// Release everything the engine could be holding, balanced: a pressed trigger is cancelled (no click),
// a pressed button gets its release (inactive pointer, like the Quest host on tracking loss).
void VisionInteraction::engineNeutral() {
	if (!bridge_) return;
	if (triggerDown_) { engineTrigger(false, false, false); }
	if (spatialActive_) { engineSpatialPointer(false); }
	if (pointerActive_ || pointerSelect_) { enginePointer(false, lastPx_, lastPy_, false); }
}
bool VisionInteraction::effectiveAdditive() const {
	return additiveToggle_ || (modifiers_ & XR_MOD_SHIFT) != 0 || secondHandAdditive_;
}

// =============================================================== update

void VisionInteraction::update(const VisionHostState &host, const XRInteractionEvent *events, size_t count,
	VisionInteractionOutput &out) {
	++stats_.updates;
	prevHost_ = host_;
	host_ = host;
	events_ = 0;
	tapPreview_ = VisionIntentPreview();
	engineTouched_ = false;
	activationCount_ = 0;
	cursorVisible_ = cursorOnBoard_ = rayVisible_ = false;
	placementFollowing_ = placementRotating_ = false;
	placementCancelArmed_ = false;
	panelPointerPanel_ = -1;
	activeRegion_ = 0;
	groundTargetKnown_ = groundTargetValid_ = false;

	// Board ownership: the host applies the last proposal; we re-read it every frame.
	if (host.boardPlaced) {
		board_ = host.board;
		boardInit_ = true;
	} else if (!boardInit_ && (host.headTracked || stats_.updates >= 90)) {
		board_ = visionInitialBoard(cfg_, host, cfg_.boardDefaultWidthM);
		boardInit_ = true;
		boardDirty_ = true;
	}
	if (!host.boardPlaced && boardInit_) { /* keep our proposal until the host adopts it */ }
	worldZoom_ = host.worldZoom;
	zoomDirty_ = false;
	if (host.boardPlaced) boardDirty_ = false;

	if (neutralNextFrame_) { // a commit last frame queued clicks the engine has consumed by now
		neutralNextFrame_ = false;
		if (activePointerCount() == 0) engineNeutral();
	}
	for (size_t i = 0; i < count; ++i) pending_.push_back(events[i]);
	if (pending_.size() > 512) {
		stats_.droppedEvents += pending_.size();
		pending_.clear();
		cancelAll(kVisionResetFlush, false);
	}

	// Suspension: no focus or no tracking cancels everything once, then swallows input.
	const bool suspended = !host.sessionFocused || !host.headTracked;
	if (suspended) {
		if (haveHost_ && (!suspended_ || activePointerCount() > 0 || observer_.mode != XrObserverMode::Off ||
			host.engine.placementPending || host.engine.armedCommand)) {
			cancelAll(host.sessionFocused ? kVisionResetTrackingLost : kVisionResetFocus, true);
		}
		suspended_ = true;
		// Events still describe pinches that will end later: remember their ids, ignore the rest.
		for (const auto &e : pending_) {
			if (e.type == XR_EVENT_PINCH_BEGIN) addDead(e.pointer_id);
		}
		pending_.clear();
	} else {
		suspended_ = false;
		while (!pending_.empty()) {
			if (!processEvent(pending_.front())) break; // deferred to the next frame (route change hazard)
			pending_.pop_front();
		}
	}
	haveHost_ = true;

	tick();
	fillOutput(out);
}

void VisionInteraction::addDead(uint32_t id) {
	if (id == 0) return;
	for (int i = 0; i < deadCount_; ++i) if (dead_[i] == id) return;
	if (deadCount_ == 16) { memmove(dead_, dead_ + 1, sizeof(dead_[0]) * 15); --deadCount_; }
	dead_[deadCount_++] = id;
}
bool VisionInteraction::isDead(uint32_t id) const {
	for (int i = 0; i < deadCount_; ++i) if (dead_[i] == id) return true;
	return false;
}
void VisionInteraction::removeDead(uint32_t id) {
	for (int i = 0; i < deadCount_; ++i) if (dead_[i] == id) {
		memmove(dead_ + i, dead_ + i + 1, sizeof(dead_[0]) * (deadCount_ - i - 1));
		--deadCount_;
		return;
	}
}

// Returns false when the event must wait for the next frame.
bool VisionInteraction::processEvent(const XRInteractionEvent &ev) {
	if (ev.modifiers_valid) onModifiers(ev.modifiers);
	switch (ev.type) {
	case XR_EVENT_PINCH_BEGIN:
		return beginPointer(ev);
	case XR_EVENT_PINCH_DRAG:
		updatePointer(ev);
		return true;
	case XR_EVENT_PINCH_END:
		endPointer(ev, false);
		return true;
	case XR_EVENT_PINCH_CANCEL:
		endPointer(ev, true);
		return true;
	case XR_EVENT_TRACKING_LOST:
		cancelAll(kVisionResetTrackingLost, true);
		return true;
	case XR_EVENT_FLUSH:
		cancelAll(kVisionResetFlush, false);
		++stats_.flushes;
		return true;
	case XR_EVENT_COMMAND:
		handleCommand(ev.command, ev.command_value);
		return true;
	case XR_EVENT_MODIFIERS:
		onModifiers(ev.modifiers);
		return true;
	case XR_EVENT_HAND_UPDATE: {
		const int i = ev.hand == XR_HAND_LEFT ? 0 : ev.hand == XR_HAND_RIGHT ? 1 : -1;
		if (i >= 0) {
			hands_[i].valid = true;
			hands_[i].tracked = ev.hand_tracked;
			hands_[i].pinching = ev.hand_pinching;
			hands_[i].hostTime = host_.time_s;
			if (ev.has_hand_pose) hands_[i].pos = fromXR(ev.hand_pose.position);
		}
		return true;
	}
	default: // TWO_HAND_* (computed here from the pointers), NONE, unknown
		return true;
	}
}

void VisionInteraction::onModifiers(uint32_t mods) {
	const uint32_t old = modifiers_;
	modifiers_ = mods;
	if (!cfg_.emulateSecondHandWithOption) return;
	const bool was = (old & XR_MOD_OPTION) != 0, now = (mods & XR_MOD_OPTION) != 0;
	if (now && !was) {
		Ptr *a = nullptr;
		int real = 0;
		for (auto &p : ptr_) if (p.used && !p.released && !p.simulated) { a = &p; ++real; }
		const bool haveSim = [&] { for (auto &p : ptr_) if (p.used && p.simulated) return true; return false; }();
		if (real == 1 && !haveSim && a->role != Role::Consumed && a->role != Role::Panel) spawnSimulated(*a);
	} else if (!now && was) {
		for (auto &p : ptr_) if (p.used && p.simulated && !p.released) {
			XRInteractionEvent e = {};
			e.type = XR_EVENT_PINCH_END;
			e.pointer_id = p.id;
			endPointer(e, false);
		}
	}
}

void VisionInteraction::spawnSimulated(Ptr &a) {
	Ptr *b = allocate();
	if (!b) return;
	b->used = true;
	b->simulated = true;
	b->id = nextSimId_++;
	b->hand = a.hand == XR_HAND_LEFT ? XR_HAND_RIGHT : XR_HAND_LEFT;
	b->kind = a.kind;
	b->tsBegin = b->tsLast = a.tsLast;
	b->hostBegin = host_.time_s;
	XrVector3f e = xrRotate(board_.pose.orientation, {1, 0, 0});
	e = norm3(e);
	if (xrLength(e) < 0.5f) e = {1, 0, 0};
	b->simPivot = xrAdd(a.pos, xrScale(e, cfg_.simSecondHandSeparationM * 0.5f));
	b->pos0 = b->pos = xrSub(xrScale(b->simPivot, 2.0f), a.pos);
	startSecond(a, *b);
}

void VisionInteraction::assignPosition(Ptr &p, const XRInteractionEvent &ev, bool begin) {
	XrVector3f pos = ev.has_hand_pose ? fromXR(ev.hand_pose.position) : fromXR(ev.position_world);
	// A fresh ARKit sample is the better source when the spatial event carried no pose.
	const int h = p.hand == XR_HAND_LEFT ? 0 : p.hand == XR_HAND_RIGHT ? 1 : -1;
	if (!ev.has_hand_pose && ev.position_world.x == 0 && ev.position_world.y == 0 && ev.position_world.z == 0 &&
		h >= 0 && hands_[h].valid && hands_[h].tracked && host_.time_s - hands_[h].hostTime < 0.1)
		pos = hands_[h].pos;
	if (!finite3(pos)) return;
	p.pos = pos;
	if (ev.has_hand_pose) {
		const XrQuaternionf q = {ev.hand_pose.orientation.x, ev.hand_pose.orientation.y,
			ev.hand_pose.orientation.z, ev.hand_pose.orientation.w};
		if (std::isfinite(q.x + q.y + q.z + q.w)) { p.rot = xrNormalize(q); p.hasRot = true; }
	}
	if (begin) { p.pos0 = p.pos; p.rot0 = p.rot; }
	if (ev.has_current_ray && !begin) {
		const XrVector3f o = fromXR(ev.current_ray.origin), d = norm3(fromXR(ev.current_ray.direction));
		if (finite3(o) && xrLength(d) > 0.5f) { p.hasCurrentRay = true; p.curRayO = o; p.curRayD = d; }
	}
	p.travel = std::max(p.travel, xrLength(xrSub(p.pos, p.pos0)));
	p.tsLast = ev.timestamp_s;
}

// =============================================================== pointer lifecycle

bool VisionInteraction::beginPointer(const XRInteractionEvent &ev) {
	if (isDead(ev.pointer_id)) return true;
	if (find(ev.pointer_id)) return true; // duplicate begin
	// The same hand cannot pinch twice: its previous pinch lost its end event (queue overflow).
	if (ev.hand != XR_HAND_UNKNOWN) {
		for (auto &q : ptr_) if (q.used && !q.simulated && !q.released && q.hand == ev.hand) {
			cancelPointer(q, false);
		}
	}
	Ptr *p = allocate();
	if (!p) { ++stats_.droppedEvents; addDead(ev.pointer_id); return true; }
	p->used = true;
	p->id = ev.pointer_id;
	p->hand = ev.hand;
	p->kind = ev.pointer_kind;
	p->tsBegin = ev.timestamp_s;
	p->hostBegin = host_.time_s;
	p->area = ev.tracking_area_id;
	assignPosition(*p, ev, true);
	p->travel = 0;
	if (ev.has_ray) {
		const XrVector3f o = fromXR(ev.ray_world.origin), d = norm3(fromXR(ev.ray_world.direction));
		if (finite3(o) && xrLength(d) > 0.5f) { p->hasRay = true; p->rayO = o; p->rayD = d; }
	}
	// The amplification model needs the EYE as its origin. If the platform's ray starts at the head use it, otherwise
	// (a ray that starts at the hand) use the tracked head so the head->hand direction never degenerates.
	p->eye = p->hasRay && xrLength(xrSub(p->rayO, host_.head.position)) < 0.25f ? p->rayO : host_.head.position;
	if (modifiers_ & XR_MOD_OPTION) { /* simulated second hand spawns after the first is classified */ }

	Ptr *primary = other(p);
	if (primary && !primary->simulated) {
		startSecond(*primary, *p);
		++stats_.begins;
		return true;
	}
	if (primary && primary->simulated) { // a real second hand replaces the emulated one
		XRInteractionEvent e = {};
		e.type = XR_EVENT_PINCH_END;
		e.pointer_id = primary->id;
		endPointer(e, false);
	}
	// Route-change hazard: a queued button-up must reach its original recipient (XrPointerRoute rule).
	const Target t = classify(*p, ev.tracking_area_id);
	const int need = routeFor(t);
	if (need >= 0 && need != route_ && engineTouched_) {
		p->used = false;
		return false; // retry next frame
	}
	p->target = t;
	++stats_.begins;
	startRole(*p);
	if (modifiers_ & XR_MOD_OPTION) {
		if (cfg_.emulateSecondHandWithOption && p->role != Role::Consumed && p->role != Role::Panel) spawnSimulated(*p);
	}
	return true;
}

void VisionInteraction::updatePointer(const XRInteractionEvent &ev) {
	if (isDead(ev.pointer_id)) return;
	Ptr *p = find(ev.pointer_id);
	if (!p || p->released) { ++stats_.droppedEvents; return; } // a DRAG never starts a pinch
	assignPosition(*p, ev, false);
}

void VisionInteraction::endPointer(const XRInteractionEvent &ev, bool cancelled) {
	if (isDead(ev.pointer_id)) { removeDead(ev.pointer_id); return; }
	Ptr *p = find(ev.pointer_id);
	if (!p || p->released) return;
	if (!p->simulated) assignPosition(*p, ev, false);
	++stats_.ends;
	p->released = true;
	p->hostEnd = host_.time_s;
	if (cancelled) { ++stats_.cancels; cancelPointer(*p, false); return; }
	Ptr *o = other(p);
	if (debugInput())
		fprintf(stderr, "[vision-input] end role=%d travel=%.3f picked=%d hit=(%.1f,%.1f)\n", int(p->role), p->travel,
			int(p->pickedStart), p->startHit.x, p->startHit.y);
	switch (p->role) {
	case Role::Select: releaseSelect(*p); break;
	case Role::Box: releaseBox(*p); break;
	case Role::Aim:
	case Role::AimMiss: releaseAim(*p); break;
	case Role::GroundTap: releaseGround(*p); break;
	case Role::Grab: grabActive_ = false; break;
	case Role::TwoCamera: endTwo(p); break;
	case Role::TwoGrab: endTwo(p); break;
	case Role::Candidate:
		if (o && p->travel < cfg_.secondHandTapTravelM && host_.time_s - p->hostBegin <= cfg_.secondHandTapSeconds)
			secondTap(*o, *p);
		break;
	case Role::Panel: return; // the release is sent by tickPanel one frame after the press
	default: break;
	}
	freePointer(*p);
}

void VisionInteraction::freePointer(Ptr &p) {
	const bool wasSim = p.simulated;
	p = Ptr();
	(void)wasSim;
	// The emulated second hand cannot outlive the real one.
	for (auto &q : ptr_) if (q.used && q.simulated && !q.released) {
		bool realLeft = false;
		for (auto &r : ptr_) if (r.used && !r.simulated && !r.released) realLeft = true;
		if (!realLeft) {
			XRInteractionEvent e = {};
			e.type = XR_EVENT_PINCH_END;
			e.pointer_id = q.id;
			endPointer(e, true);
		}
	}
	if (activePointerCount() == 0) secondHandAdditive_ = false;
}

// Cancel semantics: no click, no order. Engine state is released in a balanced way.
void VisionInteraction::cancelPointer(Ptr &p, bool killed) {
	if (killed && !p.simulated && !p.released) addDead(p.id);
	switch (p.role) {
	case Role::Select:
	case Role::Aim:
	case Role::AimMiss:
		engineNeutral();
		break;
	case Role::Box:
		engineNeutral();
		boxState_ = VisionBoxRect();
		raise(kVisionEventBoxCancelled);
		break;
	case Role::Panel:
		if (p.panelStage >= 1) enginePointer(false, lastPx_, lastPy_, false);
		break;
	case Role::Grab: grabActive_ = false; grab_.cancel(); break;
	case Role::TwoGrab: grab_.cancel(); grabActive_ = false; two_.active = false; break;
	case Role::TwoCamera: two_.active = false; break;
	default: break;
	}
	p.released = true;
	if (p.role == Role::TwoCamera || p.role == Role::TwoGrab) {
		if (Ptr *o = other(&p)) { o->role = Role::Consumed; }
	}
	freePointer(p);
}

void VisionInteraction::cancelAll(VisionResetReason reason, bool cancelEngineTargets) {
	for (auto &p : ptr_) {
		if (!p.used) continue;
		if (!p.released && !p.simulated) addDead(p.id);
		if (p.role == Role::Panel && p.panelStage >= 1) enginePointer(false, lastPx_, lastPy_, false);
		p = Ptr();
	}
	engineNeutral();
	if (cancelEngineTargets && bridge_ && (host_.engine.placementPending || host_.engine.armedCommand ||
		reason == kVisionResetTrackingLost || reason == kVisionResetFocus)) {
		bridge_->CancelTarget();
		engineTouched_ = true;
	}
	if (boxState_.active) raise(kVisionEventBoxCancelled);
	boxState_ = VisionBoxRect();
	two_ = TwoHand();
	grab_.cancel();
	grabActive_ = false;
	secondHandAdditive_ = false;
	invalidStreak_ = 0;
	if ((reason == kVisionResetTrackingLost || reason == kVisionResetFocus || reason == kVisionResetShutdown) &&
		observer_.mode != XrObserverMode::Off) exitGround(true);
	holdProgress_ = 0;
	raise(kVisionEventCancelledAll);
}

void VisionInteraction::reset(VisionResetReason reason) {
	cancelAll(reason, reason != kVisionResetFlush);
	pending_.clear();
	suspended_ = false;
}

// =============================================================== commands

void VisionInteraction::handleCommand(int cmd, int value) {
	switch (cmd) {
	case XR_CMD_SET_ADDITIVE: additiveToggle_ = value != 0; break;
	case XR_CMD_CANCEL_PLACEMENT:
		for (auto &p : ptr_) if (p.used && (p.role == Role::Aim || p.role == Role::AimMiss)) { engineNeutral(); p.role = Role::Consumed; }
		if (bridge_) { bridge_->CancelTarget(); engineTouched_ = true; }
		raise(kVisionEventPlacementCancelled);
		invalidStreak_ = 0;
		break;
	case XR_CMD_CANCEL_ALL:
		cancelAll(kVisionResetFlush, true);
		if (observer_.mode == XrObserverMode::Armed) exitGround(false);
		break;
	case XR_CMD_RECENTER_BOARD: recenterRequested_ = true; break;
	case XR_CMD_RESET_WORKSPACE: resetWorkspaceRequested_ = true; break;
	case XR_CMD_ENTER_GROUND_VIEW:
		if (host_.engine.canObserveGround && observer_.mode == XrObserverMode::Off) {
			// Any pinch in flight belongs to the tabletop and is cancelled first.
			for (auto &p : ptr_) if (p.used && !p.released) cancelPointer(p, true);
			engineNeutral();
			if (observer_.arm(true)) { grab_.cancel(); }
		}
		break;
	case XR_CMD_EXIT_GROUND_VIEW:
		if (observer_.mode != XrObserverMode::Off) exitGround(true);
		break;
	case XR_CMD_ROTATE_PLACEMENT_STEP:
		if (bridge_ && host_.engine.canRotatePlacement && bridge_->CanRotatePlacement()) {
			// RotatePlacement requires the spatial pointer to be active (snapshot of the last pick).
			if (!spatialActive_) engineSpatialPointer(true);
			bridge_->RotatePlacement(float(value) * kPi / 180.0f);
			engineTouched_ = true;
			if (activePointerCount() == 0) engineSpatialPointer(false);
		}
		break;
	case XR_CMD_ENGINE_BACK:
		if (bridge_) { bridge_->Key(VisionKey::Back, true); bridge_->Key(VisionKey::Back, false); engineTouched_ = true; }
		break;
	default: break;
	}
}

// =============================================================== classification

int VisionInteraction::routeFor(const Target &t) const {
	switch (t.kind) {
	case Kind::Board: return kVisionRouteWorld;
	case Kind::Panel: {
		if (t.panel < 0 || t.panel >= host_.panelCount) return -1;
		const VisionPanelKind k = host_.panels[t.panel].kind;
		if (k == kVisionPanelGameScreen) return kVisionRouteComposed;
		if (k == kVisionPanelWorld2D) return kVisionRouteWorld;
		if (k == kVisionPanelGameUI || k == kVisionPanelGameHud) return kVisionRouteWindows;
		return -1; // host-owned panels do not touch the engine route
	}
	default: return -1;
	}
}

VisionInteraction::Target VisionInteraction::classify(const Ptr &p, uint32_t area) {
	Target best;
	best.t = 1e30f;
	// The gaze ray, or a synthesized ray where the platform gave none (direct pinch, simulator pointer).
	XrVector3f origin = p.rayO, dir = p.rayD;
	if (!p.hasRay) {
		if (p.kind == XR_POINTER_DIRECT_PINCH && host_.boardVisible) {
			const XrVector3f n = xrRotate(board_.pose.orientation, {0, 0, 1});
			origin = xrAdd(p.pos, xrScale(n, 0.05f));
			dir = xrScale(n, -1.0f);
		} else {
			origin = host_.head.position;
			dir = norm3(xrSub(p.pos, origin));
			if (xrLength(dir) < 0.5f) dir = {0, 0, -1};
		}
	}
	const XrPosef aim = visionAimFromRay(origin, dir);

	// ---- panels ----
	for (int i = 0; i < host_.panelCount && i < kVisionMaxPanels; ++i) {
		const VisionPanel &panel = host_.panels[i];
		if (!panel.visible || observer_.mode != XrObserverMode::Off) continue;
		float m[16], u = 0, v = 0;
		surfaceMatrix(panel.surface, m);
		XrPosef panelAim = aim;
		XrVector3f panelOrigin = origin;
		if (!p.hasRay && p.kind == XR_POINTER_DIRECT_PINCH) {
			// Fingers touching the panel: probe straight into its face from the pinch point.
			const XrVector3f n = xrRotate(panel.surface.pose.orientation, {0, 0, 1});
			panelOrigin = xrAdd(p.pos, xrScale(n, 0.05f));
			panelAim = visionAimFromRay(panelOrigin, xrScale(n, -1.0f));
		}
		if (!panelRayUV(m, panel.aspect, panelAim, &u, &v)) continue;
		const XrVector3f point = xrAdd(panel.surface.pose.position, xrRotate(panel.surface.pose.orientation,
			{(u - 0.5f) * panel.surface.width, (v - 0.5f) * panel.surface.width * panel.aspect, 0}));
		const float dist = xrLength(xrSub(point, panelOrigin));
		int control = -1;
		if (panel.kind == kVisionPanelGameHud || (panel.kind == kVisionPanelGameUI && !host_.engine.expandedUI)) {
			// The unframed HUD's empty space must not become an invisible input wall. The same holds for the control-bar
			// console in front of the near edge: its see-through top rows overlap the near part of the map in view, and a
			// pinch there must reach the map. (A dialog canvas keeps catching every pinch, as on Quest.)
			const float px = (panel.rect.x + u * panel.rect.w) * float(host_.engine.gameWidth - 1);
			const float py = (1 - panel.rect.y - v * panel.rect.h) * float(host_.engine.gameHeight - 1);
			if (!bridge_ || !bridge_->HasUIAt(px, py)) continue;
		} else if (panel.kind == kVisionPanelCommandsButton) {
			control = 100;
		} else if (panel.kind == kVisionPanelCommandsConsole) {
			control = xrCommandHit(u, v, panel.help, panel.tactics);
		}
		if (dist < best.t) {
			best = Target();
			best.kind = Kind::Panel;
			best.panel = i;
			best.t = dist;
			best.planePoint = point;
			best.u = u;
			best.v = v;
			best.control = control;
		}
	}

	// ---- board regions ----
	if (host_.boardVisible && boardInit_ && observer_.mode == XrObserverMode::Off) {
		const BoardGeometry g = geometry(board_, host_.boardAspect);
		XrVector3f local;
		float t;
		Kind kind = Kind::None;
		XrVector3f plane = {};
		if (visionRayBoardPlane(board_, origin, dir, local, t)) {
			plane = visionBoardToWorld(board_, local);
			const float barLen = clampf(board_.width * cfg_.grabBarLengthFraction, cfg_.grabBarMinLengthM, cfg_.grabBarMaxLengthM);
			const float barY = -(g.hy + cfg_.grabBarOffsetM);
			const bool inBar = std::fabs(local.x) <= barLen * 0.5f + cfg_.grabBarPadM &&
				std::fabs(local.y - barY) <= cfg_.grabBarThicknessM * 0.5f + cfg_.grabBarPadM;
			const bool inOuter = std::fabs(local.x) <= g.hx + cfg_.rimOuterM && std::fabs(local.y) <= g.hy + cfg_.rimOuterM;
			const bool inInner = std::fabs(local.x) <= g.hx - cfg_.rimInnerM && std::fabs(local.y) <= g.hy - cfg_.rimInnerM;
			const bool inRect = std::fabs(local.x) <= g.hx && std::fabs(local.y) <= g.hy;
			if (inBar) kind = Kind::GrabBar;
			else if (inOuter && !inInner) kind = Kind::Rim;
			else if (inRect) kind = Kind::Board;
		}
		if (kind == Kind::None && bridge_ && host_.engine.canStereoWorld) {
			// A tall unit above the map can be hit where its plane projection lies beyond the map edge.
			XrWorldHit hit;
			if (bridge_->PickWorld(board_, aim, hit)) { kind = Kind::Board; t = hit.distance; plane = hit.room; }
		}
		if (kind != Kind::None && t < best.t) {
			best = Target();
			best.kind = kind;
			best.t = t;
			best.planePoint = plane;
		}
	}
	// Tracking-area hint: the system did the gaze test itself (visionOS 26) and named the region.
	if (best.kind == Kind::None && area != 0 && host_.boardVisible && boardInit_) {
		if (area == kVisionRegionGrabBar) best.kind = Kind::GrabBar;
		else if (area == kVisionRegionPanHandle) best.kind = Kind::Rim;
	}
	return best;
}

// =============================================================== roles


void VisionInteraction::makeCursor(Ptr &p, XrVector3f planeP, XrVector3f planeN, XrVector3f start) {
	PlaneCursor c;
	c.valid = true;
	c.planeP = planeP;
	c.planeN = planeN;
	c.start = start;
	c.hand0 = p.pos0;
	c.gain = cfg_.cursorFallbackGain;
	if (p.kind == XR_POINTER_DIRECT_PINCH && !p.hasRay) {
		c.direct = true;
		c.start = xrSub(p.pos0, xrScale(planeN, xrDot(xrSub(p.pos0, planeP), planeN)));
	} else {
		c.origin = p.eye;
		XrVector3f dir = xrSub(p.pos0, c.origin);
		const float len = xrLength(dir);
		if (len >= cfg_.minHeadHandDistanceM && std::isfinite(len)) {
			dir = xrScale(dir, 1.0f / len);
			float t;
			c.hit0Valid = rayPlane(c.origin, dir, planeP, planeN, c.hit0, t);
		}
	}
	p.cursor = c;
	p.cursorPoint = c.start;
	p.cursorKnown = true;
}

XrVector3f VisionInteraction::cursorPoint(Ptr &p, const PlaneCursor &c) const {
	// Mouse / trackpad: an absolute pointing ray is available on every event.
	if (p.kind == XR_POINTER_DEVICE && p.hasCurrentRay) {
		XrVector3f hit;
		float t;
		if (rayPlane(p.curRayO, p.curRayD, c.planeP, c.planeN, hit, t)) return hit;
	}
	return c.at(p.pos, cfg_.minHeadHandDistanceM);
}

void VisionInteraction::startRole(Ptr &p) {
	p.role = Role::Consumed;
	if (observer_.mode != XrObserverMode::Off) {
		p.role = Role::GroundTap;
		p.holdStartHost = host_.time_s;
		previewGround(p);
		return;
	}
	const Target &t = p.target;
	const VisionEngineFlags &e = host_.engine;
	const XrVector3f boardN = xrRotate(board_.pose.orientation, {0, 0, 1});
	const bool aiming = e.placementPending || e.armedCommand;
	switch (t.kind) {
	case Kind::Panel: {
		p.role = Role::Panel;
		p.panelIndex = t.panel;
		beginPanel(p);
		break;
	}
	case Kind::GrabBar:
		p.role = Role::Grab;
		grabActive_ = true;
		grab_.armed = true; // a fresh pinch is by definition a release-then-press: capture immediately
		break;
	case Kind::Rim:
		if (e.expandedUI || !e.canAdjustWorld) break; // camera is locked (script, dialog): swallow
		p.role = Role::Pan;
		makeCursor(p, board_.pose.position, boardN, t.planePoint);
		break;
	case Kind::Board: {
		if (e.expandedUI || !e.canAdjustWorld) break;
		p.role = aiming ? Role::Aim : Role::Select;
		makeCursor(p, board_.pose.position, boardN, t.planePoint);
		p.startAim = visionAimFromRay(p.hasRay ? p.rayO : host_.head.position,
			norm3(xrSub(t.planePoint, p.hasRay ? p.rayO : host_.head.position)));
		if (p.hasRay) p.startAim = visionAimFromRay(p.rayO, p.rayD);
		XrWorldHit hit;
		if (!enginePick(p.startAim, hit)) { p.role = Role::Miss; break; }
		p.pickedStart = true;
		p.startHit = hit;
		p.startLocal = visionBoardToLocal(board_, hit.room);
		engineRoute(kVisionRouteWorld);
		engineSpatialPointer(true);
		enginePointer(true, hit.x, hit.y, false);
		placementPoint_ = hit.room;
		placementHasPoint_ = true;
		if (p.role == Role::Select) {
			p.preview = engineIntent(); // the pointer is live at the start hit: ask what a tap here will do
			// Our own hand-space classifier (same class and 2 cm threshold as the engine's trigger) must be armed too.
			p.trig = XrTriggerGesture();
			p.trig.update(false, true, p.pos, false, false);
			p.trig.update(true, true, p.pos, true, effectiveAdditive());
		}
		break;
	}
	case Kind::None:
		if (aiming && cfg_.missCancelsPlacement && e.canAdjustWorld && !e.expandedUI) p.role = Role::AimMiss;
		else p.role = Role::Miss;
		break;
	}
	if (debugInput())
		fprintf(stderr, "[vision-input] start kind=%d target=%d panel=%d role=%d picked=%d ray=%d o=(%.2f,%.2f,%.2f) d=(%.2f,%.2f,%.2f) "
			"plane=(%.2f,%.2f,%.2f) board=(%.2f,%.2f,%.2f) canAdjust=%d expanded=%d\n",
			int(p.kind), int(t.kind), t.panel, int(p.role), int(p.pickedStart), int(p.hasRay), p.rayO.x, p.rayO.y, p.rayO.z,
			p.rayD.x, p.rayD.y, p.rayD.z, t.planePoint.x, t.planePoint.y, t.planePoint.z, board_.pose.position.x,
			board_.pose.position.y, board_.pose.position.z, int(e.canAdjustWorld), int(e.expandedUI));
}

// Second pinch while another is held.
void VisionInteraction::startSecond(Ptr &a, Ptr &b) {
	b.role = Role::Consumed;
	switch (a.role) {
	case Role::Grab:
		a.role = b.role = Role::TwoGrab;
		grabActive_ = true;
		break;
	case Role::TwoGrab:
	case Role::TwoCamera:
		break;
	case Role::Select:
	case Role::Pan:
	case Role::Box:
	case Role::Aim:
	case Role::AimMiss:
		b.role = Role::Candidate;
		break;
	default:
		break; // panel / ground / consumed: the second pinch is swallowed
	}
}

void VisionInteraction::secondTap(Ptr &a, Ptr &b) {
	(void)b;
	raise(kVisionEventSecondHandTap);
	switch (a.role) {
	case Role::Select:
		secondHandAdditive_ = !secondHandAdditive_;
		break;
	case Role::Box:
		engineTrigger(false, false, false); // engine cancels the box: no selection, no click
		engineSpatialPointer(false);
		enginePointer(false, lastPx_, lastPy_, false);
		boxState_ = VisionBoxRect();
		raise(kVisionEventBoxCancelled);
		a.role = Role::Consumed;
		break;
	case Role::Aim:
	case Role::AimMiss:
		engineNeutral();
		if (bridge_) { bridge_->CancelTarget(); engineTouched_ = true; }
		raise(kVisionEventPlacementCancelled);
		a.role = Role::Consumed;
		invalidStreak_ = 0;
		break;
	default:
		break;
	}
}

void VisionInteraction::beginTwoCamera(Ptr &a, Ptr &b) {
	// Withdraw a not-yet-begun single-hand world gesture from the engine.
	if (a.role == Role::Select) { engineNeutral(); }
	a.role = b.role = Role::TwoCamera;
	two_ = TwoHand();
	two_.active = true;
	two_.p0[0] = a.pos;
	two_.p0[1] = b.pos;
	const XrVector3f v = xrSub(b.pos, a.pos);
	two_.sep0 = std::max(xrLength(v), 0.05f);
	const XrVector3f bx = xrRotate(board_.pose.orientation, {1, 0, 0}), by = xrRotate(board_.pose.orientation, {0, 1, 0});
	two_.v0Board = {xrDot(v, bx), xrDot(v, by), 0};
	two_.ang = 0;
	two_.angPrev = atan2f(two_.v0Board.y, two_.v0Board.x);
	two_.zoom0 = worldZoom_;
	two_.mid0 = xrScale(xrAdd(a.pos, b.pos), 0.5f);
	// Midpoint cursor: head->midpoint ray amplification on the board plane (no gaze needed).
	Ptr tmp;
	tmp.pos0 = tmp.pos = two_.mid0;
	tmp.eye = host_.head.position;
	tmp.kind = XR_POINTER_INDIRECT_PINCH;
	tmp.hasRay = false;
	makeCursor(tmp, board_.pose.position, xrRotate(board_.pose.orientation, {0, 0, 1}), board_.pose.position);
	two_.midCursor = tmp.cursor;
	if (two_.midCursor.hit0Valid) two_.midCursor.start = two_.midCursor.hit0;
	// The first hand's engine gesture was withdrawn; the engine pointer is released.
	engineNeutral();
}

void VisionInteraction::endTwo(Ptr *ended) {
	Ptr *o = ended ? other(ended) : nullptr;
	if (ended && ended->role == Role::TwoGrab) {
		// XrSurfaceGrab rebases on the transition: the remaining hand keeps moving the board, no snap.
		if (o) o->role = Role::Grab;
		else grabActive_ = false;
		two_.active = false;
		return;
	}
	two_.active = false;
	if (o) o->role = Role::Consumed; // the remaining hand must not turn into a click on release
}

// =============================================================== panels

void VisionInteraction::panelPixel(Ptr &p, XrVector3f point) {
	const VisionPanel &panel = host_.panels[p.panelIndex];
	const XrVector3f local = xrRotate(xrConjugate(panel.surface.pose.orientation), xrSub(point, panel.surface.pose.position));
	const float u = clampf(local.x / panel.surface.width + 0.5f, 0.0f, 1.0f);
	const float v = clampf(local.y / (panel.surface.width * panel.aspect) + 0.5f, 0.0f, 1.0f);
	p.panelU = u;
	p.panelV = v;
	p.panelX = (panel.rect.x + u * panel.rect.w) * float(host_.engine.gameWidth - 1);
	p.panelY = (1 - panel.rect.y - v * panel.rect.h) * float(host_.engine.gameHeight - 1);
	if (panel.kind == kVisionPanelCommandsConsole)
		p.panelControl = xrCommandHit(u, v, panel.help, panel.tactics);
	else if (panel.kind == kVisionPanelCommandsButton)
		p.panelControl = 100;
	else
		p.panelControl = -1;
}

void VisionInteraction::beginPanel(Ptr &p) {
	const VisionPanel &panel = host_.panels[p.panelIndex];
	const XrVector3f n = xrRotate(panel.surface.pose.orientation, {0, 0, 1});
	makeCursor(p, panel.surface.pose.position, n, p.target.planePoint);
	panelPixel(p, p.target.planePoint);
	p.panelPressControl = p.panelControl;
	const bool hostOwned = panel.kind == kVisionPanelCommandsButton || panel.kind == kVisionPanelCommandsConsole;
	p.panelStageFrame = stats_.updates;
	if (hostOwned) {
		// Host panels never reach the game: the engine sees an idle controller (XrCommandUI.h behaviour).
		if (spatialActive_ || pointerActive_ || triggerDown_) engineNeutral();
		p.panelStage = 2; // no engine press/release script
		return;
	}
	engineRoute(routeFor(p.target));
	enginePointer(true, p.panelX, p.panelY, false); // hover first: the GUI needs the move before the press
	p.panelStage = 1;
}

void VisionInteraction::tickPanel(Ptr &p) {
	if (p.panelIndex < 0 || p.panelIndex >= host_.panelCount) { p.role = Role::Consumed; return; }
	const VisionPanel &panel = host_.panels[p.panelIndex];
	const bool hostOwned = panel.kind == kVisionPanelCommandsButton || panel.kind == kVisionPanelCommandsConsole;
	panelPointerPanel_ = p.panelIndex;
	if (!p.released) {
		p.cursorPoint = cursorPoint(p, p.cursor);
		panelPixel(p, p.cursorPoint);
		panelU_ = p.panelU;
		panelV_ = p.panelV;
		cursorVisible_ = true;
		cursorWorld_ = p.cursorPoint;
		activeRegion_ = kVisionRegionPanelBase + p.panelIndex;
	}
	if (hostOwned) {
		if (p.released) { /* handled in finishPanel */ }
		return;
	}
	if (p.panelStage == 1 && stats_.updates > p.panelStageFrame) {
		enginePointer(true, p.panelX, p.panelY, true); // press
		p.panelStage = 2;
		p.panelStageFrame = stats_.updates;
	} else if (p.panelStage == 2 && !p.released) {
		enginePointer(true, p.panelX, p.panelY, true); // drag with the button held
	}
}

void VisionInteraction::finishPanel(Ptr &p) {
	if (p.panelIndex < 0 || p.panelIndex >= host_.panelCount) return;
	const VisionPanel &panel = host_.panels[p.panelIndex];
	const bool hostOwned = panel.kind == kVisionPanelCommandsButton || panel.kind == kVisionPanelCommandsConsole;
	if (hostOwned) {
		// Release-on-same-control rule (XrMenuState): the press and the release must be over the same control.
		panelPixel(p, p.cursorPoint);
		if (p.panelControl >= 0 && p.panelControl == p.panelPressControl) activatePanel(p);
		return;
	}
	panelPixel(p, p.cursorPoint);
	enginePointer(true, p.panelX, p.panelY, false); // release: the GUI turns press+release into the click
	enginePointer(false, p.panelX, p.panelY, false);
	activatePanel(p);
	++stats_.panelClicks;
}

int VisionInteraction::commandTactic(int control) {
	const int a = xrCommandAction(control);
	if (a >= 0) return a;
	if (control >= 40 && control <= 42) return control;
	return -1;
}

void VisionInteraction::activatePanel(Ptr &p) {
	if (activationCount_ >= kVisionMaxActivations) return;
	const VisionPanel &panel = host_.panels[p.panelIndex];
	VisionPanelActivation &a = activations_[activationCount_++];
	a.panel = p.panelIndex;
	a.kind = panel.kind;
	a.control = p.panelControl;
	a.u = p.panelU;
	a.v = p.panelV;
	a.tacticId = -1;
	if (panel.kind == kVisionPanelCommandsConsole && p.panelControl >= 0) {
		a.tacticId = commandTactic(p.panelControl);
		if (cfg_.applyCommandActions && a.tacticId >= 0 && bridge_ && bridge_->CanAdjustWorld()) {
			bridge_->TacticalAction(a.tacticId);
			engineTouched_ = true;
		}
	}
	raise(kVisionEventPanelActivated);
}

// =============================================================== world gestures

// Converts a cursor point on the board plane into a PickWorld result along the eye->cursor ray.
bool VisionInteraction::pickCursor(Ptr &p, XrVector3f planePoint, XrWorldHit &hit) {
	const XrVector3f origin = p.eye;
	const XrVector3f dir = norm3(xrSub(planePoint, origin));
	if (xrLength(dir) < 0.5f) return false;
	return enginePick(visionAimFromRay(origin, dir), hit);
}

XrVector3f VisionInteraction::clampToBoard(XrVector3f world) const {
	XrVector3f l = visionBoardToLocal(board_, world);
	const BoardGeometry g = geometry(board_, host_.boardAspect);
	l.x = clampf(l.x, -g.hx + 0.005f, g.hx - 0.005f);
	l.y = clampf(l.y, -g.hy + 0.005f, g.hy - 0.005f);
	l.z = 0;
	return visionBoardToWorld(board_, l);
}

void VisionInteraction::updateBox(Ptr &p, const XrWorldHit &hit) {
	const XrVector3f a = p.startLocal, b = visionBoardToLocal(board_, hit.room);
	boxState_.active = true;
	boxState_.additive = p.boxAdditive;
	boxState_.minX = std::min(a.x, b.x);
	boxState_.maxX = std::max(a.x, b.x);
	boxState_.minY = std::min(a.y, b.y);
	boxState_.maxY = std::max(a.y, b.y);
	const float z = 0.002f; // draw just above the surface
	const XrVector3f c[4] = {{boxState_.minX, boxState_.minY, z}, {boxState_.maxX, boxState_.minY, z},
		{boxState_.maxX, boxState_.maxY, z}, {boxState_.minX, boxState_.maxY, z}};
	for (int i = 0; i < 4; ++i) boxState_.corners[i] = visionBoardToWorld(board_, c[i]);
}

void VisionInteraction::startBox(Ptr &p) {
	// The engine press happens at the FROZEN start ray so its ground corner is exactly what was gazed at.
	XrWorldHit hit;
	if (!enginePick(p.startAim, hit)) { p.role = Role::Consumed; engineNeutral(); return; }
	engineSpatialPointer(true);
	enginePointer(true, hit.x, hit.y, false);
	p.boxAdditive = effectiveAdditive();
	engineTrigger(false, true, p.boxAdditive); // the engine's deferred trigger needs a released sample first
	engineTrigger(true, true, p.boxAdditive);
	p.engineBegun = true;
	p.role = Role::Box;
	updateBox(p, hit);
}

void VisionInteraction::tickSelect(Ptr &p) {
	if (!host_.engine.canAdjustWorld || host_.engine.expandedUI) {
		engineNeutral();
		p.role = Role::Consumed;
		return;
	}
	p.cursorPoint = cursorPoint(p, p.cursor);
	cursorVisible_ = true;
	cursorWorld_ = p.cursorPoint;
	// A second pinch that is still undecided (tap or two-hand?) holds the box decision back: otherwise a
	// hand that starts moving a moment before its partner would always win and turn the gesture into a box.
	if (Ptr *o = other(&p)) if (o->role == Role::Candidate) return;
	// Hands: the shared XrTriggerGesture (2 cm of hand travel). A pointing device with an absolute ray has no hand
	// motion, so its cursor travel on the board plane is measured instead (tickPointer keeps p.travel current).
	const bool rayDriven = p.kind == XR_POINTER_DEVICE && p.hasCurrentRay;
	const XrTriggerEvent ev = p.trig.update(true, true, p.pos, true, effectiveAdditive());
	if (rayDriven ? p.travel > cfg_.dragThresholdM : ev == XrTriggerEvent::Drag) {
		startBox(p);
		if (p.role == Role::Box) tickBox(p);
	}
}

void VisionInteraction::tickBox(Ptr &p) {
	if (!host_.engine.canAdjustWorld || host_.engine.expandedUI) {
		cancelPointerRole(p);
		return;
	}
	p.cursorPoint = clampToBoard(cursorPoint(p, p.cursor));
	cursorVisible_ = true;
	cursorWorld_ = p.cursorPoint;
	XrWorldHit hit;
	if (pickCursor(p, p.cursorPoint, hit)) {
		engineSpatialPointer(true);
		enginePointer(true, hit.x, hit.y, false);
		engineTrigger(true, true, p.boxAdditive);
		updateBox(p, hit);
		boxHit_ = hit;
	}
	activeRegion_ = kVisionRegionBoard;
}

void VisionInteraction::cancelPointerRole(Ptr &p) {
	if (p.role == Role::Box) {
		engineTrigger(false, false, false);
		boxState_ = VisionBoxRect();
		raise(kVisionEventBoxCancelled);
	}
	engineNeutral();
	p.role = Role::Consumed;
}

void VisionInteraction::releaseSelect(Ptr &p) {
	const bool rayDriven = p.kind == XR_POINTER_DEVICE && p.hasCurrentRay;
	const XrTriggerEvent ev = p.trig.update(false, true, p.pos, true, effectiveAdditive());
	const bool add = effectiveAdditive();
	if (!p.pickedStart) return;
	if (rayDriven ? p.travel > cfg_.dragThresholdM : ev == XrTriggerEvent::Drop) {
		// Release was the first sample beyond the drag threshold: a very fast flick is still a box.
		startBox(p);
		if (p.role == Role::Box) { tickBox(p); releaseBox(p); }
		return;
	}
	// Tap: press and release at the START ray (release jitter must not change the target).
	XrWorldHit hit;
	if (!enginePick(p.startAim, hit)) { engineNeutral(); return; }
	engineSpatialPointer(true);
	enginePointer(true, hit.x, hit.y, false);
	tapPreview_ = engineIntent(); // before the click changes the selection
	engineTrigger(false, true, add); // arm (idempotent)
	engineTrigger(true, true, add);  // press: nothing is ordered yet
	engineTrigger(false, true, add); // release without drag: the engine issues Click (select / contextual order)
	engineSpatialPointer(false);
	enginePointer(false, hit.x, hit.y, false);
	raise(kVisionEventTap);
	++stats_.taps;
}

void VisionInteraction::releaseBox(Ptr &p) {
	XrWorldHit hit;
	XrVector3f pt = clampToBoard(cursorPoint(p, p.cursor));
	if (pickCursor(p, pt, hit)) {
		engineSpatialPointer(true);
		enginePointer(true, hit.x, hit.y, false);
		updateBox(p, hit);
	}
	engineTrigger(false, true, p.boxAdditive); // Drop: the engine selects everything inside the corners
	neutralNextFrame_ = true;
	boxState_ = VisionBoxRect();
	raise(kVisionEventBoxCommitted);
	++stats_.boxes;
}

void VisionInteraction::tickPan(Ptr &p) {
	if (!host_.engine.canAdjustWorld) { p.role = Role::Consumed; return; }
	// Gesture entry to Ground View: a still pinch held on the pan handle. Dragging it is a pan, so no conflict.
	if (cfg_.groundEnterHoldSeconds > 0 && host_.engine.canObserveGround && observer_.mode == XrObserverMode::Off &&
		p.travel < cfg_.dragThresholdM) {
		const double held = host_.time_s - p.hostBegin;
		holdProgress_ = clampf(float(held / cfg_.groundEnterHoldSeconds), 0.0f, 1.0f);
		if (held >= cfg_.groundEnterHoldSeconds) {
			holdProgress_ = 0;
			p.role = Role::Consumed; // the pinch that armed it must not act again on release
			handleCommand(XR_CMD_ENTER_GROUND_VIEW, 0);
			return;
		}
	} else if (p.travel >= cfg_.dragThresholdM) holdProgress_ = 0;
	p.cursorPoint = cursorPoint(p, p.cursor);
	cursorVisible_ = true;
	cursorWorld_ = p.cursorPoint;
	const XrVector3f a = visionBoardToLocal(board_, p.cursor.start), b = visionBoardToLocal(board_, p.cursorPoint);
	applyPan(b.x - a.x, b.y - a.y, p.panAppliedX, p.panAppliedY);
	activeRegion_ = kVisionRegionPanHandle;
}

// Content follows the hand: dragging the map by d (board fractions) moves the camera by -d.
// NavigateWorld's right/forward act on the board basis and are clamped to 0.05 per call (pan = value * span / 2,
// span = game units per board width), so `right = -2 * fraction`, applied in chunks with a carried remainder.
void VisionInteraction::applyPan(float dxMetres, float dyMetres, float &appliedX, float &appliedY) {
	if (!bridge_ || board_.width < 0.01f) return;
	const float dx = dxMetres / board_.width, dy = dyMetres / board_.width;
	float remR = -2.0f * (dx - appliedX), remF = -2.0f * (dy - appliedY);
	int calls = 0;
	while ((std::fabs(remR) > 1e-5f || std::fabs(remF) > 1e-5f) && calls < cfg_.maxPanCallsPerFrame) {
		float r = clampf(remR, -cfg_.panChunk, cfg_.panChunk), f = clampf(remF, -cfg_.panChunk, cfg_.panChunk);
		const float len = sqrtf(r * r + f * f);
		if (len > cfg_.panChunk) { r *= cfg_.panChunk / len; f *= cfg_.panChunk / len; } // engine caps the diagonal too
		engineTouched_ = true;
		if (!bridge_->NavigateWorld(r, f, 0)) { appliedX = dx; appliedY = dy; return; } // refused (script lock): drop the residual
		remR -= r;
		remF -= f;
		++calls;
	}
	appliedX = dx + remR / 2.0f;
	appliedY = dy + remF / 2.0f;
}

void VisionInteraction::tickAim(Ptr &p) {
	const VisionEngineFlags &e = host_.engine;
	if (!e.placementPending && !e.armedCommand) { // the engine already finished or dropped it
		engineNeutral();
		p.role = Role::Consumed;
		return;
	}
	if (p.role == Role::AimMiss) { placementCancelArmed_ = true; return; }
	p.cursorPoint = clampToBoard(cursorPoint(p, p.cursor));
	cursorVisible_ = true;
	cursorWorld_ = p.cursorPoint;
	activeRegion_ = kVisionRegionBoard;
	XrWorldHit hit;
	if (pickCursor(p, p.cursorPoint, hit)) {
		engineSpatialPointer(true);
		enginePointer(true, hit.x, hit.y, false); // the ghost follows the engine pointer
		placementPoint_ = hit.room;
		placementHasPoint_ = true;
		placementFollowing_ = true;
	}
	// Hand twist rotates the ghost (RotatePlacement needs the spatial pointer snapshot: engine active above).
	if (p.hasRot && bridge_ && e.canRotatePlacement && placementFollowing_) {
		const float tw = wrapPi(visionTwistAbout(p.rot0, p.rot, cfg_.twistAxis)) * cfg_.twistSign;
		const float eff = deadZone(tw, cfg_.twistDeadzoneRad), delta = eff - p.twistApplied;
		if (std::fabs(delta) > 1e-4f && bridge_->RotatePlacement(delta)) {
			p.twistApplied = eff;
			p.twistLatched = true;
			engineTouched_ = true;
		}
		placementRotating_ = p.twistLatched;
	}
}

void VisionInteraction::releaseAim(Ptr &p) {
	const bool tap = p.travel < cfg_.dragThresholdM && !p.twistLatched;
	if (p.role == Role::AimMiss) {
		if (tap && cfg_.missCancelsPlacement && bridge_) {
			bridge_->CancelTarget();
			engineTouched_ = true;
			raise(kVisionEventPlacementCancelled);
			invalidStreak_ = 0;
		}
		return;
	}
	if (!bridge_) return;
	XrWorldHit hit;
	const XrVector3f pt = clampToBoard(cursorPoint(p, p.cursor));
	if (!pickCursor(p, pt, hit)) { engineNeutral(); return; } // dragged off the board: keep the placement pending
	engineSpatialPointer(true);
	enginePointer(true, hit.x, hit.y, false);
	const int legal = host_.engine.placementLegal;
	bridge_->SpatialClick(false); // native PlaceEventTranslator / GUICommandTranslator commit at the pointer
	engineTouched_ = true;
	neutralNextFrame_ = true;
	raise(kVisionEventPlacementConfirmed);
	++stats_.placements;
	if (legal == 0) {
		++invalidStreak_;
		if (cfg_.invalidConfirmCancelsAfter > 0 && invalidStreak_ >= cfg_.invalidConfirmCancelsAfter && tap) {
			bridge_->CancelTarget();
			raise(kVisionEventPlacementCancelled);
			invalidStreak_ = 0;
		}
	} else invalidStreak_ = 0;
}

// =============================================================== Ground View

void VisionInteraction::exitGround(bool fade) {
	if (observer_.mode == XrObserverMode::Off) return;
	observer_.cancel();
	holdProgress_ = 0;
	if (fade) startFade();
	raise(kVisionEventGroundExited);
	engineNeutral();
}

// What a release would choose: Armed asks the engine (terrain slope, shroud, models; the engine owns the rules),
// Active shows the gaze point on the virtual ground plane if it is within teleport range.
void VisionInteraction::previewGround(Ptr &p) {
	p.groundPreview = false;
	if (!bridge_ || !p.hasRay) return;
	if (observer_.mode == XrObserverMode::Armed) {
		XrVector3f ground = {}, room = {};
		p.groundPreviewValid = host_.engine.canObserveGround &&
			bridge_->PickObserverGround(board_, visionAimFromRay(p.rayO, p.rayD), ground, &room);
		p.groundPreviewRoom = room;
		p.groundPreview = true;
	} else if (observer_.mode == XrObserverMode::Active) {
		XrVector3f hit;
		float t;
		const float groundY = observer_.head.y - kXrObserverEyeHeightMetres;
		if (rayPlane(p.rayO, p.rayD, {0, groundY, 0}, {0, 1, 0}, hit, t)) {
			const float dx = hit.x - host_.head.position.x, dz = hit.z - host_.head.position.z;
			p.groundPreviewRoom = hit;
			p.groundPreviewValid = std::sqrt(dx * dx + dz * dz) <= cfg_.teleportMaxMetres;
			p.groundPreview = true;
		}
	}
}

void VisionInteraction::tickGround(Ptr &p) {
	if (p.released) return;
	const bool still = p.travel < cfg_.dragThresholdM;
	const double held = host_.time_s - p.holdStartHost;
	holdProgress_ = still && cfg_.groundExitHoldSeconds > 0 ? clampf(float(held / cfg_.groundExitHoldSeconds), 0.0f, 1.0f) : 0;
	if (still && held >= cfg_.groundExitHoldSeconds) {
		exitGround(true);
		p.role = Role::Consumed;
		holdProgress_ = 0;
		return;
	}
	if (p.groundPreview) {
		groundTargetKnown_ = true;
		groundTargetValid_ = p.groundPreviewValid;
		groundTargetRoom_ = p.groundPreviewRoom;
	}
}

void VisionInteraction::releaseGround(Ptr &p) {
	holdProgress_ = 0;
	const bool tap = p.travel < cfg_.dragThresholdM && host_.time_s - p.hostBegin <= cfg_.groundTapMaxSeconds;
	if (!tap || !bridge_ || !p.hasRay) return;
	const XrVector3f head = host_.head.position;
	float fx = 0, fz = -1;
	yawForwardFromQuat(host_.head.orientation, &fx, &fz);
	if (observer_.mode == XrObserverMode::Armed) {
		if (observer_.requireRelease) return;
		XrVector3f ground = {}, room = {};
		const XrPosef aim = visionAimFromRay(p.rayO, p.rayD);
		if (host_.engine.canObserveGround && bridge_->PickObserverGround(board_, aim, ground, &room)) {
			if (observer_.choose(ground, head, {fx, 0, fz})) {
				startFade();
				raise(kVisionEventGroundEntered);
				engineNeutral();
				return;
			}
		}
		raise(kVisionEventGroundInvalid);
		return;
	}
	if (observer_.mode == XrObserverMode::Active && !observer_.requireRelease) {
		if (!teleport(p)) raise(kVisionEventGroundInvalid);
	}
}

// Teleport: the gaze ray meets the virtual ground plane (entry head height - 1.65 m); the observer walks there in
// engine-validated steps (terrain slope, cliffs, shroud, collisions), stopping at the first refused step. The head
// pose is re-anchored, so player height always comes from the current head position.
bool VisionInteraction::teleport(Ptr &p) {
	float m[16];
	if (!xrObserverWorldToRoom(m, observer_.ground, observer_.head, observer_.forward)) return false;
	const float groundY = observer_.head.y - kXrObserverEyeHeightMetres;
	XrVector3f hit;
	float t;
	if (!rayPlane(p.rayO, p.rayD, {0, groundY, 0}, {0, 1, 0}, hit, t)) return false;
	XrVector3f target = hit;
	// Limit the jump length in room metres (observer scale is 1:1 with the room).
	XrVector3f flat = {hit.x - host_.head.position.x, 0, hit.z - host_.head.position.z};
	const float len = xrLength(flat);
	if (len > cfg_.teleportMaxMetres) {
		const float s = cfg_.teleportMaxMetres / len;
		target = {host_.head.position.x + flat.x * s, groundY, host_.head.position.z + flat.z * s};
	}
	const XrVector3f game = xrInversePoint(m, target);
	// Displacement from where the user stands (their head is at the anchor), in game units.
	const XrVector3f standing = xrInversePoint(m, {host_.head.position.x, groundY, host_.head.position.z});
	XrVector3f delta = {game.x - standing.x, game.y - standing.y, 0};
	const float dist = sqrtf(delta.x * delta.x + delta.y * delta.y);
	if (dist < 0.5f) return false;
	int steps = std::min(cfg_.teleportMaxSteps, std::max(1, int(ceilf(dist / cfg_.teleportStepUnits))));
	const XrVector3f step = {delta.x / steps, delta.y / steps, 0};
	XrVector3f current = observer_.ground;
	// The observer stands under the user's head, which is not necessarily the entry anchor: start from there.
	current.x += standing.x - observer_.ground.x;
	current.y += standing.y - observer_.ground.y;
	bool moved = false;
	for (int i = 0; i < steps; ++i) {
		XrVector3f next = {};
		if (bridge_->ObserverStep(current, step, next) ||
			(std::fabs(step.x) > 1e-4f && bridge_->ObserverStep(current, {step.x, 0, 0}, next)) ||
			(std::fabs(step.y) > 1e-4f && bridge_->ObserverStep(current, {0, step.y, 0}, next))) {
			current = next;
			moved = true;
		} else break;
	}
	if (!moved) return false;
	observer_.ground = current;
	observer_.head = host_.head.position; // re-anchor: physical head stays 1:1, height re-derived from the head
	observer_.forward = observer_.forward; // heading unchanged: no forced rotation
	startFade();
	raise(kVisionEventTeleported);
	++stats_.teleports;
	return true;
}

// =============================================================== camera two-hand / workspace

void VisionInteraction::tickTwoCamera() {
	Ptr *a = &ptr_[0], *b = &ptr_[1];
	if (!a->used || !b->used || a->released || b->released || !two_.active) return;
	if (!host_.engine.canAdjustWorld) { two_.active = false; a->role = b->role = Role::Consumed; return; }
	const XrVector3f v = xrSub(b->pos, a->pos);
	const float sep = xrLength(v);
	// pan: midpoint cursor
	const XrVector3f mid = xrScale(xrAdd(a->pos, b->pos), 0.5f);
	const XrVector3f pt = two_.midCursor.at(mid, cfg_.minHeadHandDistanceM);
	const XrVector3f s = visionBoardToLocal(board_, two_.midCursor.start), c = visionBoardToLocal(board_, pt);
	applyPan(c.x - s.x, c.y - s.y, two_.panAppliedX, two_.panAppliedY);
	cursorVisible_ = true;
	cursorWorld_ = pt;
	activeRegion_ = kVisionRegionBoard;
	// rotation about the board normal
	const XrVector3f bx = xrRotate(board_.pose.orientation, {1, 0, 0}), by = xrRotate(board_.pose.orientation, {0, 1, 0});
	const float vx = xrDot(v, bx), vy = xrDot(v, by);
	if (std::sqrt(vx * vx + vy * vy) > 0.02f) {
		const float angNow = atan2f(vy, vx);
		two_.ang += wrapPi(angNow - two_.angPrev);
		two_.angPrev = angNow;
	}
	{
		const float eff = deadZone(two_.ang, cfg_.rotateDeadzoneRad), delta = eff - two_.rotApplied;
		if (std::fabs(delta) > 1e-5f && bridge_ && bridge_->AdjustCamera(cfg_.rotateSign * delta, 0)) {
			two_.rotApplied = eff;
			engineTouched_ = true;
		}
	}
	// zoom from hand distance ratio (spread = zoom in = smaller map span = smaller coverage multiplier)
	const float logScale = logf(std::max(sep, 0.02f) / two_.sep0);
	{
		const float eff = deadZone(logScale, cfg_.zoomDeadzone);
		const float zoom = clampf(two_.zoom0 * expf(-eff), cfg_.zoomMin, cfg_.zoomMax);
		if (zoom != worldZoom_) { worldZoom_ = zoom; zoomDirty_ = true; }
	}
}

void VisionInteraction::tickWorkspace() {
	XrPosef hands[2] = {{{0, 0, 0, 1}, ptr_[0].pos}, {{0, 0, 0, 1}, ptr_[1].pos}};
	bool held[2] = {false, false}, valid[2] = {false, false};
	int n = 0;
	for (int i = 0; i < 2; ++i) {
		const Ptr &p = ptr_[i];
		if (p.used && !p.released && (p.role == Role::Grab || p.role == Role::TwoGrab)) {
			held[i] = valid[i] = true;
			++n;
		} else valid[i] = true; // idle hands are valid so an all-released sample can arm the grab
	}
	XrSurface work = board_;
	if (n == 1) {
		// One hand: translation only (identity hand orientation) with a gain about the pinch start.
		for (int i = 0; i < 2; ++i) if (held[i]) {
			const Ptr &p = ptr_[i];
			hands[i].position = xrAdd(p.pos0, xrScale(xrSub(p.pos, p.pos0), cfg_.workspaceOneHandGain));
		}
		grabActive_ = true;
	} else if (n == 2) {
		// Two hands: keep only the horizontal component of the hand axis so the board yaws but never tilts.
		const XrVector3f mid = xrScale(xrAdd(ptr_[0].pos, ptr_[1].pos), 0.5f);
		XrVector3f vh = xrSub(ptr_[1].pos, ptr_[0].pos);
		vh.y = 0;
		hands[0].position = xrSub(mid, xrScale(vh, 0.5f));
		hands[1].position = xrAdd(mid, xrScale(vh, 0.5f));
		grabActive_ = true;
	}
	const XrSurface before = work;
	const bool changed = grab_.update(work, hands, held, valid, cfg_.boardMaxWidthM);
	if (n > 0 && changed) {
		visionClampBoard(cfg_, host_, work);
		const bool moved = xrLength(xrSub(work.pose.position, before.pose.position)) > 1e-6f ||
			std::fabs(work.width - before.width) > 1e-6f ||
			std::fabs(work.pose.orientation.x - before.pose.orientation.x) + std::fabs(work.pose.orientation.y - before.pose.orientation.y) +
			std::fabs(work.pose.orientation.z - before.pose.orientation.z) + std::fabs(work.pose.orientation.w - before.pose.orientation.w) > 1e-6f;
		if (moved) {
			board_ = work;
			boardDirty_ = true;
			raise(kVisionEventBoardMoved);
		}
		activeRegion_ = kVisionRegionGrabBar;
	}
}

// =============================================================== per frame

void VisionInteraction::tickPointer(Ptr &p) {
	// A mouse/trackpad pointer may report no hand motion at all: its absolute ray is the motion.
	if (p.kind == XR_POINTER_DEVICE && p.hasCurrentRay && p.cursor.valid && !p.simulated)
		p.travel = std::max(p.travel, xrLength(xrSub(cursorPoint(p, p.cursor), p.cursor.start)));
	switch (p.role) {
	case Role::Select: tickSelect(p); break;
	case Role::Box: tickBox(p); break;
	case Role::Pan: tickPan(p); break;
	case Role::Aim:
	case Role::AimMiss: tickAim(p); break;
	case Role::Panel: tickPanel(p); break;
	case Role::GroundTap: tickGround(p); break;
	case Role::Candidate: {
		Ptr *a = other(&p);
		if (!a) { p.role = Role::Consumed; break; }
		const double age = host_.time_s - p.hostBegin;
		const bool moved = p.travel > cfg_.secondHandTapTravelM;
		if (a->role == Role::Select || a->role == Role::Pan) {
			if (moved || age > cfg_.secondHandTapSeconds) beginTwoCamera(*a, p);
		} else if (age > cfg_.secondHandTapSeconds || moved) {
			p.role = Role::Consumed; // box / placement in progress: only a quick tap means something
		}
		break;
	}
	default: break;
	}
}

void VisionInteraction::tick() {
	// Panel pointers wait one frame after the press before their release is sent.
	for (auto &p : ptr_) {
		if (!p.used) continue;
		if (p.simulated && !p.released) {
			// mirror the real hand about the pivot
			for (auto &q : ptr_) if (q.used && !q.simulated && !q.released) {
				p.pos = xrSub(xrScale(p.simPivot, 2.0f), q.pos);
				p.travel = std::max(p.travel, xrLength(xrSub(p.pos, p.pos0)));
			}
		}
		if (!p.released) {
			// tracking loss of the hand while pinching: ARKit says the hand is gone for a while
			const int h = p.hand == XR_HAND_LEFT ? 0 : p.hand == XR_HAND_RIGHT ? 1 : -1;
			if (h >= 0 && !p.simulated && hands_[h].valid && !hands_[h].tracked && host_.time_s - hands_[h].hostTime < 0.05 &&
				host_.time_s - p.hostBegin > 0.25) {
				Ptr *pp = &p;
				cancelPointer(*pp, true);
				raise(kVisionEventCancelledAll);
				continue;
			}
			tickPointer(p);
		}
	}
	// two-hand and workspace math
	if (two_.active) tickTwoCamera();
	bool anyGrab = false;
	for (auto &p : ptr_) if (p.used && !p.released && (p.role == Role::Grab || p.role == Role::TwoGrab)) anyGrab = true;
	if (host_.boardVisible && boardInit_ && observer_.mode == XrObserverMode::Off) tickWorkspace();
	if (!anyGrab) grabActive_ = false;
	// released panel pointers: press -> (one frame) -> release
	for (auto &p : ptr_) {
		if (!p.used || p.role != Role::Panel || !p.released) continue;
		const VisionPanel *panel = p.panelIndex >= 0 && p.panelIndex < host_.panelCount ? &host_.panels[p.panelIndex] : nullptr;
		const bool hostOwned = panel && (panel->kind == kVisionPanelCommandsButton || panel->kind == kVisionPanelCommandsConsole);
		if (hostOwned) { finishPanel(p); p = Ptr(); continue; }
		if (p.panelStage == 1 && stats_.updates > p.panelStageFrame) {
			enginePointer(true, p.panelX, p.panelY, true);
			p.panelStage = 2;
			p.panelStageFrame = stats_.updates;
		} else if (p.panelStage >= 2 && stats_.updates > p.panelStageFrame) {
			finishPanel(p);
			p = Ptr();
		} else if (p.panelStage == 0) {
			p = Ptr();
		}
	}
	if (activePointerCount() == 0 && !anyPanelPending()) {
		// nothing is held: the engine must be idle, and armed Ground View input may re-arm
		observer_.neutral(true);
	}
	// Recenter / reset requests apply when no grab is in flight.
	if ((recenterRequested_ || resetWorkspaceRequested_) && host_.boardVisible) {
		const float width = resetWorkspaceRequested_ ? cfg_.boardDefaultWidthM : (boardInit_ ? board_.width : cfg_.boardDefaultWidthM);
		board_ = visionInitialBoard(cfg_, host_, width);
		boardInit_ = true;
		boardDirty_ = true;
		grab_.cancel();
		if (resetWorkspaceRequested_) { worldZoom_ = 1.0f; zoomDirty_ = true; }
		recenterRequested_ = resetWorkspaceRequested_ = false;
		raise(kVisionEventRecentered);
	}
	// Ground View gating: the engine can withdraw permission at any time (dialog, movie, camera lock).
	if (observer_.mode != XrObserverMode::Off && !host_.engine.canObserveGround) exitGround(true);
}

bool VisionInteraction::anyPanelPending() const {
	for (const auto &p : ptr_) if (p.used && p.role == Role::Panel && p.released) return true;
	return false;
}

// =============================================================== output

void VisionInteraction::fillOutput(VisionInteractionOutput &out) {
	out = VisionInteractionOutput();
	out.board = board_;
	out.boardChanged = boardDirty_ && boardInit_;
	out.worldZoom = worldZoom_;
	out.worldZoomChanged = zoomDirty_;
	out.events = events_;
	out.additive = effectiveAdditive();
	out.box = boxState_;
	out.cursorVisible = cursorVisible_;
	out.cursorWorld = cursorWorld_;
	out.cursorOnBoard = cursorVisible_ && [&] {
		const XrVector3f l = visionBoardToLocal(board_, cursorWorld_);
		const BoardGeometry g = geometry(board_, host_.boardAspect);
		return std::fabs(l.x) <= g.hx && std::fabs(l.y) <= g.hy;
	}();
	for (const auto &p : ptr_)
		if (p.used && !p.released && p.role == Role::Select && p.pickedStart) { out.preview = p.preview; break; }
	out.tapPreview = tapPreview_;
	out.panelPointerPanel = panelPointerPanel_;
	out.panelU = panelU_;
	out.panelV = panelV_;
	for (int i = 0; i < activationCount_ && i < kVisionMaxActivations; ++i) out.activations[i] = activations_[i];
	out.activationCount = activationCount_;

	// Mode: strongest active gesture.
	VisionMode mode = VisionMode::Idle;
	if (observer_.mode != XrObserverMode::Off) mode = VisionMode::GroundView;
	else {
		int rank = 0;
		auto consider = [&](VisionMode m, int r) { if (r > rank) { rank = r; mode = m; } };
		for (const auto &p : ptr_) {
			if (!p.used || p.released) continue;
			switch (p.role) {
			case Role::TwoCamera: consider(VisionMode::CameraTwoHand, 9); break;
			case Role::TwoGrab: consider(VisionMode::WorkspaceTwoHand, 9); break;
			case Role::Grab: consider(VisionMode::WorkspaceMove, 8); break;
			case Role::Box: consider(VisionMode::BoxSelect, 7); break;
			case Role::Pan: consider(VisionMode::CameraPan, 6); break;
			case Role::Aim:
			case Role::AimMiss: consider(VisionMode::Placement, 5); break;
			case Role::Select: consider(VisionMode::Select, 4); break;
			case Role::Panel: consider(VisionMode::PanelPointer, 3); break;
			default: break;
			}
		}
	}
	out.mode = mode;

	// Placement feedback
	VisionPlacementFeedback &pf = out.placement;
	pf.active = host_.engine.placementPending || host_.engine.armedCommand;
	pf.ghostFollowing = placementFollowing_;
	pf.rotating = placementRotating_;
	pf.cancelArmed = placementCancelArmed_;
	pf.legal = host_.engine.placementLegal;
	pf.degrees = host_.engine.placementDegrees;
	pf.hasPoint = placementHasPoint_ && pf.active;
	pf.roomPoint = placementPoint_;

	// Ground View
	VisionGroundView &g = out.ground;
	g.mode = observer_.mode;
	g.ground = observer_.ground;
	g.head = observer_.head;
	g.forward = observer_.forward;
	g.fadeAlpha = fadeRunning_ ? visionFadeAlpha(cfg_, fadeStart_, host_.time_s) : 0;
	if (fadeRunning_ && g.fadeAlpha <= 0) fadeRunning_ = false;
	g.holdProgress = holdProgress_;
	g.hasTarget = groundTargetKnown_;
	g.targetRoom = groundTargetRoom_;
	g.targetValid = groundTargetValid_;

	// Grab bar and regions
	if (host_.boardVisible && boardInit_ && observer_.mode == XrObserverMode::Off) {
		const BoardGeometry geo = geometry(board_, host_.boardAspect);
		const float barLen = clampf(board_.width * cfg_.grabBarLengthFraction, cfg_.grabBarMinLengthM, cfg_.grabBarMaxLengthM);
		out.grabBar.visible = true;
		out.grabBar.active = grabActive_;
		out.grabBar.length = barLen;
		out.grabBar.thickness = cfg_.grabBarThicknessM;
		out.grabBar.pose.orientation = board_.pose.orientation;
		out.grabBar.pose.position = visionBoardToWorld(board_, {0, -(geo.hy + cfg_.grabBarOffsetM), 0});

		auto quad = [&](int id, float x0, float y0, float x1, float y1) {
			if (out.regionCount >= kVisionMaxRegions) return;
			VisionRegion &r = out.regions[out.regionCount++];
			r.id = id;
			r.active = activeRegion_ == id;
			r.corners = 4;
			const XrVector3f c[4] = {{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}};
			for (int i = 0; i < 4; ++i) r.quad[i] = visionBoardToWorld(board_, c[i]);
		};
		quad(kVisionRegionBoard, -geo.hx, -geo.hy, geo.hx, geo.hy);
		const float halfBar = barLen * 0.5f, barY = -(geo.hy + cfg_.grabBarOffsetM), halfT = cfg_.grabBarThicknessM * 0.5f;
		quad(kVisionRegionGrabBar, -halfBar, barY - halfT, halfBar, barY + halfT);
		const float ox = geo.hx + cfg_.rimOuterM, oy = geo.hy + cfg_.rimOuterM, ix = geo.hx - cfg_.rimInnerM, iy = geo.hy - cfg_.rimInnerM;
		quad(kVisionRegionPanHandle, -ox, -oy, ox, -iy); // near strip
		quad(kVisionRegionPanHandle, -ox, iy, ox, oy);   // far strip
		quad(kVisionRegionPanHandle, -ox, -iy, -ix, iy); // left strip
		quad(kVisionRegionPanHandle, ix, -iy, ox, iy);   // right strip
		for (int i = 0; i < host_.panelCount && out.regionCount < kVisionMaxRegions; ++i) {
			const VisionPanel &panel = host_.panels[i];
			if (!panel.visible) continue;
			VisionRegion &r = out.regions[out.regionCount++];
			r.id = kVisionRegionPanelBase + i;
			r.active = activeRegion_ == r.id;
			r.corners = 4;
			const float hw = panel.surface.width * 0.5f, hh = hw * panel.aspect;
			const XrVector3f c[4] = {{-hw, -hh, 0}, {hw, -hh, 0}, {hw, hh, 0}, {-hw, hh, 0}};
			for (int k = 0; k < 4; ++k) r.quad[k] = xrAdd(panel.surface.pose.position, xrRotate(panel.surface.pose.orientation, c[k]));
		}
	}
	// A debug/visual ray from the head to the active cursor.
	if (cursorVisible_) {
		out.rayVisible = true;
		out.rayStart = host_.head.position;
		out.rayEnd = cursorWorld_;
	}
	mode_ = mode;
}
