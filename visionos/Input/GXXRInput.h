// GXXRInput.h - raw spatial-event intake for the visionOS shell.
//
// SwiftUI delivers LayerRenderer spatial events on the main actor. Swift copies each one into a
// GXXRRawSpatialEvent (plain C, no Apple types) and calls GXXRInputPushRawSpatialEvent. This layer
// normalizes them into the platform-neutral XRInteraction.h event stream (see docs/visionos-interaction.md):
//   * it assigns stable pointer ids, keeps press/release BALANCED (every BEGIN is followed by an END or a
//     CANCEL, also across GXXRInputFlush), coalesces drag storms and never lets a stale drag restart as a
//     new pinch;
//   * it forwards hand pose, modifier keys, tracking-area ids and continuous ARKit hand samples;
//   * it carries UI commands (cancel placement, Ground View, recenter, ...) in the same ordered queue.
// Everything here is thread-safe: producers run on the main thread / ARKit tasks, the consumer is the
// render thread calling XRInteraction_PollEvent.
#ifndef GXXR_INPUT_H
#define GXXR_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GXXRRawKindTouch = 0,
    GXXRRawKindDirectPinch = 1,
    GXXRRawKindIndirectPinch = 2,
    GXXRRawKindPointer = 3,
    GXXRRawKindOther = 4
};
enum { GXXRRawPhaseActive = 0, GXXRRawPhaseEnded = 1, GXXRRawPhaseCancelled = 2 };
enum { GXXRRawChiralityUnknown = 0, GXXRRawChiralityLeft = 1, GXXRRawChiralityRight = 2 };

// Modifier bits (same values as XRModifierFlags in Platform/XRInteraction.h).
enum { GXXRModShift = 1, GXXRModControl = 2, GXXRModOption = 4, GXXRModCommand = 8 };

// UI commands (same values as XRInteractionCommand in Platform/XRInteraction.h).
enum {
    GXXRCommandSetAdditive = 1,          // value 0/1: persistent additive-selection toggle
    GXXRCommandCancelPlacement = 2,
    GXXRCommandCancelAll = 3,
    GXXRCommandRecenterBoard = 4,
    GXXRCommandResetWorkspace = 5,
    GXXRCommandEnterGroundView = 6,
    GXXRCommandExitGroundView = 7,
    GXXRCommandRotatePlacementStep = 8,  // value = degrees, signed
    GXXRCommandEngineBack = 9
};

// Flush reasons.
enum { GXXRFlushReasonFlush = 0, GXXRFlushReasonTrackingLost = 1, GXXRFlushReasonFocus = 2 };

typedef struct GXXRRawSpatialEvent {
    uint64_t event_id;  // stable across the life of one pinch
    double timestamp;   // seconds (SpatialEventCollection.Event.timestamp)
    int32_t kind;       // GXXRRawKind*
    int32_t phase;      // GXXRRawPhase*
    int32_t chirality;  // GXXRRawChirality*
    bool has_location3d;
    float location3d[3];  // event.location3D
    bool has_ray;
    float ray_origin[3];  // event.selectionRay (gaze ray on pinch start, nil afterwards for pinches)
    float ray_direction[3];
    bool has_pose;
    float pose_position[3];  // event.inputDevicePose.pose3D
    float pose_rotation[4];  // x, y, z, w
    // ---- interaction API v2 ----
    bool modifiers_valid;
    uint32_t modifiers;          // GXXRMod* (event.modifierKeys)
    uint32_t tracking_area_id;   // event.trackingAreaIdentifier (visionOS 26), 0 = none
} GXXRRawSpatialEvent;

// One continuous ARKit hand sample (HandTrackingProvider). Not available in the simulator.
typedef struct GXXRRawHandSample {
    double timestamp;
    int32_t chirality;   // GXXRRawChirality*
    bool tracked;        // joints tracked this sample
    bool pinching;       // thumb-index pinch (1.5 cm engage / 3 cm release)
    bool palm_up;        // palm faces up (menu gesture candidate)
    float pinch_position[3];  // midpoint of thumb tip and index tip, world space
    float wrist_position[3];
} GXXRRawHandSample;

typedef struct GXXRInputStats {
    uint64_t raw_events;
    uint64_t begins, drags, ends, cancels;
    uint64_t two_hand_updates;
    uint32_t active_pointers;
    bool has_last_ray;
    float last_ray_origin[3];
    float last_ray_direction[3];
    bool last_board_hit_valid;
    float last_board_x_m, last_board_z_m;
    // ---- v2 ----
    uint64_t flushes;
    uint64_t swallowed_events;   // events dropped because they belonged to a flushed / unknown pinch
    uint64_t coalesced_drags;
    uint64_t dropped_events;     // queue overflow (a FLUSH is raised so the consumer resynchronises)
    uint64_t hand_samples;
    uint64_t commands;
    uint32_t queue_depth;
} GXXRInputStats;

void GXXRInputPushRawSpatialEvent(const GXXRRawSpatialEvent* event);
void GXXRInputPushHandSample(const GXXRRawHandSample* sample);
// Enqueue a UI command (see GXXRCommand*). Thread-safe.
void GXXRInputPostCommand(int32_t command, int32_t value);
// Modifier keys from a source other than the pinch events (e.g. a keyboard handler). Thread-safe.
void GXXRInputSetModifiers(uint32_t modifiers);
// Cancel everything in flight (see XRInteraction_Flush). reason: GXXRFlushReason*.
void GXXRInputFlush(int32_t reason);
void GXXRInputGetStats(GXXRInputStats* out_stats);

#ifdef __cplusplus
}
#endif

#endif  // GXXR_INPUT_H
