// GXXRInput.mm - normalizes spatial events into the XRInteraction.h stream.
#import <Foundation/Foundation.h>
#import <os/log.h>
#import <simd/simd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "GXXRInput.h"
#include "XRInteraction.h"

namespace {

os_log_t InputLog() {
    static os_log_t log = os_log_create("com.generalsx.zerohour.xr.vision", "input");
    return log;
}

struct Pointer {
    uint32_t id = 0;
    XRHand hand = XR_HAND_UNKNOWN;
    XRPointerKind kind = XR_POINTER_UNKNOWN;
    simd_float3 startPos = {0, 0, 0};
    simd_float3 pos = {0, 0, 0};
    bool hasRay = false;
    XRRay startRay = {};
    XRBoardHit startHit = {};
    uint64_t dragCount = 0;
};

struct Manipulation {
    bool active = false;
    uint64_t idA = 0, idB = 0;
    float startSep = 1.0f;
    float startYaw = 0.0f;
    simd_float3 startMid = {0, 0, 0};
};

// The two enum families are duplicated in GXXRInput.h (Swift-visible) and XRInteraction.h (engine-facing).
#define GXXR_SAME(a, b) (static_cast<int>(a) == static_cast<int>(b))
static_assert(GXXR_SAME(GXXRModShift, XR_MOD_SHIFT) && GXXR_SAME(GXXRModControl, XR_MOD_CONTROL) &&
              GXXR_SAME(GXXRModOption, XR_MOD_OPTION) && GXXR_SAME(GXXRModCommand, XR_MOD_COMMAND),
              "modifier bits must match");
static_assert(GXXR_SAME(GXXRCommandSetAdditive, XR_CMD_SET_ADDITIVE) &&
              GXXR_SAME(GXXRCommandCancelPlacement, XR_CMD_CANCEL_PLACEMENT) &&
              GXXR_SAME(GXXRCommandCancelAll, XR_CMD_CANCEL_ALL) &&
              GXXR_SAME(GXXRCommandRecenterBoard, XR_CMD_RECENTER_BOARD) &&
              GXXR_SAME(GXXRCommandResetWorkspace, XR_CMD_RESET_WORKSPACE) &&
              GXXR_SAME(GXXRCommandEnterGroundView, XR_CMD_ENTER_GROUND_VIEW) &&
              GXXR_SAME(GXXRCommandExitGroundView, XR_CMD_EXIT_GROUND_VIEW) &&
              GXXR_SAME(GXXRCommandRotatePlacementStep, XR_CMD_ROTATE_PLACEMENT_STEP) &&
              GXXR_SAME(GXXRCommandEngineBack, XR_CMD_ENGINE_BACK),
              "command ids must match");
#undef GXXR_SAME

constexpr size_t kQueueCapacity = 256;

struct InputState {
    std::mutex mutex;
    std::map<uint64_t, Pointer> pointers;
    // Raw ids of pinches that were flushed while the fingers were still down: their remaining drag/end events are
    // swallowed so a stale drag can never restart as a brand-new pinch.
    std::set<uint64_t> ignored;
    uint32_t nextPointerId = 1;
    uint32_t modifiers = 0;
    std::deque<XRInteractionEvent> queue;
    XRInteractionCallback callback = nullptr;
    void* callbackUser = nullptr;
    Manipulation manip;
    GXXRInputStats stats = {};

    bool hasBoard = false;
    simd_float4x4 worldFromBoard = matrix_identity_float4x4;
    simd_float4x4 boardFromWorld = matrix_identity_float4x4;
    float halfX = 0.5f, halfZ = 0.3f;
};

InputState& S() {
    static InputState s;
    return s;
}

simd_float3 V(const XRVec3& v) { return simd_make_float3(v.x, v.y, v.z); }
XRVec3 X(simd_float3 v) { return XRVec3{v.x, v.y, v.z}; }
simd_float3 F3(const float* a) { return simd_make_float3(a[0], a[1], a[2]); }

// Rigid inverse for board_from_world.
simd_float4x4 rigidInverse(simd_float4x4 m) { return simd_inverse(m); }

XRBoardHit hitFromBoardPoint(InputState& s, simd_float3 p) {
    XRBoardHit h = {};
    if (!s.hasBoard) return h;
    h.valid = true;
    h.x_m = p.x;
    h.z_m = p.z;
    h.u = (p.x + s.halfX) / (2 * s.halfX);
    h.v = (p.z + s.halfZ) / (2 * s.halfZ);
    h.on_board = std::fabs(p.x) <= s.halfX && std::fabs(p.z) <= s.halfZ;
    return h;
}

// Ray/plane intersection in board space (plane y = 0). Returns the board-space point.
bool intersectBoard(InputState& s, const XRRay& ray, simd_float3* outBoardPoint) {
    if (!s.hasBoard) return false;
    simd_float4 o = simd_mul(s.boardFromWorld, simd_make_float4(V(ray.origin), 1.0f));
    simd_float4 d = simd_mul(s.boardFromWorld, simd_make_float4(V(ray.direction), 0.0f));
    if (std::fabs(d.y) < 1e-6f) return false;
    const float t = -o.y / d.y;
    if (t < 0.0f) return false;
    *outBoardPoint = simd_make_float3(o.x + t * d.x, 0.0f, o.z + t * d.z);
    return true;
}

bool isDroppable(const XRInteractionEvent& e) {
    return e.type == XR_EVENT_PINCH_DRAG || e.type == XR_EVENT_TWO_HAND_UPDATE || e.type == XR_EVENT_HAND_UPDATE;
}

// Must be called with the lock held. Drag storms are coalesced (only the newest sample of a run matters); on overflow
// the oldest droppable sample goes first, and if a BEGIN/END/CANCEL/COMMAND had to be dropped a FLUSH is queued so the
// consumer can never be left with an unbalanced press.
void enqueue(InputState& s, const XRInteractionEvent& e) {
    if (!s.queue.empty() && isDroppable(e)) {
        XRInteractionEvent& b = s.queue.back();
        if (b.type == e.type && ((e.type == XR_EVENT_PINCH_DRAG && b.pointer_id == e.pointer_id) ||
                                 (e.type == XR_EVENT_HAND_UPDATE && b.hand == e.hand) || e.type == XR_EVENT_TWO_HAND_UPDATE)) {
            b = e;
            s.stats.coalesced_drags++;
            return;
        }
    }
    if (s.queue.size() >= kQueueCapacity) {
        auto it = std::find_if(s.queue.begin(), s.queue.end(), isDroppable);
        if (it != s.queue.end()) {
            s.queue.erase(it);
            s.stats.dropped_events++;
        } else {
            // Only BEGIN/END/CANCEL/COMMAND events are queued: the consumer has stopped draining. Any event we drop
            // could unbalance a press, so drop them ALL and tell the consumer to reset its gesture state. Pinches
            // still held by the fingers keep sending drags/ends; the consumer ignores those (it never saw the begin).
            s.stats.dropped_events += s.queue.size();
            s.queue.clear();
            XRInteractionEvent f = {};
            f.type = XR_EVENT_FLUSH;
            f.timestamp_s = e.timestamp_s;
            s.queue.push_back(f);
            s.stats.flushes++;
            if (e.type == XR_EVENT_PINCH_BEGIN) return;  // its END would arrive without a begin anyway
        }
    }
    s.queue.push_back(e);
}

void fillBoardDelta(InputState& s, XRInteractionEvent& e, simd_float3 deltaWorld) {
    e.drag_delta_world = X(deltaWorld);
    simd_float4 db = simd_mul(s.boardFromWorld, simd_make_float4(deltaWorld, 0.0f));
    e.drag_delta_board = XRVec3{db.x, db.y, db.z};
}

}  // namespace

extern "C" {

void XRInteraction_SetBoardTransform(const float world_from_board[16], float half_extent_x_m, float half_extent_z_m) {
    InputState& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    memcpy(&s.worldFromBoard, world_from_board, sizeof(float) * 16);
    s.boardFromWorld = rigidInverse(s.worldFromBoard);
    s.halfX = half_extent_x_m;
    s.halfZ = half_extent_z_m;
    s.hasBoard = true;
}

void XRInteraction_ClearBoardTransform(void) {
    InputState& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.hasBoard = false;
}

bool XRInteraction_PollEvent(XRInteractionEvent* out_event) {
    InputState& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.queue.empty() || !out_event) return false;
    *out_event = s.queue.front();
    s.queue.pop_front();
    return true;
}

void XRInteraction_SetEventCallback(XRInteractionCallback callback, void* user) {
    InputState& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.callback = callback;
    s.callbackUser = user;
}

XRBoardHit XRInteraction_IntersectBoard(const XRRay* ray_world) {
    InputState& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    XRBoardHit h = {};
    simd_float3 p;
    if (ray_world && intersectBoard(s, *ray_world, &p)) h = hitFromBoardPoint(s, p);
    return h;
}

void GXXRInputGetStats(GXXRInputStats* out_stats) {
    InputState& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!out_stats) return;
    *out_stats = s.stats;
    out_stats->active_pointers = (uint32_t)s.pointers.size();
    out_stats->queue_depth = (uint32_t)s.queue.size();
}

void XRInteraction_PostCommand(XRInteractionCommand command, int32_t value) {
    InputState& s = S();
    XRInteractionEvent e = {};
    e.type = XR_EVENT_COMMAND;
    e.command = command;
    e.command_value = value;
    XRInteractionCallback cb;
    void* user;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.stats.commands++;
        enqueue(s, e);
        cb = s.callback;
        user = s.callbackUser;
    }
    if (cb) cb(user, &e);
}

void XRInteraction_SetModifiers(uint32_t modifiers) {
    InputState& s = S();
    XRInteractionEvent e = {};
    e.type = XR_EVENT_MODIFIERS;
    e.modifiers = modifiers;
    e.modifiers_valid = true;
    XRInteractionCallback cb;
    void* user;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.modifiers == modifiers) return;
        s.modifiers = modifiers;
        enqueue(s, e);
        cb = s.callback;
        user = s.callbackUser;
    }
    if (cb) cb(user, &e);
}

// Cancel everything in flight. Balanced: each active pointer is delivered as PINCH_CANCEL first, then one
// FLUSH / TRACKING_LOST event tells the consumer to reset all remaining gesture state. Pinches whose fingers are
// still down are remembered and swallowed until the system reports their end.
void XRInteraction_Flush(int32_t reason) {
    InputState& s = S();
    std::vector<XRInteractionEvent> produced;
    XRInteractionCallback cb;
    void* user;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        for (auto& kv : s.pointers) {
            Pointer& p = kv.second;
            XRInteractionEvent e = {};
            e.type = XR_EVENT_PINCH_CANCEL;
            e.pointer_id = p.id;
            e.hand = p.hand;
            e.pointer_kind = p.kind;
            e.has_ray = p.hasRay;
            e.ray_world = p.startRay;
            e.position_world = X(p.pos);
            e.modifiers_valid = true;
            e.modifiers = s.modifiers;
            produced.push_back(e);
            s.ignored.insert(kv.first);
            s.stats.cancels++;
        }
        s.pointers.clear();
        if (s.manip.active) {
            s.manip.active = false;
            XRInteractionEvent t = {};
            t.type = XR_EVENT_TWO_HAND_END;
            t.scale = 1.0f;
            produced.push_back(t);
        }
        XRInteractionEvent f = {};
        f.type = reason == 0 ? XR_EVENT_FLUSH : XR_EVENT_TRACKING_LOST;
        f.command_value = reason;
        produced.push_back(f);
        s.stats.flushes++;
        // bounded: forget the oldest ignored ids first if a client never reports ends
        while (s.ignored.size() > 64) s.ignored.erase(s.ignored.begin());
        for (auto& e : produced) enqueue(s, e);
        cb = s.callback;
        user = s.callbackUser;
    }
    if (cb) for (auto& e : produced) cb(user, &e);
}

void GXXRInputFlush(int32_t reason) { XRInteraction_Flush(reason); }
void GXXRInputPostCommand(int32_t command, int32_t value) { XRInteraction_PostCommand((XRInteractionCommand)command, value); }
void GXXRInputSetModifiers(uint32_t modifiers) { XRInteraction_SetModifiers(modifiers); }

void GXXRInputPushHandSample(const GXXRRawHandSample* h) {
    if (!h) return;
    InputState& s = S();
    XRInteractionEvent e = {};
    e.type = XR_EVENT_HAND_UPDATE;
    e.timestamp_s = h->timestamp;
    e.hand = h->chirality == GXXRRawChiralityLeft ? XR_HAND_LEFT : (h->chirality == GXXRRawChiralityRight ? XR_HAND_RIGHT : XR_HAND_UNKNOWN);
    e.hand_tracked = h->tracked;
    e.hand_pinching = h->pinching;
    e.hand_palm_up = h->palm_up;
    e.has_hand_pose = true;
    e.hand_pose.position = XRVec3{h->pinch_position[0], h->pinch_position[1], h->pinch_position[2]};
    e.hand_pose.orientation = XRQuat{0, 0, 0, 1};
    e.position_world = e.hand_pose.position;
    XRInteractionCallback cb;
    void* user;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.stats.hand_samples++;
        enqueue(s, e);
        cb = s.callback;
        user = s.callbackUser;
    }
    if (cb) cb(user, &e);
}

void GXXRInputPushRawSpatialEvent(const GXXRRawSpatialEvent* raw) {
    if (!raw) return;
    InputState& s = S();
    std::vector<XRInteractionEvent> produced;
    XRInteractionCallback cb = nullptr;
    void* cbUser = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.stats.raw_events++;
        // A pinch that was flushed while the fingers were down: swallow it until the system ends it.
        auto ign = s.ignored.find(raw->event_id);
        if (ign != s.ignored.end()) {
            if (raw->phase != GXXRRawPhaseActive) s.ignored.erase(ign);
            s.stats.swallowed_events++;
            return;
        }
        if (raw->modifiers_valid) s.modifiers = raw->modifiers;

        XRInteractionEvent e = {};
        e.timestamp_s = raw->timestamp;
        e.modifiers_valid = raw->modifiers_valid;
        e.modifiers = s.modifiers;
        e.tracking_area_id = raw->tracking_area_id;
        e.hand = raw->chirality == GXXRRawChiralityLeft ? XR_HAND_LEFT : (raw->chirality == GXXRRawChiralityRight ? XR_HAND_RIGHT : XR_HAND_UNKNOWN);
        XRPointerKind kind = XR_POINTER_DEVICE;
        if (raw->kind == GXXRRawKindDirectPinch) kind = XR_POINTER_DIRECT_PINCH;
        else if (raw->kind == GXXRRawKindIndirectPinch) kind = XR_POINTER_INDIRECT_PINCH;
        e.pointer_kind = kind;
        if (raw->has_pose) {
            e.has_hand_pose = true;
            e.hand_pose.position = XRVec3{raw->pose_position[0], raw->pose_position[1], raw->pose_position[2]};
            e.hand_pose.orientation = XRQuat{raw->pose_rotation[0], raw->pose_rotation[1], raw->pose_rotation[2], raw->pose_rotation[3]};
        }

        // Position used for drags: the hand pose when available (indirect pinch: real hand motion), else the
        // reported 3D location.
        simd_float3 pos = raw->has_pose ? F3(raw->pose_position) : (raw->has_location3d ? F3(raw->location3d) : simd_make_float3(0, 0, 0));
        const bool havePos = raw->has_pose || raw->has_location3d;

        auto it = s.pointers.find(raw->event_id);
        if (raw->phase == GXXRRawPhaseActive) {
            if (it == s.pointers.end()) {
                Pointer p;
                p.id = s.nextPointerId++;
                p.hand = e.hand;
                p.kind = kind;
                p.startPos = pos;
                p.pos = pos;
                if (raw->has_ray) {
                    p.hasRay = true;
                    p.startRay.origin = XRVec3{raw->ray_origin[0], raw->ray_origin[1], raw->ray_origin[2]};
                    simd_float3 d = simd_normalize(F3(raw->ray_direction));
                    p.startRay.direction = X(d);
                    simd_float3 bp;
                    if (intersectBoard(s, p.startRay, &bp)) p.startHit = hitFromBoardPoint(s, bp);
                } else if (raw->has_location3d && s.hasBoard) {
                    simd_float4 b = simd_mul(s.boardFromWorld, simd_make_float4(F3(raw->location3d), 1.0f));
                    p.startHit = hitFromBoardPoint(s, simd_make_float3(b.x, 0.0f, b.z));
                }
                it = s.pointers.emplace(raw->event_id, p).first;
                e.type = XR_EVENT_PINCH_BEGIN;
                s.stats.begins++;
            } else {
                it->second.pos = pos;
                it->second.dragCount++;
                e.type = XR_EVENT_PINCH_DRAG;
                s.stats.drags++;
                if (raw->has_ray) {  // pointing devices (mouse/trackpad) report a fresh ray on every event
                    e.has_current_ray = true;
                    e.current_ray.origin = XRVec3{raw->ray_origin[0], raw->ray_origin[1], raw->ray_origin[2]};
                    e.current_ray.direction = X(simd_normalize(F3(raw->ray_direction)));
                }
            }
            Pointer& p = it->second;
            e.pointer_id = p.id;
            e.hand = p.hand != XR_HAND_UNKNOWN ? p.hand : e.hand;
            e.pointer_kind = p.kind;
            e.has_ray = p.hasRay;
            e.ray_world = p.startRay;
            e.position_world = X(havePos ? p.pos : simd_make_float3(0, 0, 0));
            simd_float3 delta = havePos ? (p.pos - p.startPos) : simd_make_float3(0, 0, 0);
            fillBoardDelta(s, e, delta);
            // Board hit: the gaze-targeted point at pinch start, moved by the hand delta (advisory; the interaction
            // layer does its own board math from the host state).
            e.board_hit = p.startHit;
            if (p.startHit.valid) {
                simd_float3 moved = simd_make_float3(p.startHit.x_m + e.drag_delta_board.x, 0.0f, p.startHit.z_m + e.drag_delta_board.z);
                e.board_hit = hitFromBoardPoint(s, moved);
            }
            if (p.hasRay) {
                s.stats.has_last_ray = true;
                memcpy(s.stats.last_ray_origin, raw->ray_origin, sizeof(float) * 3);
                memcpy(s.stats.last_ray_direction, raw->ray_direction, sizeof(float) * 3);
            }
            s.stats.last_board_hit_valid = e.board_hit.valid;
            s.stats.last_board_x_m = e.board_hit.x_m;
            s.stats.last_board_z_m = e.board_hit.z_m;
            if (e.type == XR_EVENT_PINCH_BEGIN || (p.dragCount % 30) == 0) {
                os_log(InputLog(), "spatial %{public}s id=%u hand=%d kind=%d pos=(%.3f,%.3f,%.3f) board=(%.3f,%.3f) valid=%d",
                       e.type == XR_EVENT_PINCH_BEGIN ? "BEGIN" : "DRAG", p.id, (int)e.hand, (int)e.pointer_kind,
                       e.position_world.x, e.position_world.y, e.position_world.z, e.board_hit.x_m, e.board_hit.z_m, (int)e.board_hit.valid);
            }
            produced.push_back(e);
        } else if (it != s.pointers.end()) {
            Pointer& p = it->second;
            const bool ended = raw->phase == GXXRRawPhaseEnded;
            if (havePos) p.pos = pos;
            e.type = ended ? XR_EVENT_PINCH_END : XR_EVENT_PINCH_CANCEL;
            e.pointer_id = p.id;
            e.hand = p.hand;
            e.pointer_kind = p.kind;
            e.has_ray = p.hasRay;
            e.ray_world = p.startRay;
            e.position_world = X(p.pos);
            fillBoardDelta(s, e, p.pos - p.startPos);
            e.board_hit = p.startHit;
            if (p.startHit.valid) {
                e.board_hit = hitFromBoardPoint(s, simd_make_float3(p.startHit.x_m + e.drag_delta_board.x, 0.0f, p.startHit.z_m + e.drag_delta_board.z));
            }
            if (ended) s.stats.ends++; else s.stats.cancels++;
            os_log(InputLog(), "spatial %{public}s id=%u board=(%.3f,%.3f) valid=%d", ended ? "END" : "CANCEL", p.id, e.board_hit.x_m, e.board_hit.z_m, (int)e.board_hit.valid);
            s.pointers.erase(it);
            produced.push_back(e);
        } else {
            s.stats.swallowed_events++;  // an END/CANCEL for a pinch we never saw begin
        }

        // ---- Two-hand manipulation (advisory TWO_HAND_* events): two simultaneously held pinches. ----
        std::vector<std::pair<uint64_t, Pointer*>> held;
        for (auto& kv : s.pointers) held.push_back({kv.first, &kv.second});
        Manipulation& m = s.manip;
        if (held.size() >= 2) {
            Pointer& a = *held[0].second;
            Pointer& b = *held[1].second;
            simd_float3 axis = b.pos - a.pos;
            const float sep = simd_length(axis);
            const float yaw = std::atan2(axis.x, axis.z);
            simd_float3 mid = (a.pos + b.pos) * 0.5f;
            XRInteractionEvent t = {};
            t.timestamp_s = raw->timestamp;
            t.pointer_id = a.id;
            t.pointer_kind = a.kind;
            t.hand = XR_HAND_UNKNOWN;
            if (!m.active || m.idA != held[0].first || m.idB != held[1].first) {
                m.active = true;
                m.idA = held[0].first;
                m.idB = held[1].first;
                m.startSep = std::max(sep, 1e-3f);
                m.startYaw = yaw;
                m.startMid = mid;
                t.type = XR_EVENT_TWO_HAND_BEGIN;
            } else {
                t.type = XR_EVENT_TWO_HAND_UPDATE;
                s.stats.two_hand_updates++;
            }
            t.scale = sep / m.startSep;
            float dyaw = yaw - m.startYaw;
            while (dyaw > (float)M_PI) dyaw -= 2.0f * (float)M_PI;
            while (dyaw < -(float)M_PI) dyaw += 2.0f * (float)M_PI;
            t.yaw_rad = dyaw;
            t.translation_world = X(mid - m.startMid);
            t.midpoint_world = X(mid);
            produced.push_back(t);
        } else if (m.active) {
            m.active = false;
            XRInteractionEvent t = {};
            t.type = XR_EVENT_TWO_HAND_END;
            t.timestamp_s = raw->timestamp;
            t.scale = 1.0f;
            produced.push_back(t);
        }

        for (auto& ev : produced) enqueue(s, ev);
        cb = s.callback;
        cbUser = s.callbackUser;
    }
    if (cb) {
        for (auto& ev : produced) cb(cbUser, &ev);
    }
}

}  // extern "C"
