// GXXRFrameMailbox.h - the two lock-protected hand-offs between the compositor thread and the engine thread.
//
//   head mailbox   compositor -> engine   the newest XRFrameInfo (head + eye poses, viewports, matrices) plus the
//                                         ARKit device anchor it was built from. Latest wins; nothing queues.
//   frame mailbox  engine -> compositor   the newest COMPLETED engine frame: which ring slot, the XRFrameInfo and
//                                         anchor it was rendered with, and what to composite (stereo eyes, layers).
//
// The frame mailbox owns one ring-slot reference for its latest frame (the writer reference is transferred to it by
// -publishFrame:). -acquireLatestFrame: takes one more for the caller, released with
// -[GXXRTargetRing releaseSlot:afterCommandBuffer:]. Lock order: mailbox, then ring.
#import <ARKit/ARKit.h>
#import <Foundation/Foundation.h>

#import "GXXRTargetRing.h"
#include "GXEngineHostServices.h"
#include "XRPresentation.h"

NS_ASSUME_NONNULL_BEGIN

/// Immutable head/eye snapshot published by the compositor.
@interface GXXRHeadSnapshot : NSObject {
   @public
    XRFrameInfo info;  // native handles (color/depth targets, command buffer) are NULL
}
@property(nonatomic) uint64_t seq;
@property(nonatomic) CFTimeInterval time;
@property(nonatomic, strong, nullable) ar_device_anchor_t anchor;
@end

/// Immutable record of one completed engine frame.
@interface GXXRPublishedFrame : NSObject {
   @public
    XRFrameInfo info;           // the snapshot the frame was rendered for
    GXHostFrameOutput output;   // stereo / layers
}
@property(nonatomic) NSUInteger slot;
@property(nonatomic) uint64_t seq;         // engine frame counter
@property(nonatomic) uint64_t headSeq;     // compositor snapshot it was rendered for
@property(nonatomic) CFTimeInterval publishTime;
@property(nonatomic, strong, nullable) ar_device_anchor_t anchor;
@end

@interface GXXRFrameMailbox : NSObject

@property(nonatomic, weak, nullable) GXXRTargetRing* ring;

// ---- compositor thread ----
/// Publishes the newest head/eye snapshot (copied; native handles cleared).
- (void)publishHeadInfo:(const XRFrameInfo*)info anchor:(nullable ar_device_anchor_t)anchor;
/// The latest completed engine frame with a slot reference taken for the caller (release it with the ring), or nil.
- (nullable GXXRPublishedFrame*)acquireLatestFrame;

// ---- engine thread ----
- (nullable GXXRHeadSnapshot*)latestHead;
/// Seconds since the newest head snapshot was published (1e9 when none yet).
- (double)headAge;
/// Publishes a finished frame: its slot's writer reference moves to the mailbox, the previous latest frame's is dropped.
- (void)publishFrame:(GXXRPublishedFrame*)frame;
/// Drops the latest frame (and its slot reference), e.g. before the ring is resized or torn down.
- (void)dropLatestFrame;

// ---- statistics (any thread) ----
@property(nonatomic, readonly) uint64_t publishedFrames;
@property(nonatomic, readonly) uint64_t publishedHeads;
/// Seconds since the latest frame was published (1e9 when none).
- (double)latestFrameAge;

@end

NS_ASSUME_NONNULL_END
