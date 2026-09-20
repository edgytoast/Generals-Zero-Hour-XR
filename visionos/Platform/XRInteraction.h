/*
 * XRInteraction.h - platform-neutral spatial pointer / pinch / drag stream.
 *
 * The shell normalizes platform input (visionOS gaze+pinch spatial events,
 * a Quest controller ray, a desktop mouse) into this event stream. The engine
 * polls it once per simulation frame and turns it into its own mouse/selection
 * messages. Everything is expressed both in world space and in board space so
 * the engine never needs to know where the tabletop floats.
 *
 * Board space: origin at the board center, +X right along the long edge,
 * +Z toward the player (screen-down in the flat game), +Y up out of the board;
 * meters. See XRPresentation_GetTabletopPlacement for the board transform.
 *
 * Privacy note (visionOS): the app never receives a continuous gaze ray. A ray
 * arrives only when a pinch begins (selectionRay), then follows the hand.
 * XR_EVENT_PINCH_BEGIN carries the gaze-derived ray; later events carry the ray
 * from the current hand pose where available.
 */
#ifndef GX_XR_INTERACTION_H
#define GX_XR_INTERACTION_H

#include <stdbool.h>
#include <stdint.h>
#include "XRPresentation.h" /* XRVec3 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XRHand {
    XR_HAND_UNKNOWN = 0,
    XR_HAND_LEFT = 1,
    XR_HAND_RIGHT = 2
} XRHand;

typedef enum XRPointerKind {
    XR_POINTER_UNKNOWN = 0,
    XR_POINTER_INDIRECT_PINCH = 1, /* gaze targets, pinch confirms (visionOS default) */
    XR_POINTER_DIRECT_PINCH = 2,   /* fingers touching the target */
    XR_POINTER_DEVICE = 3          /* mouse / trackpad / controller / simulator pointer */
} XRPointerKind;

typedef enum XRInteractionType {
    XR_EVENT_NONE = 0,
    XR_EVENT_PINCH_BEGIN = 1,
    XR_EVENT_PINCH_DRAG = 2,
    XR_EVENT_PINCH_END = 3,
    XR_EVENT_PINCH_CANCEL = 4,
    XR_EVENT_TWO_HAND_BEGIN = 5,  /* both hands pinching: manipulation gesture starts */
    XR_EVENT_TWO_HAND_UPDATE = 6, /* deltas since gesture start are cumulative */
    XR_EVENT_TWO_HAND_END = 7
} XRInteractionType;

typedef struct XRRay {
    XRVec3 origin;
    XRVec3 direction; /* unit length */
} XRRay;

/* Where a ray meets the board plane (Y = 0 in board space). */
typedef struct XRBoardHit {
    bool valid;     /* false when the ray misses the plane or the board is not placed yet */
    bool on_board;  /* inside the board rectangle */
    float x_m, z_m; /* board space, meters from board center */
    float u, v;     /* 0..1 across the board (u along X, v along Z); may be outside 0..1 */
} XRBoardHit;

typedef struct XRInteractionEvent {
    XRInteractionType type;
    uint32_t pointer_id;   /* stable while a pinch is held */
    XRHand hand;
    XRPointerKind pointer_kind;
    double timestamp_s;

    bool has_ray;
    XRRay ray_world;           /* gaze ray at PINCH_BEGIN, hand ray afterwards (if known) */
    XRVec3 position_world;     /* pinch / contact point (or ray point on the board plane) */
    XRBoardHit board_hit;      /* ray_world intersected with the board plane */
    XRVec3 drag_delta_world;   /* cumulative since PINCH_BEGIN */
    XRVec3 drag_delta_board;   /* cumulative since PINCH_BEGIN, board axes */

    /* Two-hand manipulation (TWO_HAND_*), cumulative since TWO_HAND_BEGIN. */
    float scale;               /* current hand separation / initial separation */
    float yaw_rad;             /* rotation of the hand axis about world +Y */
    XRVec3 translation_world;  /* motion of the hand midpoint */
    XRVec3 midpoint_world;
} XRInteractionEvent;

/* The shell tells the input layer where the board is (column-major world_from_board). */
void XRInteraction_SetBoardTransform(const float world_from_board[16],
                                     float half_extent_x_m, float half_extent_z_m);
void XRInteraction_ClearBoardTransform(void);

/* Pull model: returns false when the queue is empty. The queue is bounded (drops oldest). */
bool XRInteraction_PollEvent(XRInteractionEvent* out_event);

/* Push model (optional): called on the platform's main thread. Pass NULL to clear. */
typedef void (*XRInteractionCallback)(void* user, const XRInteractionEvent* event);
void XRInteraction_SetEventCallback(XRInteractionCallback callback, void* user);

/* Intersect a world-space ray with the board; useful for hover feedback. */
XRBoardHit XRInteraction_IntersectBoard(const XRRay* ray_world);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GX_XR_INTERACTION_H */
