// GXXRFakeEngine.mm - see header. Objective-C++ with ARC.
#import "GXXRFakeEngine.h"

#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "GXEngineHostServices.h"
#include "GXXRD3D8GLES.h"

#import "GXXRANGLEContext.h"
#import "GXXRAngleOptions.h"
#import "GXXRGLTestScene.h"

#define GXXR_LOG(fmt, ...) fprintf(stderr, "[fake-engine] " fmt "\n", ##__VA_ARGS__)

namespace {

GXXRGLTestScene* gScene = nil;
CFTimeInterval gLastStall = 0;
uint64_t gStalls = 0;

bool Boot(void*, const GXHostGLInfo*, void (*progress)(const char*), char* error, size_t errorCapacity) {
    const GXXRAngleOptions& opt = GXXRAngleLaunchOptions();
    const CFTimeInterval t0 = CACurrentMediaTime();
    GXXR_LOG("booting (%.1f s simulated)", opt.fakeBootSeconds);
    while (true) {
        const double left = opt.fakeBootSeconds - (CACurrentMediaTime() - t0);
        if (left <= 0) break;
        char text[128];
        snprintf(text, sizeof(text), "Fake engine booting... %.0f s left", std::ceil(left));
        progress(text);
        [NSThread sleepForTimeInterval:std::min(0.25, left)];
    }
    GXXRANGLEContext* ctx = [GXXRANGLEContext sharedContext];
    gScene = ctx ? [[GXXRGLTestScene alloc] initWithContext:ctx] : nil;
    if (!gScene) {
        snprintf(error, errorCapacity, "the GLES3 test scene failed to build (see [GXXR/scene] lines)");
        return false;
    }
    gLastStall = CACurrentMediaTime();
    progress("Fake engine running");
    return true;
}

void Describe(void*, const XRFrameInfo* head, GXHostFrameRequest* req) {
    const GXXRAngleOptions& opt = GXXRAngleLaunchOptions();
    memset(req, 0, sizeof(*req));
    req->targetMask = GX_TARGET_STEREO | (opt.noUIPanel ? 0u : GX_TARGET_UI);
    req->eyeCount = (int)head->eye_count;
    const XRRect& vp = head->eyes[0].viewport;
    req->eyeWidth = std::max(64, (int)std::lround(vp.width * opt.eyeScale));
    req->eyeHeight = std::max(64, (int)std::lround(vp.height * opt.eyeScale));
    req->gameWidth = 1280;
    req->gameHeight = 720;
}

bool Frame(void*, GXHostFrame* frame, GXHostFrameOutput* out) {
    const GXXRAngleOptions& opt = GXXRAngleLaunchOptions();
    [gScene renderFrame:&frame->info targets:frame->targets output:out];
    if (opt.fakeStallSeconds > 0 && CACurrentMediaTime() - gLastStall >= 10.0) {
        ++gStalls;
        GXXR_LOG("stall #%llu: sleeping %.1f s inside the frame (simulated map load); the compositor must keep presenting",
                 (unsigned long long)gStalls, opt.fakeStallSeconds);
        [NSThread sleepForTimeInterval:opt.fakeStallSeconds];
        gLastStall = CACurrentMediaTime();
        GXXR_LOG("stall #%llu over", (unsigned long long)gStalls);
    }
    return true;
}

void SetPaused(void*, bool paused) { GXXR_LOG("%s", paused ? "paused" : "resumed"); }

void Shutdown(void*) {
    [gScene teardown];
    gScene = nil;
}

}  // namespace

extern "C" bool GXXRFakeEngineStart(void) {
    const GXXRAngleOptions& opt = GXXRAngleLaunchOptions();
    static GXEngineClient client;
    client = GXEngineClient{};
    client.name = "fake-engine";
    client.boot = Boot;
    client.describe = Describe;
    client.frame = Frame;
    client.setPaused = SetPaused;
    client.shutdown = Shutdown;
    client.selfPaced = 0;
    client.fpsCap = opt.fakeFps;
    return GXEngineHost_StartClient(&client);
}
