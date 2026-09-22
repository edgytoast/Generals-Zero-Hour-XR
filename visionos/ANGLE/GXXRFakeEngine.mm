// GXXRFakeEngine.mm - see header. Objective-C++ with ARC.
#import "GXXRFakeEngine.h"

#import <QuartzCore/QuartzCore.h>
#import <simd/simd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "GXEngineHostServices.h"
#include "GXXRD3D8GLES.h"
#include "XRPresentation.h"

// This file is compiled by the Xcode app target (visionos/project.yml), whose header search path does not reach
// GeneralsMD/Code/Main (only .../visionos: see project.yml's comment on HEADER_SEARCH_PATHS), so it deliberately does
// NOT include VisionPresentationLogic.h (which pulls in the Quest XrLayers.h/XrWorld.h/... chain through
// VisionHostState.h/VisionEngineBridge.h). These four ordinals mirror VisionPresentationMode
// (GeneralsMD/Code/Main/visionos/VisionPresentationLogic.h:53) exactly, for the diagnostic-only
// GXHostFrameOutput.presentationMode int; keep them in sync if that enum's order ever changes.
enum { kVisionModeIdle = 0, kVisionModeLoading = 1, kVisionModeMenu = 2, kVisionModeTabletop = 4, kVisionModeGroundView = 5 };

#import "GXXRANGLEContext.h"
#import "GXXRAngleOptions.h"
#import "GXXRGLTestScene.h"

#define GXXR_LOG(fmt, ...) fprintf(stderr, "[fake-engine] " fmt "\n", ##__VA_ARGS__)

namespace {

GXXRGLTestScene* gScene = nil;
CFTimeInterval gLastStall = 0;
uint64_t gStalls = 0;

// ---- package C2: scripted mode sequence (docs/visionos-presentation.md section 10) --------------------------------
// -fakeEngineScript cycles loading -> menu -> tabletop -> ground-view -> tabletop -> ground-view -> ... so a soak run
// keeps exercising every mode; the FIRST loading phase additionally runs a real GXEngineHost_PresentNested burst
// (mission item 3: "a fake-engine mode that blocks and presents").
enum class ScriptPhase { Loading, Menu, Tabletop, GroundView };

CFTimeInterval gScriptT0 = 0;
ScriptPhase gScriptPhase = ScriptPhase::Loading;
bool gScriptPhaseLogged = false;
bool gLoadingBurstDone = false;  // only the very first loading phase runs the nested-present demonstration

const char* PhaseName(ScriptPhase p) {
    switch (p) {
        case ScriptPhase::Loading: return "loading";
        case ScriptPhase::Menu: return "menu";
        case ScriptPhase::Tabletop: return "tabletop";
        case ScriptPhase::GroundView: return "ground-view";
    }
    return "?";
}

int32_t VisionModeOrdinalFor(ScriptPhase p) {
    switch (p) {
        case ScriptPhase::Loading: return kVisionModeLoading;
        case ScriptPhase::Menu: return kVisionModeMenu;
        case ScriptPhase::Tabletop: return kVisionModeTabletop;
        case ScriptPhase::GroundView: return kVisionModeGroundView;
    }
    return kVisionModeIdle;
}

// Where in the loading -> menu -> tabletop -> ground-view cycle `t` (seconds since the script started) falls.
// `phaseT` (out) is seconds into that phase, `phaseDur` (out) is the phase's total duration.
ScriptPhase PhaseAt(double t, const GXXRAngleOptions& opt, double* phaseT, double* phaseDur) {
    const double cycle = opt.scriptLoading + opt.scriptMenu + opt.scriptTabletop + opt.scriptGround;
    double tt = cycle > 0 ? std::fmod(std::max(0.0, t), cycle) : 0.0;
    if (tt < opt.scriptLoading) { *phaseT = tt; *phaseDur = opt.scriptLoading; return ScriptPhase::Loading; }
    tt -= opt.scriptLoading;
    if (tt < opt.scriptMenu) { *phaseT = tt; *phaseDur = opt.scriptMenu; return ScriptPhase::Menu; }
    tt -= opt.scriptMenu;
    if (tt < opt.scriptTabletop) { *phaseT = tt; *phaseDur = opt.scriptTabletop; return ScriptPhase::Tabletop; }
    tt -= opt.scriptTabletop;
    *phaseT = tt; *phaseDur = opt.scriptGround; return ScriptPhase::GroundView;
}

// Runs a blocking loop that publishes nested frames through GXEngineHost_PresentNested while "loading" -- the same
// protocol the real engine's XrGameBoot_SetLoadingPresenter path uses (GXEngineHostEngine_PresentLoading), proving it
// end to end without game data: the compositor must keep showing fresh frames (and its frame counter must keep moving)
// for the whole burst even though this function does not return until it is over.
void PresentLoadingBurst(GXHostFrame* frame, double seconds) {
    const CFTimeInterval end = CACurrentMediaTime() + seconds;
    int nested = 0, lost = 0;
    while (CACurrentMediaTime() < end && frame->targets != nullptr) {
        GXHostFrameOutput out;
        [gScene renderFrame:&frame->info targets:frame->targets output:&out];
        out.stereoValid = false;  // loading: the upright panel only, no stereo world
        out.presentationMode = kVisionModeLoading;
        GXEngineHost_SetPresentationStatus("loading", 0, 0, 1280, 720);
        GXHostFrameRequest req = {};
        req.targetMask = GX_TARGET_GAME | GX_TARGET_UI;
        req.eyeCount = (int)frame->info.eye_count;
        req.gameWidth = 1280;
        req.gameHeight = 720;
        const int result = GXEngineHost_PresentNested(frame, &out, &req);
        ++nested;
        if (result == GX_NESTED_LOST) ++lost;  // the fake engine has no d3d8gles target to re-arm; frame->slot == -1 until the next real frame
        [NSThread sleepForTimeInterval:0.10];
    }
    GXXR_LOG("loading burst: %d nested frames presented over %.1f s (%d lost slot)", nested, seconds, lost);
}

// Synthesizes a plausible GXHostFeedback for the tabletop / ground-view phases: a box-select rectangle and cursor on
// the board (tabletop), a fading waypoint marker, and the comfort-fade veil ramping at ground-view entry/exit. This
// stands in for the interaction layer (package E), which has nothing to drive it without real input.
void FillScriptedFeedback(ScriptPhase phase, double phaseT, double phaseDur, GXHostFrameOutput& out) {
    float wfb[16];
    float hx = 0, hz = 0;
    if (!XRPresentation_GetTabletopPlacement(wfb, &hx, &hz)) return;
    simd_float4x4 worldFromBoard;
    memcpy(&worldFromBoard, wfb, sizeof(worldFromBoard));
    const simd_quatf boardOrientation = simd_quaternion(worldFromBoard);
    auto boardPoint = [&](float bx, float bz) {
        const simd_float4 p = simd_mul(worldFromBoard, simd_make_float4(bx, 0, bz, 1));
        return simd_make_float3(p.x, p.y, p.z);
    };

    GXHostFeedback& fb = out.feedback;
    memset(&fb, 0, sizeof(fb));
    fb.boardOrientation[0] = boardOrientation.vector.x;
    fb.boardOrientation[1] = boardOrientation.vector.y;
    fb.boardOrientation[2] = boardOrientation.vector.z;
    fb.boardOrientation[3] = boardOrientation.vector.w;
    fb.boardWidth = hx * 2.0f;

    if (phase == ScriptPhase::Tabletop) {
        // A box-select rectangle over one quadrant of the board (counter-clockwise from above) and a cursor near its
        // corner, as if a box-select drag just ended there.
        const float x0 = -hx * 0.55f, x1 = -hx * 0.10f, z0 = -hz * 0.35f, z1 = hz * 0.20f;
        const simd_float3 c0 = boardPoint(x0, z0), c1 = boardPoint(x1, z0), c2 = boardPoint(x1, z1), c3 = boardPoint(x0, z1);
        auto store = [](float dst[3], simd_float3 v) { dst[0] = v.x; dst[1] = v.y; dst[2] = v.z; };
        store(fb.boxCorners[0], c0);
        store(fb.boxCorners[1], c1);
        store(fb.boxCorners[2], c2);
        store(fb.boxCorners[3], c3);
        fb.boxAdditive = (phaseT > phaseDur * 0.5) ? 1 : 0;  // halfway through the phase: show the additive-select tint too
        fb.flags |= GX_FB_BOX;

        store(fb.cursor, boardPoint(x1, z0));
        fb.cursorOnBoard = 1;
        fb.flags |= GX_FB_CURSOR;

        // The board grab bar under the near edge, lightly lit (not actively grabbed).
        store(fb.grabPosition, boardPoint(0, -hz - 0.04f));
        fb.grabOrientation[0] = boardOrientation.vector.x;
        fb.grabOrientation[1] = boardOrientation.vector.y;
        fb.grabOrientation[2] = boardOrientation.vector.z;
        fb.grabOrientation[3] = boardOrientation.vector.w;
        fb.grabLength = hx * 0.6f;
        fb.grabThickness = 0.03f;
        fb.grabActive = 0;
        fb.flags |= GX_FB_GRAB_BAR;

        // A waypoint marker that ages across the phase (fades over its last 1.2 s, GXXRFeedbackRenderer's rule).
        store(fb.waypoint, boardPoint(hx * 0.3f, hz * 0.25f));
        fb.waypointAge = (float)phaseT;
        fb.flags |= GX_FB_WAYPOINT;
    } else if (phase == ScriptPhase::GroundView) {
        // A teleport reticle near the board center (Ground View "PickObserverGround" target).
        const simd_float3 target = boardPoint(0, hz * 0.15f);
        fb.groundTarget[0] = target.x;
        fb.groundTarget[1] = 0.0f;  // ground-plane marker: y is handled by the renderer (up * 0.01 offset)
        fb.groundTarget[2] = target.z;
        fb.groundTargetValid = 1;
        fb.groundHold = 0.0f;
        fb.flags |= GX_FB_GROUND_TARGET;

        // Comfort fade: veil to black for the first/last 0.5 s of the phase (simulating the enter/exit teleport cut),
        // clear in between.
        const double rampIn = std::min(0.5, phaseDur * 0.3), rampOut = std::min(0.5, phaseDur * 0.3);
        float alpha = 0.0f;
        if (phaseT < rampIn) alpha = 1.0f - (float)(phaseT / rampIn);
        else if (phaseT > phaseDur - rampOut) alpha = 1.0f - (float)((phaseDur - phaseT) / rampOut);
        fb.fadeAlpha = std::clamp(alpha, 0.0f, 1.0f);
    }
}

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
    gScriptT0 = CACurrentMediaTime();
    gScriptPhase = ScriptPhase::Loading;
    gScriptPhaseLogged = false;
    gLoadingBurstDone = false;
    if (opt.script) GXXR_LOG("scripted mode: loading %.1fs -> menu %.1fs -> tabletop %.1fs -> ground-view %.1fs (repeats)",
                              opt.scriptLoading, opt.scriptMenu, opt.scriptTabletop, opt.scriptGround);
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

    if (!opt.script) {
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

    // ---- scripted mode: loading -> menu -> tabletop -> ground-view -> tabletop -> ... ------------------------------
    double phaseT = 0, phaseDur = 1;
    const ScriptPhase phase = PhaseAt(CACurrentMediaTime() - gScriptT0, opt, &phaseT, &phaseDur);
    if (phase != gScriptPhase || !gScriptPhaseLogged) {
        GXXR_LOG("scripted mode: entering phase '%s' (%.1f s)", PhaseName(phase), phaseDur);
        gScriptPhase = phase;
        gScriptPhaseLogged = true;
    }

    if (phase == ScriptPhase::Loading && !gLoadingBurstDone) {
        // Mission item 3: prove GXEngineHost_PresentNested actually blocks-and-presents, once, in the very first
        // loading phase (a real synchronous loader only blocks once per load; repeating it every cycle would just be
        // the ordinary stall test above).
        PresentLoadingBurst(frame, std::min(1.5, phaseDur * 0.6));
        gLoadingBurstDone = true;
    }

    [gScene renderFrame:&frame->info targets:frame->targets output:out];
    out->presentationMode = VisionModeOrdinalFor(phase);
    if (phase == ScriptPhase::Loading || phase == ScriptPhase::Menu) {
        out->stereoValid = false;  // upright panel only: no stereo world while loading or in a menu
    }
    if (phase == ScriptPhase::GroundView) {
        out->groundView = true;  // compositor clears to opaque black behind the eyes (docs: no passthrough)
        out->stereoValid = false;  // the fake engine has no distinct ground-eye camera; show the reticle + fade only
    }
    FillScriptedFeedback(phase, phaseT, phaseDur, *out);
    GXEngineHost_SetPresentationStatus(PhaseName(phase), out->stereoValid ? (int)frame->info.eyes[0].viewport.width : 0,
                                        out->stereoValid ? (int)frame->info.eyes[0].viewport.height : 0, 1280, 720);
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
