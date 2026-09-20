// GXXRPlatformLifecycle.mm - implements PlatformLifecycle.h.
#import <Foundation/Foundation.h>
#include <atomic>
#include <mutex>

#include "PlatformLifecycle.h"

namespace {
std::mutex gMutex;
PlatformLifecycleCallbacks gCallbacks = {nullptr, nullptr};
std::atomic<int> gLast{0};
}  // namespace

extern "C" {

void PlatformLifecycle_SetCallbacks(const PlatformLifecycleCallbacks* callbacks) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (callbacks) gCallbacks = *callbacks;
    else gCallbacks = {nullptr, nullptr};
}

void PlatformLifecycle_Notify(PlatformLifecycleEvent event) {
    PlatformLifecycleCallbacks cb;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        cb = gCallbacks;
    }
    gLast.store((int)event);
    if (cb.on_event) cb.on_event(cb.user, event);
}

PlatformLifecycleEvent PlatformLifecycle_LastEvent(void) {
    return (PlatformLifecycleEvent)gLast.load();
}

}  // extern "C"
