// GXXRBridge.h - small C-callable surface between the SwiftUI shell and the
// Objective-C++ compositor/render thread. Imported by the Swift bridging
// header, so keep it plain C + Compositor Services types only.
//
// A C++ engine attaches later by including Platform/XRPresentation.h and
// registering XRPresentation_SetFrameCallback; it never needs this header.
#ifndef GXXR_BRIDGE_H
#define GXXR_BRIDGE_H

#import <CompositorServices/CompositorServices.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Snapshot of the render loop for the launcher UI and for logs. All strings are
/// NUL-terminated and truncated to fit.
typedef struct GXXRBridgeStatus {
    bool attached;              // a LayerRenderer has been handed to the bridge
    int32_t layerState;         // 1 paused, 2 running, 3 invalidated (cp_layer_renderer_state); 0 unknown
    uint64_t framesRendered;    // frames whose drawables were presented
    uint64_t framesSkipped;     // frames where the callback returned XR_FRAME_SKIP
    double fps;                 // presented frames per second, 1 s window
    uint32_t drawableCount;     // drawables returned by the last frame
    uint32_t viewCount;         // views in the first drawable of the last frame
    bool headTracked;           // false = fallback head pose (no ARKit device anchor)
    bool placementValid;        // tabletop has been placed in the world
    bool externalEyeTextures;   // engine-simulation path in use
    char layout[16];            // "layered" | "shared" | "dedicated"
    char colorFormat[24];       // e.g. "bgra8Unorm_srgb"
    char depthFormat[24];
    bool foveation;
    char lastMessage[160];      // most recent notable event / error
} GXXRBridgeStatus;

/// Hand the layer renderer from CompositorLayer{} to the bridge. Starts the render
/// thread. Safe to call again after the immersive space was dismissed and reopened.
void GXXRBridgeAttachLayerRenderer(cp_layer_renderer_t layerRenderer);

/// Copy the current status. Cheap; safe from any thread.
void GXXRBridgeGetStatus(GXXRBridgeStatus* outStatus);

/// Boolean options, read when the next frame loop starts:
///   "externalEyeTextures": render the test scene into offscreen per-eye textures and
///     submit them through XRPresentation_SubmitEyeTexture, so the compositor-side
///     copy path the real engine will use is exercised.
void GXXRBridgeSetBoolOption(const char* key, bool value);

/// Ask for the tabletop to be re-placed in front of the current head pose.
void GXXRBridgeRecenter(void);

/// Lifecycle events from the shell (see PlatformLifecycle.h for the values).
void GXXRBridgeNotifyLifecycle(int32_t event);

/// Game data directory as a UTF-8 string plus a presence heuristic, for the launcher.
bool GXXRBridgeGetGameDataInfo(char* outPath, uint32_t capacity, bool* outLooksPresent);

#ifdef __cplusplus
}
#endif

#endif  // GXXR_BRIDGE_H
