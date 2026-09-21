// GXXREngineSession.mm - see header. Objective-C++ with ARC.
#import "GXXREngineSession.h"

#import <Metal/Metal.h>
#import <os/log.h>

#include <cstdio>
#include <cstring>

#include "GXEngineHostServices.h"
#include "GXXRD3D8GLES.h"

#import "GXXRAngleOptions.h"
#import "GXXRFakeEngine.h"

#define GXXR_LOG(fmt, ...) fprintf(stderr, "[GXXR/session] " fmt "\n", ##__VA_ARGS__)

@implementation GXXREngineSession {
    GXXRANGLEContext* _ctx;
    GXXRTargetRing* _ring;
    GXXRFrameMailbox* _mailbox;
    D3D8GLES_XRTargets _targets;   // storage behind GXHostFrame.targets (engine thread only)
    uint64_t _engineFrames;
}

+ (GXXREngineSession*)shared {
    static GXXREngineSession* s = [[GXXREngineSession alloc] init];
    return s;
}

- (instancetype)init {
    self = [super init];
    if (self) _mailbox = [[GXXRFrameMailbox alloc] init];
    return self;
}

- (GXXRFrameMailbox*)mailbox { return _mailbox; }
- (nullable GXXRTargetRing*)ring { return _ring; }
- (nullable GXXRANGLEContext*)context { return _ctx; }

#pragma mark - services (engine thread)

- (bool)attachForceAtlas:(bool)forceAtlas info:(GXHostGLInfo*)out {
    const GXXRAngleOptions& opt = GXXRAngleLaunchOptions();
    _ctx = [GXXRANGLEContext sharedContext];
    if (!_ctx) {
        GXXR_LOG("ANGLE context creation failed (see [GXXR/ANGLE] lines)");
        return false;
    }
    if (![_ctx makeCurrent]) return false;
    if (!_ring) {
        _ring = [[GXXRTargetRing alloc] initWithContext:_ctx pixelFormat:opt.format slotCount:4];
        if (!_ring) {
            GXXR_LOG("target ring creation failed");
            [_ctx releaseCurrent];
            return false;
        }
        _ring.forceGLFinish = opt.forceGLFinish;
        _ring.atlas = opt.atlas || forceAtlas;
        _mailbox.ring = _ring;
    }
    memset(out, 0, sizeof(*out));
    out->eglDisplay = _ctx.eglDisplay;
    out->eglContext = _ctx.eglContext;
    out->getProcAddress = (void* (*)(const char*))_ctx.getProcAddress;
    out->flags = D3D8GLES_XRFLAG_NO_MULTIVIEW | (_ring.atlas ? D3D8GLES_XRFLAG_FORCE_ATLAS : 0u);
    out->atlas = _ring.atlas;
    GXXR_LOG("attached on the engine thread: renderer=%s atlas=%d sync=%s", _ctx.rendererString.UTF8String, (int)_ring.atlas,
             _ring.usesSharedEventSync ? "metal-shared-event" : "glFinish");
    return true;
}

- (void)detach {
    if (!_ctx) return;
    [_ctx makeCurrent];
    [_mailbox dropLatestFrame];
    [_ring teardown];
    _ring = nil;
    _mailbox.ring = nil;
    [_ctx releaseCurrent];
}

- (bool)acquireHead:(GXHostFrame*)frame {
    GXXRHeadSnapshot* head = [_mailbox latestHead];
    if (!head) return false;
    frame->info = head->info;
    frame->headSeq = head.seq;
    frame->token = (void*)CFBridgingRetain(head);
    return true;
}

- (bool)beginFrame:(GXHostFrame*)frame request:(const GXHostFrameRequest*)req {
    if (!_ring) return false;
    // Stereo pair (or atlas) and the engine's 2D targets. A size of 0 disables a target.
    if (req->targetMask & GX_TARGET_STEREO) {
        [_ring configureStereoEyeWidth:req->eyeWidth height:req->eyeHeight eyeCount:req->eyeCount];
    } else {
        [_ring configureStereoEyeWidth:0 height:0 eyeCount:0];
    }
    [_ring setSizeWidth:(req->targetMask & GX_TARGET_GAME) ? req->gameWidth : 0 height:(req->targetMask & GX_TARGET_GAME) ? req->gameHeight : 0 forTarget:D3D8GLES_XRT_GAME];
    [_ring setSizeWidth:(req->targetMask & GX_TARGET_WORLD) ? req->gameWidth : 0 height:(req->targetMask & GX_TARGET_WORLD) ? req->gameHeight : 0 forTarget:D3D8GLES_XRT_WORLD];
    [_ring setSizeWidth:(req->targetMask & GX_TARGET_UI) ? req->gameWidth : 0 height:(req->targetMask & GX_TARGET_UI) ? req->gameHeight : 0 forTarget:D3D8GLES_XRT_UI];
    // A resize drains every composite; the published frame holds a slot reference, so drop it first.
    if ([_ring needsResize]) [_mailbox dropLatestFrame];
    const NSInteger slot = [_ring beginFrame];
    if (slot < 0) return false;
    [_ring fillTargets:&_targets];
    frame->targets = reinterpret_cast<struct D3D8GLES_XRTargets*>(&_targets);
    frame->slot = (int32_t)slot;
    return true;
}

- (void)endFrame:(GXHostFrame*)frame output:(const GXHostFrameOutput*)out {
    [_ring endGLWork];
    GXXRHeadSnapshot* head = frame->token ? (__bridge GXXRHeadSnapshot*)frame->token : nil;
    GXXRPublishedFrame* pf = [GXXRPublishedFrame new];
    pf.slot = (NSUInteger)frame->slot;
    pf.headSeq = frame->headSeq;
    pf.anchor = head.anchor;
    pf->info = frame->info;
    pf->output = *out;
    pf->output.atlas = _ring.atlas;
    [_mailbox publishFrame:pf];  // the writer reference moves to the mailbox
    if (frame->token) CFBridgingRelease(frame->token);
    frame->token = NULL;
    frame->targets = NULL;
}

- (void)abortFrame:(GXHostFrame*)frame {
    [_ring abortFrame];
    if (frame->token) CFBridgingRelease(frame->token);
    frame->token = NULL;
    frame->targets = NULL;
}

- (void)describeRing:(GXHostRingInfo*)out {
    memset(out, 0, sizeof(*out));
    if (!_ring) return;
    out->slots = (uint32_t)_ring.slotCount;
    out->slotsInUse = (uint32_t)_ring.slotsInUse;
    out->megabytes = (double)_ring.allocatedBytes / (1024.0 * 1024.0);
    snprintf(out->syncMode, sizeof(out->syncMode), "%s", _ring.usesSharedEventSync ? "metal-shared-event" : "glFinish");
    snprintf(out->renderer, sizeof(out->renderer), "%s", _ctx.rendererString.UTF8String ?: "");
}

@end

// ---------------------------------------------------------------------------
// C services table
// ---------------------------------------------------------------------------

namespace {
GXXREngineSession* S(void* user) { return (__bridge GXXREngineSession*)user; }
}

static const GXEngineHostServices* SessionServices() {
    static GXEngineHostServices services = {};
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        services.user = (__bridge void*)[GXXREngineSession shared];
        services.attach = [](void* u, bool forceAtlas, GXHostGLInfo* out) { return [S(u) attachForceAtlas:forceAtlas info:out]; };
        services.detach = [](void* u) { [S(u) detach]; };
        services.headAge = [](void* u) { return [S(u).mailbox headAge]; };
        services.acquireHead = [](void* u, GXHostFrame* f) { return [S(u) acquireHead:f]; };
        services.beginFrame = [](void* u, GXHostFrame* f, const GXHostFrameRequest* r) { return [S(u) beginFrame:f request:r]; };
        services.endFrame = [](void* u, GXHostFrame* f, const GXHostFrameOutput* o) { [S(u) endFrame:f output:o]; };
        services.abortFrame = [](void* u, GXHostFrame* f) { [S(u) abortFrame:f]; };
        services.describeRing = [](void* u, GXHostRingInfo* o) { [S(u) describeRing:o]; };
    });
    return &services;
}

// Installed before main() so GXEngineHost_Start works without any set-up call from Swift.
__attribute__((constructor)) static void GXXRInstallEngineServices(void) {
    @autoreleasepool {
        GXEngineHost_SetServices(SessionServices());
    }
}

extern "C" bool GXXRBridgeStartFakeEngine(void) {
    GXEngineHost_SetServices(SessionServices());
    return GXXRFakeEngineStart();
}
