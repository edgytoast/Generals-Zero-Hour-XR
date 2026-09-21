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
    // ---- appended for the ANGLE (GLES3-on-Metal) render path; older readers ignore these ----
    bool angleActive;           // frames come from the ANGLE/GLES3 scene (or engine), not the direct-Metal scene
    bool devicesMatch;          // ANGLE's MTLDevice is the Compositor Services device
    char renderer[112];         // GL_RENDERER of the ANGLE context ("" when ANGLE is not in use)
    char syncMode[40];          // "metal-shared-event" | "glFinish" | ""
    double glSubmitMs;          // CPU ms per frame issuing GL (scene/engine + glFlush), 1 s average
    double syncWaitMs;          // CPU ms per frame waiting on slot release / glFinish, 1 s average
    double compositeMs;         // CPU ms per frame encoding the Metal composite, 1 s average
    double frameMs;             // CPU ms per frame from submission start to commit, 1 s average
    uint32_t loopGeneration;    // increments every time a (re)opened immersive space starts a render loop
    uint64_t releaseTimeouts;   // ring-slot release waits that timed out
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
///   "angleTestScene": render the GLES 3.0 test scene through ANGLE (Metal) into host-owned
///     ring targets instead of the direct-Metal scene. Also enabled by the launch argument
///     -angleTestScene (read by the bridge itself).
/// Other launch arguments read by the bridge: -angleEyeScale <f>, -angleAtlas,
/// -angleSync glfinish, -angleTargetFormat rgba|bgra, -angleNoUIPanel.
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
