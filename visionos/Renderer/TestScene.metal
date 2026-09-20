// TestScene.metal - shaders for the temporary stereo test tabletop and for the
// external-eye-texture composite pass.
//
// Output convention: the compositor layer uses an sRGB color format, so shaders
// work in LINEAR space and the hardware encodes on write. Colors in vertices are
// authored in sRGB and converted here. Output alpha is PREMULTIPLIED (mixed
// immersion blends with passthrough using ONE / ONE_MINUS_SRC_ALPHA).
#include <metal_stdlib>
#include "ShaderTypes.h"

using namespace metal;

struct SceneVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float4 color    [[attribute(2)]];
};

struct SceneVertexOut {
    float4 position [[position]];
    float3 worldPosition;
    float3 worldNormal;
    float4 color;
};

static float3 srgbToLinear(float3 c) {
    return select(c / 12.92, pow((c + 0.055) / 1.055, 2.4), c > 0.04045);
}

vertex SceneVertexOut scene_vertex(SceneVertexIn in [[stage_in]],
                                   constant GXXRSceneUniforms& u [[buffer(GXXRBufferIndexUniforms)]]) {
    SceneVertexOut out;
    float4 world = u.worldFromModel * float4(in.position, 1.0);
    out.position = u.clipFromWorld * world;
    out.worldPosition = world.xyz;
    float3x3 n3 = float3x3(u.worldFromModel[0].xyz, u.worldFromModel[1].xyz, u.worldFromModel[2].xyz);
    out.worldNormal = n3 * in.normal;
    out.color = in.color;
    return out;
}

// Opaque, lit geometry: hemisphere ambient + one directional light + soft rim.
fragment float4 scene_fragment(SceneVertexOut in [[stage_in]],
                               constant GXXRSceneUniforms& u [[buffer(GXXRBufferIndexUniforms)]]) {
    float3 n = normalize(in.worldNormal);
    float3 l = normalize(u.lightDirection.xyz);
    float3 v = normalize(u.eyePosition.xyz - in.worldPosition);
    float ndl = saturate(dot(n, l));
    float hemi = 0.5 + 0.5 * n.y;
    float3 albedo = srgbToLinear(in.color.rgb);
    float3 ambient = albedo * mix(float3(0.10, 0.10, 0.12), float3(0.22, 0.24, 0.28), hemi);
    float3 diffuse = albedo * ndl * float3(1.0, 0.96, 0.90) * 0.95;
    float3 h = normalize(l + v);
    float spec = pow(saturate(dot(n, h)), 48.0) * 0.18 * ndl;
    float rim = pow(1.0 - saturate(dot(n, v)), 3.0) * 0.06;
    float3 color = ambient + diffuse + spec + rim;
    return float4(color, 1.0);  // opaque: premultiplied == straight
}

// Translucent ground reference (grid lines, contact shadow). Alpha comes from
// the vertex color and fades with distance from the world origin; the result is
// premultiplied for mixed immersion. Depth is tested but not written.
fragment float4 ground_fragment(SceneVertexOut in [[stage_in]],
                                constant GXXRSceneUniforms& u [[buffer(GXXRBufferIndexUniforms)]]) {
    float dist = length(in.worldPosition.xz);
    float fade = 1.0 - smoothstep(u.params.y, u.params.z, dist);
    float a = in.color.a * fade;
    float3 rgb = srgbToLinear(in.color.rgb);
    return float4(rgb * a, a);
}

// --- Composite: external per-eye texture -> drawable ------------------------

struct CompositeOut {
    float4 position [[position]];
    float2 uv;
};

vertex CompositeOut composite_vertex(uint vid [[vertex_id]]) {
    // Fullscreen triangle.
    float2 p = float2((vid << 1) & 2, vid & 2);
    CompositeOut out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(p.x, 1.0 - p.y);
    return out;
}

fragment float4 composite_fragment(CompositeOut in [[stage_in]],
                                   texture2d<float> source [[texture(0)]],
                                   constant GXXRCompositeParams& params [[buffer(GXXRBufferIndexCompositeParams)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float2 uv = in.uv;
    if (params.flags & GXXR_COMPOSITE_FLIP_Y) uv.y = 1.0 - uv.y;
    float4 c = source.sample(s, uv);
    if (params.flags & GXXR_COMPOSITE_PREMULTIPLY) c.rgb *= c.a;
    return c;
}
