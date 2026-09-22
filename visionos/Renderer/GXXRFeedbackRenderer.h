// GXXRFeedbackRenderer.h - Metal drawing of the interaction layer's per-frame feedback (GXHostFeedback,
// GeneralsMD/Code/Main/visionos/GXEngineHostServices.h): box-select rectangle, board grab bar, cursor and panel-pointer
// markers, placement / Ground View reticles, the hover outline, the destination waypoint, and the comfort fade veil.
//
// World markers (box, grab bar, cursor, reticles, hover, waypoint) are drawn depth-tested and depth-writing at their true
// world position, in the SAME render pass as the composite layers (after them, so they sit in front of a panel they touch);
// the comfort fade veil is a separate, depth-ignoring full-view pass drawn last.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <simd/simd.h>

#include "GXEngineHostServices.h" // struct GXHostFeedback, GXHostLayer

NS_ASSUME_NONNULL_BEGIN

@interface GXXRFeedbackRenderer : NSObject

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                             colorFormat:(MTLPixelFormat)colorFormat
                             depthFormat:(MTLPixelFormat)depthFormat;

/// Draws the world-space markers of `feedback` into `color`/`depth` with the eye's `clipFromWorld`. `layerPose` maps a
/// GXHostFeedback.pointerLayer index (into the frame's published layers) to that layer's world pose, for the panel-pointer
/// marker: pass a block that looks the pose up, or nil results in the marker being skipped. Loads the existing color/depth
/// (encode after the eye composite and the layers).
- (void)encodeMarkersInto:(id<MTLCommandBuffer>)commandBuffer
                    color:(id<MTLTexture>)color
               colorSlice:(NSUInteger)slice
                    depth:(id<MTLTexture>)depth
                 viewport:(MTLViewport)viewport
            clipFromWorld:(simd_float4x4)clipFromWorld
                 feedback:(const GXHostFeedback*)feedback
               layerPoses:(NSArray<NSValue*>*)layerWorldFromQuad;

/// Draws the full-view comfort fade veil (black at `alpha`) over the whole viewport, on top of everything. No depth test.
- (void)encodeFadeInto:(id<MTLCommandBuffer>)commandBuffer
                  color:(id<MTLTexture>)color
             colorSlice:(NSUInteger)slice
               viewport:(MTLViewport)viewport
                  alpha:(float)alpha;

@end

NS_ASSUME_NONNULL_END
