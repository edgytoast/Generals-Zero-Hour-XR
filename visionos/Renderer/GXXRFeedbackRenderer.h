// GXXRFeedbackRenderer.h - Metal drawing of the interaction layer's per-frame feedback (GXHostFeedback,
// GeneralsMD/Code/Main/visionos/GXEngineHostServices.h): box-select rectangle, board grab bar, cursor and panel-pointer
// markers, placement / Ground View reticles, the hover outline, the destination waypoint, and the comfort fade veil.
//
// World markers (box, grab bar, cursor, reticles, hover, waypoint) are projected from their true world position but drawn
// WITHOUT a depth test, in the SAME render pass as the composite layers (after them, so they always sit in front of a
// panel or the board picture they touch): the board/UI picture behind them is a flat 2D render composited as a full-screen
// quad with one constant depth for reprojection (docs/visionos-engine-host.md section 4), not real per-pixel depth, so a
// true depth test against it would discard a marker wherever its own true distance differs from that single constant --
// not an occlusion decision. The comfort fade veil is a separate, also depth-ignoring, full-view pass drawn last.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <simd/simd.h>

#include "GXEngineHostServices.h" // struct GXHostFeedback, GXHostLayer

NS_ASSUME_NONNULL_BEGIN

@interface GXXRFeedbackRenderer : NSObject

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                             colorFormat:(MTLPixelFormat)colorFormat
                             depthFormat:(MTLPixelFormat)depthFormat;

/// Draws the world-space markers of `feedback` into `color`/`depth` with the eye's `clipFromWorld`. `layerWorldFromQuad`/
/// `layerCount` index exactly like GXHostFrameOutput.layers (GXHostFeedback.pointerLayer is an index into that same
/// array): the world pose of each published layer, for the panel-pointer marker (an index outside the array, or a nil
/// array, just skips that marker). Loads the existing color/depth (encode after the eye composite and the layers).
- (void)encodeMarkersInto:(id<MTLCommandBuffer>)commandBuffer
                    color:(id<MTLTexture>)color
               colorSlice:(NSUInteger)slice
                    depth:(id<MTLTexture>)depth
                 viewport:(MTLViewport)viewport
            clipFromWorld:(simd_float4x4)clipFromWorld
                 feedback:(const GXHostFeedback*)feedback
       layerWorldFromQuad:(nullable const simd_float4x4*)layerWorldFromQuad
               layerCount:(NSUInteger)layerCount;

/// Draws the full-view comfort fade veil (black at `alpha`) over the whole viewport, on top of everything. No depth test.
- (void)encodeFadeInto:(id<MTLCommandBuffer>)commandBuffer
                  color:(id<MTLTexture>)color
             colorSlice:(NSUInteger)slice
               viewport:(MTLViewport)viewport
                  alpha:(float)alpha;

@end

NS_ASSUME_NONNULL_END
