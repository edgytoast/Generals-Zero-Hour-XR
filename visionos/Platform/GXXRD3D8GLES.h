/*
 * GXXRD3D8GLES.h - host side mirror of the d3d8gles "host targets" contract.
 *
 * The authoritative declarations live in Core/Libraries/Source/d3d8gles/include/d3d8gles.h
 * (package B). This header carries an IDENTICAL copy of the names and layouts so the visionOS
 * host (which must not include the engine headers) can fill and pass them without a
 * dependency on the engine tree. If either copy changes, both must change: the struct layouts
 * are an ABI between the host and the engine backend.
 *
 * Do not include this header and d3d8gles.h in the same translation unit (the enum and the
 * structs would be redeclared). Host-only translation units include this header; the engine
 * bridge (package C) includes d3d8gles.h. Both headers may define
 * D3D8GLES_XR_HOST_TARGETS_DEFINED, which is checked below, so an engine header that defines
 * it first turns this header into a no-op.
 *
 * Frame protocol for the host (see docs/visionos-shell.md, "Engine attach guide"):
 *   1. make the ANGLE context current on the render thread
 *   2. d3d8gles_SetXRHostTargets(targets for this ring slot)
 *   3. run the engine frame
 *   4. glFlush + signal the shared event for the slot
 *   5. d3d8gles_InvalidateCachedState() after any host GL use
 *   6. composite the slot's MTLTextures in Metal after waiting on the event
 */
#ifndef GX_XR_D3D8GLES_HOST_H
#define GX_XR_D3D8GLES_HOST_H

#ifndef D3D8GLES_XR_HOST_TARGETS_DEFINED
#define D3D8GLES_XR_HOST_TARGETS_DEFINED 1

#ifdef __cplusplus
extern "C" {
#endif

/* Names of the host-owned render targets, in D3D8GLES_XRTargets::slot[]. */
enum {
    D3D8GLES_XRT_STEREO_LEFT = 0,
    D3D8GLES_XRT_STEREO_RIGHT = 1,
    D3D8GLES_XRT_GAME = 2,
    D3D8GLES_XRT_WORLD = 3,
    D3D8GLES_XRT_UI = 4,
    D3D8GLES_XRT_COUNT = 5
};

/* glTexture == 0 means "the backend allocates its own texture exactly as on Android". */
struct D3D8GLES_XRHostTarget {
    unsigned glTexture;
    int width;
    int height;
};

/* atlas != 0: both eyes render into slot STEREO_LEFT, eye e uses eyeRect[e] = {x, y, w, h}. */
struct D3D8GLES_XRTargets {
    struct D3D8GLES_XRHostTarget slot[D3D8GLES_XRT_COUNT];
    int atlas;
    int eyeRect[2][4];
};

/* NULL clears. Call every frame before the engine frame; the GL names must stay valid for that frame. */
void d3d8gles_SetXRHostTargets(const struct D3D8GLES_XRTargets *targets);

/* D3D8GLES_XRConfig (existing: void *eglDisplay; void *eglContext;) gains, AT THE END:
 *     void *(*getProcAddress)(const char *);
 *     unsigned flags;
 * with the flag values below. When getProcAddress is set the GL dispatch loads through it
 * (ANGLE's eglGetProcAddress) and multiview is not used. */
#define D3D8GLES_XRFLAG_NO_MULTIVIEW 1u
#define D3D8GLES_XRFLAG_FORCE_ATLAS 2u

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* D3D8GLES_XR_HOST_TARGETS_DEFINED */
#endif /* GX_XR_D3D8GLES_HOST_H */
