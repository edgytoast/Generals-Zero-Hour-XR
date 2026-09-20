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

// Composite pass flags (external per-eye texture -> compositor drawable).
#define GXXR_COMPOSITE_FLIP_Y (1u << 0)
#define GXXR_COMPOSITE_PREMULTIPLY (1u << 1)  // source is straight alpha; premultiply on write

typedef struct {
    unsigned int flags;
    unsigned int pad0, pad1, pad2;
} GXXRCompositeParams;

enum {
    GXXRBufferIndexVertices = 0,
    GXXRBufferIndexUniforms = 1,
    GXXRBufferIndexCompositeParams = 0,
};

#endif  // GXXR_SHADER_TYPES_H
