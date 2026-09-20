// visionOS spatial interaction layer.
//
// VisionInteraction is a PURE state machine: no Apple types, no OpenXR, no engine
// symbols. Inputs are XRInteractionEvents (visionos/Platform/XRInteraction.h) plus a
// per-frame VisionHostState; outputs are calls on a VisionEngineBridge (the same
// XrGameBoot_* surface the Quest host drives) and a VisionInteractionOutput that the
// renderer / shell consume. It reuses the pure Quest headers (XrTactics.h,
// XrPlacement.h, XrWorld.h, XrLayers.h, XrPanelLayout.h, XrBuildRotation.h math and
// conventions) through xr_shim/openxr/openxr.h on non-OpenXR builds.
//
// Design in one paragraph: gaze+pinch gives one gaze ray at pinch start and hand motion
// afterwards, never a continuous pointer. Every pinch is therefore classified ONCE, at
// its start, from that ray (panel, board, board rim pan handle, workspace grab bar,
// nothing) and then follows the hand relative to that start point (head->hand ray
// amplification). World gestures are translated into the canonical Quest call sequence
// (PickWorld -> SpatialPointer -> SpatialTrigger press/release, or SpatialClick for
// placement) so selection, contextual commands and boxes are decided by the engine.
// docs/visionos-interaction.md is the full behaviour reference.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

#include "../../../../visionos/Platform/XRInteraction.h"
#include "VisionEngineBridge.h"
#include "VisionHostState.h"
#include "XrPanelLayout.h"
#include "XrTactics.h"
#include "XrCommands.h"

// ------------------------------------------------------------------ configuration

struct VisionConfig {
	// --- gesture recognition ---
	float dragThresholdM = 0.02f;         // hand travel that turns a pinch into a drag (XrTriggerGesture value)
	float secondHandTapSeconds = 0.35f;   // second-hand pinch shorter than this (and still) is a tap
	float secondHandTapTravelM = 0.015f;
	float cursorFallbackGain = 2.5f;      // board-plane travel per metre of hand travel when no eye ray is known
	float minHeadHandDistanceM = 0.12f;   // closer than this the head->hand ray is degenerate

	// --- board regions (board-local metres) ---
	float rimInnerM = 0.01f;              // pan handle band starts this far inside the map edge ...
	float rimOuterM = 0.07f;              // ... and ends this far outside it
	float grabBarOffsetM = 0.11f;         // centre of the grab bar in front of the near edge
	float grabBarThicknessM = 0.05f;
	float grabBarPadM = 0.02f;            // gaze tolerance around the visible bar
	float grabBarLengthFraction = 0.30f;  // of the board width, clamped
	float grabBarMinLengthM = 0.30f, grabBarMaxLengthM = 0.60f;

	// --- workspace (the physical board) ---
	float boardDefaultWidthM = 1.0f;
	float boardMinWidthM = 0.45f;         // XrSurfaceGrab lower bound: units stay readable
	float boardMaxWidthM = 2.0f;          // comfortable upper bound (95 degrees wide at 0.9 m)
	float initialDistanceM = 0.9f;
	float headRelativeDistanceM = 1.25f;  // simulator-style origin at head height (same numbers as the shell)
	float headRelativeDropM = 0.45f;      // board this far below the eyes when origin is not on the floor
	float floorOriginTableHeightM = 0.8f; // XR_TABLETOP_SURFACE_HEIGHT_M
	float boardMinHeadDistanceM = 0.35f, boardMaxHeadDistanceM = 3.0f;
	float boardMinBelowHeadM = 0.10f, boardMaxBelowHeadM = 1.40f;
	float workspaceOneHandGain = 1.5f;    // board travel per metre of hand travel, one hand

	// --- camera (map) gestures ---
	float panChunk = 0.05f;               // engine clamps |right|,|forward| per NavigateWorld call to this
	int maxPanCallsPerFrame = 24;
	float rotateSign = -1.0f;             // AdjustCamera yaw per CCW hand rotation (see docs, derived from xrWorldToBoard)
	float rotateDeadzoneRad = 0.0524f;    // 3 degrees before two-hand rotation engages
	float zoomDeadzone = 0.06f;           // |ln(scale)| before two-hand zoom engages
	float zoomMin = 0.5f, zoomMax = 3.0f; // worldZoom clamp (Quest semantics)

	// --- placement ---
	float twistDeadzoneRad = 0.1396f;     // 8 degrees of wrist twist before the ghost rotates
	float twistSign = 1.0f;               // ghost radians per radian of twist about the hand's local Z (docs)
	int twistAxis = 2;                    // hand-local axis that is the twist axis (0 x, 1 y, 2 z)
	int invalidConfirmCancelsAfter = 2;   // consecutive illegal confirms that cancel the placement (0 = never)
	bool missCancelsPlacement = true;     // tap while looking away from the board cancels the placement

	// --- Ground View ---
	float groundTapMaxSeconds = 0.6f;
	float groundExitHoldSeconds = 1.2f;   // stationary pinch held this long leaves Ground View
	float comfortFadeSeconds = 0.18f;     // black veil fade at every teleport / mode change
	float teleportMaxMetres = 6.0f;       // furthest single teleport (observer scale)
	float teleportStepUnits = 1.8f;       // engine ObserverStep limit is 2 game units per call
	int teleportMaxSteps = 80;

	// --- simulator / keyboard ---
	bool emulateSecondHandWithOption = true;
	float simSecondHandSeparationM = 0.30f;

	// --- panels ---
	bool applyCommandActions = true;      // call TacticalAction for Commands-console tactic buttons

	// --- misc ---
	bool additiveToggle = false;          // initial state of the persistent additive toggle
};

// ------------------------------------------------------------------ output

enum class VisionMode {
	Idle,
	PanelPointer,     // pinch is driving an engine UI panel or a host Commands panel
	Select,           // pinch on the map, undecided: tap = select / contextual command, drag = box
	BoxSelect,
	CameraPan,        // one-hand drag from the board rim (pan handle)
	CameraTwoHand,    // two hands on the map: rotate + zoom + pan
	WorkspaceMove,    // one hand on the grab bar
	WorkspaceTwoHand, // two hands: move + yaw + scale of the board
	Placement,        // building ghost / armed command aiming
	GroundView        // Ground View armed or active; see output.ground
};

enum VisionEventBits : uint32_t {
	kVisionEventTap = 1u << 0,             // a world tap was sent to the engine (select or command)
	kVisionEventBoxCommitted = 1u << 1,
	kVisionEventBoxCancelled = 1u << 2,
	kVisionEventPlacementConfirmed = 1u << 3,
	kVisionEventPlacementCancelled = 1u << 4,
	kVisionEventSecondHandTap = 1u << 5,
	kVisionEventGroundEntered = 1u << 6,
	kVisionEventGroundExited = 1u << 7,
	kVisionEventTeleported = 1u << 8,
	kVisionEventGroundInvalid = 1u << 9,   // teleport target refused (no safe ground)
	kVisionEventBoardMoved = 1u << 10,
	kVisionEventRecentered = 1u << 11,
	kVisionEventCancelledAll = 1u << 12,   // tracking/focus loss or flush cancelled everything in flight
	kVisionEventPanelActivated = 1u << 13
};

// Stable ids for interactive regions; they double as visionOS 26 tracking-area identifiers.
enum VisionRegionId {
	kVisionRegionBoard = 1,
	kVisionRegionGrabBar = 2,
	kVisionRegionPanHandle = 3,  // one region for the whole rim; four quads share the id
	kVisionRegionPanelBase = 16  // + panel index
};
struct VisionRegion {
	int id = 0;
	bool active = false;         // a gesture is currently using it
	int corners = 0;             // 4 for a quad
	XrVector3f quad[4] = {};     // world-space corners, counter-clockwise seen from the front
};

struct VisionBoxRect {
	bool active = false;
	bool additive = false;
	// Board-space corners (metres, x right, y toward the far edge): the rectangle the engine will select
	// with (terrain/model hit points of the press and the cursor).
	float minX = 0, minY = 0, maxX = 0, maxY = 0;
	XrVector3f corners[4] = {};  // world-space corners on the board surface, counter-clockwise from above
};

struct VisionPlacementFeedback {
	bool active = false;         // a building preview or armed command exists in the engine
	bool ghostFollowing = false; // a pinch is moving the ghost
	bool rotating = false;       // wrist twist is rotating the ghost
	bool cancelArmed = false;    // this pinch began away from the board: release cancels the placement
	int legal = -1;              // -1 unknown, 0 illegal (renderer may tint), 1 legal
	float degrees = 0;           // current ghost orientation
	bool hasPoint = false;
	XrVector3f roomPoint = {};   // last picked ghost location (room space)
};

struct VisionGroundView {
	XrObserverMode mode = XrObserverMode::Off;
	XrVector3f ground = {}, head = {}, forward = {0, 0, -1}; // XrWorldFrame.observer* when mode == Active
	float fadeAlpha = 0;         // black veil opacity 0..1 (comfort fade), 0.18 s ramp from 1 to 0
	float holdProgress = 0;      // 0..1 while a stationary pinch counts toward "exit Ground View"
	bool hasTarget = false;      // a pinch is held and its gaze target is known
	XrVector3f targetRoom = {};
	bool targetValid = false;    // the engine accepted it as safe ground (armed mode preview at release)
};

struct VisionGrabBar {
	bool visible = false;
	bool active = false;
	XrPosef pose = {{0, 0, 0, 1}, {0, 0, 0}}; // centre, board orientation
	float length = 0, thickness = 0;          // along board x / board y, metres
};

struct VisionPanelActivation {
	int panel = -1;              // index into VisionHostState::panels
	VisionPanelKind kind = kVisionPanelGameUI;
	int control = -1;            // XrPanelLayout control id (Commands panels); -1 for engine UI clicks
	int tacticId = -1;           // xrCommandAction(control) when it maps to a tactical action
	float u = 0, v = 0;
};

constexpr int kVisionMaxRegions = 16;
constexpr int kVisionMaxActivations = 4;

struct VisionInteractionOutput {
	// Workspace: the board pose the host should adopt (equals host.board when unchanged).
	bool boardChanged = false;
	XrSurface board;
	// Map zoom: the host stores it and rebuilds XrWorldFrame::coverage = xrMapCoverage(worldZoom, board.width).
	float worldZoom = 1.0f;
	bool worldZoomChanged = false;

	VisionMode mode = VisionMode::Idle;
	uint32_t events = 0;         // VisionEventBits raised this frame
	bool additive = false;       // effective additive selection (toggle, Shift, second-hand tap)

	VisionBoxRect box;
	VisionPlacementFeedback placement;
	VisionGroundView ground;
	VisionGrabBar grabBar;

	// Cursor / ray drawing: the current gesture point on the board (or panel) and an eye-to-cursor ray.
	bool cursorVisible = false;
	XrVector3f cursorWorld = {};
	bool cursorOnBoard = false;
	bool rayVisible = false;
	XrVector3f rayStart = {}, rayEnd = {};
	// Which panel the pinch is over (engine pixel pointer, UV in the GL-native bottom-up convention).
	int panelPointerPanel = -1;
	float panelU = 0, panelV = 0;

	VisionPanelActivation activations[kVisionMaxActivations];
	int activationCount = 0;

	VisionRegion regions[kVisionMaxRegions];
	int regionCount = 0;
};

struct VisionStats {
	uint64_t updates = 0, begins = 0, ends = 0, cancels = 0, taps = 0, boxes = 0;
	uint64_t droppedEvents = 0, panelClicks = 0, placements = 0, teleports = 0, flushes = 0;
};

enum VisionResetReason { kVisionResetFlush = 0, kVisionResetTrackingLost = 1, kVisionResetFocus = 2, kVisionResetShutdown = 3 };

// ------------------------------------------------------------------ pure helpers (unit tested)

// Board space <-> world. Metres; x right, y toward the far edge, z out of the board.
XrVector3f visionBoardToLocal(const XrSurface &board, XrVector3f world);
XrVector3f visionBoardToWorld(const XrSurface &board, XrVector3f local);
// Intersect a world ray with the board plane. Returns board-local (x,y,0) and the ray parameter.
bool visionRayBoardPlane(const XrSurface &board, XrVector3f origin, XrVector3f direction, XrVector3f &local, float &t);
// XrPosef whose -Z axis points along `direction` (what PickWorld / panelRayUV expect as the aim).
XrPosef visionAimFromRay(XrVector3f origin, XrVector3f direction);
// Initial board placement from the head pose: yaw only, 0.9 m ahead, flat, table height (see docs).
XrSurface visionInitialBoard(const VisionConfig &cfg, const VisionHostState &host, float width);
// Keep the board comfortable: bounded distance from the head, bounded height below the eyes, bounded width.
void visionClampBoard(const VisionConfig &cfg, const VisionHostState &host, XrSurface &board);
// Convert to the shell's XRInteraction_SetBoardTransform convention (Y up, Z toward the player, X right).
void visionBoardTransformForShell(const XrSurface &board, float aspect, float worldFromBoard[16],
	float &halfExtentX, float &halfExtentZ);
// Fill VisionEngineFlags from the bridge (call once per frame before update()).
void visionCaptureEngineFlags(VisionEngineBridge &bridge, VisionEngineFlags &flags);
// Rectangle membership in board space using the shared xrSelectionContains rule.
bool visionBoxContains(const VisionBoxRect &box, XrVector3f boardLocalPoint);
// Fade veil opacity for a transition that started at `start` (linear 1 -> 0 over cfg.comfortFadeSeconds).
float visionFadeAlpha(const VisionConfig &cfg, double start, double now);
// Wrist twist (radians) of `now` relative to `start` about the hand-local axis (0 x, 1 y, 2 z): swing-twist.
float visionTwistAbout(XrQuaternionf start, XrQuaternionf now, int axis);

// Drain XRInteraction_PollEvent (implemented by the shell, visionos/Input/GXXRInput.mm) into `out`; call once per frame.
inline size_t visionDrainEvents(XRInteractionEvent *out, size_t max) {
	size_t n = 0;
	while (n < max && XRInteraction_PollEvent(&out[n])) ++n;
	return n;
}

// Copy the parts of the output the engine frame description needs: the physical board, the map coverage from the
// zoom, and Ground View (observer) placement. Everything else in XrWorldFrame (eyes, fov, sizes, toggles) stays the
// caller's. `output.worldZoom` already includes the host's stored zoom, so the caller just stores it back.
inline void visionApplyToWorldFrame(const VisionInteractionOutput &output, XrWorldFrame &frame) {
	frame.board = output.board;
	frame.coverage = xrMapCoverage(output.worldZoom, output.board.width);
	frame.observer = output.ground.mode == XrObserverMode::Active;
	if (frame.observer) {
		frame.observerGround = output.ground.ground;
		frame.observerHead = output.ground.head;
		frame.observerForward = output.ground.forward;
	}
}

// ------------------------------------------------------------------ the state machine

class VisionInteraction {
public:
	explicit VisionInteraction(VisionEngineBridge *bridge = nullptr, const VisionConfig &config = VisionConfig());

	void setBridge(VisionEngineBridge *bridge) { bridge_ = bridge; }
	VisionConfig &config() { return cfg_; }
	const VisionConfig &config() const { return cfg_; }

	// One call per rendered frame, on the render thread, before the engine frame. `events` are the
	// XRInteraction_PollEvent results since the last frame (may be empty). Never blocks.
	void update(const VisionHostState &host, const XRInteractionEvent *events, size_t count,
		VisionInteractionOutput &out);

	// Cancel everything in flight and release the engine (balanced): pointers, boxes, placement targets
	// (CancelTarget), Ground View, workspace grabs. Pointers that are still physically pinching are
	// swallowed until the system reports their end.
	void reset(VisionResetReason reason);

	// Same as an XR_EVENT_COMMAND event, applied at the next update().
	void command(XRInteractionCommand command, int value = 0);

	bool additiveToggle() const { return additiveToggle_; }
	const VisionStats &stats() const { return stats_; }
	int activePointerCount() const;

private:
	enum class Role { None, Consumed, Miss, Panel, Select, Box, Pan, Aim, AimMiss, Grab, TwoCamera, TwoGrab,
		GroundTap, Candidate };
	enum class Kind { None, Panel, Board, Rim, GrabBar };

	struct Target {
		Kind kind = Kind::None;
		int panel = -1;
		float t = 0;
		XrVector3f planePoint = {};  // gaze ray hit on the board plane (world)
		float u = 0, v = 0;
		int control = -1;
		bool hudRoute = false;
	};

	// Maps hand motion to a point on a plane (head->hand ray amplification with a start offset).
	struct PlaneCursor {
		bool valid = false;
		XrVector3f origin = {}, hand0 = {}, planeP = {}, planeN = {0, 1, 0}, start = {}, hit0 = {};
		bool hit0Valid = false;
		float gain = 2.5f;
		bool direct = false;   // direct pinch: project the hand straight onto the plane
		XrVector3f at(XrVector3f hand, float minHeadHand) const;
	};

	struct Ptr {
		bool used = false, simulated = false, released = false;
		uint32_t id = 0;
		XRHand hand = XR_HAND_UNKNOWN;
		XRPointerKind kind = XR_POINTER_UNKNOWN;
		Role role = Role::None;
		double tsBegin = 0, tsLast = 0, hostBegin = 0, hostEnd = 0;
		XrTriggerGesture trig;         // hand-space tap/drag classifier (2 cm, shared with the Quest host)
		XrVector3f simPivot = {};      // simulated second hand: mirror pivot
		XrVector3f pos0 = {}, pos = {};
		bool hasRot = false;
		XrQuaternionf rot0 = {0, 0, 0, 1}, rot = {0, 0, 0, 1};
		bool hasRay = false;
		XrVector3f rayO = {}, rayD = {0, 0, -1};
		bool hasCurrentRay = false;
		XrVector3f curRayO = {}, curRayD = {0, 0, -1};
		float travel = 0;
		uint32_t area = 0;
		Target target;
		PlaneCursor cursor;
		XrVector3f cursorPoint = {};   // last cursor position on its plane (world)
		bool cursorKnown = false;
		// world gesture bookkeeping
		bool engineBegun = false;      // SpatialTrigger(true) was sent
		XrPosef startAim = {{0, 0, 0, 1}, {0, 0, 0}};
		bool pickedStart = false;
		XrWorldHit startHit;
		XrVector3f startLocal = {};    // press hit in board space (metres)
		bool boxAdditive = false;
		// panel bookkeeping
		int panelStage = 0;            // 0 none, 1 hover sent, 2 pressed, 3 released
		uint64_t panelStageFrame = 0;
		int panelIndex = -1;
		float panelU = 0, panelV = 0, panelX = 0, panelY = 0;
		int panelControl = -1, panelPressControl = -1;
		// aim / placement
		float twistApplied = 0;
		bool twistLatched = false;     // the twist left its dead zone at least once (a rotated ghost is never a tap)
		bool placementMiss = false;
		// pan bookkeeping (board fractions applied so far)
		float panAppliedX = 0, panAppliedY = 0;
		// ground view
		double holdStartHost = 0;
		bool groundPreview = false, groundPreviewValid = false;
		XrVector3f groundPreviewRoom = {};
	};

	// -- event intake --
	bool processEvent(const XRInteractionEvent &ev);
	bool beginPointer(const XRInteractionEvent &ev);
	void updatePointer(const XRInteractionEvent &ev);
	void endPointer(const XRInteractionEvent &ev, bool cancelled);
	void handleCommand(int cmd, int value);
	void onModifiers(uint32_t mods);
	Ptr *find(uint32_t id);
	Ptr *allocate();
	Ptr *other(const Ptr *p);
	void assignPosition(Ptr &p, const XRInteractionEvent &ev, bool begin);
	void addDead(uint32_t id);
	bool isDead(uint32_t id) const;
	void removeDead(uint32_t id);
	void spawnSimulated(Ptr &primary);

	// -- per frame --
	void tick();
	void tickPointer(Ptr &p);
	void fillOutput(VisionInteractionOutput &out);
	bool anyPanelPending() const;

	// -- classification & gesture starts --
	Target classify(const Ptr &p, uint32_t area);
	int routeFor(const Target &t) const;
	void startRole(Ptr &p);
	void startSecond(Ptr &primary, Ptr &second);
	void secondTap(Ptr &primary, Ptr &second);
	void beginTwoCamera(Ptr &a, Ptr &b);
	void endTwo(Ptr *ended);
	void makeCursor(Ptr &p, XrVector3f planeP, XrVector3f planeN, XrVector3f start);
	XrVector3f cursorPoint(Ptr &p, const PlaneCursor &c) const;

	// -- gestures --
	void tickSelect(Ptr &p);
	void tickBox(Ptr &p);
	void tickPan(Ptr &p);
	void tickAim(Ptr &p);
	void tickPanel(Ptr &p);
	void tickGround(Ptr &p);
	void tickTwoCamera();
	void tickWorkspace();
	void releaseSelect(Ptr &p);
	void releaseBox(Ptr &p);
	void releaseAim(Ptr &p);
	void releaseGround(Ptr &p);
	void applyPan(float dxMetres, float dyMetres, float &appliedX, float &appliedY);
	bool pickCursor(Ptr &p, XrVector3f planePoint, XrWorldHit &hit);
	XrVector3f clampToBoard(XrVector3f world) const;
	void startBox(Ptr &p);
	void updateBox(Ptr &p, const XrWorldHit &hit);
	void cancelPointerRole(Ptr &p);
	void beginPanel(Ptr &p);
	void panelPixel(Ptr &p, XrVector3f point);
	void finishPanel(Ptr &p);
	void activatePanel(Ptr &p);
	static int commandTactic(int control);

	// -- Ground View --
	void exitGround(bool fade);
	void previewGround(Ptr &p);
	bool teleport(Ptr &p);
	void startFade() { fadeStart_ = host_.time_s; fadeRunning_ = true; }

	// -- engine calls (tracked so cancel/flush are always balanced) --
	bool enginePick(const XrPosef &aim, XrWorldHit &hit);
	void enginePointer(bool active, float x, float y, bool select);
	void engineSpatialPointer(bool active);
	void engineTrigger(bool down, bool available, bool additive);
	void engineRoute(int target);
	void engineNeutral();
	bool effectiveAdditive() const;

	// -- cancellation --
	void cancelPointer(Ptr &p, bool killed);
	void freePointer(Ptr &p);
	void cancelAll(VisionResetReason reason, bool cancelEngineTargets);

	void raise(uint32_t bit) { events_ |= bit; }

	VisionEngineBridge *bridge_;
	VisionConfig cfg_;
	VisionStats stats_;
	VisionHostState host_;
	VisionHostState prevHost_;
	bool haveHost_ = false;
	std::deque<XRInteractionEvent> pending_;
	Ptr ptr_[2];
	uint32_t dead_[16] = {};
	int deadCount_ = 0;
	uint32_t nextSimId_ = 0xFFFF0000u;

	// workspace / camera
	XrSurface board_;
	bool boardInit_ = false;
	bool boardDirty_ = false;
	float worldZoom_ = 1.0f;
	bool zoomDirty_ = false;
	XrSurfaceGrab grab_;
	struct TwoHand {
		bool active = false, grab = false;
		XrVector3f p0[2] = {}, v0Board = {}, mid0 = {};
		float sep0 = 1, ang = 0, angPrev = 0;
		float zoom0 = 1;
		float rotApplied = 0;
		PlaneCursor midCursor;
		float panAppliedX = 0, panAppliedY = 0;
		float wp0[2][3] = {};
	} two_;
	bool recenterRequested_ = false, resetWorkspaceRequested_ = false;
	bool suspended_ = false;
	XrWorldHit boxHit_;

	// modifiers / additive
	uint32_t modifiers_ = 0;
	bool additiveToggle_ = false;
	bool secondHandAdditive_ = false;

	// hand samples (ARKit)
	struct HandSample { bool valid = false, tracked = false, pinching = false; double hostTime = 0; XrVector3f pos = {}; } hands_[2];

	// engine-side state we must release on cancel
	bool triggerDown_ = false, spatialActive_ = false, pointerActive_ = false, pointerSelect_ = false;
	float lastPx_ = 0, lastPy_ = 0;
	int route_ = -1;
	bool engineTouched_ = false;

	// placement
	int invalidStreak_ = 0;

	// ground view
	XrObserverState observer_;
	bool fadeRunning_ = false;
	double fadeStart_ = 0;
	float holdProgress_ = 0;
	bool groundTargetKnown_ = false, groundTargetValid_ = false;
	XrVector3f groundTargetRoom_ = {};

	// per-frame output scratch
	uint32_t events_ = 0;
	VisionBoxRect boxState_;
	int activationCount_ = 0;
	VisionPanelActivation activations_[kVisionMaxActivations];
	bool placementRotating_ = false, placementFollowing_ = false, placementCancelArmed_ = false;
	XrVector3f placementPoint_ = {};
	bool placementHasPoint_ = false;
	bool cursorVisible_ = false, cursorOnBoard_ = false;
	XrVector3f cursorWorld_ = {};
	bool rayVisible_ = false;
	bool neutralNextFrame_ = false; // release the spatial pointer one frame after a commit (engine consumes queued clicks)
	bool grabActive_ = false;
	int panelPointerPanel_ = -1;
	float panelU_ = 0, panelV_ = 0;
	VisionMode mode_ = VisionMode::Idle;
	int activeRegion_ = 0;
};
