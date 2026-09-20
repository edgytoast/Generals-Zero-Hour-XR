// GXXRMetalRenderer.h - Metal drawing helpers for the visionOS shell.
//
// Two jobs:
//  1. Draw the temporary stereo test tabletop into any color/depth target
//     (the compositor drawable directly, or an offscreen per-eye texture that
//     emulates what the real GL-on-Metal engine will hand over).
//  2. Composite an externally produced per-eye MTLTexture into a compositor
//     drawable view (the path the real engine will use).
//
// Depth is REVERSE-Z (1 = near, 0 = far) because Compositor Services requires it.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <simd/simd.h>

NS_ASSUME_NONNULL_BEGIN

@interface GXXRMetalRenderer : NSObject

@property(nonatomic, readonly) id<MTLDevice> device;
@property(nonatomic, readonly) MTLPixelFormat colorFormat;
@property(nonatomic, readonly) MTLPixelFormat depthFormat;

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                            colorFormat:(MTLPixelFormat)colorFormat
                            depthFormat:(MTLPixelFormat)depthFormat;

/// Encodes one render pass that draws the test tabletop scene.
/// @param clear YES clears color to transparent black (alpha 0 shows passthrough) and depth to 0 (far).
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
                      clear:(BOOL)clear;

/// Encodes one render pass that draws `source` over the whole viewport (premultiplied alpha).
/// `flags` are the GXXR_COMPOSITE_* bits from ShaderTypes.h.
- (void)encodeCompositeInto:(id<MTLCommandBuffer>)commandBuffer
                     source:(id<MTLTexture>)source
                      flags:(uint32_t)flags
                      color:(id<MTLTexture>)color
                 colorSlice:(NSUInteger)slice
                      depth:(id<MTLTexture>)depth
                   viewport:(MTLViewport)viewport
                      clear:(BOOL)clear;

/// Encodes an empty pass that just clears the target (used when a frame is skipped but the drawable must still be presented).
- (void)encodeClearInto:(id<MTLCommandBuffer>)commandBuffer
                  color:(id<MTLTexture>)color
             colorSlice:(NSUInteger)slice
                  depth:(id<MTLTexture>)depth;

@end

NS_ASSUME_NONNULL_END
