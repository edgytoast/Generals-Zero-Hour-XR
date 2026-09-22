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

// --- Composite: external texture -> drawable ---------------------------------

struct CompositeOut {
    float4 position [[position]];
    float2 uv;
};

struct CompositeFragOut {
    float4 color [[color(0)]];
    float depth [[depth(any)]];
};

vertex CompositeOut composite_vertex(uint vid [[vertex_id]]) {
    // Fullscreen triangle.
    float2 p = float2((vid << 1) & 2, vid & 2);
    CompositeOut out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(p.x, 1.0 - p.y);
    return out;
}

// Samples a client texture and returns LINEAR, PREMULTIPLIED color (what the sRGB drawable wants).
static float4 sampleClient(texture2d<float> source, float2 uv, constant GXXRCompositeParams& params) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    if (params.flags & GXXR_COMPOSITE_FLIP_Y) uv.y = 1.0 - uv.y;
    uv = params.uvRect.xy + uv * params.uvRect.zw;
    float4 c = source.sample(s, uv);
    const bool straightAlpha = (params.flags & GXXR_COMPOSITE_PREMULTIPLY) != 0;
    if (params.flags & GXXR_COMPOSITE_SRGB_DECODE) {
        float3 straight = straightAlpha ? c.rgb : (c.a > 1e-5 ? c.rgb / c.a : float3(0.0));
        c.rgb = srgbToLinear(saturate(straight));
        if (!straightAlpha) c.rgb *= c.a;
    }
    if (straightAlpha) c.rgb *= c.a;
    return c;
}

fragment CompositeFragOut composite_fragment(CompositeOut in [[stage_in]],
                                             texture2d<float> source [[texture(0)]],
                                             constant GXXRCompositeParams& params [[buffer(GXXRBufferIndexCompositeParams)]]) {
    float4 c = sampleClient(source, in.uv, params);
    // Transparent pixels must keep depth 0 (far) so passthrough shows and the compositor does
    // not treat them as geometry.
    if (c.a < 1e-4) discard_fragment();
    CompositeFragOut out;
    out.color = c;
    out.depth = params.depth;
    return out;
}

// --- World-anchored quad layer -------------------------------------------------

struct LayerOut {
    float4 position [[position]];
    float2 uv;
};

vertex LayerOut layer_vertex(uint vid [[vertex_id]],
                             constant GXXRLayerUniforms& u [[buffer(GXXRBufferIndexUniforms)]]) {
    // Triangle strip: (-,+) (+,+) (-,-) (+,-); uv (0,0) is the top-left of the texture image.
    const float2 corner = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? -1.0 : 1.0);
    LayerOut out;
    float4 world = u.worldFromQuad * float4(corner * u.halfSize, 0.0, 1.0);
    out.position = u.clipFromWorld * world;
    out.uv = float2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    return out;
}

fragment float4 layer_fragment(LayerOut in [[stage_in]],
                               texture2d<float> source [[texture(0)]],
                               constant GXXRCompositeParams& params [[buffer(GXXRBufferIndexCompositeParams)]]) {
    float4 c = sampleClient(source, in.uv, params);
    if (c.a < 1e-3) discard_fragment();  // keep depth for translucent-but-visible pixels only
    return c;
}

// --- Flat-color world geometry: box outline/fill, grab bar, markers ---------------------

struct FlatOut {
    float4 position [[position]];
    float4 color;
};

vertex FlatOut flat_vertex(uint vid [[vertex_id]],
                            const device GXXRFlatVertex* verts [[buffer(GXXRBufferIndexVertices)]],
                            constant GXXRFlatUniforms& u [[buffer(GXXRBufferIndexUniforms)]]) {
    const device GXXRFlatVertex& v = verts[vid];
    FlatOut out;
    out.position = u.clipFromWorld * float4(v.position[0], v.position[1], v.position[2], 1.0);
    out.color = float4(v.color[0], v.color[1], v.color[2], v.color[3]);
    return out;
}

fragment float4 flat_fragment(FlatOut in [[stage_in]]) {
    float3 rgb = srgbToLinear(in.color.rgb);
    float a = in.color.a;
    return float4(rgb * a, a); // premultiplied for the mixed-immersion blend
}

// --- Full-view comfort fade veil: fullscreen triangle, ignores clipFromWorld -------------

vertex CompositeOut fade_vertex(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    CompositeOut out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(0, 0);
    return out;
}

fragment float4 fade_fragment(CompositeOut in [[stage_in]],
                              constant GXXRFadeParams& p [[buffer(GXXRBufferIndexFadeParams)]]) {
    (void)in;
    if (p.color.a < 1e-4) discard_fragment();
    float3 rgb = srgbToLinear(p.color.rgb);
    return float4(rgb * p.color.a, p.color.a);
}
