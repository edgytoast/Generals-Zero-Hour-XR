// GXEngineHost.mm - the engine thread: boot, frame loop, pause, posted work, status and log.
// Objective-C++ with ARC. It contains no engine headers (those live in GXEngineHostEngine.cpp); it drives a
// GXEngineClient (the real engine, or the app's fake engine) against the app's GXEngineHostServices.
//
// Threads (docs/visionos-engine-host.md): this file's loop is the ONLY thread that runs engine code and GL. The
// compositor thread never calls into it except for GXEngineHost_GetStatus / _IsActive / _Pause.
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <os/log.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include "GXEngineHost.h"
#include "GXEngineHostEngine.h"
#include "GXEngineHostServices.h"

namespace {

constexpr double kHeadStaleSeconds = 0.5;   // no fresh head pose for this long: the compositor is not running
constexpr double kBootLogEverySeconds = 5.0;

os_log_t Log() {
    static os_log_t log = os_log_create("com.generalsx.zerohour.xr.vision", "engine-host");
    return log;
}
#define HLOG(fmt, ...)                                                    \
    do {                                                                  \
        fprintf(stderr, "[engine-host] " fmt "\n", ##__VA_ARGS__);        \
    } while (0)

double Now() { return CACurrentMediaTime(); }

struct Host {
    std::mutex mutex;
    std::condition_variable cv;
    GXEngineHostServices services = {};
    bool haveServices = false;

    GXEngineClient client = {};
    std::string clientName;
    GXEngineHostConfig config = {};
    std::string zhRoot, baseRoot, userDataRoot, appSupportRoot, logPath, textLanguage;
    bool useRealEngine = false;

    std::atomic<int> phase{GX_ENGINE_IDLE};
    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> pauseMask{0};
    std::atomic<bool> waitingForCompositor{false};
    std::deque<void (^)(void)> posted;

    // status (guarded by mutex)
    std::string progress, lastError;
    double bootStart = 0, bootEnd = 0;
    double engineFps = 0, logicHz = 0;
    bool logicInGame = false;
    double lastFrameMs = 0, longestFrameMs = 0, lastPublishTime = 0;
    std::atomic<uint64_t> produced{0}, skipped{0};
};

Host& H() {
    static Host h;
    return h;
}

void SetProgress(const char* text) {
    Host& h = H();
    std::lock_guard<std::mutex> lock(h.mutex);
    h.progress = text ? text : "";
}

std::string LastErrorLine();

void SetFailed(const std::string& reasonIn) {
    Host& h = H();
    std::string reason = reasonIn;
    [NSThread sleepForTimeInterval:0.15];  // the stderr tee thread writes the file asynchronously; let the last lines land
    const std::string detail = LastErrorLine();
    if (!detail.empty() && reason.find(detail) == std::string::npos) reason += " | last engine error line: " + detail;
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        h.lastError = reason;
        h.progress = "Engine stopped: " + reason;
        if (h.bootEnd == 0) h.bootEnd = Now();
    }
    h.phase.store(GX_ENGINE_FAILED);
    HLOG("FAILED: %s", reason.c_str());
    os_log_error(Log(), "engine failed: %{public}s", reason.c_str());
}

// Boot marker: exists while the real engine is booting. A fatal engine error that ends the process (ReleaseCrash -> _exit) leaves it
// behind, so the next launch can tell the player what happened (AppModel.startEngineInfrastructure reads it).
std::string MarkerPath() {
    Host& h = H();
    std::lock_guard<std::mutex> lock(h.mutex);
    return h.appSupportRoot.empty() ? std::string() : h.appSupportRoot + "/engine-boot.marker";
}
void WriteMarker() {
    const std::string path = MarkerPath();
    if (path.empty()) return;
    if (FILE* f = fopen(path.c_str(), "w")) {
        fprintf(f, "engine boot started\n");
        fclose(f);
    }
}
void RemoveMarker() {
    const std::string path = MarkerPath();
    if (!path.empty()) remove(path.c_str());
}

// ---- log tail ------------------------------------------------------------------------------------------------

std::string ReadTail(size_t bytes) {
    Host& h = H();
    std::string path;
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        path = h.logPath;
    }
    if (path.empty()) return {};
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    const long start = size > (long)bytes ? size - (long)bytes : 0;
    fseek(f, start, SEEK_SET);
    std::string out((size_t)(size - start), '\0');
    const size_t n = fread(out.data(), 1, out.size(), f);
    out.resize(n);
    fclose(f);
    return out;
}

// Last log line that says something about the engine: the compositor's / host's once-a-second statistics lines are skipped.
std::string LastLine(const std::string& text) {
    size_t end = text.size();
    for (int tries = 0; tries < 40 && end > 0; ++tries) {
        while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r')) --end;
        if (end == 0) break;
        const size_t nl = end > 0 ? text.rfind('\n', end - 1) : std::string::npos;
        const size_t begin = nl == std::string::npos ? 0 : nl + 1;
        const std::string line = text.substr(begin, end - begin);
        const bool noise = line.rfind("[GXXR] pipeline", 0) == 0 || line.rfind("[GXXR] frame loop", 0) == 0 ||
                           line.rfind("[GXXR] timing", 0) == 0 || line.rfind("[engine-host] engine:", 0) == 0;
        if (!line.empty() && !noise) return line;
        if (nl == std::string::npos) break;
        end = nl;
    }
    return {};
}

// The last "ERROR" line of the log (engine INI / asset errors precede a fatal error and say what was missing).
std::string LastErrorLine() {
    const std::string tail = ReadTail(16 * 1024);
    size_t end = tail.size();
    while (end > 0) {
        const size_t nl = tail.rfind('\n', end - 1);
        const size_t begin = nl == std::string::npos ? 0 : nl + 1;
        const std::string line = tail.substr(begin, end - begin);
        if (line.find("ERROR") != std::string::npos && line.find("[GXXR") == std::string::npos) return line;
        if (nl == std::string::npos || nl == 0) break;
        end = nl;
    }
    return {};
}

// ---- the loop --------------------------------------------------------------------------------------------------

void RunPosted(Host& h) {
    while (true) {
        void (^work)(void) = nil;
        {
            std::lock_guard<std::mutex> lock(h.mutex);
            if (h.posted.empty()) return;
            work = h.posted.front();
            h.posted.pop_front();
        }
        work();
    }
}

void WaitFor(Host& h, double seconds) {
    std::unique_lock<std::mutex> lock(h.mutex);
    h.cv.wait_for(lock, std::chrono::duration<double>(seconds));
}

void EngineThread() {
    Host& h = H();
    const GXEngineHostServices& sv = h.services;
    pthread_setname_np("GXXR.Engine");
    HLOG("engine thread started (client %s)", h.clientName.c_str());

    // ---- attach: ANGLE context + ring on THIS thread ----
    GXHostGLInfo gl = {};
    SetProgress("Preparing the graphics context...");
    if (!sv.attach(sv.user, h.config.forceAtlas, &gl)) {
        SetFailed("the ANGLE graphics context could not be created (see the [GXXR/ANGLE] lines in the log)");
        return;
    }

    // ---- boot (blocking; about a minute for the real engine) ----
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        h.bootStart = Now();
    }
    SetProgress("Booting the engine (this takes about a minute)...");
    char error[512] = {};
    HLOG("boot starting");
    if (h.useRealEngine) WriteMarker();
    const bool booted = h.client.boot(h.client.user, &gl, [](const char* text) { SetProgress(text); }, error, sizeof(error));
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        h.bootEnd = Now();
    }
    RemoveMarker();  // reached only when the process survived the boot (graceful failure or success)
    if (!booted) {
        SetFailed(error[0] ? error : "the engine did not boot (no reason recorded; see the log)");
        if (h.client.shutdown) h.client.shutdown(h.client.user);
        sv.detach(sv.user);
        return;
    }
    HLOG("boot finished in %.1f s", h.bootEnd - h.bootStart);
    SetProgress("Engine running");
    h.phase.store(GX_ENGINE_RUNNING);

    // ---- frames ----
    bool parked = false;
    double windowStart = Now(), lastLog = windowStart, lastLogicSample = windowStart;
    uint64_t windowFrames = 0;
    bool loggedWaiting = false;
    double nextDeadline = Now();
    bool leaveLoop = false;
    while (!h.stop.load() && !leaveLoop) {
        @autoreleasepool {  // per-iteration pool: ObjC temporaries of the services / mailbox must not pile up for hours
            RunPosted(h);
            const uint32_t mask = h.pauseMask.load();
            if (mask != 0) {
                if (!parked) {
                    parked = true;
                    if (h.client.setPaused) h.client.setPaused(h.client.user, true);
                    h.phase.store(GX_ENGINE_PAUSED);
                    HLOG("parked (pause mask 0x%x)", mask);
                }
                WaitFor(h, 0.05);
                continue;
            }
            if (parked) {
                parked = false;
                if (h.client.setPaused) h.client.setPaused(h.client.user, false);
                h.phase.store(GX_ENGINE_RUNNING);
                HLOG("unparked");
            }
            if (sv.headAge(sv.user) > kHeadStaleSeconds) {
                // No compositor (immersive space closed / not opened yet): nothing to render for. The engine, the
                // context and the ring stay alive.
                if (!h.waitingForCompositor.exchange(true) && !loggedWaiting) {
                    loggedWaiting = true;
                    HLOG("waiting for the compositor (no fresh head pose)");
                }
                WaitFor(h, 0.02);
                continue;
            }
            if (h.waitingForCompositor.exchange(false)) HLOG("compositor is back; frames resume");

            GXHostFrame frame = {};
            if (!sv.acquireHead(sv.user, &frame)) {
                WaitFor(h, 0.005);
                continue;
            }
            GXHostFrameRequest request = {};
            h.client.describe(h.client.user, &frame.info, &request);
            if (!sv.beginFrame(sv.user, &frame, &request)) {
                sv.abortFrame(sv.user, &frame);
                h.skipped.fetch_add(1);
                WaitFor(h, 0.005);
                continue;
            }
            const double t0 = Now();
            GXHostFrameOutput output = {};
            const bool keepRunning = h.client.frame(h.client.user, &frame, &output);
            if (!keepRunning) {
                sv.abortFrame(sv.user, &frame);
                HLOG("the engine stopped");
                if (h.useRealEngine && GXEngineHostEngine_LastError()[0] != '\0') {
                    SetFailed(std::string("the engine stopped: ") + GXEngineHostEngine_LastError());
                } else {
                    SetProgress("The game exited. Restart the app to play again.");
                    h.phase.store(GX_ENGINE_STOPPING);
                }
                break;
            }
            sv.endFrame(sv.user, &frame, &output);
            const double t1 = Now();
            h.produced.fetch_add(1);
            ++windowFrames;
            {
                std::lock_guard<std::mutex> lock(h.mutex);
                h.lastPublishTime = t1;
                h.lastFrameMs = (t1 - t0) * 1000.0;
                h.longestFrameMs = std::max(h.longestFrameMs, h.lastFrameMs);
            }
            if (h.lastFrameMs > 500) HLOG("long engine frame: %.0f ms (map load or stall); the compositor keeps presenting the last frame", h.lastFrameMs);

            // Fake clients are paced here; the real engine's FramePacer sleeps inside the frame.
            if (!h.client.selfPaced && h.client.fpsCap > 0) {
                // Deadline schedule (not "sleep the remainder"): a late wake-up does not lower the average rate.
                const double target = 1.0 / h.client.fpsCap;
                nextDeadline = std::max(nextDeadline + target, t1 - target);  // never try to catch up more than one frame
                const double wait = nextDeadline - Now();
                if (wait > 0) [NSThread sleepForTimeInterval:wait];
            }

            const double now = Now();
            if (now - windowStart >= 1.0) {
                std::lock_guard<std::mutex> lock(h.mutex);
                h.engineFps = (double)windowFrames / (now - windowStart);
                windowFrames = 0;
                windowStart = now;
            }
            if (now - lastLog >= kBootLogEverySeconds) {
                lastLog = now;
                GXHostRingInfo ring = {};
                if (sv.describeRing) sv.describeRing(sv.user, &ring);
                HLOG("engine: %.1f fps, %llu frames, %llu skipped, last frame %.1f ms, longest %.0f ms; ring %u slots (%u in use)", h.engineFps,
                     (unsigned long long)h.produced.load(), (unsigned long long)h.skipped.load(), h.lastFrameMs, h.longestFrameMs, ring.slots,
                     ring.slotsInUse);
            }
            if (h.useRealEngine && now - lastLogicSample >= 10.0) {
                double logicHz = 0, engineFps = 0;
                bool inGame = false;
                if (GXEngineHostEngine_SampleLogicRate(10.0, &logicHz, &engineFps, &inGame)) {
                    lastLogicSample = now;
                    {
                        std::lock_guard<std::mutex> lock(h.mutex);
                        h.logicHz = logicHz;
                        h.logicInGame = inGame;
                    }
                    HLOG("effective logic rate: %.1f Hz at %.1f engine fps over 10 s%s", logicHz, engineFps,
                         inGame ? " (match running; target 30 Hz)" : " (no unpaused match in the window, not meaningful)");
                }
            }
        }
    }

    HLOG("engine thread leaving the frame loop");
    if (h.client.shutdown) h.client.shutdown(h.client.user);
    sv.detach(sv.user);
    if (h.phase.load() != GX_ENGINE_FAILED) h.phase.store(GX_ENGINE_STOPPING);
}

// ---- the real engine as a client ----------------------------------------------------------------------------------

bool RealBoot(void*, const GXHostGLInfo* gl, void (*progress)(const char*), char* error, size_t capacity) {
    progress("Initializing the engine (about a minute)...");
    return GXEngineHostEngine_Boot(&H().config, gl, error, capacity);
}
void RealDescribe(void*, const XRFrameInfo* head, GXHostFrameRequest* request) { GXEngineHostEngine_Describe(head, request); }
bool RealFrame(void*, GXHostFrame* frame, GXHostFrameOutput* output) { return GXEngineHostEngine_Frame(frame, output); }
void RealSetPaused(void*, bool paused) { GXEngineHostEngine_SetPaused(paused); }
void RealShutdown(void*) { GXEngineHostEngine_Shutdown(); }

bool LaunchThread() {
    Host& h = H();
    NSThread* thread = [[NSThread alloc] initWithBlock:^{
        @autoreleasepool {
            EngineThread();
        }
    }];
    thread.name = @"GXXR.Engine";
    thread.qualityOfService = NSQualityOfServiceUserInitiated;  // below the compositor's user-interactive thread
    thread.stackSize = 16 * 1024 * 1024;                        // deep engine recursion (INI parsing, map loading)
    (void)h;
    [thread start];
    return true;
}

bool Begin(bool real) {
    Host& h = H();
    if (h.started.exchange(true)) return false;
    if (!h.haveServices) {
        SetFailed("the host services are not installed (the app did not link visionos/Bridge)");
        return true;  // started, and failed asynchronously like every other boot failure
    }
    h.useRealEngine = real;
    h.stop.store(false);
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        h.progress = "Starting the engine thread...";
        h.lastError.clear();
    }
    h.phase.store(GX_ENGINE_BOOTING);
    return LaunchThread();
}

}  // namespace

// ---------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------

extern "C" {

void GXEngineHost_SetServices(const GXEngineHostServices* services) {
    Host& h = H();
    std::lock_guard<std::mutex> lock(h.mutex);
    if (services) {
        h.services = *services;
        h.haveServices = true;
    }
}

void GXEngineHost_SetProgress(const char* text) { SetProgress(text); }

void GXEngineHost_BeginLogging(const char* path) {
    if (!path || !path[0]) return;
    Host& h = H();
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        if (!h.logPath.empty()) return;
        h.logPath = path;
    }
    GXEngineHostEngine_InstallLogSink(path);
    HLOG("logging to %s", path);
}

const char* GXEngineHost_LogPath(void) {
    Host& h = H();
    std::lock_guard<std::mutex> lock(h.mutex);
    return h.logPath.c_str();  // never modified after BeginLogging set it
}

size_t GXEngineHost_ReadLogTail(char* out, size_t capacity, int maxLines) {
    if (!out || capacity == 0) return 0;
    out[0] = '\0';
    std::string tail = ReadTail(16 * 1024);
    // keep the last maxLines lines
    int lines = 0;
    size_t cut = tail.size();
    if (!tail.empty() && tail.back() == '\n') --cut;
    while (cut > 0 && lines < maxLines) {
        const size_t nl = tail.rfind('\n', cut - 1);
        if (nl == std::string::npos) {
            cut = 0;
            break;
        }
        cut = nl;
        ++lines;
    }
    const std::string kept = tail.substr(cut == 0 ? 0 : cut + 1);
    snprintf(out, capacity, "%s", kept.c_str());
    return strlen(out);
}

bool GXEngineHost_IsActive(void) { return H().started.load(); }

bool GXEngineHost_Start(const GXEngineHostConfig* config) {
    Host& h = H();
    if (!config || !config->zhRoot || !config->zhRoot[0] || !config->userDataRoot || !config->userDataRoot[0]) return false;
    if (h.started.load()) return false;
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        h.zhRoot = config->zhRoot;
        h.baseRoot = config->baseRoot ? config->baseRoot : "";
        h.userDataRoot = config->userDataRoot;
        if (config->appSupportRoot && config->appSupportRoot[0]) {
            h.appSupportRoot = config->appSupportRoot;
        } else {
            NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
            h.appSupportRoot = std::string(dirs.firstObject.UTF8String ?: "/tmp") + "/GeneralsX";
        }
        h.textLanguage = config->textLanguage ? config->textLanguage : "";
        h.config = *config;
        h.config.zhRoot = h.zhRoot.c_str();
        h.config.baseRoot = h.baseRoot.c_str();
        h.config.userDataRoot = h.userDataRoot.c_str();
        h.config.appSupportRoot = h.appSupportRoot.c_str();
        h.config.textLanguage = h.textLanguage.empty() ? nullptr : h.textLanguage.c_str();
        if (config->logPath && config->logPath[0]) {
            if (h.logPath.empty()) h.logPath = config->logPath;
        } else if (h.logPath.empty()) {
            h.logPath = h.appSupportRoot + "/generals-xr-stderr.log";
        }
        h.config.logPath = h.logPath.c_str();
    }
    [[NSFileManager defaultManager] createDirectoryAtPath:@(h.appSupportRoot.c_str()) withIntermediateDirectories:YES attributes:nil error:nil];
    GXEngineHostEngine_InstallLogSink(h.logPath.c_str());
    h.client = GXEngineClient{};
    h.client.name = "engine";
    h.clientName = "engine";
    h.client.boot = RealBoot;
    h.client.describe = RealDescribe;
    h.client.frame = RealFrame;
    h.client.setPaused = RealSetPaused;
    h.client.shutdown = RealShutdown;
    h.client.selfPaced = 1;
    HLOG("GXEngineHost_Start: zh=%s base=%s userData=%s support=%s log=%s", h.zhRoot.c_str(), h.baseRoot.c_str(), h.userDataRoot.c_str(),
         h.appSupportRoot.c_str(), h.logPath.c_str());
    return Begin(true);
}

bool GXEngineHost_StartClient(const GXEngineClient* client) {
    Host& h = H();
    if (!client || !client->boot || !client->describe || !client->frame) return false;
    if (h.started.load()) return false;
    h.client = *client;
    h.clientName = client->name ? client->name : "client";
    h.config = GXEngineHostConfig{};
    return Begin(false);
}

void GXEngineHost_Pause(GXEngineHostPauseReason reason, bool paused) {
    Host& h = H();
    const uint32_t before = h.pauseMask.load();
    if (paused) h.pauseMask.fetch_or((uint32_t)reason);
    else h.pauseMask.fetch_and(~(uint32_t)reason);
    if (h.pauseMask.load() != before) h.cv.notify_all();
}

bool GXEngineHost_Post(void (^work)(void)) {
    Host& h = H();
    if (!work || !h.started.load()) return false;
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        h.posted.push_back([work copy]);
    }
    h.cv.notify_all();
    return true;
}

void GXEngineHost_GetStatus(GXEngineHostStatus* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    Host& h = H();
    out->phase = (GXEngineHostPhase)h.phase.load();
    out->pauseMask = h.pauseMask.load();
    out->waitingForCompositor = h.waitingForCompositor.load();
    out->framesProduced = h.produced.load();
    out->framesSkipped = h.skipped.load();
    std::string logPath;
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        out->fake = h.started.load() && !h.useRealEngine;
        out->servicesInstalled = h.haveServices;
        snprintf(out->progress, sizeof(out->progress), "%s", h.progress.c_str());
        snprintf(out->lastError, sizeof(out->lastError), "%s", h.lastError.c_str());
        snprintf(out->logPath, sizeof(out->logPath), "%s", h.logPath.c_str());
        logPath = h.logPath;
        const double end = h.bootEnd > 0 ? h.bootEnd : (h.bootStart > 0 && out->phase == GX_ENGINE_BOOTING ? Now() : h.bootStart);
        out->bootSeconds = h.bootStart > 0 ? std::max(0.0, end - h.bootStart) : 0;
        out->engineFps = (Now() - h.lastPublishTime < 1.5) ? h.engineFps : 0.0;  // decays while the engine is stalled / parked
        out->logicHz = h.logicHz;
        out->logicInGame = h.logicInGame;
        out->lastFrameMs = h.lastFrameMs;
        out->longestFrameMs = h.longestFrameMs;
    }
    if (h.haveServices && h.services.describeRing && h.started.load()) {
        GXHostRingInfo ring = {};
        h.services.describeRing(h.services.user, &ring);
        out->ringSlots = ring.slots;
        out->ringSlotsInUse = ring.slotsInUse;
        out->ringMegabytes = ring.megabytes;
        snprintf(out->syncMode, sizeof(out->syncMode), "%s", ring.syncMode);
        snprintf(out->renderer, sizeof(out->renderer), "%s", ring.renderer);
    }
    if (out->phase == GX_ENGINE_BOOTING || out->phase == GX_ENGINE_FAILED || out->phase == GX_ENGINE_STOPPING) {
        const std::string line = LastLine(ReadTail(4096));
        snprintf(out->lastLogLine, sizeof(out->lastLogLine), "%s", line.c_str());
    }
}

}  // extern "C"
