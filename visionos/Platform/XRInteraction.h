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
    XR_EVENT_TWO_HAND_END = 7,
    /* ---- Added in interaction API v2 (values never reused; v1 consumers ignore them) ---- */
    XR_EVENT_TRACKING_LOST = 8, /* head/hand tracking or focus lost: cancel everything in flight */
    XR_EVENT_FLUSH = 9,         /* input queue flushed (scene change, modal): reset gesture state */
    XR_EVENT_COMMAND = 10,      /* UI command (SwiftUI button, keyboard): see XRInteractionCommand */
    XR_EVENT_HAND_UPDATE = 11,  /* continuous ARKit hand sample (no pinch pointer required) */
    XR_EVENT_MODIFIERS = 12     /* modifier key state changed (Shift = additive, Option = 2nd hand) */
} XRInteractionType;

/* Modifier flags carried in XRInteractionEvent.modifiers. */
typedef enum XRModifierFlags {
    XR_MOD_SHIFT = 1u << 0,   /* additive selection (keyboard Shift in the simulator) */
    XR_MOD_CONTROL = 1u << 1,
    XR_MOD_OPTION = 1u << 2,  /* simulator: emulate the second hand (mirror about the pinch start) */
    XR_MOD_COMMAND = 1u << 3
} XRModifierFlags;

/* UI -> interaction layer commands, carried by XR_EVENT_COMMAND so they share the
 * ordering and thread-safety of the pointer queue. */
typedef enum XRInteractionCommand {
    XR_CMD_NONE = 0,
    XR_CMD_SET_ADDITIVE = 1,           /* value 0/1: persistent additive-selection toggle */
    XR_CMD_CANCEL_PLACEMENT = 2,       /* cancel the pending building placement / armed command */
    XR_CMD_CANCEL_ALL = 3,             /* cancel gestures, targets and Ground View arming */
    XR_CMD_RECENTER_BOARD = 4,         /* put the board 0.9 m ahead of the head again */
    XR_CMD_RESET_WORKSPACE = 5,        /* recenter AND restore the default board size */
    XR_CMD_ENTER_GROUND_VIEW = 6,      /* arm Ground View (teleport target is chosen by pinch) */
    XR_CMD_EXIT_GROUND_VIEW = 7,       /* leave Ground View, back to the unchanged tabletop */
    XR_CMD_ROTATE_PLACEMENT_STEP = 8,  /* value = degrees (signed): button/keyboard rotation of the ghost */
    XR_CMD_ENGINE_BACK = 9             /* Escape/Back key into the engine (menus, dialogs) */
} XRInteractionCommand;

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

    /* ---- Interaction API v2 (appended; a v1 consumer that zero-initialises and
     * ignores unknown fields keeps working). ---- */
    uint32_t modifiers;        /* XRModifierFlags; meaningful when modifiers_valid */
    bool modifiers_valid;
    bool has_hand_pose;        /* hand_pose is the pinch pose (position + orientation) in world space */
    XRPose hand_pose;
    bool has_current_ray;      /* current_ray is a fresh pointing ray on a non-BEGIN event (mouse/trackpad) */
    XRRay current_ray;
    uint32_t tracking_area_id; /* visionOS 26 tracking-area identifier the system hit (0 = none/unknown) */
    int32_t command;           /* XR_EVENT_COMMAND: XRInteractionCommand */
    int32_t command_value;     /* XR_EVENT_COMMAND: argument */
    bool hand_tracked;         /* XR_EVENT_HAND_UPDATE: joints tracked this sample */
    bool hand_pinching;        /* XR_EVENT_HAND_UPDATE: thumb-index pinch (hysteresis applied) */
    bool hand_palm_up;         /* XR_EVENT_HAND_UPDATE: palm faces up (menu gesture candidate) */
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

/* ---- Interaction API v2 ---- */

/* UI command (see XRInteractionCommand). Thread-safe; enqueued in order with pointer events. */
void XRInteraction_PostCommand(XRInteractionCommand command, int32_t value);

/* Reset all gesture state: every active pointer is delivered as PINCH_CANCEL, a FLUSH event follows,
 * and events for pointers that were still physically pinching are swallowed until the system
 * reports their end, so a stale drag can never restart as a new begin. reason: 0 flush, 1 tracking
 * loss, 2 focus/scene phase change. Thread-safe. */
void XRInteraction_Flush(int32_t reason);

/* Keyboard modifier state (XRModifierFlags) from the platform, when it is not on the pinch events. */
void XRInteraction_SetModifiers(uint32_t modifiers);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GX_XR_INTERACTION_H */
