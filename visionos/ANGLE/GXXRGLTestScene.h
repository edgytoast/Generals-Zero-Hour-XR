// GXXRGLTestScene.h - GLES 3.0 (GLSL ES 3.00) test scene that runs on ANGLE and renders into the
// host target ring, standing in for the engine until the engine is attached.
//
//  * The same test tabletop as the direct-Metal scene (board, units, floor grid, contact
//    shadow, animated orbiter; shared mesh in Renderer/GXXRTestGeometry.h), drawn PER EYE with the
//    eye's view/projection from XRFrameInfo into the ring's stereo targets (one target per eye,
//    or one atlas target with per-eye rectangles when the ring is in atlas mode).
//  * A 1280x720 "UI panel" test pattern (title bar, buttons, text-like bars, a frame counter)
//    drawn into the ring's UI target; the bridge composites it as a world-anchored quad.
//
// Like the engine will, it emits gamma-encoded (sRGB) values, premultiplied by coverage, into
// non-sRGB RGBA8 targets; the Metal composite decodes them.
#import <Foundation/Foundation.h>

#import "GXXRANGLEContext.h"
#import "GXXRMetalRenderer.h"
#import "GXXRTargetRing.h"
#include "XRPresentation.h"

NS_ASSUME_NONNULL_BEGIN

/// What the bridge asks of whatever renders into the target ring (this test scene now, the engine
/// host later).
@protocol GXXRHostFrameClient <NSObject>
/// Runs on the render thread with the ANGLE context current and the ring slot acquired. Renders
/// (GL), submits per-eye textures through XRPresentation_SubmitEyeTexture and returns the result.
- (XRFrameResult)renderFrame:(const XRFrameInfo*)frame;
@optional
/// World-anchored layers to composite after the eyes (UI / world / game textures of the current slot).
- (NSArray<GXXRCompositeLayer*>*)compositeLayersForFrame:(const XRFrameInfo*)frame;
@end

@interface GXXRGLTestScene : NSObject <GXXRHostFrameClient>

/// The ANGLE context must be current. The scene declares its UI target on the ring.
- (nullable instancetype)initWithContext:(GXXRANGLEContext*)context ring:(GXXRTargetRing*)ring;

/// Deletes GL objects. The ANGLE context must be current.
- (void)teardown;

@end

NS_ASSUME_NONNULL_END
