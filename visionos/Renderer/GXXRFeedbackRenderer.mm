// GXXRFeedbackRenderer.mm - see header. Objective-C++ with ARC.
#import "GXXRFeedbackRenderer.h"

#include <cmath>
#include <vector>

#import "ShaderTypes.h"

namespace {
simd_float3 V(const float p[3]) { return simd_make_float3(p[0], p[1], p[2]); }
simd_quatf Q(const float q[4]) { return simd_quaternion(q[0], q[1], q[2], q[3]); }

void pushTri(std::vector<GXXRFlatVertex> &v, simd_float3 a, simd_float3 b, simd_float3 c, simd_float4 color) {
    for (simd_float3 p : {a, b, c}) v.push_back({{p.x, p.y, p.z}, {color.r, color.g, color.b, color.a}});
}
void pushQuad(std::vector<GXXRFlatVertex> &v, simd_float3 a, simd_float3 b, simd_float3 c, simd_float3 d, simd_float4 color) {
    pushTri(v, a, b, c, color);
    pushTri(v, a, c, d, color);
}
// A thin ribbon from `a` to `b`, `halfWidth` wide in the direction `perp` (unit length).
void pushRibbon(std::vector<GXXRFlatVertex> &v, simd_float3 a, simd_float3 b, simd_float3 perp, float halfWidth, simd_float4 color) {
    const simd_float3 off = perp * halfWidth;
    pushQuad(v, a - off, b - off, b + off, a + off, color);
}
// A filled + outlined n-gon disk in the plane through `center` with unit normal `normal`.
void pushDisk(std::vector<GXXRFlatVertex> &v, simd_float3 center, simd_float3 normal, float radius, simd_float4 fill, int sides = 20) {
    simd_float3 ref = std::fabs(normal.y) < 0.9f ? simd_make_float3(0, 1, 0) : simd_make_float3(1, 0, 0);
    const simd_float3 u = simd_normalize(simd_cross(ref, normal));
    const simd_float3 w = simd_cross(normal, u);
    simd_float3 prev = center + u * radius;
    for (int i = 1; i <= sides; ++i) {
        const float a = 2.0f * float(M_PI) * float(i) / float(sides);
        const simd_float3 cur = center + u * (radius * std::cos(a)) + w * (radius * std::sin(a));
        pushTri(v, center, prev, cur, fill);
        prev = cur;
    }
}
// A ring (annulus) outline, optionally a partial arc (`fraction` of the full circle, from angle 0).
void pushRing(std::vector<GXXRFlatVertex> &v, simd_float3 center, simd_float3 normal, float radius, float thickness, simd_float4 color,
              float fraction = 1.0f, int sides = 28) {
    simd_float3 ref = std::fabs(normal.y) < 0.9f ? simd_make_float3(0, 1, 0) : simd_make_float3(1, 0, 0);
    const simd_float3 u = simd_normalize(simd_cross(ref, normal));
    const simd_float3 w = simd_cross(normal, u);
    const int steps = std::max(1, int(std::ceil(sides * simd_clamp(fraction, 0.0f, 1.0f))));
    const float total = 2.0f * float(M_PI) * simd_clamp(fraction, 0.0f, 1.0f);
    auto point = [&](float a, float r) { return center + u * (r * std::cos(a)) + w * (r * std::sin(a)); };
    simd_float3 innerPrev = point(0, radius - thickness * 0.5f), outerPrev = point(0, radius + thickness * 0.5f);
    for (int i = 1; i <= steps; ++i) {
        const float a = total * float(i) / float(steps);
        const simd_float3 innerCur = point(a, radius - thickness * 0.5f), outerCur = point(a, radius + thickness * 0.5f);
        pushQuad(v, innerPrev, outerPrev, outerCur, innerCur, color);
        innerPrev = innerCur;
        outerPrev = outerCur;
    }
}
} // namespace

@implementation GXXRFeedbackRenderer {
    id<MTLDevice> _device;
    id<MTLRenderPipelineState> _flatPipeline;
    id<MTLRenderPipelineState> _fadePipeline;
    // The board/UI picture behind these markers is a flat 2D GL render composited as a full-screen quad: the compositor
    // writes ONE constant depth across it (docs/visionos-engine-host.md section 4: "so system reprojection sees a
    // plane there"), not the real per-pixel depth of what it depicts. Testing a marker's true per-vertex depth against
    // that constant would discard it whenever the marker's true distance differs from the single focus distance
    // (which is most of the board away from dead center) -- not an occlusion decision, just an artifact of the
    // technique. Markers are therefore drawn UNCONDITIONALLY on top (matching the documented "a marker sitting on a
    // panel draws in front of it"), never tested and never written: there is nothing genuinely 3D drawn after them
    // in this pass except the comfort-fade veil, which also ignores depth.
    id<MTLDepthStencilState> _depthOff;
    std::vector<GXXRFlatVertex> _verts;
}

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device colorFormat:(MTLPixelFormat)colorFormat depthFormat:(MTLPixelFormat)depthFormat {
    self = [super init];
    if (!self) return nil;
    _device = device;
    NSError* error = nil;
    id<MTLLibrary> library = [device newDefaultLibraryWithBundle:[NSBundle mainBundle] error:&error];
    if (!library) {
        NSLog(@"[GXXR] feedback: Metal library load failed: %@", error);
        return nil;
    }
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat3;
    vd.attributes[0].offset = offsetof(GXXRFlatVertex, position);
    vd.attributes[0].bufferIndex = GXXRBufferIndexVertices;
    vd.attributes[1].format = MTLVertexFormatFloat4;
    vd.attributes[1].offset = offsetof(GXXRFlatVertex, color);
    vd.attributes[1].bufferIndex = GXXRBufferIndexVertices;
    vd.layouts[GXXRBufferIndexVertices].stride = sizeof(GXXRFlatVertex);
    vd.layouts[GXXRBufferIndexVertices].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [library newFunctionWithName:@"flat_vertex"];
    pd.fragmentFunction = [library newFunctionWithName:@"flat_fragment"];
    pd.vertexDescriptor = vd;
    pd.colorAttachments[0].pixelFormat = colorFormat;
    pd.depthAttachmentPixelFormat = depthFormat;
    pd.label = @"GXXR feedback markers (premultiplied)";
    MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[0];
    ca.blendingEnabled = YES;
    ca.rgbBlendOperation = MTLBlendOperationAdd;
    ca.alphaBlendOperation = MTLBlendOperationAdd;
    ca.sourceRGBBlendFactor = MTLBlendFactorOne;
    ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
    ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    _flatPipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!_flatPipeline) { NSLog(@"[GXXR] feedback: flat pipeline failed: %@", error); return nil; }

    MTLRenderPipelineDescriptor* fd = [MTLRenderPipelineDescriptor new];
    fd.vertexFunction = [library newFunctionWithName:@"fade_vertex"];
    fd.fragmentFunction = [library newFunctionWithName:@"fade_fragment"];
    fd.colorAttachments[0].pixelFormat = colorFormat;
    fd.label = @"GXXR comfort fade";  // no depth attachment: the veil ignores depth entirely
    MTLRenderPipelineColorAttachmentDescriptor* fca = fd.colorAttachments[0];
    fca.blendingEnabled = YES;
    fca.rgbBlendOperation = MTLBlendOperationAdd;
    fca.alphaBlendOperation = MTLBlendOperationAdd;
    fca.sourceRGBBlendFactor = MTLBlendFactorOne;
    fca.sourceAlphaBlendFactor = MTLBlendFactorOne;
    fca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    fca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    _fadePipeline = [device newRenderPipelineStateWithDescriptor:fd error:&error];
    if (!_fadePipeline) { NSLog(@"[GXXR] feedback: fade pipeline failed: %@", error); return nil; }

    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = NO;
    _depthOff = [device newDepthStencilStateWithDescriptor:dd];
    return self;
}

- (MTLRenderPassDescriptor*)passWithColor:(id<MTLTexture>)color slice:(NSUInteger)slice depth:(id<MTLTexture>)depth {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].slice = slice;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (depth) {
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.slice = slice;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
    }
    return pass;
}

- (void)encodeMarkersInto:(id<MTLCommandBuffer>)commandBuffer
                    color:(id<MTLTexture>)color
               colorSlice:(NSUInteger)slice
                    depth:(id<MTLTexture>)depth
                 viewport:(MTLViewport)viewport
            clipFromWorld:(simd_float4x4)clipFromWorld
                 feedback:(const GXHostFeedback*)fb
       layerWorldFromQuad:(const simd_float4x4*)layerWorldFromQuad
               layerCount:(NSUInteger)layerCount {
    if (fb == nullptr || fb->flags == 0) return;
    _verts.clear();
    std::vector<GXXRFlatVertex> translucent; // box fill / rings: depth-tested, not written (never occludes a later opaque marker)
    const simd_float3 boardNormal = simd_act(Q(fb->boardOrientation), simd_make_float3(0, 0, 1));
    const float markerRadius = std::max(0.006f, fb->boardWidth * 0.012f);
    const float ribbonHalf = std::max(0.0015f, fb->boardWidth * 0.0025f);

    if (fb->flags & GX_FB_BOX) {
        const simd_float4 tint = fb->boxAdditive ? simd_make_float4(0.25f, 0.85f, 0.95f, 0.22f) : simd_make_float4(0.95f, 0.85f, 0.25f, 0.22f);
        const simd_float4 outline = simd_make_float4(tint.r, tint.g, tint.b, 0.9f);
        simd_float3 c[4];
        for (int i = 0; i < 4; ++i) c[i] = V(fb->boxCorners[i]);
        pushQuad(translucent, c[0], c[1], c[2], c[3], tint);
        for (int i = 0; i < 4; ++i) pushRibbon(_verts, c[i], c[(i + 1) % 4], boardNormal, ribbonHalf, outline);
    }
    if (fb->flags & GX_FB_GRAB_BAR) {
        const simd_quatf q = Q(fb->grabOrientation);
        const simd_float3 pos = V(fb->grabPosition);
        const simd_float3 right = simd_act(q, simd_make_float3(1, 0, 0)), forward = simd_act(q, simd_make_float3(0, 1, 0));
        const simd_float4 col = fb->grabActive ? simd_make_float4(0.35f, 0.85f, 1.0f, 0.55f) : simd_make_float4(0.7f, 0.8f, 0.9f, 0.28f);
        const simd_float3 hr = right * (fb->grabLength * 0.5f), hf = forward * (fb->grabThickness * 0.5f);
        pushQuad(translucent, pos - hr - hf, pos + hr - hf, pos + hr + hf, pos - hr + hf, col);
    }
    if (fb->flags & GX_FB_CURSOR) {
        pushDisk(_verts, V(fb->cursor) + boardNormal * 0.001f, boardNormal, markerRadius, simd_make_float4(1.0f, 0.95f, 0.35f, 0.85f));
    }
    if (fb->flags & GX_FB_PANEL_POINTER && layerWorldFromQuad != nullptr && fb->pointerLayer >= 0 && (NSUInteger)fb->pointerLayer < layerCount) {
        const simd_float4x4& pose = layerWorldFromQuad[(NSUInteger)fb->pointerLayer];
        const simd_float3 normal = simd_normalize(simd_make_float3(pose.columns[2].x, pose.columns[2].y, pose.columns[2].z));
        pushDisk(_verts, V(fb->pointerPos) + normal * 0.002f, normal, markerRadius * 0.7f, simd_make_float4(1.0f, 1.0f, 1.0f, 0.9f), 14);
    }
    if (fb->flags & GX_FB_PLACEMENT) {
        const simd_float4 col = fb->placementLegal == 0 ? simd_make_float4(0.95f, 0.25f, 0.2f, 0.8f)
                               : fb->placementLegal == 1 ? simd_make_float4(0.3f, 0.9f, 0.35f, 0.8f)
                                                          : simd_make_float4(0.85f, 0.85f, 0.3f, 0.8f);
        pushRing(_verts, V(fb->placementPoint) + boardNormal * 0.001f, boardNormal, markerRadius * 1.4f, ribbonHalf * 1.5f, col);
    }
    if (fb->flags & GX_FB_GROUND_TARGET) {
        const simd_float4 col = fb->groundTargetValid ? simd_make_float4(0.3f, 0.9f, 0.5f, 0.85f) : simd_make_float4(0.9f, 0.3f, 0.3f, 0.85f);
        const simd_float3 up = simd_make_float3(0, 1, 0);
        pushRing(_verts, V(fb->groundTarget) + up * 0.01f, up, 0.35f, 0.03f, col);
        if (fb->groundHold > 0.001f)
            pushRing(_verts, V(fb->groundTarget) + up * 0.015f, up, 0.35f, 0.05f, simd_make_float4(1, 1, 1, 0.9f), fb->groundHold);
    }
    if (fb->flags & GX_FB_RAY) {
        pushRibbon(_verts, V(fb->rayStart), V(fb->rayEnd), boardNormal, ribbonHalf * 0.6f, simd_make_float4(1, 1, 1, 0.35f));
    }
    if (fb->flags & GX_FB_HOVER) {
        simd_float3 c[4];
        for (int i = 0; i < 4; ++i) c[i] = V(fb->hoverQuad[i]);
        const simd_float3 n = simd_normalize(simd_cross(c[1] - c[0], c[3] - c[0]));
        const simd_float4 col = simd_make_float4(0.9f, 0.95f, 1.0f, 0.8f);
        for (int i = 0; i < 4; ++i) pushRibbon(_verts, c[i], c[(i + 1) % 4], n, ribbonHalf * 0.6f, col);
    }
    if (fb->flags & GX_FB_WAYPOINT) {
        const float age = std::max(0.0f, fb->waypointAge);
        const float alpha = std::max(0.0f, 1.0f - age / 1.2f);
        if (alpha > 0.01f) {
            const float grow = 1.0f + age * 0.6f; // the ring expands slightly as it fades
            pushRing(_verts, V(fb->waypoint) + boardNormal * 0.001f, boardNormal, markerRadius * 1.1f * grow, ribbonHalf, simd_make_float4(1, 0.75f, 0.2f, alpha));
        }
    }
    _verts.insert(_verts.end(), translucent.begin(), translucent.end());
    if (_verts.empty()) return;

    id<MTLBuffer> buf = [_device newBufferWithBytes:_verts.data() length:_verts.size() * sizeof(GXXRFlatVertex) options:MTLResourceStorageModeShared];
    id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:[self passWithColor:color slice:slice depth:depth]];
    enc.label = @"GXXR feedback markers";
    [enc setViewport:viewport];
    [enc setCullMode:MTLCullModeNone];
    [enc setRenderPipelineState:_flatPipeline];
    [enc setDepthStencilState:_depthOff]; // always on top of the board/UI picture and the layers; see the ivar comment above
    GXXRFlatUniforms u = {};
    u.clipFromWorld = clipFromWorld;
    [enc setVertexBuffer:buf offset:0 atIndex:GXXRBufferIndexVertices];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:GXXRBufferIndexUniforms];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:_verts.size()];
    [enc endEncoding];
}

- (void)encodeFadeInto:(id<MTLCommandBuffer>)commandBuffer color:(id<MTLTexture>)color colorSlice:(NSUInteger)slice viewport:(MTLViewport)viewport
                  alpha:(float)alpha {
    if (alpha <= 0.001f) return;
    id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:[self passWithColor:color slice:slice depth:nil]];
    enc.label = @"GXXR comfort fade";
    [enc setViewport:viewport];
    [enc setCullMode:MTLCullModeNone];
    [enc setRenderPipelineState:_fadePipeline];
    GXXRFadeParams p = {};
    p.color = simd_make_float4(0, 0, 0, simd_clamp(alpha, 0.0f, 1.0f));
    [enc setFragmentBytes:&p length:sizeof(p) atIndex:GXXRBufferIndexFadeParams];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
}

@end
