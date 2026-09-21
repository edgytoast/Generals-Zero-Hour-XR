// GXXREngineSession.h - the app side of the engine hand-off (see GeneralsMD/Code/Main/visionos/GXEngineHostServices.h).
//
// One process-wide session owns what outlives an immersive space: the ANGLE context (created and made current on the
// ENGINE thread), the render-target ring and the two mailboxes. The compositor loop (GXXRBridge.mm) only ever uses the
// mailboxes and the ring's Metal-side methods; it never touches GL. Closing and re-opening the immersive space
// therefore restarts only the compositor loop: the context, the ring and the latest published frame survive.
//
// The session installs its services into GXEngineHost from a constructor, so the launcher can call
// GXEngineHost_Start without any extra setup.
#import <Foundation/Foundation.h>

#import "GXXRANGLEContext.h"
#import "GXXRFrameMailbox.h"
#import "GXXRTargetRing.h"

NS_ASSUME_NONNULL_BEGIN

@interface GXXREngineSession : NSObject
+ (GXXREngineSession*)shared;
@property(nonatomic, readonly) GXXRFrameMailbox* mailbox;
/// nil until the engine thread attached (the ring is created there).
@property(nonatomic, readonly, nullable) GXXRTargetRing* ring;
@property(nonatomic, readonly, nullable) GXXRANGLEContext* context;
@end

#ifdef __cplusplus
extern "C" {
#endif
/// Starts the synthetic GLES3 test client on the engine thread (launch argument -fakeEngine). Idempotent.
bool GXXRBridgeStartFakeEngine(void);
#ifdef __cplusplus
}
#endif

NS_ASSUME_NONNULL_END
