/*
 * XRPresentation.h - platform-neutral stereo presentation contract.
 *
 * This is the surface the Generals engine (through a GL-on-Metal backend, or
 * any other renderer) targets. It contains NO Apple types: only plain structs,
 * column-major float[16] matrices and opaque handles. The visionOS shell
 * implements it on top of Compositor Services (see visionos/Bridge). A Quest
 * (OpenXR) or desktop stereo shell could implement the same header.
 *
 * Conventions (all shells must follow them):
 *   - World space: right-handed, +Y up, meters, origin on the floor.
 *   - Poses: position in world space; orientation is a unit quaternion (x,y,z,w)
 *     that rotates the local frame into world space. Local frames look down -Z.
 *   - Matrices: column-major float[16] (element [col*4 + row]), so they can be
 *     passed straight to glUniformMatrix4fv(..., GL_FALSE, m) or to Metal.
 *   - clip_from_view maps view space to clip space with the compositor's depth
 *     convention: see XRFrameInfo.reverse_z (visionOS: reverse-Z, depth 1 = near,
 *     0 = far, Metal clip space z in [0,1]). An engine that wants a classic
 *     forward-Z projection must build its own from XREyeView.fov and depth_near/far.
 *   - Time: seconds on the host monotonic clock (mach absolute time on Apple).
 *
 * Threading: all XRPresentation_* calls that take a frame (SubmitEyeTexture)
 * must be made from inside the frame callback, on the thread that invokes it.
 */
#ifndef GX_XR_PRESENTATION_H
#define GX_XR_PRESENTATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_MAX_EYES 2

typedef struct XRVec3 { float x, y, z; } XRVec3;
typedef struct XRQuat { float x, y, z, w; } XRQuat;
typedef struct XRPose { XRVec3 position; XRQuat orientation; } XRPose;

/* Half-angles in radians, OpenXR style: angle_left and angle_down are negative
 * for a symmetric frustum. */
typedef struct XRFov {
    float angle_left, angle_right, angle_up, angle_down;
} XRFov;

/* Pixel rectangle inside a render target; origin top-left as Metal reports it. */
typedef struct XRRect { int32_t x, y, width, height; } XRRect;

/* Opaque platform texture / command buffer handle. On Apple platforms these are
 * `id<MTLTexture>` / `id<MTLCommandBuffer>` passed as unretained void* (valid only
 * for the duration of the frame callback unless the callee retains them). The
 * platform-neutral engine code never dereferences them. */
typedef void* XRNativeTexture;
typedef void* XRNativeCommandBuffer;

typedef enum XRAlphaMode {
    /* Opaque scene: alpha is ignored; content replaces passthrough. */
    XR_ALPHA_MODE_OPAQUE = 0,
    /* Mixed immersion: color is PREMULTIPLIED by alpha, alpha 0 shows passthrough. */
    XR_ALPHA_MODE_PREMULTIPLIED_PASSTHROUGH = 1
} XRAlphaMode;

typedef enum XRSessionState {
    XR_SESSION_IDLE = 0,       /* no immersive space */
    XR_SESSION_PAUSED = 1,     /* space exists but compositor is not asking for frames */
    XR_SESSION_RUNNING = 2,    /* frames are being requested */
    XR_SESSION_INVALIDATED = 3 /* space was dismissed; the frame loop has exited */
} XRSessionState;

/* One eye (view) for the frame. */
typedef struct XREyeView {
    uint32_t eye_index;      /* 0 = left, 1 = right (as reported by the platform) */
    XRPose pose;             /* eye pose in world space */
    XRFov fov;               /* asymmetric frustum half-angles */
    float world_from_view[16];
    float view_from_world[16];
    float clip_from_view[16]; /* platform projection (reverse-Z on visionOS) */
    float clip_from_world[16];
    XRRect viewport;         /* where this eye's pixels live inside color_target */
    uint32_t texture_index;  /* index into the drawable's texture set */
    uint32_t array_slice;    /* slice for layered layout, else 0 */
    uint32_t target_width;    /* full size of color_target */
    uint32_t target_height;
    /* True when this is the first eye this frame that touches this texture slice.
     * A direct-rendering client should CLEAR (color to transparent black, depth to
     * the far value) when true and LOAD when false; that matters for the "shared"
     * side-by-side layout where both eyes live in one texture. */
    bool first_use_of_target;
    /* Platform render targets. The default frame client draws into these
     * directly. An engine that renders elsewhere ignores them and calls
     * XRPresentation_SubmitEyeTexture instead. Valid only inside the callback. */
    XRNativeTexture color_target;
    XRNativeTexture depth_target; /* may be NULL */
} XREyeView;

typedef struct XRFrameInfo {
    uint64_t frame_index;
    double predicted_display_time_s;
    double optimal_input_time_s;
    double rendering_deadline_s;
    bool head_tracked;       /* false = fallback pose is in use (no device anchor) */
    XRPose head_pose;
    float world_from_head[16];
    uint32_t eye_count;      /* 1 (mono fallback) or 2 */
    XREyeView eyes[XR_MAX_EYES];
    float depth_near_m;
    float depth_far_m;
    bool reverse_z;
    bool foveation_enabled;  /* false initially so a plain copy is geometrically correct */
    XRAlphaMode alpha_mode;
    XRNativeCommandBuffer command_buffer; /* platform command buffer for this frame's drawable */
} XRFrameInfo;

/* What the frame callback did. */
typedef enum XRFrameResult {
    XR_FRAME_SKIP = 0,             /* present nothing this frame */
    XR_FRAME_RENDERED_DIRECT = 1,  /* client encoded into eyes[i].color_target itself */
    XR_FRAME_SUBMITTED_TEXTURES = 2 /* client called XRPresentation_SubmitEyeTexture per eye */
} XRFrameResult;

typedef enum XRSubmitFlags {
    XR_SUBMIT_PREMULTIPLIED_ALPHA = 1u << 0, /* texture color is already premultiplied */
    XR_SUBMIT_FLIP_Y = 1u << 1               /* texture is bottom-up (OpenGL convention) */
} XRSubmitFlags;

/* A finished per-eye image produced by the engine (e.g. GL-on-Metal). */
typedef struct XREyeSubmit {
    uint32_t eye_index;
    XRNativeTexture texture; /* full-resolution 2D texture for this eye */
    uint32_t width, height;
    uint32_t flags;          /* XRSubmitFlags */
    XRPose render_pose;      /* head/eye pose the image was rendered with (for reprojection checks) */
    XRFov render_fov;        /* frustum the image was rendered with; must match eyes[i].fov
                                while foveation is off and the plain copy is used */
} XREyeSubmit;

typedef XRFrameResult (*XRFrameCallback)(void* user, const XRFrameInfo* frame);
typedef void (*XRSessionStateCallback)(void* user, XRSessionState state);

/* Registration. Pass NULL to clear. The callback runs on the compositor render thread. */
void XRPresentation_SetFrameCallback(XRFrameCallback callback, void* user);
void XRPresentation_SetSessionStateCallback(XRSessionStateCallback callback, void* user);

/* Call from inside the frame callback (one per eye) when returning XR_FRAME_SUBMITTED_TEXTURES.
 * Returns false when called outside a frame or with an invalid eye. */
bool XRPresentation_SubmitEyeTexture(const XREyeSubmit* submit);

/* Alpha / passthrough mode for subsequent frames. */
void XRPresentation_SetAlphaMode(XRAlphaMode mode);
XRAlphaMode XRPresentation_GetAlphaMode(void);

/* Ask the shell to re-derive the tabletop placement from the current head pose. */
void XRPresentation_Recenter(void);

XRSessionState XRPresentation_GetSessionState(void);

/* The tabletop play surface is this far above the floor. The shell derives the floor as
 * (board Y - XR_TABLETOP_SURFACE_HEIGHT_M); on devices whose world origin is not on the
 * floor (e.g. the visionOS simulator, origin at head height) the board is placed relative
 * to the head instead of at absolute Y. */
#define XR_TABLETOP_SURFACE_HEIGHT_M 0.8f

/* Latest world-space tabletop placement decided by the shell; the engine anchors
 * its board here. world_from_board is column-major; the board spans
 * +-half_extent_x_m along board X and +-half_extent_z_m along board Z with Y up
 * out of the play surface. Returns false until placement is known. */
bool XRPresentation_GetTabletopPlacement(float out_world_from_board[16],
                                         float* out_half_extent_x_m,
                                         float* out_half_extent_z_m);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GX_XR_PRESENTATION_H */
