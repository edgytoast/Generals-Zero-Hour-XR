// GXXRMetalRenderer.mm - see header. Objective-C++ with ARC.
#import "GXXRMetalRenderer.h"

#include <cmath>
#include <cstddef>
#include <vector>

#import "GXXRTestGeometry.h"
#import "ShaderTypes.h"

using gxxr::Range;

@implementation GXXRCompositeLayer

+ (instancetype)layerWithName:(NSString*)name
                      texture:(id<MTLTexture>)texture
                     position:(simd_float3)position
                  orientation:(simd_quatf)orientation
                   sizeMeters:(simd_float2)sizeMeters
                        flipY:(BOOL)flipY {
    GXXRCompositeLayer* l = [[GXXRCompositeLayer alloc] init];
    l.name = name;
    l.texture = texture;
    l.position = position;
    l.orientation = orientation;
    l.sizeMeters = sizeMeters;
    l.flipY = flipY;
    l.premultipliedAlpha = YES;
    l.gammaEncoded = ![GXXRMetalRenderer isSRGBFormat:texture.pixelFormat];
    return l;
}

- (simd_float4x4)worldFromQuad {
    simd_float4x4 m = simd_matrix4x4(_orientation);
    m.columns[3] = simd_make_float4(_position, 1.0f);
    return m;
}

@end

@implementation GXXRMetalRenderer {
    id<MTLDevice> _device;
    MTLPixelFormat _colorFormat;
    MTLPixelFormat _depthFormat;

    id<MTLRenderPipelineState> _scenePipeline;
    id<MTLRenderPipelineState> _groundPipeline;
    id<MTLRenderPipelineState> _compositePipeline;
    id<MTLRenderPipelineState> _layerPipeline;
    id<MTLDepthStencilState> _depthAlwaysWrite;  // fullscreen eye composite: writes the fragment's depth
    id<MTLDepthStencilState> _depthWrite;   // reverse-Z, write on
    id<MTLDepthStencilState> _depthTestOnly;
    id<MTLDepthStencilState> _depthOff;

    id<MTLBuffer> _vertexBuffer;
    Range _board;      // static, board-local
    Range _orbiter;    // board-local, animated per frame
    Range _floorGrid;  // world space
    Range _shadow;     // floor-relative, board-local xz
}

+ (BOOL)isSRGBFormat:(MTLPixelFormat)f {
    return f == MTLPixelFormatBGRA8Unorm_sRGB || f == MTLPixelFormatRGBA8Unorm_sRGB;
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

    MTLRenderPipelineDescriptor* ld = [MTLRenderPipelineDescriptor new];
    ld.vertexFunction = [library newFunctionWithName:@"layer_vertex"];
    ld.fragmentFunction = [library newFunctionWithName:@"layer_fragment"];
    ld.colorAttachments[0].pixelFormat = colorFormat;
    ld.depthAttachmentPixelFormat = depthFormat;
    ld.label = @"GXXR layer quad (premultiplied)";
    MTLRenderPipelineColorAttachmentDescriptor* lca = ld.colorAttachments[0];
    lca.blendingEnabled = YES;
    lca.rgbBlendOperation = MTLBlendOperationAdd;
    lca.alphaBlendOperation = MTLBlendOperationAdd;
    lca.sourceRGBBlendFactor = MTLBlendFactorOne;
    lca.sourceAlphaBlendFactor = MTLBlendFactorOne;
    lca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    lca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    _layerPipeline = [device newRenderPipelineStateWithDescriptor:ld error:&error];
    if (!_layerPipeline) { NSLog(@"[GXXR] layer pipeline failed: %@", error); return nil; }

    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionGreaterEqual;  // reverse-Z
    dd.depthWriteEnabled = YES;
    _depthWrite = [device newDepthStencilStateWithDescriptor:dd];
    dd.depthWriteEnabled = NO;
    _depthTestOnly = [device newDepthStencilStateWithDescriptor:dd];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    _depthOff = [device newDepthStencilStateWithDescriptor:dd];
    dd.depthWriteEnabled = YES;
    _depthAlwaysWrite = [device newDepthStencilStateWithDescriptor:dd];

    [self buildGeometry];
    return self;
}

#pragma mark - Geometry

- (void)buildGeometry {
    gxxr::TestGeometry g = gxxr::BuildTestGeometry();
    _board = g.board;
    _orbiter = g.orbiter;
    _floorGrid = g.floorGrid;
    _shadow = g.shadow;
    _vertexBuffer = [_device newBufferWithBytes:g.vertices.data()
                                         length:g.vertices.size() * sizeof(GXXRVertex)
                                        options:MTLResourceStorageModeShared];
    _vertexBuffer.label = @"GXXR test scene vertices";
    NSLog(@"[GXXR] test scene: %zu vertices (%lu bytes)", g.vertices.size(), (unsigned long)(g.vertices.size() * sizeof(GXXRVertex)));
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
        simd_float4x4 local = gxxr::OrbiterLocalTransform(t);
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
    gu.worldFromModel = gxxr::Translation(simd_make_float3(0.0f, floorY, 0.0f));
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
    [self encodeEyeCompositeInto:commandBuffer source:source flags:flags uvRect:simd_make_float4(0, 0, 1, 1) constantDepth:0.0f
                           color:color colorSlice:slice depth:depth viewport:viewport clear:clear];
}

- (void)encodeEyeCompositeInto:(id<MTLCommandBuffer>)commandBuffer
                        source:(id<MTLTexture>)source
                         flags:(uint32_t)flags
                        uvRect:(simd_float4)uvRect
                 constantDepth:(float)constantDepth
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
    [enc setDepthStencilState:_depthAlwaysWrite];
    GXXRCompositeParams p = {};
    p.flags = flags;
    p.depth = constantDepth;
    p.uvRect = uvRect;
    [enc setFragmentBytes:&p length:sizeof(p) atIndex:GXXRBufferIndexCompositeParams];
    [enc setFragmentTexture:source atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
}

- (void)encodeLayers:(NSArray<GXXRCompositeLayer*>*)layers
                into:(id<MTLCommandBuffer>)commandBuffer
               color:(id<MTLTexture>)color
          colorSlice:(NSUInteger)slice
               depth:(id<MTLTexture>)depth
            viewport:(MTLViewport)viewport
       clipFromWorld:(simd_float4x4)clipFromWorld {
    if (layers.count == 0) return;
    id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:[self passWithColor:color slice:slice depth:depth clear:NO]];
    enc.label = @"GXXR composite layers";
    [enc setViewport:viewport];
    [enc setCullMode:MTLCullModeNone];
    [enc setRenderPipelineState:_layerPipeline];
    [enc setDepthStencilState:_depthWrite];  // reverse-Z GreaterEqual + write
    for (GXXRCompositeLayer* layer in layers) {
        if (!layer.texture) continue;
        GXXRLayerUniforms u = {};
        u.clipFromWorld = clipFromWorld;
        u.worldFromQuad = layer.worldFromQuad;
        u.halfSize = layer.sizeMeters * 0.5f;
        GXXRCompositeParams p = {};
        p.flags = (layer.flipY ? GXXR_COMPOSITE_FLIP_Y : 0u) | (layer.premultipliedAlpha ? 0u : GXXR_COMPOSITE_PREMULTIPLY) |
                  (layer.gammaEncoded ? GXXR_COMPOSITE_SRGB_DECODE : 0u);
        p.uvRect = simd_make_float4(0, 0, 1, 1);
        [enc setVertexBytes:&u length:sizeof(u) atIndex:GXXRBufferIndexUniforms];
        [enc setFragmentBytes:&p length:sizeof(p) atIndex:GXXRBufferIndexCompositeParams];
        [enc setFragmentTexture:layer.texture atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
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
