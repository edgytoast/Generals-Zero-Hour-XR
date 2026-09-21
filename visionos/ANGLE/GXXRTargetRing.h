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
// GPU-GPU synchronisation, per frame and slot:
//   GL side    -> beginFrame (waits until the compositor released this slot)
//                 ... GL rendering ...
//                 endGLWork: eglCreateSync(EGL_SYNC_METAL_SHARED_EVENT_ANGLE, glEvent, n) + glFlush
//                 (falls back to glFinish when the extension is missing or forced off)
//   Metal side -> encodeWaitForGLInto: encodeWaitForEvent(glEvent, n) on the compositor command buffer,
//                 ... composite passes that sample the textures ...
//                 encodeReleaseInto: encodeSignalEvent(releaseEvent, m); the next beginFrame that
//                 reuses the slot waits for it on the CPU.
//
// All methods except -encodeWaitForGLInto: / -encodeReleaseInto: must run on the thread that owns
// the ANGLE context, with the context current.
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
/// Slot chosen by the last successful -beginFrame.
@property(nonatomic, readonly) NSUInteger currentSlot;
/// CPU milliseconds the last frame spent waiting (slot release wait + glFinish fallback).
@property(nonatomic, readonly) double lastSyncWaitMs;
/// Times a slot-release wait timed out (compositor stalled); frames continue regardless.
@property(nonatomic, readonly) uint64_t releaseTimeouts;
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

/// Acquires the next slot: waits (CPU) until the compositor finished reading it, (re)allocates
/// targets whose size changed, imports new textures. Returns NO if allocation failed.
- (BOOL)beginFrame;

/// The struct passed to d3d8gles_SetXRHostTargets for the slot acquired by -beginFrame.
- (void)fillTargets:(struct D3D8GLES_XRTargets*)outTargets;

/// Current slot's texture / GL name for a target; nil / 0 when the target is disabled.
- (nullable id<MTLTexture>)textureForTarget:(int)target;
- (unsigned)glTextureForTarget:(int)target;
- (int)widthForTarget:(int)target;
- (int)heightForTarget:(int)target;

/// glFlush + signal (or glFinish). Call after the GL frame, before encoding the composite.
- (void)endGLWork;
/// Encodes the GPU wait for the GL work of the current slot on the compositor command buffer.
- (void)encodeWaitForGLInto:(id<MTLCommandBuffer>)commandBuffer;
/// Encodes the "slot may be rewritten" signal; call after the last composite pass sampling the slot.
- (void)encodeReleaseInto:(id<MTLCommandBuffer>)commandBuffer;

/// Waits for every outstanding composite (CPU). Used before resizing and at teardown.
- (void)drain;
/// Deletes every GL texture / sync and drops the MTLTextures. The ANGLE context must be current.
- (void)teardown;

@end

NS_ASSUME_NONNULL_END
