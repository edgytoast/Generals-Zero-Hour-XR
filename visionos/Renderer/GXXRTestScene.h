// GXXRTestScene.h - the TEMPORARY renderer-validation scene, packaged as a frame
// client that speaks Platform/XRPresentation.h. The real engine replaces this by
// registering its own XRFrameCallback; nothing else in the shell changes.
#import <Foundation/Foundation.h>

#import "GXXRMetalRenderer.h"
#include "XRPresentation.h"

NS_ASSUME_NONNULL_BEGIN

@interface GXXRTestScene : NSObject

/// @param external YES: render each eye into an offscreen texture and hand it over through
///        XRPresentation_SubmitEyeTexture (emulates the GL-on-Metal engine path).
///        NO: draw straight into the compositor drawable.
- (instancetype)initWithRenderer:(GXXRMetalRenderer*)renderer externalEyeTextures:(BOOL)external;

/// The XRFrameCallback body. Runs on the compositor render thread.
- (XRFrameResult)renderFrame:(const XRFrameInfo*)frame;

@end

NS_ASSUME_NONNULL_END
