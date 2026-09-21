// GXXRTargetRing.h - ring of host-owned render targets shared between ANGLE (GL) and the
// Metal compositor.
//
// A slot holds one MTLTexture per named target (D3D8GLES_XRT_STEREO_LEFT/RIGHT at eye size,
// GAME/WORLD/UI at independent sizes). Each texture is created from ANGLE's MTLDevice with
// RenderTarget|ShaderRead usage and private storage, imported ONCE into a GL texture name
// through EGL_ANGLE_metal_texture_client_buffer (eglCreateImageKHR + glEGLImageTargetTexture2DOES)
// and then reused for the lifetime of the ring. The GL names are what the engine attaches to
// its framebuffers (through d3d8gles_SetXRHostTargets); the MTLTextures are what the compositor
// samples.
//
// Ownership and threads (engine thread / compositor thread split, docs/visionos-engine-host.md):
//
//   Engine thread (owns the ANGLE context; GL side):
//     -beginFrame            acquires a FREE slot (reference count 0) and takes the writer reference;
//                            waits (bounded) when every slot is still being composited
//     -fillTargets:          the D3D8GLES_XRTargets of that slot for d3d8gles_SetXRHostTargets
//     -endGLWork             eglCreateSync(EGL_SYNC_METAL_SHARED_EVENT_ANGLE, glEvent, n) + glFlush
//                            (glFinish fallback), records n as the slot's GL value
//     -abortFrame            gives the writer reference back without publishing
//
//   The writer reference is TRANSFERRED to the frame mailbox by publishing; the mailbox drops it when a
//   newer frame replaces it. Every composite that samples a slot holds one more reference.
//
//   Compositor thread (Metal side; never touches GL):
//     -retainSlot:           +1 (the mailbox does it when the compositor takes the latest frame)
//     -encodeWaitForGLSlot:into:   encodeWaitForEvent(glEvent, slot's GL value) on the composite command buffer
//     -releaseSlot:afterCommandBuffer:  -1 when that command buffer has COMPLETED on the GPU
//
//   A slot is reusable exactly when its reference count is 0, so a published slot stays in use across as
//   many composites as re-present it (the compositor runs faster than the engine and repeats the last
//   frame while the engine stalls) and is released only after the last composite command buffer finished.
//   Rendering into a slot needs no GPU-side "release" fence: the completed handler is the release.
//
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#import "GXXRANGLEContext.h"
#include "GXXRD3D8GLES.h"

NS_ASSUME_NONNULL_BEGIN

@interface GXXRTargetRing : NSObject

/// Slots in the ring (3).
@property(nonatomic, readonly) NSUInteger slotCount;
/// Pixel format of every target actually in use (RGBA8Unorm when ANGLE accepts it, else BGRA8Unorm).
@property(nonatomic, readonly) MTLPixelFormat pixelFormat;
/// YES when MTLSharedEvent fences are used; NO when the glFinish fallback is in effect.
@property(nonatomic, readonly) BOOL usesSharedEventSync;
/// Slot chosen by the last successful -beginFrame (engine thread).
@property(nonatomic, readonly) NSUInteger currentSlot;
/// CPU milliseconds the last engine frame spent waiting (free-slot wait + glFinish fallback).
@property(nonatomic, readonly) double lastSyncWaitMs;
/// Times -beginFrame found no free slot within its wait budget (compositor stalled); that frame is skipped.
@property(nonatomic, readonly) uint64_t releaseTimeouts;
/// Slots whose reference count is not 0 right now (diagnostics, any thread).
@property(nonatomic, readonly) NSUInteger slotsInUse;
/// Total ring GPU memory in bytes (color only).
@property(nonatomic, readonly) uint64_t allocatedBytes;

/// Forces the glFinish fallback even when shared events exist (testing).
@property(nonatomic) BOOL forceGLFinish;
/// atlas = YES packs both eyes side by side into STEREO_LEFT and reports it through fillTargets.
@property(nonatomic) BOOL atlas;

/// `preferredFormat` is tried first; the ring falls back to the other of RGBA8/BGRA8 when the
/// import is not framebuffer-complete. The ANGLE context must be current.
- (nullable instancetype)initWithContext:(GXXRANGLEContext*)context
                             pixelFormat:(MTLPixelFormat)preferredFormat
                               slotCount:(NSUInteger)slotCount;

/// Declares the size of one named target (D3D8GLES_XRT_*). 0 x 0 disables it. Takes effect at
/// the next -beginFrame and applies to every slot. Cheap when nothing changes.
- (void)setSizeWidth:(int)width height:(int)height forTarget:(int)target;

/// Convenience for the stereo pair: eye size w x h, eyeCount 1 or 2. In atlas mode STEREO_LEFT
/// becomes (w * eyeCount) x h and STEREO_RIGHT is disabled.
- (void)configureStereoEyeWidth:(int)width height:(int)height eyeCount:(int)eyeCount;

/// (Engine thread) Would -beginFrame have to re-create targets because a declared size changed?
/// The caller must drop every published reference (mailbox) BEFORE calling -beginFrame in that case.
@property(nonatomic, readonly) BOOL needsResize;

/// (Engine thread) Acquires a free slot, taking the writer reference; (re)allocates targets whose size changed
/// (after draining every composite), imports new textures. Returns -1 when allocation failed or no slot became
/// free in time (the frame is skipped).
- (NSInteger)beginFrame;

/// (Engine thread) The struct passed to d3d8gles_SetXRHostTargets for the slot acquired by -beginFrame.
- (void)fillTargets:(struct D3D8GLES_XRTargets*)outTargets;

/// (Engine thread) glFlush + signal (or glFinish). Call after the GL frame.
- (void)endGLWork;
/// (Engine thread) Gives the writer reference back without publishing (the frame is dropped).
- (void)abortFrame;

/// (Any thread) Reference counting of a slot. The frame mailbox / composite hold references.
- (void)retainSlot:(NSUInteger)slot;
- (void)releaseSlot:(NSUInteger)slot;
/// (Compositor thread) Releases `slot` when `commandBuffer` has completed on the GPU.
- (void)releaseSlot:(NSUInteger)slot afterCommandBuffer:(id<MTLCommandBuffer>)commandBuffer;
/// (Compositor thread) Encodes the GPU wait for the GL work of `slot`.
- (void)encodeWaitForGLSlot:(NSUInteger)slot into:(id<MTLCommandBuffer>)commandBuffer;

/// (Any thread, but only for a slot the caller holds a reference to) Textures and sizes of a slot.
- (nullable id<MTLTexture>)textureForTarget:(int)target slot:(NSUInteger)slot;
- (int)widthForTarget:(int)target;
- (int)heightForTarget:(int)target;
/// (Engine thread) Current slot's texture / GL name for a target; nil / 0 when the target is disabled.
- (nullable id<MTLTexture>)textureForTarget:(int)target;
- (unsigned)glTextureForTarget:(int)target;

/// (Engine thread) Waits for every reference to drop (CPU, bounded). Used before resizing and at teardown.
- (void)drain;
/// (Engine thread) Deletes every GL texture / sync and drops the MTLTextures. The ANGLE context must be current.
- (void)teardown;

@end

NS_ASSUME_NONNULL_END
