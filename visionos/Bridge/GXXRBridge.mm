// GXXRBridge.mm - owns the Compositor Services render thread.
//
// Responsibilities:
//   * run the CP frame loop (query frame -> update -> wait optimal input time ->
//     submission -> per-drawable encode/present) on a dedicated thread;
//   * track the head with ARKit's world-tracking provider and set the device
//     anchor on every drawable (needed for correct late-stage reprojection);
//   * translate drawable views into the platform-neutral XRFrameInfo;
//   * ENGINE MODE (an engine or the fake engine was started): publish that snapshot to the engine thread (lock
//     protected mailbox) and composite the LATEST COMPLETED published engine frame, with the drawable's device anchor
//     set to the anchor that frame was rendered with so the system reprojects correctly; keep presenting the last
//     frame while the engine boots or stalls (a Metal loading indicator when nothing was published yet). This thread
//     NEVER touches GL and never waits for the engine;
//   * otherwise call the registered XRFrameCallback / draw the built-in Metal test tabletop;
//   * composite eye textures submitted through XRPresentation_SubmitEyeTexture;
//   * place the tabletop in front of the initial head pose.
//
// Verified against the Xcode 27 SDK headers: CompositorServices/{layer_renderer,
// frame, drawable, view, frame_timing}.h and ARKit/{session,world_tracking}.h.
#import "GXXRBridge.h"

#import <ARKit/ARKit.h>
#import <CompositorServices/CompositorServices.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <os/log.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

#include "PlatformFilesystem.h"
#include "PlatformLifecycle.h"
#include "XRInteraction.h"
#include "XRPresentation.h"

#import "GXXREngineSession.h"
#import "GXXRFeedbackRenderer.h"
#import "GXXRFrameMailbox.h"
#import "GXXRMetalRenderer.h"
#import "GXXRStatusPanel.h"
#import "GXXRTargetRing.h"
#import "GXXRTestScene.h"
#import "ShaderTypes.h"
#include "GXEngineHost.h"
#include "GXXRD3D8GLES.h"

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

namespace {

// Tabletop placement: board is 1.0 m x 0.6 m, its play surface 0.8 m above the
// floor origin, centered 0.9 m in front of the head (yaw only).
constexpr float kBoardHalfX = 0.5f;
constexpr float kBoardHalfZ = 0.3f;
constexpr float kBoardHeight = 0.8f;
constexpr float kBoardDistance = 0.9f;
// Head height used when ARKit gives no device anchor (e.g. some simulator states).
constexpr float kFallbackHeadHeight = 1.5f;
// Frames to wait for a tracked anchor before placing the board with the fallback pose.
constexpr uint64_t kFallbackPlacementFrames = 90;

struct Shared {
    std::mutex mutex;
    XRFrameCallback frameCallback = nullptr;
    void* frameUser = nullptr;
    XRSessionStateCallback stateCallback = nullptr;
    void* stateUser = nullptr;

    std::atomic<int> alphaMode{XR_ALPHA_MODE_PREMULTIPLIED_PASSTHROUGH};
    std::atomic<int> sessionState{XR_SESSION_IDLE};
    std::atomic<bool> recenterRequested{false};
    std::atomic<bool> optExternalEyeTextures{false};
    std::atomic<uint32_t> loopGeneration{0};

    bool placementValid = false;
    float worldFromBoard[16] = {};

    // Submissions during the current frame callback (render thread only).
    bool inFrame = false;
    bool submitted[XR_MAX_EYES] = {};
    XREyeSubmit submits[XR_MAX_EYES] = {};

    GXXRBridgeStatus status = {};
};

Shared& Sh() {
    static Shared s;
    return s;
}

// Retained submitted textures (ARC needs a strong reference outside the POD struct).
id<MTLTexture> gSubmittedTexture[XR_MAX_EYES];

os_log_t Log() {
    static os_log_t log = os_log_create("com.generalsx.zerohour.xr.vision", "compositor");
    return log;
}

void SetMessage(const char* msg) {
    Shared& s = Sh();
    std::lock_guard<std::mutex> lock(s.mutex);
    snprintf(s.status.lastMessage, sizeof(s.status.lastMessage), "%s", msg);
}

void SetSessionState(XRSessionState state) {
    Shared& s = Sh();
    XRSessionStateCallback cb = nullptr;
    void* user = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.sessionState.load() == (int)state) return;
        s.sessionState.store((int)state);
        cb = s.stateCallback;
        user = s.stateUser;
    }
    if (cb) cb(user, state);
}

const char* FormatName(MTLPixelFormat f) {
    switch (f) {
        case MTLPixelFormatBGRA8Unorm: return "bgra8Unorm";
        case MTLPixelFormatBGRA8Unorm_sRGB: return "bgra8Unorm_srgb";
        case MTLPixelFormatRGBA8Unorm: return "rgba8Unorm";
        case MTLPixelFormatRGBA8Unorm_sRGB: return "rgba8Unorm_srgb";
        case MTLPixelFormatRGBA16Float: return "rgba16Float";
        case MTLPixelFormatDepth32Float: return "depth32Float";
        case MTLPixelFormatDepth16Unorm: return "depth16Unorm";
        case MTLPixelFormatDepth32Float_Stencil8: return "depth32Float_stencil8";
        default: return "other";
    }
}

const char* LayoutName(cp_layer_renderer_layout l) {
    switch (l) {
        case cp_layer_renderer_layout_dedicated: return "dedicated";
        case cp_layer_renderer_layout_shared: return "shared";
        case cp_layer_renderer_layout_layered: return "layered";
        default: return "unknown";
    }
}

void Store(float* dst, simd_float4x4 m) { memcpy(dst, &m, sizeof(float) * 16); }

XRPose PoseFromMatrix(simd_float4x4 m) {
    XRPose p;
    p.position = XRVec3{m.columns[3].x, m.columns[3].y, m.columns[3].z};
    simd_quatf q = simd_quaternion(m);
    p.orientation = XRQuat{q.vector.x, q.vector.y, q.vector.z, q.vector.w};
    return p;
}

// Frustum half-angles from a Metal-style projection (right-up-back convention).
// clip.x = P00*x + P20*z, w = -z  =>  tan(right) = (1 + P20) / P00, tan(left) = (P20 - 1) / P00 (same for y).
XRFov FovFromProjection(simd_float4x4 p) {
    XRFov f;
    const float p00 = p.columns[0][0], p11 = p.columns[1][1];
    const float p20 = p.columns[2][0], p21 = p.columns[2][1];
    f.angle_right = std::atan((1.0f + p20) / p00);
    f.angle_left = std::atan((p20 - 1.0f) / p00);
    f.angle_up = std::atan((1.0f + p21) / p11);
    f.angle_down = std::atan((p21 - 1.0f) / p11);
    return f;
}

simd_float4x4 Translation(float x, float y, float z) {
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[3] = simd_make_float4(x, y, z, 1.0f);
    return m;
}

// One frame-loop thread at a time: a re-opened immersive space's loop waits for the previous one to finish.
// (The ANGLE context, the ring and the engine live on the engine thread and survive every loop.)
std::mutex gLoopMutex;

}  // namespace

// ---------------------------------------------------------------------------
// XRPresentation.h implementation
// ---------------------------------------------------------------------------

extern "C" {

void XRPresentation_SetFrameCallback(XRFrameCallback callback, void* user) {
    Shared& s = Sh();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.frameCallback = callback;
    s.frameUser = user;
}

void XRPresentation_SetSessionStateCallback(XRSessionStateCallback callback, void* user) {
    Shared& s = Sh();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.stateCallback = callback;
    s.stateUser = user;
}

bool XRPresentation_SubmitEyeTexture(const XREyeSubmit* submit) {
    Shared& s = Sh();
    if (!submit || !s.inFrame || submit->eye_index >= XR_MAX_EYES || !submit->texture) return false;
    s.submits[submit->eye_index] = *submit;
    s.submitted[submit->eye_index] = true;
    gSubmittedTexture[submit->eye_index] = (__bridge id<MTLTexture>)submit->texture;
    return true;
}

void XRPresentation_SetAlphaMode(XRAlphaMode mode) { Sh().alphaMode.store((int)mode); }
XRAlphaMode XRPresentation_GetAlphaMode(void) { return (XRAlphaMode)Sh().alphaMode.load(); }
void XRPresentation_Recenter(void) { Sh().recenterRequested.store(true); }
XRSessionState XRPresentation_GetSessionState(void) { return (XRSessionState)Sh().sessionState.load(); }

bool XRPresentation_GetTabletopPlacement(float out_world_from_board[16], float* out_half_extent_x_m, float* out_half_extent_z_m) {
    Shared& s = Sh();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.placementValid) return false;
    if (out_world_from_board) memcpy(out_world_from_board, s.worldFromBoard, sizeof(float) * 16);
    if (out_half_extent_x_m) *out_half_extent_x_m = kBoardHalfX;
    if (out_half_extent_z_m) *out_half_extent_z_m = kBoardHalfZ;
    return true;
}

// ---- C entry points for the Swift shell -----------------------------------

void GXXRBridgeGetStatus(GXXRBridgeStatus* out) {
    if (!out) return;
    Shared& s = Sh();
    std::lock_guard<std::mutex> lock(s.mutex);
    *out = s.status;
    out->placementValid = s.placementValid;
}

void GXXRBridgeSetBoolOption(const char* key, bool value) {
    if (!key) return;
    if (strcmp(key, "externalEyeTextures") == 0) Sh().optExternalEyeTextures.store(value);
    else if (strcmp(key, "angleTestScene") == 0 && value) GXXRBridgeStartFakeEngine();
}

void GXXRBridgeRecenter(void) { XRPresentation_Recenter(); }

void GXXRBridgeNotifyLifecycle(int32_t event) { PlatformLifecycle_Notify((PlatformLifecycleEvent)event); }

bool GXXRBridgeGetGameDataInfo(char* outPath, uint32_t capacity, bool* outLooksPresent) {
    const bool ok = PlatformFS_GetGameDataDir(outPath, capacity);
    if (outLooksPresent) *outLooksPresent = PlatformFS_GameDataLooksPresent();
    return ok;
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Compositor loop
// ---------------------------------------------------------------------------

// ---- Debug frame dump -------------------------------------------------------------------------------------------------
// GX_DEBUG_DUMP_FRAMES=<seconds> (e.g. 5): every <seconds> the compositor writes the published engine frame's eye and layer
// textures as PNGs (Metal blit into a shared buffer; never glReadPixels on a wrapped texture) plus a text description of
// the frame (stereoValid, atlas, layer names / poses / sizes, eye matrices) into <tmp>/gx-frame-dump/. For diagnosing
// "the engine renders but nothing shows up" without a headset. Off unless the variable is set.
static double GXXRDebugDumpInterval() {
    static const double interval = [] {
        const char* v = getenv("GX_DEBUG_DUMP_FRAMES");
        return v ? atof(v) : 0.0;
    }();
    return interval;
}

static NSString* GXXRDebugDumpDirectory() {
    NSString* dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"gx-frame-dump"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return dir;
}

/// Encodes a blit of `tex` (RGBA8/BGRA8) into a shared buffer; the PNG is written when `cb` completes.
static void GXXRDebugDumpTexture(id<MTLCommandBuffer> cb, id<MTLTexture> tex, NSString* path) {
    if (!tex || tex.width == 0 || tex.height == 0) return;
    if (tex.pixelFormat != MTLPixelFormatRGBA8Unorm && tex.pixelFormat != MTLPixelFormatBGRA8Unorm &&
        tex.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB && tex.pixelFormat != MTLPixelFormatBGRA8Unorm_sRGB) {
        fprintf(stderr, "[GXXR/dump] %s: pixel format %lu not dumped\n", path.lastPathComponent.UTF8String, (unsigned long)tex.pixelFormat);
        return;
    }
    const NSUInteger w = tex.width, h = tex.height, bpr = w * 4;
    id<MTLBuffer> buf = [tex.device newBufferWithLength:bpr * h options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:buf destinationOffset:0 destinationBytesPerRow:bpr destinationBytesPerImage:bpr * h];
    [blit endEncoding];
    const bool bgra = tex.pixelFormat == MTLPixelFormatBGRA8Unorm || tex.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
        const uint8_t* src = (const uint8_t*)buf.contents;
        // Count coverage so the log answers "is anything there?" without opening the image.
        size_t covered = 0;
        for (size_t i = 0; i < (size_t)w * h; ++i) covered += src[i * 4 + 3] > 0;
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        const CGBitmapInfo info = bgra ? (kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst)
                                       : (kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast);
        CGContextRef ctx = CGBitmapContextCreate((void*)src, w, h, 8, bpr, cs, info);
        CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : nullptr;
        if (img) {
            CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],
                                                                         CFSTR("public.png"), 1, nullptr);
            if (dst) {
                CGImageDestinationAddImage(dst, img, nullptr);
                CGImageDestinationFinalize(dst);
                CFRelease(dst);
            }
            CGImageRelease(img);
        }
        if (ctx) CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
        fprintf(stderr, "[GXXR/dump] %s %lux%lu covered=%.1f%%\n", path.lastPathComponent.UTF8String, (unsigned long)w,
                (unsigned long)h, 100.0 * (double)covered / (double)((size_t)w * h));
    }];
}

@interface GXXRCompositorLoop : NSObject
- (instancetype)initWithLayerRenderer:(cp_layer_renderer_t)layer;
- (void)start;
- (void)cancel;
@end

@implementation GXXRCompositorLoop {
    cp_layer_renderer_t _layer;
    NSThread* _thread;
    std::atomic<bool> _cancelled;

    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    GXXRMetalRenderer* _renderer;
    GXXRFeedbackRenderer* _feedbackRenderer;  // box-select rect, grab bar, cursor, placement/ground reticles, comfort fade
    GXXRTestScene* _testScene;

    // Engine mode (engine thread + decoupled compositor). Metal side only: no GL on this thread, ever.
    GXXREngineSession* _session;
    GXXRStatusPanel* _statusPanel;
    bool _deviceChecked;
    bool _devicesMatch;
    bool _indicatorPlaced;
    simd_float3 _indicatorCenter;
    GXEngineHostStatus _hostStatus;      // refreshed at 4 Hz (the log tail read is not per-frame work)
    CFTimeInterval _hostStatusAt;
    bool _statusPanelVisible;
    uint64_t _lastCompositedSeq;
    CFTimeInterval _lastDumpTime;  // GX_DEBUG_DUMP_FRAMES
    int _dumpIndex;

    // 1 s timing window (CPU ms, summed per frame)
    double _accComposite, _accFrame;
    uint64_t _accFrames;
    uint32_t _winNewFrames, _winRepeats;
    double _winMaxAgeMs;

    ar_session_t _arSession;
    ar_world_tracking_provider_t _worldProvider;
    bool _arRunning;

    cp_layer_renderer_layout _layout;
    bool _foveation;
    MTLPixelFormat _colorFormat;
    MTLPixelFormat _depthFormat;
    float _dbgHeadY;
    float _dbgFovDeg[4];  // left, right, up, down of eye 0
    uint32_t _dbgViewportW, _dbgViewportH;
}

- (instancetype)initWithLayerRenderer:(cp_layer_renderer_t)layer {
    self = [super init];
    if (self) {
        _layer = layer;
        _cancelled.store(false);
    }
    return self;
}

- (void)start {
    _thread = [[NSThread alloc] initWithTarget:self selector:@selector(run) object:nil];
    _thread.name = @"GXXR.Compositor";
    _thread.qualityOfService = NSQualityOfServiceUserInteractive;
    _thread.stackSize = 4 * 1024 * 1024;
    [_thread start];
}

- (void)cancel {
    _cancelled.store(true);
}

- (BOOL)setUp {
    _device = cp_layer_renderer_get_device(_layer);
    _queue = [_device newCommandQueue];
    _queue.label = @"GXXR compositor queue";

    cp_layer_renderer_configuration_t cfg = cp_layer_renderer_get_configuration(_layer);
    _colorFormat = cp_layer_renderer_configuration_get_color_format(cfg);
    _depthFormat = cp_layer_renderer_configuration_get_depth_format(cfg);
    _layout = cp_layer_renderer_configuration_get_layout(cfg);
    _foveation = cp_layer_renderer_configuration_get_foveation_enabled(cfg);

    _renderer = [[GXXRMetalRenderer alloc] initWithDevice:_device colorFormat:_colorFormat depthFormat:_depthFormat];
    if (!_renderer) {
        SetMessage("Metal renderer init failed (see log)");
        return NO;
    }
    // Non-fatal: the interaction feedback (box-select, grab bar, placement/ground markers, comfort fade) is supplementary;
    // its own init logs the reason and a nil renderer just means those markers are skipped this run.
    _feedbackRenderer = [[GXXRFeedbackRenderer alloc] initWithDevice:_device colorFormat:_colorFormat depthFormat:_depthFormat];
    const bool external = Sh().optExternalEyeTextures.load();
    _testScene = [[GXXRTestScene alloc] initWithRenderer:_renderer externalEyeTextures:external];
    _statusPanel = [[GXXRStatusPanel alloc] initWithDevice:_device];
    _session = [GXXREngineSession shared];

    // Head tracking. World tracking needs the world-sensing usage string in Info.plist.
    if (ar_world_tracking_provider_is_supported()) {
        _arSession = ar_session_create();
        ar_world_tracking_configuration_t wtc = ar_world_tracking_configuration_create();
        _worldProvider = ar_world_tracking_provider_create(wtc);
        ar_data_providers_t providers = ar_data_providers_create_with_data_providers(_worldProvider, nil);
        ar_session_run(_arSession, providers);
        _arRunning = true;
        os_log(Log(), "ARKit world tracking provider started");
    } else {
        os_log(Log(), "ARKit world tracking NOT supported; using fixed fallback head pose");
    }

    Shared& s = Sh();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        GXXRBridgeStatus& st = s.status;
        st.attached = true;
        st.externalEyeTextures = external;
        st.foveation = _foveation;
        snprintf(st.layout, sizeof(st.layout), "%s", LayoutName(_layout));
        snprintf(st.colorFormat, sizeof(st.colorFormat), "%s", FormatName(_colorFormat));
        snprintf(st.depthFormat, sizeof(st.depthFormat), "%s", FormatName(_depthFormat));
    }
    os_log(Log(), "layer configured: layout=%{public}s color=%{public}s depth=%{public}s foveation=%d external=%d",
           LayoutName(_layout), FormatName(_colorFormat), FormatName(_depthFormat), (int)_foveation, (int)external);
    fprintf(stderr, "[GXXR] layer configured: layout=%s color=%s depth=%s foveation=%d external=%d\n", LayoutName(_layout),
            FormatName(_colorFormat), FormatName(_depthFormat), (int)_foveation, (int)external);
    return YES;
}

- (void)run {
    // Only one loop at a time: a re-opened immersive space's loop waits here until the previous
    // one has torn down its per-loop resources and released the process-wide ANGLE context.
    std::lock_guard<std::mutex> loopLock(gLoopMutex);
    @autoreleasepool {
        if (_cancelled.load() || ![self setUp]) {
            SetSessionState(XR_SESSION_INVALIDATED);
            return;
        }
        const uint32_t generation = Sh().loopGeneration.fetch_add(1) + 1;
        {
            Shared& s = Sh();
            std::lock_guard<std::mutex> lock(s.mutex);
            s.status.loopGeneration = generation;
        }
        fprintf(stderr, "[GXXR] compositor loop generation %u started (engine active=%d)\n", generation, (int)GXEngineHost_IsActive());
    }
    cp_layer_renderer_state lastState = (cp_layer_renderer_state)0;
    uint64_t frameCounter = 0;
    uint64_t windowFrames = 0;
    CFTimeInterval windowStart = CACurrentMediaTime();
    uint64_t presented = 0, skipped = 0;

    while (!_cancelled.load()) {
        @autoreleasepool {
            const cp_layer_renderer_state state = cp_layer_renderer_get_state(_layer);
            if (state != lastState) {
                lastState = state;
                {
                    Shared& s = Sh();
                    std::lock_guard<std::mutex> lock(s.mutex);
                    s.status.layerState = (int32_t)state;
                }
                os_log(Log(), "layer state -> %d (1 paused, 2 running, 3 invalidated)", (int)state);
                fprintf(stderr, "[GXXR] layer state -> %d\n", (int)state);
                if (state == cp_layer_renderer_state_running) {
                    SetSessionState(XR_SESSION_RUNNING);
                    PlatformLifecycle_Notify(PLATFORM_LIFECYCLE_RESUME);
                    GXEngineHost_Pause(GX_PAUSE_LAYER, false);   // the engine thread resumes (audio, input) and produces frames
                } else if (state == cp_layer_renderer_state_paused) {
                    SetSessionState(XR_SESSION_PAUSED);
                    PlatformLifecycle_Notify(PLATFORM_LIFECYCLE_PAUSE);
                    GXEngineHost_Pause(GX_PAUSE_LAYER, true);    // the engine thread parks; we stop presenting engine frames
                } else if (state == cp_layer_renderer_state_invalidated) {
                    SetSessionState(XR_SESSION_INVALIDATED);
                    PlatformLifecycle_Notify(PLATFORM_LIFECYCLE_TERMINATE);
                    GXEngineHost_Pause(GX_PAUSE_LAYER, true);
                }
            }
            if (state == cp_layer_renderer_state_invalidated) break;
            if (state == cp_layer_renderer_state_paused) {
                // No GL and no Metal work while paused. Poll instead of cp_layer_renderer_wait_until_running so a
                // cancelled loop (immersive space closed and re-opened) can never hang a successor that is
                // waiting for the ANGLE context.
                [NSThread sleepForTimeInterval:0.02];
                continue;
            }

            cp_frame_t frame = cp_layer_renderer_query_next_frame(_layer);
            if (!frame) {
                [NSThread sleepForTimeInterval:0.001];
                continue;
            }

            cp_frame_start_update(frame);
            // (Simulation for this frame would run here. The test scene is time-driven inside its callback.)
            cp_frame_end_update(frame);

            cp_frame_timing_t timing = cp_frame_predict_timing(frame);
            if (!timing) continue;
            cp_time_wait_until(cp_frame_timing_get_optimal_input_time(timing));

            cp_frame_start_submission(frame);
            cp_drawable_array_t drawables = cp_frame_query_drawables(frame);
            const size_t drawableCount = drawables ? cp_drawable_array_get_count(drawables) : 0;
            if (drawableCount == 0) {
                // The frame is no longer valid (the layer was invalidated or paused between the state check and
                // here, which is exactly what closing the immersive space does). Compositor Services aborts the
                // process with "BUG IN CLIENT: cp_frame_end_submission() failed because the frame is not valid"
                // if a frame without drawables is ended, so drop it and let the state check at the top of the
                // loop see the invalidation.
                [NSThread sleepForTimeInterval:0.001];
                continue;
            }
            uint32_t firstViewCount = 0;
            bool anyTracked = false;

            for (size_t di = 0; di < drawableCount; ++di) {
                cp_drawable_t drawable = cp_drawable_array_get_drawable(drawables, di);
                if (!drawable) continue;
                const bool tracked = [self encodeDrawable:drawable frameIndex:frameCounter viewCountOut:di == 0 ? &firstViewCount : nullptr
                                                  skipped:&skipped presented:&presented];
                anyTracked = anyTracked || tracked;
            }
            cp_frame_end_submission(frame);

            frameCounter++;
            windowFrames++;
            const CFTimeInterval now = CACurrentMediaTime();
            {
                Shared& s = Sh();
                std::lock_guard<std::mutex> lock(s.mutex);
                s.status.framesRendered = presented;
                s.status.framesSkipped = skipped;
                s.status.drawableCount = (uint32_t)drawableCount;
                s.status.viewCount = firstViewCount;
                s.status.headTracked = anyTracked;
            }
            if (now - windowStart >= 1.0) {
                const double fps = (double)windowFrames / (now - windowStart);
                {
                    Shared& s = Sh();
                    std::lock_guard<std::mutex> lock(s.mutex);
                    s.status.fps = fps;
                }
                os_log(Log(), "frame loop: fps=%.1f frames=%llu drawables=%zu views=%u layout=%{public}s tracked=%d headY=%.2f fov(l,r,u,d)=(%.1f,%.1f,%.1f,%.1f) vp=%ux%u",
                       fps, (unsigned long long)presented, drawableCount, firstViewCount, LayoutName(_layout), (int)anyTracked,
                       _dbgHeadY, _dbgFovDeg[0], _dbgFovDeg[1], _dbgFovDeg[2], _dbgFovDeg[3], _dbgViewportW, _dbgViewportH);
                fprintf(stderr, "[GXXR] frame loop: fps=%.1f frames=%llu drawables=%zu views=%u layout=%s tracked=%d headY=%.2f fov(l,r,u,d)=(%.1f,%.1f,%.1f,%.1f) vp=%ux%u\n", fps,
                        (unsigned long long)presented, drawableCount, firstViewCount, LayoutName(_layout), (int)anyTracked,
                        _dbgHeadY, _dbgFovDeg[0], _dbgFovDeg[1], _dbgFovDeg[2], _dbgFovDeg[3], _dbgViewportW, _dbgViewportH);
                if (_accFrames > 0) {
                    const double n = (double)_accFrames;
                    const double span = std::max(1e-3, now - windowStart);
                    const bool engineMode = GXEngineHost_IsActive();
                    GXEngineHostStatus hs;
                    GXEngineHost_GetStatus(&hs);
                    {
                        Shared& s = Sh();
                        std::lock_guard<std::mutex> lock(s.mutex);
                        s.status.glSubmitMs = hs.lastFrameMs;
                        s.status.syncWaitMs = 0;
                        s.status.compositeMs = _accComposite / n;
                        s.status.frameMs = _accFrame / n;
                        s.status.releaseTimeouts = hs.framesSkipped;
                        s.status.engineMode = engineMode;
                        s.status.engineFps = hs.engineFps;
                        s.status.newFramesPerSec = (double)_winNewFrames / span;
                        s.status.repeatFramesPerSec = (double)_winRepeats / span;
                        s.status.frameAgeMs = _winMaxAgeMs;
                        s.status.angleActive = engineMode;
                        s.status.devicesMatch = _devicesMatch;
                        snprintf(s.status.renderer, sizeof(s.status.renderer), "%s", hs.renderer);
                        snprintf(s.status.syncMode, sizeof(s.status.syncMode), "%s", hs.syncMode);
                    }
                    if (engineMode) {
                        static const char* const kPhase[] = {"idle", "booting", "running", "paused", "failed", "stopping"};
                        fprintf(stderr,
                                "[GXXR] pipeline: compositor=%.1f fps | engine(%s)=%.1f fps (produced %llu, skipped %llu, last %.0f ms, longest %.0f ms) | "
                                "new frames/s=%.1f repeats/s=%.1f | frame age max=%.0f ms | composite=%.3f ms, total=%.3f ms | ring %u/%u in use, %s | %s%s\n",
                                fps, kPhase[std::min<int>(hs.phase, 5)], hs.engineFps, (unsigned long long)hs.framesProduced,
                                (unsigned long long)hs.framesSkipped, hs.lastFrameMs, hs.longestFrameMs, (double)_winNewFrames / span,
                                (double)_winRepeats / span, _winMaxAgeMs, _accComposite / n, _accFrame / n, hs.ringSlotsInUse, hs.ringSlots,
                                hs.syncMode[0] ? hs.syncMode : "-", hs.waitingForCompositor ? "engine waiting for compositor " : "",
                                _statusPanelVisible ? "loading indicator" : "engine frames");
                    } else {
                        fprintf(stderr, "[GXXR] timing (CPU ms/frame over %llu frames): composite=%.3f total=%.3f | direct Metal scene\n",
                                (unsigned long long)_accFrames, _accComposite / n, _accFrame / n);
                    }
                    _accComposite = _accFrame = 0;
                    _accFrames = 0;
                    _winNewFrames = _winRepeats = 0;
                    _winMaxAgeMs = 0;
                }
                windowStart = now;
                windowFrames = 0;
            }
        }
    }

    // The engine, its GL context and the ring survive this loop (the immersive space may re-open): only park it.
    if (GXEngineHost_IsActive()) GXEngineHost_Pause(GX_PAUSE_LAYER, true);

    if (_arRunning) {
        ar_session_stop(_arSession);
        _arRunning = false;
    }
    {
        Shared& s = Sh();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.status.attached = false;
        s.placementValid = false;
    }
    XRInteraction_ClearBoardTransform();
    SetSessionState(XR_SESSION_INVALIDATED);
    os_log(Log(), "compositor loop exited");
    fprintf(stderr, "[GXXR] compositor loop exited\n");
}

// Returns whether the head pose came from a tracked ARKit anchor.
- (bool)encodeDrawable:(cp_drawable_t)drawable
            frameIndex:(uint64_t)frameIndex
          viewCountOut:(uint32_t*)viewCountOut
               skipped:(uint64_t*)skipped
             presented:(uint64_t*)presented {
    Shared& sh = Sh();
    cp_frame_timing_t timing = cp_drawable_get_frame_timing(drawable);
    const double presentTime = cp_time_to_cf_time_interval(cp_frame_timing_get_presentation_time(timing));
    const double optimalInput = cp_time_to_cf_time_interval(cp_frame_timing_get_optimal_input_time(timing));
    const double deadline = cp_time_to_cf_time_interval(cp_frame_timing_get_rendering_deadline(timing));

    // ---- Head pose: ARKit device anchor predicted for the presentation time. ----
    simd_float4x4 worldFromDevice = Translation(0.0f, kFallbackHeadHeight, 0.0f);
    bool tracked = false;
    ar_device_anchor_t anchor = nil;
    if (_worldProvider) {
        ar_device_anchor_t query = ar_device_anchor_create();
        const ar_device_anchor_query_status_t qs = ar_world_tracking_provider_query_device_anchor_at_timestamp(_worldProvider, presentTime, query);
        if (qs == ar_device_anchor_query_status_success && ar_device_anchor_is_tracked(query)) {
            anchor = query;
            worldFromDevice = ar_device_anchor_get_origin_from_anchor_transform(query);
            tracked = true;
        }
    }
    // Engine mode sets the drawable's anchor itself (the anchor of the frame it presents); every other path uses this one.
    const bool engineMode = GXEngineHost_IsActive();
    if (!engineMode && anchor) cp_drawable_set_device_anchor(drawable, anchor);

    // ---- Tabletop placement (first tracked frame, or fallback after a timeout, or on recenter). ----
    const bool recenter = sh.recenterRequested.exchange(false);
    bool needPlace;
    {
        std::lock_guard<std::mutex> lock(sh.mutex);
        needPlace = recenter || !sh.placementValid;
    }
    if (needPlace && (tracked || recenter || frameIndex >= kFallbackPlacementFrames)) {
        // Forward direction on the horizontal plane from the device's -Z axis.
        simd_float3 fwd = simd_make_float3(-worldFromDevice.columns[2].x, 0.0f, -worldFromDevice.columns[2].z);
        if (simd_length(fwd) < 1e-3f) fwd = simd_make_float3(0.0f, 0.0f, -1.0f);
        fwd = simd_normalize(fwd);
        const simd_float3 up = simd_make_float3(0, 1, 0);
        const simd_float3 right = simd_normalize(simd_cross(fwd, up));
        simd_float4x4 wfb;
        wfb.columns[0] = simd_make_float4(right, 0.0f);
        wfb.columns[1] = simd_make_float4(up, 0.0f);
        wfb.columns[2] = simd_make_float4(-fwd, 0.0f);  // board +Z points toward the player
        const simd_float3 head = simd_make_float3(worldFromDevice.columns[3].x, 0.0f, worldFromDevice.columns[3].z);
        // Device (world origin on the floor): head is ~1.0-2.3 m up, so the surface sits 0.8 m above the origin.
        // Simulator (origin at head height, narrow 90 x 59 degree view): head Y is ~0, so put the board
        // 0.45 m below the eyes and 1.25 m ahead so all of it fits in the vertical field of view.
        const float headY = worldFromDevice.columns[3].y;
        const bool floorOrigin = headY > 1.0f && headY < 2.3f;
        const float surfaceY = floorOrigin ? kBoardHeight : headY - 0.45f;
        const simd_float3 center = head + fwd * (floorOrigin ? kBoardDistance : 1.25f);
        wfb.columns[3] = simd_make_float4(center.x, surfaceY, center.z, 1.0f);
        {
            std::lock_guard<std::mutex> lock(sh.mutex);
            Store(sh.worldFromBoard, wfb);
            sh.placementValid = true;
        }
        XRInteraction_SetBoardTransform(sh.worldFromBoard, kBoardHalfX, kBoardHalfZ);
        char msg[160];
        snprintf(msg, sizeof(msg), "tabletop at (%.2f, %.2f, %.2f); head y=%.2f (%s origin), %s", center.x, surfaceY, center.z, headY,
                 floorOrigin ? "floor" : "head-relative", tracked ? "tracked" : "fallback");
        SetMessage(msg);
        os_log(Log(), "%{public}s", msg);
        fprintf(stderr, "[GXXR] %s\n", msg);
    }

    // ---- Build the frame description from the drawable's views. ----
    XRFrameInfo info = {};
    info.frame_index = frameIndex;
    info.predicted_display_time_s = presentTime;
    info.optimal_input_time_s = optimalInput;
    info.rendering_deadline_s = deadline;
    info.head_tracked = tracked;
    info.head_pose = PoseFromMatrix(worldFromDevice);
    Store(info.world_from_head, worldFromDevice);
    const simd_float2 depthRange = cp_drawable_get_depth_range(drawable);  // x = far, y = near (reverse-Z)
    info.depth_far_m = depthRange.x;
    info.depth_near_m = depthRange.y;
    info.reverse_z = true;
    info.foveation_enabled = _foveation;
    info.alpha_mode = (XRAlphaMode)sh.alphaMode.load();

    size_t viewCount = cp_drawable_get_view_count(drawable);
    if (viewCount > XR_MAX_EYES) viewCount = XR_MAX_EYES;
    if (viewCountOut) *viewCountOut = (uint32_t)viewCount;
    info.eye_count = (uint32_t)viewCount;

    std::set<uint32_t> touched;  // (texture_index << 16 | slice)
    id<MTLTexture> firstColor = nil;
    id<MTLTexture> firstDepth = nil;
    NSUInteger firstSlice = 0;
    for (size_t vi = 0; vi < viewCount; ++vi) {
        cp_view_t view = cp_drawable_get_view(drawable, vi);
        cp_view_texture_map_t map = cp_view_get_view_texture_map(view);
        const size_t texIndex = cp_view_texture_map_get_texture_index(map);
        const size_t slice = cp_view_texture_map_get_slice_index(map);
        const MTLViewport vp = cp_view_texture_map_get_viewport(map);
        id<MTLTexture> color = cp_drawable_get_color_texture(drawable, texIndex);
        id<MTLTexture> depth = cp_drawable_get_depth_texture(drawable, texIndex);

        const simd_float4x4 worldFromView = simd_mul(worldFromDevice, cp_view_get_transform(view));
        const simd_float4x4 viewFromWorld = simd_inverse(worldFromView);
        const simd_float4x4 proj = cp_drawable_compute_projection(drawable, cp_axis_direction_convention_right_up_back, vi);
        const simd_float4x4 clipFromWorld = simd_mul(proj, viewFromWorld);

        XREyeView& e = info.eyes[vi];
        e.eye_index = (uint32_t)vi;
        e.pose = PoseFromMatrix(worldFromView);
        e.fov = FovFromProjection(proj);
        Store(e.world_from_view, worldFromView);
        Store(e.view_from_world, viewFromWorld);
        Store(e.clip_from_view, proj);
        Store(e.clip_from_world, clipFromWorld);
        e.viewport = XRRect{(int32_t)vp.originX, (int32_t)vp.originY, (int32_t)vp.width, (int32_t)vp.height};
        e.texture_index = (uint32_t)texIndex;
        e.array_slice = (uint32_t)slice;
        e.target_width = (uint32_t)color.width;
        e.target_height = (uint32_t)color.height;
        e.first_use_of_target = touched.insert((uint32_t)(texIndex << 16 | slice)).second;
        e.color_target = (__bridge void*)color;
        e.depth_target = (__bridge void*)depth;
        if (vi == 0) {
            _dbgHeadY = worldFromDevice.columns[3].y;
            _dbgFovDeg[0] = e.fov.angle_left * 57.29578f;
            _dbgFovDeg[1] = e.fov.angle_right * 57.29578f;
            _dbgFovDeg[2] = e.fov.angle_up * 57.29578f;
            _dbgFovDeg[3] = e.fov.angle_down * 57.29578f;
            _dbgViewportW = (uint32_t)vp.width;
            _dbgViewportH = (uint32_t)vp.height;
            firstColor = color;
            firstDepth = depth;
            firstSlice = slice;
        }
    }

    const CFTimeInterval tFrame0 = CACurrentMediaTime();
    id<MTLCommandBuffer> cb = [_queue commandBuffer];
    cb.label = @"GXXR frame";
    info.command_buffer = (__bridge void*)cb;

    if (engineMode) {
        [self encodeEngineFrame:drawable info:info anchor:anchor commandBuffer:cb viewCount:viewCount firstColor:firstColor firstDepth:firstDepth
                     firstSlice:firstSlice presented:presented];
        [cb commit];
        _accFrame += (CACurrentMediaTime() - tFrame0) * 1000.0;
        _accFrames++;
        return tracked;
    }
    _statusPanelVisible = false;

    // ---- Client callback (registered XRFrameCallback, or the built-in Metal test scene). ----
    XRFrameCallback userCb;
    void* userData;
    {
        std::lock_guard<std::mutex> lock(sh.mutex);
        userCb = sh.frameCallback;
        userData = sh.frameUser;
    }
    memset(sh.submitted, 0, sizeof(sh.submitted));
    sh.inFrame = true;
    XRFrameResult result = XR_FRAME_SKIP;
    if (viewCount > 0) {
        if (userCb) result = userCb(userData, &info);
        else result = [_testScene renderFrame:&info];
    }
    sh.inFrame = false;

    const CFTimeInterval tComp0 = CACurrentMediaTime();
    if (result == XR_FRAME_SUBMITTED_TEXTURES) {
        // Constant reverse-Z depth for a submitted image: the compositor needs depth for reprojection and
        // system-window occlusion and the client has no depth to share, so use the tabletop distance.
        float wfb[16];
        float hx = 0, hz = 0;
        const bool hasBoard = XRPresentation_GetTabletopPlacement(wfb, &hx, &hz);

        std::set<uint32_t> composed;
        for (size_t vi = 0; vi < viewCount; ++vi) {
            if (!sh.submitted[vi]) continue;
            const XREyeView& e = info.eyes[vi];
            const XREyeSubmit& sub = sh.submits[vi];
            id<MTLTexture> src = gSubmittedTexture[vi];
            uint32_t flags = 0;
            if (sub.flags & XR_SUBMIT_FLIP_Y) flags |= GXXR_COMPOSITE_FLIP_Y;
            if (!(sub.flags & XR_SUBMIT_PREMULTIPLIED_ALPHA)) flags |= GXXR_COMPOSITE_PREMULTIPLY;
            if (![GXXRMetalRenderer isSRGBFormat:src.pixelFormat]) flags |= GXXR_COMPOSITE_SRGB_DECODE;  // gamma values in a UNORM target
            const float depthNdc = hasBoard ? [self constantDepthForEye:e focus:simd_make_float3(wfb[12], wfb[13], wfb[14])] : 0.0f;
            const bool first = composed.insert((uint32_t)(e.texture_index << 16 | e.array_slice)).second;
            MTLViewport vp = {(double)e.viewport.x, (double)e.viewport.y, (double)e.viewport.width, (double)e.viewport.height, 0.0, 1.0};
            [_renderer encodeEyeCompositeInto:cb
                                       source:src
                                        flags:flags
                                       uvRect:simd_make_float4(0, 0, 1, 1)
                                constantDepth:depthNdc
                                        color:(__bridge id<MTLTexture>)e.color_target
                                   colorSlice:e.array_slice
                                        depth:(__bridge id<MTLTexture>)e.depth_target
                                     viewport:vp
                                        clear:first];
        }
    } else if (result == XR_FRAME_SKIP && firstColor) {
        [_renderer encodeClearInto:cb color:firstColor colorSlice:firstSlice depth:firstDepth opaqueBlack:NO];
        (*skipped)++;
    }
    const CFTimeInterval tComp1 = CACurrentMediaTime();

    cp_drawable_encode_present(drawable, cb);
    [cb commit];
    if (result != XR_FRAME_SKIP) (*presented)++;
    _accComposite += (tComp1 - tComp0) * 1000.0;
    _accFrame += (CACurrentMediaTime() - tFrame0) * 1000.0;
    _accFrames++;
    return tracked;
}

/// Constant reverse-Z depth (NDC) of a plane through `focus`, projected with the eye's own projection: what the
/// compositor writes for every opaque pixel of a submitted eye image so system reprojection sees a plane there.
- (float)constantDepthForEye:(const XREyeView&)e focus:(simd_float3)focus {
    const simd_float3 eyePos = simd_make_float3(e.pose.position.x, e.pose.position.y, e.pose.position.z);
    const float dist = simd_length(focus - eyePos);
    simd_float4x4 proj;
    memcpy(&proj, e.clip_from_view, sizeof(proj));
    const simd_float4 clip = simd_mul(proj, simd_make_float4(0, 0, -dist, 1));
    return clip.w > 1e-6f ? std::min(1.0f, std::max(0.0f, clip.z / clip.w)) : 0.0f;
}

/// Engine mode: publish this display frame's head/eye snapshot to the engine thread, then composite the LATEST COMPLETED
/// engine frame (or the loading indicator when there is none). Never waits for the engine, never touches GL.
- (void)encodeEngineFrame:(cp_drawable_t)drawable
                     info:(XRFrameInfo&)info
                   anchor:(ar_device_anchor_t)anchor
            commandBuffer:(id<MTLCommandBuffer>)cb
                viewCount:(size_t)viewCount
               firstColor:(id<MTLTexture>)firstColor
               firstDepth:(id<MTLTexture>)firstDepth
               firstSlice:(NSUInteger)firstSlice
                presented:(uint64_t*)presented {
    GXXRFrameMailbox* mailbox = _session.mailbox;
    if (!_deviceChecked && _session.context) {
        _devicesMatch = [_session.context checkDeviceMatchesCompositorDevice:_device];
        _deviceChecked = true;
    }
    [mailbox publishHeadInfo:&info anchor:anchor];

    GXXRTargetRing* ring = mailbox.ring;
    GXXRPublishedFrame* pf = ring ? [mailbox acquireLatestFrame] : nil;  // takes a ring-slot reference for this composite
    const CFTimeInterval tComp0 = CACurrentMediaTime();
    _statusPanelVisible = (pf == nil);

    if (pf) {
        // The system reprojects from the pose the picture was RENDERED for, so the drawable carries that frame's anchor.
        if (pf.anchor) cp_drawable_set_device_anchor(drawable, pf.anchor);
        else if (anchor) cp_drawable_set_device_anchor(drawable, anchor);
        const XRFrameInfo& pinfo = pf->info;
        const GXHostFrameOutput& out = pf->output;
        if (pf.seq != _lastCompositedSeq) {
            _lastCompositedSeq = pf.seq;
            _winNewFrames++;
        } else {
            _winRepeats++;
        }
        _winMaxAgeMs = std::max(_winMaxAgeMs, (CACurrentMediaTime() - pf.publishTime) * 1000.0);

        [ring encodeWaitForGLSlot:pf.slot into:cb];  // GPU-GPU: composite waits for ANGLE's work on this slot

        if (GXXRDebugDumpInterval() > 0 && CACurrentMediaTime() - _lastDumpTime >= GXXRDebugDumpInterval()) {
            _lastDumpTime = CACurrentMediaTime();
            NSString* dir = GXXRDebugDumpDirectory();
            const int n = _dumpIndex++;
            NSMutableString* desc = [NSMutableString stringWithFormat:@"frame seq=%llu slot=%u stereoValid=%d atlas=%d layers=%u groundView=%d eyes=%u\n",
                                     (unsigned long long)pf.seq, (unsigned)pf.slot, (int)out.stereoValid, (int)out.atlas, (unsigned)out.layerCount,
                                     (int)out.groundView, (unsigned)pinfo.eye_count];
            for (uint32_t i = 0; i < out.layerCount && i < GX_HOST_MAX_LAYERS; ++i) {
                const GXHostLayer& l = out.layers[i];
                [desc appendFormat:@"layer %u '%s' target=%d pos=(%.3f %.3f %.3f) size=(%.3f %.3f) flags=0x%x\n", i, l.name, l.target,
                                   l.position[0], l.position[1], l.position[2], l.size[0], l.size[1], (unsigned)l.flags];
                GXXRDebugDumpTexture(cb, [ring textureForTarget:l.target slot:pf.slot],
                                     [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%03d-layer%u-%s.png", n, i, l.name]]);
            }
            static const char* const kTargetNames[D3D8GLES_XRT_COUNT] = {"eyeL", "eyeR", "game", "world", "ui"};
            for (int t = 0; t < D3D8GLES_XRT_COUNT; ++t) {
                GXXRDebugDumpTexture(cb, [ring textureForTarget:t slot:pf.slot],
                                     [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%03d-target-%s.png", n, kTargetNames[t]]]);
            }
            for (uint32_t e = 0; e < pinfo.eye_count; ++e) {
                const float* m = pinfo.eyes[e].clip_from_world;
                [desc appendFormat:@"eye %u clip_from_world cols: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]\n", e,
                                   m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]];
            }
            [desc writeToFile:[dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%03d-frame.txt", n]] atomically:YES
                     encoding:NSUTF8StringEncoding error:nil];
            fprintf(stderr, "[GXXR/dump] %s", desc.UTF8String);
        }

        // World-anchored layers (UI / game / world textures of this slot).
        NSMutableArray<GXXRCompositeLayer*>* layers = [NSMutableArray arrayWithCapacity:out.layerCount];
        for (uint32_t i = 0; i < out.layerCount && i < GX_HOST_MAX_LAYERS; ++i) {
            const GXHostLayer& l = out.layers[i];
            id<MTLTexture> tex = [ring textureForTarget:l.target slot:pf.slot];
            if (!tex) continue;
            GXXRCompositeLayer* layer = [GXXRCompositeLayer layerWithName:[NSString stringWithUTF8String:l.name]
                                                                  texture:tex
                                                                 position:simd_make_float3(l.position[0], l.position[1], l.position[2])
                                                              orientation:simd_quaternion(l.orientation[0], l.orientation[1], l.orientation[2], l.orientation[3])
                                                               sizeMeters:simd_make_float2(l.size[0], l.size[1])
                                                                    flipY:(l.flags & GX_LAYER_FLIP_Y) != 0];
            layer.premultipliedAlpha = (l.flags & GX_LAYER_PREMULTIPLIED) != 0;
            // The engine UI is split into pieces cropped from one canvas (control bar band, HUD above it; GL bottom-up
            // UVs). Without the crop every piece showed the whole canvas: the control bar appeared twice beside the board.
            if (l.flags & GX_LAYER_HAS_UVRECT) layer.uvRect = simd_make_float4(l.uvRect[0], l.uvRect[1], l.uvRect[2], l.uvRect[3]);
            [layers addObject:layer];
        }

        // World pose of every published layer, indexed exactly like out.layers (GXHostFeedback.pointerLayer indexes into
        // out.layers, not the filtered `layers` array above, which can skip an entry whose texture was not found this frame).
        std::vector<simd_float4x4> layerPoses(out.layerCount);
        for (uint32_t i = 0; i < out.layerCount && i < GX_HOST_MAX_LAYERS; ++i) {
            const GXHostLayer& l = out.layers[i];
            simd_float4x4 m = simd_matrix4x4(simd_quaternion(l.orientation[0], l.orientation[1], l.orientation[2], l.orientation[3]));
            m.columns[3] = simd_make_float4(l.position[0], l.position[1], l.position[2], 1.0f);
            layerPoses[i] = m;
        }

        float wfb[16];
        float hx = 0, hz = 0;
        const bool hasBoard = XRPresentation_GetTabletopPlacement(wfb, &hx, &hz);
        const simd_float3 focus = out.hasFocus ? simd_make_float3(out.focus[0], out.focus[1], out.focus[2])
                                               : (hasBoard ? simd_make_float3(wfb[12], wfb[13], wfb[14]) : simd_make_float3(0, 0, 0));
        const bool hasFocus = out.hasFocus || hasBoard;

        std::set<uint32_t> composed;
        bool drewAnything = false;
        for (size_t vi = 0; vi < viewCount; ++vi) {
            const XREyeView& e = info.eyes[vi];                                           // this drawable's view: target, viewport
            const uint32_t pi = std::min<uint32_t>((uint32_t)vi, pinfo.eye_count > 0 ? pinfo.eye_count - 1 : 0);
            const XREyeView& pe = pinfo.eyes[pi];                                         // the eye the frame was rendered for
            const bool first = composed.insert((uint32_t)(e.texture_index << 16 | e.array_slice)).second;
            const MTLViewport vp = {(double)e.viewport.x, (double)e.viewport.y, (double)e.viewport.width, (double)e.viewport.height, 0.0, 1.0};
            id<MTLTexture> colorTex = (__bridge id<MTLTexture>)e.color_target;
            id<MTLTexture> depthTex = (__bridge id<MTLTexture>)e.depth_target;
            bool cleared = false;
            if (out.stereoValid) {
                const int target = out.atlas ? D3D8GLES_XRT_STEREO_LEFT : (pi == 0 ? D3D8GLES_XRT_STEREO_LEFT : D3D8GLES_XRT_STEREO_RIGHT);
                id<MTLTexture> src = [ring textureForTarget:target slot:pf.slot];
                if (src) {
                    uint32_t flags = GXXR_COMPOSITE_FLIP_Y;  // GL targets are bottom-up, coverage-premultiplied
                    if (![GXXRMetalRenderer isSRGBFormat:src.pixelFormat]) flags |= GXXR_COMPOSITE_SRGB_DECODE;
                    const simd_float4 uvRect = out.atlas ? simd_make_float4(0.5f * (float)pi, 0, 0.5f, 1) : simd_make_float4(0, 0, 1, 1);
                    [_renderer encodeEyeCompositeInto:cb source:src flags:flags uvRect:uvRect
                                        constantDepth:hasFocus ? [self constantDepthForEye:pe focus:focus] : 0.0f
                                                color:colorTex colorSlice:e.array_slice depth:depthTex viewport:vp clear:first];
                    cleared = true;
                    drewAnything = true;
                }
            }
            if (!cleared && first && colorTex) {
                // Ground View (package C2): no passthrough behind the observer (docs/visionos-presentation.md
                // section 7) -- opaque black, not the usual alpha-0 passthrough clear.
                [_renderer encodeClearInto:cb color:colorTex colorSlice:e.array_slice depth:depthTex opaqueBlack:out.groundView];
            }
            if (layers.count > 0) {
                // Layers are placed in world space and drawn with the eye the frame was RENDERED with (the drawable's
                // anchor is that frame's anchor, so the system's reprojection carries them to the present head pose).
                simd_float4x4 clipFromWorld;
                memcpy(&clipFromWorld, pe.clip_from_world, sizeof(clipFromWorld));
                [_renderer encodeLayers:layers into:cb color:colorTex colorSlice:e.array_slice depth:depthTex viewport:vp clipFromWorld:clipFromWorld];
                drewAnything = true;
            }
            if (_feedbackRenderer && out.feedback.flags != 0) {
                // Same rendered-eye clip as the layers above: box rect, grab bar, cursor and the other world markers must
                // land on the board exactly where the layers do, and ride the same reprojection.
                simd_float4x4 clipFromWorld;
                memcpy(&clipFromWorld, pe.clip_from_world, sizeof(clipFromWorld));
                [_feedbackRenderer encodeMarkersInto:cb color:colorTex colorSlice:e.array_slice depth:depthTex viewport:vp
                                        clipFromWorld:clipFromWorld feedback:&out.feedback
                                   layerWorldFromQuad:layerPoses.data() layerCount:layerPoses.size()];
                drewAnything = true;
            }
            if (_feedbackRenderer && out.feedback.fadeAlpha > 0.001f) {
                // Head/screen locked: drawn with THIS drawable's own viewport, not the rendered-eye clip.
                [_feedbackRenderer encodeFadeInto:cb color:colorTex colorSlice:e.array_slice viewport:vp alpha:out.feedback.fadeAlpha];
                drewAnything = true;
            }
        }
        if (drewAnything) (*presented)++;
        [ring releaseSlot:pf.slot afterCommandBuffer:cb];  // the slot is reusable after the LAST composite that read it completes
    } else {
        // Nothing published yet (boot, boot failure): a simple Metal loading indicator, world anchored where the player looks.
        if (anchor) cp_drawable_set_device_anchor(drawable, anchor);
        CFTimeInterval now = CACurrentMediaTime();
        if (now - _hostStatusAt > 0.25) {
            GXEngineHost_GetStatus(&_hostStatus);
            _hostStatusAt = now;
        }
        NSString* title;
        NSString* detail;
        BOOL spinner = YES;
        const GXEngineHostStatus& hs = _hostStatus;
        if (hs.phase == GX_ENGINE_FAILED) {
            title = @"Engine stopped";
            detail = [NSString stringWithFormat:@"%s\nLog: %s", hs.lastError, hs.logPath];
            spinner = NO;
        } else if (hs.phase == GX_ENGINE_BOOTING) {
            title = hs.fake ? @"Starting fake engine" : @"Starting the engine";
            detail = [NSString stringWithFormat:@"%s (%.0f s)\n%s", hs.progress, hs.bootSeconds, hs.lastLogLine];
        } else {
            title = @"Waiting for the first frame";
            detail = hs.waitingForCompositor ? @"Waiting for a head pose" : @(hs.progress);
        }
        if (!_indicatorPlaced) {
            const simd_float3 head = simd_make_float3(info.head_pose.position.x, info.head_pose.position.y, info.head_pose.position.z);
            simd_float3 fwd = simd_make_float3(-info.world_from_head[8], 0.0f, -info.world_from_head[10]);
            if (simd_length(fwd) < 1e-3f) fwd = simd_make_float3(0.0f, 0.0f, -1.0f);
            // Below the launcher window that normally hangs at the same spot, so it is never hidden behind it.
            _indicatorCenter = head + simd_normalize(fwd) * 0.9f - simd_make_float3(0, 0.32f, 0);
            _indicatorPlaced = true;
        }
        const simd_float3 viewer = simd_make_float3(info.head_pose.position.x, info.head_pose.position.y, info.head_pose.position.z);
        NSArray<GXXRCompositeLayer*>* layers = [_statusPanel layersWithTitle:title detail:detail spinner:spinner time:now center:_indicatorCenter viewer:viewer];
        std::set<uint32_t> composed;
        for (size_t vi = 0; vi < viewCount; ++vi) {
            const XREyeView& e = info.eyes[vi];
            const bool first = composed.insert((uint32_t)(e.texture_index << 16 | e.array_slice)).second;
            const MTLViewport vp = {(double)e.viewport.x, (double)e.viewport.y, (double)e.viewport.width, (double)e.viewport.height, 0.0, 1.0};
            id<MTLTexture> colorTex = (__bridge id<MTLTexture>)e.color_target;
            id<MTLTexture> depthTex = (__bridge id<MTLTexture>)e.depth_target;
            if (first && colorTex) [_renderer encodeClearInto:cb color:colorTex colorSlice:e.array_slice depth:depthTex opaqueBlack:NO];
            if (layers.count > 0) {
                simd_float4x4 clipFromWorld;
                memcpy(&clipFromWorld, e.clip_from_world, sizeof(clipFromWorld));
                [_renderer encodeLayers:layers into:cb color:colorTex colorSlice:e.array_slice depth:depthTex viewport:vp clipFromWorld:clipFromWorld];
            }
        }
        (*presented)++;
    }
    (void)firstColor; (void)firstDepth; (void)firstSlice;
    cp_drawable_encode_present(drawable, cb);
    _accComposite += (CACurrentMediaTime() - tComp0) * 1000.0;
}

@end

// ---------------------------------------------------------------------------
// Attach
// ---------------------------------------------------------------------------

static GXXRCompositorLoop* gLoop = nil;

extern "C" void GXXRBridgeAttachLayerRenderer(cp_layer_renderer_t layerRenderer) {
    if (!layerRenderer) return;
    @synchronized(GXXRCompositorLoop.class) {
        if (gLoop) [gLoop cancel];
        gLoop = [[GXXRCompositorLoop alloc] initWithLayerRenderer:layerRenderer];
        [gLoop start];
    }
    os_log(Log(), "LayerRenderer attached; render thread started");
    fprintf(stderr, "[GXXR] LayerRenderer attached; render thread started\n");
}
