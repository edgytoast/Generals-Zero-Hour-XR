// MODEL test of the frame-pacing policy (docs/visionos-engine-host.md section 7). It re-implements, line for line, the decision of
// GameEngine::canUpdateRegularGameLogic (GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp), FramePacer's limiter and
// CommandXlat.cpp changeLogicTimeScale, and drives them with a simulated clock. It is NOT the engine: it shows that the arithmetic the
// policy relies on holds (30 Hz logic at any capped render rate, the unguarded key path reaching the display rate, the guarded envelope),
// so a change to the engine's algorithm would need this model updated. The effective rate of the real engine is measured at run time
// by the 10 s self-check ("effective logic rate" log line), which needs an unpaused match (game data).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

struct Pacer {
    bool limiter = false, scale = false;
    int limit = 30, scaleFps = 30;
    double acc = 0;
    static constexpr int kUncapped = 1000000;
    int actualLimit() const { return limiter ? limit : kUncapped; }
    int actualScale() const { return scale ? scaleFps : kUncapped; }
    // canUpdateRegularGameLogic
    bool canUpdate(double updateTime) {
        const int s = actualScale(), r = actualLimit();
        if (s >= r) return true;
        const double target = 1.0 / s;
        acc += std::min(updateTime, target);
        if (acc >= target) { acc -= target; return true; }
        return false;
    }
    // CommandXlat.cpp changeLogicTimeScale(Increase)
    void keyIncrease() {
        const int maxRender = limit;
        int rem = 5 - (maxRender % 5); rem %= 5;
        int fps = scaleFps;
        if (!scale) fps = maxRender + rem;
        fps = std::min(fps, maxRender + rem);
        fps += 5;
        if (scale) scaleFps = fps;
        scale = fps < maxRender;
        if (scale) scaleFps = fps;
    }
};

// Simulate `seconds` of a host whose display runs at `displayHz` (the engine thread frame time is 1/min(displayHz, cap)).
static double logicHz(Pacer p, double displayHz, double seconds) {
    const double frame = 1.0 / std::min(displayHz, (double)p.actualLimit());
    long logic = 0;
    for (double t = 0; t < seconds; t += frame) logic += p.canUpdate(frame) ? 1 : 0;
    return logic / seconds;
}

static int failures = 0;
static void expect(const char* what, double value, double lo, double hi) {
    const bool ok = value >= lo && value <= hi;
    failures += ok ? 0 : 1;
    printf("%s %-64s %.2f Hz (expected %.1f..%.1f)\n", ok ? "PASS" : "FAIL", what, value, lo, hi);
}

int main() {
    Pacer quest;  // today's Quest host: no limiter, no logic scale
    expect("unpatched host at 90 Hz display (the R1 bug)", logicHz(quest, 90, 20), 89, 91);
    expect("unpatched host at 72 Hz display", logicHz(quest, 72, 20), 71, 73);

    Pacer policy; policy.scale = true; policy.scaleFps = 30; policy.limiter = true; policy.limit = 45;
    expect("policy (30 Hz scale, cap 45) at 90 Hz display", logicHz(policy, 90, 20), 29.5, 30.5);
    expect("policy at 60 Hz display", logicHz(policy, 60, 20), 29.5, 30.5);
    expect("policy at 45 Hz display", logicHz(policy, 45, 20), 29.5, 30.5);
    expect("policy at 36 Hz display (engine slower than the cap)", logicHz(policy, 36, 20), 29.5, 30.5);
    expect("policy at 24 Hz display (below 30: logic follows render, as designed)", logicHz(policy, 24, 20), 23.5, 24.5);

    // Speed keys, unguarded and limiter switched off (options / LOD): the R1 bug returns.
    Pacer unguarded = policy;
    for (int i = 0; i < 6; ++i) unguarded.keyIncrease();
    unguarded.limiter = false;
    expect("unguarded: keys up + limiter lost, 90 Hz display", logicHz(unguarded, 90, 20), 89, 91);

    // Guarded: limiter restored, keys reach the limit and switch the scale off: bounded by the (limited) render rate.
    Pacer guarded = policy;
    for (int i = 0; i < 6; ++i) guarded.keyIncrease();
    guarded.limiter = true;  // guard: limiter forced on, limit clamped to [30, max(cap, 60)]
    expect("guarded: keys up (scale off), 90 Hz display -> cap 45", logicHz(guarded, 90, 20), 44, 46);
    for (int i = 0; i < 6; ++i) guarded.keyIncrease();
    guarded.limit = std::clamp(guarded.limit, 30, 60);
    expect("guarded: more key presses stay bounded", logicHz(guarded, 90, 20), 44, 61);
    // New match: the guard re-applies the default.
    guarded.scale = true; guarded.scaleFps = 30; guarded.limit = 45; guarded.acc = 0;
    expect("guarded: new match starts at the default again", logicHz(guarded, 90, 20), 29.5, 30.5);
    printf("%s\n", failures ? "FAILED" : "all model checks passed");
    return failures ? 1 : 0;
}
