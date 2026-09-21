// GXXRGLTestScene.h - GLES 3.0 (GLSL ES 3.00) test scene that runs on ANGLE on the ENGINE thread, standing in for the
// engine when there is no game data (launch argument -fakeEngine; GXXRFakeEngine.mm wraps it as an engine client).
//
//  * The same test tabletop as the direct-Metal scene (board, units, floor grid, contact
//    shadow, animated orbiter; shared mesh in Renderer/GXXRTestGeometry.h), drawn PER EYE with the
//    eye's view/projection from the head snapshot into the frame's stereo targets (one target per eye,
//    or one atlas target with per-eye rectangles when the ring is in atlas mode).
//  * A 1280x720 "UI panel" test pattern (title bar, buttons, text-like bars, a frame counter)
//    drawn into the frame's UI target; the output describes it as a world-anchored layer.
//
// Like the engine will, it emits gamma-encoded (sRGB) values, premultiplied by coverage, into
// non-sRGB RGBA8 targets; the Metal composite decodes them. It never touches Metal or the ring: it only sees GL names.
#import <Foundation/Foundation.h>

#import "GXXRANGLEContext.h"
#include "GXEngineHostServices.h"
#include "GXXRD3D8GLES.h"
#include "XRPresentation.h"

NS_ASSUME_NONNULL_BEGIN

@interface GXXRGLTestScene : NSObject

/// The ANGLE context must be current (engine thread).
- (nullable instancetype)initWithContext:(GXXRANGLEContext*)context;

/// Renders one frame into `targets` (the ring slot of this frame). Returns NO when nothing was drawn.
- (BOOL)renderFrame:(const XRFrameInfo*)frame targets:(const struct D3D8GLES_XRTargets*)targets output:(GXHostFrameOutput*)output;

/// Deletes GL objects. The ANGLE context must be current.
- (void)teardown;

@end

NS_ASSUME_NONNULL_END
