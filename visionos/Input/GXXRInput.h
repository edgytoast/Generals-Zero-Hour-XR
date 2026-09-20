// GXXRInput.h - raw spatial-event intake for the visionOS shell.
//
// SwiftUI delivers LayerRenderer spatial events on the main actor. Swift copies
// each one into a GXXRRawSpatialEvent (plain C, no Apple types) and calls
// GXXRInputPushRawSpatialEvent. This layer normalizes them into the
// platform-neutral XRInteraction.h event stream and keeps the latest ray.
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

typedef struct GXXRRawSpatialEvent {
    uint64_t event_id;  // stable across the life of one pinch
    double timestamp;   // seconds (SpatialEventCollection.Event.timestamp)
    int32_t kind;       // GXXRRawKind*
    int32_t phase;      // GXXRRawPhase*
    int32_t chirality;  // GXXRRawChirality*
    bool has_location3d;
    float location3d[3];  // event.location3D
    bool has_ray;
    float ray_origin[3];  // event.selectionRay (gaze ray on pinch start)
    float ray_direction[3];
    bool has_pose;
    float pose_position[3];  // event.inputDevicePose.pose3D
    float pose_rotation[4];  // x, y, z, w
} GXXRRawSpatialEvent;

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
} GXXRInputStats;

void GXXRInputPushRawSpatialEvent(const GXXRRawSpatialEvent* event);
void GXXRInputGetStats(GXXRInputStats* out_stats);

#ifdef __cplusplus
}
#endif

#endif  // GXXR_INPUT_H
