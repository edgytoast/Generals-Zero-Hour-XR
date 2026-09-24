// ShaderTypes.h - types shared by the Metal shaders and the Objective-C++ renderer.
#ifndef GXXR_SHADER_TYPES_H
#define GXXR_SHADER_TYPES_H

#include <simd/simd.h>

// Vertex stream layout: 40 bytes, tightly packed (float3 attributes at 0 and 12,
// float4 at 24). Declared with plain float arrays so the CPU and GPU agree.
typedef struct {
    float position[3];
    float normal[3];
    float color[4];  // straight sRGB color, alpha used by the ground pipeline only
} GXXRVertex;

typedef struct {
    simd_float4x4 clipFromWorld;
    simd_float4x4 worldFromModel;  // rigid transform, no scale (normals use its 3x3)
    simd_float4 eyePosition;       // world space
    simd_float4 lightDirection;    // xyz = direction TOWARD the light, world space
    simd_float4 params;            // x = time (s), y = fade inner radius, z = fade outer radius, w = unused
} GXXRSceneUniforms;

// Composite pass flags (external texture -> compositor drawable).
#define GXXR_COMPOSITE_FLIP_Y (1u << 0)       // source is bottom-up (OpenGL); sample with v' = 1 - v
#define GXXR_COMPOSITE_PREMULTIPLY (1u << 1)  // source is straight alpha; premultiply on write
// Source RGB holds gamma-encoded (sRGB) values in a NON-sRGB pixel format, which is what the
// engine's D3D8/GL pipeline writes (gamma-space blending). Decode to linear on sampling so the
// sRGB compositor drawable re-encodes correctly. Un-premultiplies first when the source is
// premultiplied, decodes, and premultiplies again.
#define GXXR_COMPOSITE_SRGB_DECODE (1u << 2)
// Per-pixel depth from the tabletop plane instead of the constant depth: each pixel's eye ray is intersected with the plane
// (GXXRCompositeParams.plane) and the hit's depth is written (clipFromWorld / worldFromClip of the eye the picture was
// rendered for). The system reprojects the picture between engine frames by this depth; a single constant depth made the
// tilted table and its units swim when the head moved (the engine renders 15-30 fps against a 90 Hz display).
#define GXXR_COMPOSITE_PLANE_DEPTH (1u << 3)

typedef struct {
    unsigned int flags;
    float depth;          // reverse-Z NDC depth written where alpha > 0 (fullscreen eye composite); 0 = far
    float pad0, pad1;
    simd_float4 uvRect;   // xy = origin, zw = size of the source rectangle in [0,1] (atlas eye rects)
    simd_float4x4 clipFromWorld;  // GXXR_COMPOSITE_PLANE_DEPTH: the eye the picture was rendered for
    simd_float4x4 worldFromClip;
    simd_float4 plane;            // xyz = unit normal, w = -dot(normal, point on plane)
} GXXRCompositeParams;

// World-anchored textured quad ("layer") composite: unit quad centered on the local origin,
// facing local +Z, scaled by halfSize * 2 meters.
typedef struct {
    simd_float4x4 clipFromWorld;
    simd_float4x4 worldFromQuad;
    simd_float2 halfSize;
    float pad0, pad1;
} GXXRLayerUniforms;

// Flat-color world geometry (box outline/fill, grab bar, markers): position + straight RGBA, alpha blended, no texture.
typedef struct {
    float position[3];
    float color[4];
} GXXRFlatVertex;

typedef struct {
    simd_float4x4 clipFromWorld;
} GXXRFlatUniforms;

// Full-view comfort fade veil: a flat color over the whole drawable, ignoring clipFromWorld (head/screen locked).
typedef struct {
    simd_float4 color; // straight RGBA; alpha 0 draws nothing
} GXXRFadeParams;

enum {
    GXXRBufferIndexVertices = 0,
    GXXRBufferIndexUniforms = 1,
    GXXRBufferIndexCompositeParams = 0,
    GXXRBufferIndexFadeParams = 0,
};

#endif  // GXXR_SHADER_TYPES_H
