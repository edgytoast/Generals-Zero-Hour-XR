// GXXRHostFrameClient.h - what the compositor loop asks of whatever renders into the ANGLE
// target ring: the GLES3 test scene today, the engine bridge (package C) later.
//
// Register a client with GXXRBridgeSetHostFrameClient() (GXXRBridgeHost.h) before the immersive
// space opens. Everything here runs on the render thread with the ANGLE context current.
#import <Foundation/Foundation.h>

#import "GXXRANGLEContext.h"
#import "GXXRMetalRenderer.h"
#import "GXXRTargetRing.h"
#include "XRPresentation.h"

NS_ASSUME_NONNULL_BEGIN

@protocol GXXRHostFrameClient <NSObject>

/// One frame. The ring slot has been acquired (`[ring fillTargets:]` describes it). Render with GL
/// (for the engine: d3d8gles_SetXRHostTargets, the engine frame, d3d8gles_InvalidateCachedState),
/// submit the finished eye textures with XRPresentation_SubmitEyeTexture (texture = the ring's
/// MTLTexture for that eye, flags = XR_SUBMIT_FLIP_Y | XR_SUBMIT_PREMULTIPLIED_ALPHA) and return
/// XR_FRAME_SUBMITTED_TEXTURES (or XR_FRAME_SKIP). Do NOT glFlush/glFinish: the host does that.
- (XRFrameResult)renderFrame:(const XRFrameInfo*)frame;

@optional

/// Called once each time a render loop starts (first loop and after every immersive-space re-open),
/// after the context is current and the ring exists. A persistent client (the engine) keeps its own
/// state across calls and only re-binds to the new `ring`.
- (void)hostDidAttachWithContext:(GXXRANGLEContext*)context ring:(GXXRTargetRing*)ring;

/// World-anchored layers to composite after the eyes, using the current slot's textures (UI / world /
/// game). Called after -renderFrame: on the same frame.
- (NSArray<GXXRCompositeLayer*>*)compositeLayersForFrame:(const XRFrameInfo*)frame;

/// The render loop is ending (layer invalidated); the context is still current, the ring not yet torn
/// down. Release anything that references the ring's textures. Not "the engine is done": the engine
/// survives loops.
- (void)hostWillDetach;

@end

NS_ASSUME_NONNULL_END
