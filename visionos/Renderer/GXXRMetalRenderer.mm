// GXXRMetalRenderer.mm - see header. Objective-C++ with ARC.
#import "GXXRMetalRenderer.h"

#include <cmath>
#include <cstddef>
#include <vector>

#import "ShaderTypes.h"

namespace {

struct Range {
    NSUInteger first = 0;
    NSUInteger count = 0;
};

// Sizes of the test tabletop, meters. Board space: origin at board center on the
// play surface, +X right, +Z toward the player, +Y up.
constexpr float kBoardHalfX = 0.5f;
constexpr float kBoardHalfZ = 0.3f;

class MeshBuilder {
public:
    std::vector<GXXRVertex> v;

    void tri(simd_float3 a, simd_float3 b, simd_float3 c, simd_float3 n, simd_float4 col) {
        push(a, n, col);
        push(b, n, col);
        push(c, n, col);
    }
    void quad(simd_float3 a, simd_float3 b, simd_float3 c, simd_float3 d, simd_float3 n, simd_float4 col) {
        tri(a, b, c, n, col);
        tri(a, c, d, n, col);
    }
    // Axis-aligned box.
    void box(simd_float3 c, simd_float3 h, simd_float4 col) {
        const float x0 = c.x - h.x, x1 = c.x + h.x, y0 = c.y - h.y, y1 = c.y + h.y, z0 = c.z - h.z, z1 = c.z + h.z;
        quad({x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}, {0, 1, 0}, col);   // top
        quad({x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, {0, -1, 0}, col);  // bottom
        quad({x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {0, 0, 1}, col);   // +Z
        quad({x1, y0, z0}, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {0, 0, -1}, col);  // -Z
        quad({x1, y0, z1}, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {1, 0, 0}, col);   // +X
        quad({x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {-1, 0, 0}, col);  // -X
    }
    // Square-base pyramid; c is the center of the base.
    void pyramid(simd_float3 c, float halfBase, float height, simd_float4 col) {
        const simd_float3 p00 = {c.x - halfBase, c.y, c.z - halfBase};
        const simd_float3 p10 = {c.x + halfBase, c.y, c.z - halfBase};
        const simd_float3 p11 = {c.x + halfBase, c.y, c.z + halfBase};
        const simd_float3 p01 = {c.x - halfBase, c.y, c.z + halfBase};
        const simd_float3 apex = {c.x, c.y + height, c.z};
        quad(p00, p10, p11, p01, {0, -1, 0}, col);
        side(p01, p11, apex, col);  // +Z
        side(p11, p10, apex, col);  // +X
        side(p10, p00, apex, col);  // -Z
        side(p00, p01, apex, col);  // -X
    }
    // Flat horizontal quad from (x0,z0) to (x1,z1) at height y, facing +Y.
    void floorQuad(float x0, float z0, float x1, float z1, float y, simd_float4 col) {
        quad({x0, y, z1}, {x1, y, z1}, {x1, y, z0}, {x0, y, z0}, {0, 1, 0}, col);
    }
    Range mark(NSUInteger first) const { return Range{first, (NSUInteger)v.size() - first}; }

private:
    void push(simd_float3 p, simd_float3 n, simd_float4 c) {
        GXXRVertex vert = {{p.x, p.y, p.z}, {n.x, n.y, n.z}, {c.x, c.y, c.z, c.w}};
        v.push_back(vert);
    }
    void side(simd_float3 a, simd_float3 b, simd_float3 apex, simd_float4 col) {
        simd_float3 n = simd_normalize(simd_cross(b - a, apex - a));
        tri(a, b, apex, n, col);
    }
};

simd_float4x4 translation(simd_float3 t) {
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[3] = simd_make_float4(t.x, t.y, t.z, 1.0f);
    return m;
}

simd_float4x4 rotationY(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[0] = simd_make_float4(c, 0, -s, 0);
    m.columns[2] = simd_make_float4(s, 0, c, 0);
    return m;
}

}  // namespace

@implementation GXXRMetalRenderer {
    id<MTLDevice> _device;
    MTLPixelFormat _colorFormat;
    MTLPixelFormat _depthFormat;

    id<MTLRenderPipelineState> _scenePipeline;
    id<MTLRenderPipelineState> _groundPipeline;
    id<MTLRenderPipelineState> _compositePipeline;
    id<MTLDepthStencilState> _depthWrite;   // reverse-Z, write on
    id<MTLDepthStencilState> _depthTestOnly;
    id<MTLDepthStencilState> _depthOff;

    id<MTLBuffer> _vertexBuffer;
    Range _board;      // static, board-local
    Range _orbiter;    // board-local, animated per frame
    Range _floorGrid;  // world space
    Range _shadow;     // floor-relative, board-local xz
}

- (id<MTLDevice>)device { return _device; }
- (MTLPixelFormat)colorFormat { return _colorFormat; }
- (MTLPixelFormat)depthFormat { return _depthFormat; }

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                            colorFormat:(MTLPixelFormat)colorFormat
                            depthFormat:(MTLPixelFormat)depthFormat {
    self = [super init];
    if (!self) return nil;
    _device = device;
    _colorFormat = colorFormat;
    _depthFormat = depthFormat;

    NSError* error = nil;
    id<MTLLibrary> library = [device newDefaultLibraryWithBundle:[NSBundle mainBundle] error:&error];
    if (!library) {
        NSLog(@"[GXXR] Metal library load failed: %@", error);
        return nil;
    }

    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat3;
    vd.attributes[0].offset = offsetof(GXXRVertex, position);
    vd.attributes[0].bufferIndex = GXXRBufferIndexVertices;
    vd.attributes[1].format = MTLVertexFormatFloat3;
    vd.attributes[1].offset = offsetof(GXXRVertex, normal);
    vd.attributes[1].bufferIndex = GXXRBufferIndexVertices;
    vd.attributes[2].format = MTLVertexFormatFloat4;
    vd.attributes[2].offset = offsetof(GXXRVertex, color);
    vd.attributes[2].bufferIndex = GXXRBufferIndexVertices;
    vd.layouts[GXXRBufferIndexVertices].stride = sizeof(GXXRVertex);
    vd.layouts[GXXRBufferIndexVertices].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [library newFunctionWithName:@"scene_vertex"];
    pd.fragmentFunction = [library newFunctionWithName:@"scene_fragment"];
    pd.vertexDescriptor = vd;
    pd.colorAttachments[0].pixelFormat = colorFormat;
    pd.depthAttachmentPixelFormat = depthFormat;
    pd.label = @"GXXR scene";
    _scenePipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!_scenePipeline) { NSLog(@"[GXXR] scene pipeline failed: %@", error); return nil; }

    pd.fragmentFunction = [library newFunctionWithName:@"ground_fragment"];
    pd.label = @"GXXR ground (premultiplied)";
    MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[0];
    ca.blendingEnabled = YES;
    ca.rgbBlendOperation = MTLBlendOperationAdd;
    ca.alphaBlendOperation = MTLBlendOperationAdd;
    ca.sourceRGBBlendFactor = MTLBlendFactorOne;  // shader output is premultiplied
    ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
    ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    _groundPipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!_groundPipeline) { NSLog(@"[GXXR] ground pipeline failed: %@", error); return nil; }

    MTLRenderPipelineDescriptor* cd = [MTLRenderPipelineDescriptor new];
    cd.vertexFunction = [library newFunctionWithName:@"composite_vertex"];
    cd.fragmentFunction = [library newFunctionWithName:@"composite_fragment"];
    cd.colorAttachments[0].pixelFormat = colorFormat;
    cd.depthAttachmentPixelFormat = depthFormat;
    cd.label = @"GXXR composite";
    _compositePipeline = [device newRenderPipelineStateWithDescriptor:cd error:&error];
    if (!_compositePipeline) { NSLog(@"[GXXR] composite pipeline failed: %@", error); return nil; }

    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionGreaterEqual;  // reverse-Z
    dd.depthWriteEnabled = YES;
    _depthWrite = [device newDepthStencilStateWithDescriptor:dd];
    dd.depthWriteEnabled = NO;
    _depthTestOnly = [device newDepthStencilStateWithDescriptor:dd];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    _depthOff = [device newDepthStencilStateWithDescriptor:dd];

    [self buildGeometry];
    return self;
}

#pragma mark - Geometry

- (void)buildGeometry {
    MeshBuilder m;
    auto rgb = [](float r, float g, float b, float a = 1.0f) { return simd_make_float4(r, g, b, a); };

    // ---- Static board (board-local) ----
    NSUInteger start = m.v.size();
    m.box({0, -0.0205f, 0}, {kBoardHalfX + 0.02f, 0.02f, kBoardHalfZ + 0.02f}, rgb(0.13f, 0.14f, 0.16f));  // slab
    const int nx = 10, nz = 6;
    const float tx = 2 * kBoardHalfX / nx, tz = 2 * kBoardHalfZ / nz;
    for (int iz = 0; iz < nz; ++iz) {
        for (int ix = 0; ix < nx; ++ix) {
            const bool dark = ((ix + iz) & 1) != 0;
            simd_float4 c = dark ? rgb(0.30f, 0.38f, 0.22f) : rgb(0.42f, 0.50f, 0.30f);
            const float x0 = -kBoardHalfX + ix * tx, z0 = -kBoardHalfZ + iz * tz;
            m.floorQuad(x0, z0, x0 + tx, z0 + tz, 0.0f, c);
        }
    }
    // Lake and road (flush details so the board reads as terrain).
    m.floorQuad(-0.02f, -0.30f, 0.14f, -0.06f, 0.0008f, rgb(0.16f, 0.32f, 0.55f));
    m.floorQuad(-0.50f, 0.16f, 0.50f, 0.20f, 0.0008f, rgb(0.55f, 0.52f, 0.45f));
    // Corner posts: orientation reference (mirroring or swapped eyes is obvious).
    const float py = 0.03f;
    m.box({-kBoardHalfX, py, -kBoardHalfZ}, {0.012f, py, 0.012f}, rgb(0.90f, 0.15f, 0.12f));  // back-left  red
    m.box({kBoardHalfX, py, -kBoardHalfZ}, {0.012f, py, 0.012f}, rgb(0.15f, 0.80f, 0.25f));   // back-right green
    m.box({kBoardHalfX, py, kBoardHalfZ}, {0.012f, py, 0.012f}, rgb(0.20f, 0.35f, 0.95f));    // front-right blue
    m.box({-kBoardHalfX, py, kBoardHalfZ}, {0.012f, py, 0.012f}, rgb(0.95f, 0.85f, 0.15f));   // front-left yellow
    // Blue team: cubes with turrets.
    const simd_float2 blue[] = {{-0.32f, -0.12f}, {-0.26f, 0.00f}, {-0.32f, 0.10f}};
    for (auto p : blue) {
        m.box({p.x, 0.015f, p.y}, {0.03f, 0.015f, 0.045f}, rgb(0.20f, 0.40f, 0.95f));
        m.box({p.x, 0.035f, p.y}, {0.018f, 0.008f, 0.02f}, rgb(0.12f, 0.25f, 0.70f));
        m.box({p.x + 0.04f, 0.036f, p.y}, {0.025f, 0.004f, 0.004f}, rgb(0.85f, 0.85f, 0.90f));
    }
    // Red team: pyramids.
    const simd_float2 red[] = {{0.30f, -0.10f}, {0.24f, 0.03f}, {0.32f, 0.11f}};
    for (auto p : red) m.pyramid({p.x, 0.0f, p.y}, 0.03f, 0.07f, rgb(0.92f, 0.22f, 0.18f));
    // Objective: tall yellow pyramid at center; green crate; orange crate.
    m.pyramid({0.0f, 0.0f, 0.02f}, 0.035f, 0.13f, rgb(0.95f, 0.78f, 0.12f));
    m.box({0.20f, 0.02f, -0.20f}, {0.02f, 0.02f, 0.02f}, rgb(0.20f, 0.75f, 0.35f));
    m.box({-0.12f, 0.012f, -0.20f}, {0.03f, 0.012f, 0.015f}, rgb(0.95f, 0.50f, 0.10f));
    _board = m.mark(start);

    // ---- Orbiter (drawn at origin, animated) ----
    start = m.v.size();
    m.box({0, 0, 0}, {0.015f, 0.015f, 0.015f}, rgb(0.98f, 0.98f, 1.0f));
    m.pyramid({0, 0.015f, 0}, 0.012f, 0.02f, rgb(0.98f, 0.30f, 0.80f));
    _orbiter = m.mark(start);

    // ---- World floor grid (translucent, y ~ 0) ----
    start = m.v.size();
    const float extent = 6.0f;
    for (int i = -12; i <= 12; ++i) {
        const float p = i * 0.5f;
        const bool major = (i % 4) == 0;
        const float w = major ? 0.008f : 0.004f;
        const float a = major ? 0.55f : 0.30f;
        simd_float4 c = rgb(0.62f, 0.78f, 0.95f, a);
        m.floorQuad(p - w, -extent, p + w, extent, 0.001f, c);  // line along Z
        m.floorQuad(-extent, p - w, extent, p + w, 0.001f, c);  // line along X
    }
    // Origin cross (where the player's floor origin is).
    m.floorQuad(-0.25f, -0.012f, 0.25f, 0.012f, 0.0015f, rgb(0.25f, 0.85f, 0.95f, 0.85f));
    m.floorQuad(-0.012f, -0.25f, 0.012f, 0.25f, 0.0015f, rgb(0.25f, 0.85f, 0.95f, 0.85f));
    _floorGrid = m.mark(start);

    // ---- Contact shadow under the board: nested translucent quads (soft-ish edge) ----
    start = m.v.size();
    for (int i = 0; i < 5; ++i) {
        const float grow = 0.05f * (float)(4 - i);
        m.floorQuad(-(kBoardHalfX + 0.02f + grow), -(kBoardHalfZ + 0.02f + grow),
                    (kBoardHalfX + 0.02f + grow), (kBoardHalfZ + 0.02f + grow), 0.002f + 0.0001f * i,
                    rgb(0.0f, 0.0f, 0.0f, 0.07f));
    }
    _shadow = m.mark(start);

    _vertexBuffer = [_device newBufferWithBytes:m.v.data()
                                         length:m.v.size() * sizeof(GXXRVertex)
                                        options:MTLResourceStorageModeShared];
    _vertexBuffer.label = @"GXXR test scene vertices";
    NSLog(@"[GXXR] test scene: %zu vertices (%lu bytes)", m.v.size(), (unsigned long)(m.v.size() * sizeof(GXXRVertex)));
}

#pragma mark - Encoding

- (MTLRenderPassDescriptor*)passWithColor:(id<MTLTexture>)color
                                    slice:(NSUInteger)slice
                                    depth:(id<MTLTexture>)depth
                                    clear:(BOOL)clear {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].slice = slice;
    pass.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);  // alpha 0 = passthrough
    if (depth) {
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.slice = slice;
        pass.depthAttachment.loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;  // compositor uses depth for reprojection
        pass.depthAttachment.clearDepth = 0.0;                   // reverse-Z: 0 = far
    }
    return pass;
}

- (void)encodeTestSceneInto:(id<MTLCommandBuffer>)commandBuffer
                      color:(id<MTLTexture>)color
                 colorSlice:(NSUInteger)slice
                      depth:(id<MTLTexture>)depth
                   viewport:(MTLViewport)viewport
              clipFromWorld:(simd_float4x4)clipFromWorld
                eyePosition:(simd_float3)eyePosition
             worldFromBoard:(simd_float4x4)worldFromBoard
                  hasBoard:(BOOL)hasBoard
                       time:(float)timeSeconds
                      clear:(BOOL)clear {
    id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:[self passWithColor:color slice:slice depth:depth clear:clear]];
    enc.label = @"GXXR test tabletop";
    [enc setViewport:viewport];
    [enc setCullMode:MTLCullModeNone];
    [enc setVertexBuffer:_vertexBuffer offset:0 atIndex:GXXRBufferIndexVertices];

    GXXRSceneUniforms u = {};
    u.clipFromWorld = clipFromWorld;
    u.worldFromModel = matrix_identity_float4x4;
    u.eyePosition = simd_make_float4(eyePosition, 1.0f);
    u.lightDirection = simd_make_float4(simd_normalize(simd_make_float3(0.35f, 0.85f, 0.40f)), 0.0f);
    u.params = simd_make_float4(timeSeconds, 3.0f, 6.0f, 0.0f);

    auto draw = ^(Range r, const GXXRSceneUniforms& uniforms) {
        [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:GXXRBufferIndexUniforms];
        [enc setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:GXXRBufferIndexUniforms];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:r.first vertexCount:r.count];
    };

    // Opaque pass.
    [enc setRenderPipelineState:_scenePipeline];
    [enc setDepthStencilState:_depthWrite];
    if (hasBoard) {
        GXXRSceneUniforms bu = u;
        bu.worldFromModel = worldFromBoard;
        draw(_board, bu);

        // Animated orbiter, in board space.
        const float t = timeSeconds;
        simd_float4x4 local = simd_mul(translation(simd_make_float3(0.15f * std::cos(t * 0.9f), 0.05f + 0.01f * std::sin(t * 2.3f), 0.15f * std::sin(t * 0.9f))),
                                       rotationY(t * 1.7f));
        GXXRSceneUniforms ou = u;
        ou.worldFromModel = simd_mul(worldFromBoard, local);
        draw(_orbiter, ou);
    }

    // Translucent ground reference (premultiplied), depth-tested, no depth write.
    [enc setRenderPipelineState:_groundPipeline];
    [enc setDepthStencilState:_depthTestOnly];
    // Floor is 0.8 m below the play surface (XR_TABLETOP_SURFACE_HEIGHT_M); that is world Y = 0 on
    // floor-origin devices and head-relative on the simulator.
    const float floorY = hasBoard ? worldFromBoard.columns[3].y - 0.8f : 0.0f;
    GXXRSceneUniforms gu = u;
    gu.worldFromModel = translation(simd_make_float3(0.0f, floorY, 0.0f));
    draw(_floorGrid, gu);
    if (hasBoard) {
        // Shadow lives on the floor directly below the board.
        simd_float4x4 onFloor = worldFromBoard;
        onFloor.columns[3].y = floorY;
        GXXRSceneUniforms su = u;
        su.worldFromModel = onFloor;
        draw(_shadow, su);
    }
    [enc endEncoding];
}

- (void)encodeCompositeInto:(id<MTLCommandBuffer>)commandBuffer
                     source:(id<MTLTexture>)source
                      flags:(uint32_t)flags
                      color:(id<MTLTexture>)color
                 colorSlice:(NSUInteger)slice
                      depth:(id<MTLTexture>)depth
                   viewport:(MTLViewport)viewport
                      clear:(BOOL)clear {
    id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:[self passWithColor:color slice:slice depth:depth clear:clear]];
    enc.label = @"GXXR composite external eye";
    [enc setViewport:viewport];
    [enc setCullMode:MTLCullModeNone];
    [enc setRenderPipelineState:_compositePipeline];
    [enc setDepthStencilState:_depthOff];
    GXXRCompositeParams p = {};
    p.flags = flags;
    [enc setFragmentBytes:&p length:sizeof(p) atIndex:GXXRBufferIndexCompositeParams];
    [enc setFragmentTexture:source atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
}

- (void)encodeClearInto:(id<MTLCommandBuffer>)commandBuffer
                  color:(id<MTLTexture>)color
             colorSlice:(NSUInteger)slice
                  depth:(id<MTLTexture>)depth {
    id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:[self passWithColor:color slice:slice depth:depth clear:YES]];
    enc.label = @"GXXR clear";
    [enc endEncoding];
}

@end
