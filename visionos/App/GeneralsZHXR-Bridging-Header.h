// Bridging header for the Swift shell. Only plain-C surfaces from the
// Objective-C++ layers are exposed here; the C++ engine-facing headers
// (Platform/XR*.h) stay out of Swift.
#import "GXXRBridge.h"
#import "GXXRInput.h"
#import "PlatformLifecycle.h"
