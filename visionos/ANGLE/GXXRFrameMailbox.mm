// GXXRFrameMailbox.mm - see header. Objective-C++ with ARC.
#import "GXXRFrameMailbox.h"

#import <QuartzCore/QuartzCore.h>

#include <cstring>
#include <mutex>

@implementation GXXRHeadSnapshot
@end

@implementation GXXRPublishedFrame
@end

@implementation GXXRFrameMailbox {
    std::mutex _lock;
    GXXRHeadSnapshot* _head;
    GXXRPublishedFrame* _latest;
    uint64_t _headSeq;
    uint64_t _frameSeq;
}

- (void)publishHeadInfo:(const XRFrameInfo*)info anchor:(nullable ar_device_anchor_t)anchor {
    GXXRHeadSnapshot* s = [GXXRHeadSnapshot new];
    s->info = *info;
    s->info.command_buffer = NULL;
    for (int i = 0; i < XR_MAX_EYES; ++i) {
        s->info.eyes[i].color_target = NULL;
        s->info.eyes[i].depth_target = NULL;
    }
    s.anchor = anchor;
    s.time = CACurrentMediaTime();
    std::lock_guard<std::mutex> lock(_lock);
    s.seq = ++_headSeq;
    _head = s;
}

- (nullable GXXRHeadSnapshot*)latestHead {
    std::lock_guard<std::mutex> lock(_lock);
    return _head;
}

- (double)headAge {
    std::lock_guard<std::mutex> lock(_lock);
    return _head ? CACurrentMediaTime() - _head.time : 1e9;
}

- (nullable GXXRPublishedFrame*)acquireLatestFrame {
    std::lock_guard<std::mutex> lock(_lock);
    if (!_latest) return nil;
    [_ring retainSlot:_latest.slot];
    return _latest;
}

- (void)publishFrame:(GXXRPublishedFrame*)frame {
    GXXRPublishedFrame* old;
    {
        std::lock_guard<std::mutex> lock(_lock);
        frame.seq = ++_frameSeq;
        frame.publishTime = CACurrentMediaTime();
        old = _latest;
        _latest = frame;  // the writer reference of frame.slot is now the mailbox's
        if (old) [_ring releaseSlot:old.slot];
    }
}

- (void)dropLatestFrame {
    std::lock_guard<std::mutex> lock(_lock);
    if (_latest) {
        [_ring releaseSlot:_latest.slot];
        _latest = nil;
    }
}

- (uint64_t)publishedFrames {
    std::lock_guard<std::mutex> lock(_lock);
    return _frameSeq;
}
- (uint64_t)publishedHeads {
    std::lock_guard<std::mutex> lock(_lock);
    return _headSeq;
}
- (double)latestFrameAge {
    std::lock_guard<std::mutex> lock(_lock);
    return _latest ? CACurrentMediaTime() - _latest.publishTime : 1e9;
}

@end
