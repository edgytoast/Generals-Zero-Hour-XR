// GXXRStatusPanel.h - the Metal-drawn "loading" indicator the compositor shows while the engine has not published a frame
// yet (boot, or a boot failure): a text card (title, detail lines) and an optional rotating spinner, as world-anchored
// composite layers. It never touches GL and never waits on the engine. The card texture is redrawn (CoreGraphics, 4 Hz at most)
// only when its text changes; the spinner is a static texture that is rotated every frame.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <simd/simd.h>

#import "GXXRMetalRenderer.h"

NS_ASSUME_NONNULL_BEGIN

@interface GXXRStatusPanel : NSObject

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device;

/// `center` is the world position of the card, `viewer` the head position it turns towards (yaw only).
- (NSArray<GXXRCompositeLayer*>*)layersWithTitle:(NSString*)title
                                          detail:(NSString*)detail
                                         spinner:(BOOL)spinner
                                            time:(CFTimeInterval)time
                                          center:(simd_float3)center
                                          viewer:(simd_float3)viewer;

@end

NS_ASSUME_NONNULL_END
