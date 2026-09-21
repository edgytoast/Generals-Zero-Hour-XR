// GXXRBridgeHost.h - Objective-C++ only additions to the bridge for the engine host (package C).
// Not imported by Swift; GXXRBridge.h stays plain C.
#import <Foundation/Foundation.h>

#import "GXXRHostFrameClient.h"

NS_ASSUME_NONNULL_BEGIN

#ifdef __cplusplus
extern "C" {
#endif

/// Installs (or, with nil, removes) the client that renders into the ANGLE target ring. While a client is
/// installed the bridge runs the ANGLE path (ANGLE context, ring, per-frame GL sync, Metal composite)
/// regardless of -angleTestScene, and does not create the built-in GLES3 test scene. Takes effect at the next
/// render-loop start; call it before the immersive space opens. The client is retained.
void GXXRBridgeSetHostFrameClient(id<GXXRHostFrameClient> _Nullable client);

#ifdef __cplusplus
}
#endif

NS_ASSUME_NONNULL_END
