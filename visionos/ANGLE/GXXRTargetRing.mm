// GXXRTargetRing.mm - see header. Objective-C++ with ARC.
#import "GXXRTargetRing.h"

#import <QuartzCore/QuartzCore.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

#define GXXR_LOG(fmt, ...) fprintf(stderr, "[GXXR/ring] " fmt "\n", ##__VA_ARGS__)

namespace {
constexpr NSUInteger kMaxSlots = 4;
constexpr int kTargets = D3D8GLES_XRT_COUNT;
constexpr const char* kTargetNames[kTargets] = {"stereoLeft", "stereoRight", "game", "world", "ui"};
constexpr int kSlotWaitMs = 500;   // -beginFrame waits this long for a free slot, then skips the frame
constexpr int kDrainWaitMs = 3000; // -drain waits this long for the compositor, then proceeds (logged)
}  // namespace

@implementation GXXRTargetRing {
    GXXRANGLEContext* _ctx;
    id<MTLDevice> _device;
    EGLDisplay _dpy;
    NSUInteger _slotCount;
    MTLPixelFormat _format;

    PFNEGLCREATEIMAGEKHRPROC _createImage;
    PFNEGLDESTROYIMAGEKHRPROC _destroyImage;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC _targetTexture;

    int _reqW[kTargets];
    int _reqH[kTargets];
    int _curW[kTargets];
    int _curH[kTargets];
    id<MTLTexture> _tex[kMaxSlots][kTargets];
    GLuint _gl[kMaxSlots][kTargets];

    id<MTLSharedEvent> _glEvent;       // signalled by ANGLE when a slot's GL work is done
    uint64_t _glValue;                 // engine thread only
    EGLSync _slotSync[kMaxSlots];      // engine thread only

    // Slot ownership (see the header): reference counts and the per-slot GL event value are shared between
    // the engine thread and the compositor thread and guarded by _lock.
    std::mutex _lock;
    std::condition_variable _freed;
    int _refs[kMaxSlots];
    uint64_t _slotGLValue[kMaxSlots];

    NSUInteger _next;
    NSUInteger _current;
    BOOL _frameOpen;
    BOOL _sharedEvents;
    double _syncWaitMs;
    uint64_t _timeouts;
    uint64_t _bytes;
}

@synthesize forceGLFinish = _forceGLFinish;
@synthesize atlas = _atlas;

- (NSUInteger)slotCount { return _slotCount; }
- (MTLPixelFormat)pixelFormat { return _format; }
- (BOOL)usesSharedEventSync { return _sharedEvents && !_forceGLFinish; }
- (NSUInteger)currentSlot { return _current; }
- (double)lastSyncWaitMs { return _syncWaitMs; }
- (uint64_t)releaseTimeouts { return _timeouts; }
- (NSUInteger)slotsInUse {
    std::lock_guard<std::mutex> lock(_lock);
    NSUInteger n = 0;
    for (NSUInteger s = 0; s < _slotCount; ++s) n += _refs[s] > 0 ? 1 : 0;
    return n;
}
- (uint64_t)allocatedBytes { return _bytes; }

static NSUInteger BytesPerPixel(MTLPixelFormat) { return 4; }

- (nullable instancetype)initWithContext:(GXXRANGLEContext*)context
                             pixelFormat:(MTLPixelFormat)preferredFormat
                               slotCount:(NSUInteger)slotCount {
    self = [super init];
    if (!self) return nil;
    _ctx = context;
    _device = context.metalDevice;
    _dpy = (EGLDisplay)context.eglDisplay;
    _slotCount = MIN(MAX(slotCount, (NSUInteger)2), kMaxSlots);
    _createImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    _destroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    _targetTexture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!_createImage || !_destroyImage || !_targetTexture) {
        GXXR_LOG("EGLImage entry points missing");
        return nil;
    }
    for (int t = 0; t < kTargets; ++t) _reqW[t] = _reqH[t] = _curW[t] = _curH[t] = 0;
    for (NSUInteger s = 0; s < kMaxSlots; ++s) {
        _slotSync[s] = EGL_NO_SYNC;
        _slotGLValue[s] = 0;
        _refs[s] = 0;
        for (int t = 0; t < kTargets; ++t) _gl[s][t] = 0;
    }

    // Pick the pixel format: preferred first, then the other 8-bit format.
    const MTLPixelFormat candidates[2] = {preferredFormat, preferredFormat == MTLPixelFormatRGBA8Unorm ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm};
    _format = MTLPixelFormatInvalid;
    for (MTLPixelFormat f : candidates) {
        id<MTLTexture> probe = nil;
        GLuint probeGL = 0;
        const BOOL ok = [self createTextureWidth:16 height:16 format:f label:@"probe" texture:&probe glName:&probeGL];
        if (probeGL) glDeleteTextures(1, &probeGL);
        GXXR_LOG("probe target format %s: %s", f == MTLPixelFormatRGBA8Unorm ? "RGBA8Unorm" : "BGRA8Unorm", ok ? "framebuffer complete" : "REJECTED");
        if (ok) {
            _format = f;
            break;
        }
    }
    if (_format == MTLPixelFormatInvalid) {
        GXXR_LOG("no usable target format");
        return nil;
    }

    _glEvent = [_device newSharedEvent];
    _sharedEvents = context.supportsSharedEventSync && _glEvent;
    GXXR_LOG("ring: %lu slots, format %s, sync=%s", (unsigned long)_slotCount, _format == MTLPixelFormatRGBA8Unorm ? "RGBA8Unorm" : "BGRA8Unorm",
             _sharedEvents ? "EGL_ANGLE_metal_shared_event_sync" : "glFinish (fallback: extension unavailable)");
    return self;
}

- (void)dealloc {
    // teardown must have run with the context current; nothing GL is touched here.
}

#pragma mark - Allocation

/// Creates an MTLTexture on ANGLE's device and imports it as a GL texture name. Returns whether
/// the texture is framebuffer-complete as a color attachment.
- (BOOL)createTextureWidth:(int)w height:(int)h format:(MTLPixelFormat)format label:(NSString*)label
                   texture:(id<MTLTexture> _Nullable __autoreleasing*)outTex glName:(GLuint*)outGL {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;  // NEVER glReadPixels these (see docs/visionos-shell.md)
    id<MTLTexture> tex = [_device newTextureWithDescriptor:d];
    if (!tex) return NO;
    tex.label = label;
    const EGLint attribs[] = {EGL_NONE};
    EGLImageKHR image = _createImage(_dpy, EGL_NO_CONTEXT, EGL_METAL_TEXTURE_ANGLE, (__bridge EGLClientBuffer)tex, attribs);
    if (image == EGL_NO_IMAGE_KHR) {
        GXXR_LOG("eglCreateImageKHR failed for %s: 0x%x", label.UTF8String, eglGetError());
        return NO;
    }
    GLuint name = 0;
    glGenTextures(1, &name);
    glBindTexture(GL_TEXTURE_2D, name);
    _targetTexture(GL_TEXTURE_2D, (GLeglImageOES)image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    _destroyImage(_dpy, image);  // the GL texture keeps its own reference to the MTLTexture
    glBindTexture(GL_TEXTURE_2D, 0);

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, name, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    *outTex = tex;
    *outGL = name;
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        GXXR_LOG("imported %s is NOT framebuffer complete (0x%x)", label.UTF8String, status);
        return NO;
    }
    return YES;
}

- (void)releaseTarget:(int)t {
    for (NSUInteger s = 0; s < _slotCount; ++s) {
        if (_gl[s][t]) {
            glDeleteTextures(1, &_gl[s][t]);
            _gl[s][t] = 0;
        }
        _tex[s][t] = nil;
    }
    if (_curW[t] > 0) _bytes -= (uint64_t)_curW[t] * (uint64_t)_curH[t] * BytesPerPixel(_format) * _slotCount;
    _curW[t] = _curH[t] = 0;
}

- (BOOL)allocateTarget:(int)t {
    const int w = _reqW[t], h = _reqH[t];
    for (NSUInteger s = 0; s < _slotCount; ++s) {
        NSString* label = [NSString stringWithFormat:@"GXXR ring slot %lu %s %dx%d", (unsigned long)s, kTargetNames[t], w, h];
        id<MTLTexture> tex = nil;
        GLuint name = 0;
        const BOOL ok = [self createTextureWidth:w height:h format:_format label:label texture:&tex glName:&name];
        _tex[s][t] = tex;
        _gl[s][t] = name;
        if (!ok || !tex || !name) {
            GXXR_LOG("allocating %s (%dx%d) slot %lu failed", kTargetNames[t], w, h, (unsigned long)s);
            [self releaseTarget:t];
            return NO;
        }
    }
    _curW[t] = w;
    _curH[t] = h;
    _bytes += (uint64_t)w * (uint64_t)h * BytesPerPixel(_format) * _slotCount;
    {
        char names[128] = {};
        size_t used = 0;
        for (NSUInteger s = 0; s < _slotCount && used < sizeof(names); ++s)
            used += (size_t)snprintf(names + used, sizeof(names) - used, "%s%u", s ? "," : "", _gl[s][t]);
        GXXR_LOG("target %s GL names per slot: %s", kTargetNames[t], names);
    }
    GXXR_LOG("target %s: %dx%d x%lu slots (%.1f MB)", kTargetNames[t], w, h, (unsigned long)_slotCount,
             (double)w * h * BytesPerPixel(_format) * _slotCount / (1024.0 * 1024.0));
    return YES;
}

- (void)setSizeWidth:(int)width height:(int)height forTarget:(int)target {
    if (target < 0 || target >= kTargets) return;
    _reqW[target] = width > 0 && height > 0 ? width : 0;
    _reqH[target] = width > 0 && height > 0 ? height : 0;
}

- (void)configureStereoEyeWidth:(int)width height:(int)height eyeCount:(int)eyeCount {
    if (eyeCount < 1) {
        [self setSizeWidth:0 height:0 forTarget:D3D8GLES_XRT_STEREO_LEFT];
        [self setSizeWidth:0 height:0 forTarget:D3D8GLES_XRT_STEREO_RIGHT];
        return;
    }
    // The engine always renders BOTH eyes (d3d8gles_BeginXRStereo takes two clips), whatever the display has, so both
    // eye targets exist even when the drawable reports one view (the visionOS simulator). d3d8gles only uses host
    // targets when both are supplied; with the right eye missing it silently fell back to its own private textures and
    // the texture the compositor samples stayed empty (found 22/09/2026: the tabletop was invisible in the simulator).
    (void)eyeCount;
    if (_atlas) {
        [self setSizeWidth:width * 2 height:height forTarget:D3D8GLES_XRT_STEREO_LEFT];
        [self setSizeWidth:0 height:0 forTarget:D3D8GLES_XRT_STEREO_RIGHT];
    } else {
        [self setSizeWidth:width height:height forTarget:D3D8GLES_XRT_STEREO_LEFT];
        [self setSizeWidth:width height:height forTarget:D3D8GLES_XRT_STEREO_RIGHT];
    }
}

#pragma mark - Frame protocol

- (void)destroySyncForSlot:(NSUInteger)s {
    if (_slotSync[s] != EGL_NO_SYNC) {
        eglDestroySync(_dpy, _slotSync[s]);
        _slotSync[s] = EGL_NO_SYNC;
    }
}

- (BOOL)needsResize {
    for (int t = 0; t < kTargets; ++t)
        if (_reqW[t] != _curW[t] || _reqH[t] != _curH[t]) return YES;
    return NO;
}

/// Waits until every slot's reference count is 0 (composites finished). Returns NO on timeout.
- (BOOL)waitAllFreeTimeoutMs:(int)ms {
    std::unique_lock<std::mutex> lock(_lock);
    return _freed.wait_for(lock, std::chrono::milliseconds(ms), [&] {
        for (NSUInteger s = 0; s < _slotCount; ++s)
            if (_refs[s] > 0) return false;
        return true;
    });
}

- (void)drain {
    if (![self waitAllFreeTimeoutMs:kDrainWaitMs]) {
        GXXR_LOG("drain: %lu slot(s) still referenced after %d ms (compositor stalled); continuing", (unsigned long)self.slotsInUse, kDrainWaitMs);
    }
    if (!_sharedEvents || _forceGLFinish) return;
    // GL work of every slot must be finished before textures are deleted.
    glFinish();
}

- (NSInteger)beginFrame {
    _frameOpen = NO;
    _syncWaitMs = 0;
    const CFTimeInterval t0 = CACurrentMediaTime();

    // Size changes: drain everything in flight, then rebuild the affected targets.
    if ([self needsResize]) {
        [self drain];
        for (NSUInteger s = 0; s < _slotCount; ++s) [self destroySyncForSlot:s];
        for (int t = 0; t < kTargets; ++t) {
            if (_reqW[t] == _curW[t] && _reqH[t] == _curH[t]) continue;
            [self releaseTarget:t];
            if (_reqW[t] > 0 && ![self allocateTarget:t]) {
                _reqW[t] = _reqH[t] = 0;
                return -1;
            }
        }
    }

    // A free slot: reference count 0. Round-robin from the slot after the last one, so consecutive frames
    // rotate through the ring (and the compositor's older slot is not touched first).
    NSInteger picked = -1;
    {
        std::unique_lock<std::mutex> lock(_lock);
        const auto pick = [&]() -> NSInteger {
            for (NSUInteger i = 0; i < _slotCount; ++i) {
                const NSUInteger s = (_next + i) % _slotCount;
                if (_refs[s] == 0) return (NSInteger)s;
            }
            return -1;
        };
        if (!_freed.wait_for(lock, std::chrono::milliseconds(kSlotWaitMs), [&] { return pick() >= 0; })) {
            _timeouts++;
            lock.unlock();
            GXXR_LOG("no free ring slot within %d ms (%lu in use); skipping this engine frame", kSlotWaitMs, (unsigned long)self.slotsInUse);
            _syncWaitMs = (CACurrentMediaTime() - t0) * 1000.0;
            return -1;
        }
        picked = pick();
        _refs[picked] = 1;  // the writer reference
        _next = (NSUInteger)picked + 1;
    }
    [self destroySyncForSlot:(NSUInteger)picked];
    _current = (NSUInteger)picked;
    _frameOpen = YES;
    _syncWaitMs = (CACurrentMediaTime() - t0) * 1000.0;
    return picked;
}

- (void)abortFrame {
    if (!_frameOpen) return;
    _frameOpen = NO;
    [self releaseSlot:_current];
}

- (void)retainSlot:(NSUInteger)slot {
    if (slot >= _slotCount) return;
    std::lock_guard<std::mutex> lock(_lock);
    _refs[slot]++;
}

- (void)releaseSlot:(NSUInteger)slot {
    if (slot >= _slotCount) return;
    {
        std::lock_guard<std::mutex> lock(_lock);
        if (_refs[slot] > 0) _refs[slot]--;
    }
    _freed.notify_all();
}

- (void)releaseSlot:(NSUInteger)slot afterCommandBuffer:(id<MTLCommandBuffer>)cb {
    // The block retains self (the ring outlives every command buffer: teardown drains first).
    [cb addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull) {
        [self releaseSlot:slot];
    }];
}

- (void)fillTargets:(struct D3D8GLES_XRTargets*)out {
    if (!out) return;
    *out = {};
    const NSUInteger s = _current;
    for (int t = 0; t < kTargets; ++t) {
        out->slot[t].glTexture = _curW[t] > 0 ? _gl[s][t] : 0;
        out->slot[t].width = _curW[t];
        out->slot[t].height = _curH[t];
    }
    out->atlas = _atlas ? 1 : 0;
    if (_atlas && _curW[D3D8GLES_XRT_STEREO_LEFT] > 0) {
        const int w = _curW[D3D8GLES_XRT_STEREO_LEFT], h = _curH[D3D8GLES_XRT_STEREO_LEFT];
        const int eyeW = w / 2 > 0 ? w / 2 : w;
        out->eyeRect[0][0] = 0; out->eyeRect[0][1] = 0; out->eyeRect[0][2] = eyeW; out->eyeRect[0][3] = h;
        out->eyeRect[1][0] = eyeW; out->eyeRect[1][1] = 0; out->eyeRect[1][2] = eyeW; out->eyeRect[1][3] = h;
    } else {
        for (int e = 0; e < 2; ++e) {
            out->eyeRect[e][0] = 0;
            out->eyeRect[e][1] = 0;
            out->eyeRect[e][2] = _curW[e];
            out->eyeRect[e][3] = _curH[e];
        }
    }
}

- (nullable id<MTLTexture>)textureForTarget:(int)t {
    return (t >= 0 && t < kTargets && _curW[t] > 0) ? _tex[_current][t] : nil;
}
- (nullable id<MTLTexture>)textureForTarget:(int)t slot:(NSUInteger)slot {
    return (t >= 0 && t < kTargets && slot < _slotCount && _curW[t] > 0) ? _tex[slot][t] : nil;
}
- (unsigned)glTextureForTarget:(int)t { return (t >= 0 && t < kTargets && _curW[t] > 0) ? _gl[_current][t] : 0; }
- (int)widthForTarget:(int)t { return (t >= 0 && t < kTargets) ? _curW[t] : 0; }
- (int)heightForTarget:(int)t { return (t >= 0 && t < kTargets) ? _curH[t] : 0; }

- (void)endGLWork {
    if (!_frameOpen) return;
    const NSUInteger s = _current;
    if (_sharedEvents && !_forceGLFinish) {
        const uint64_t value = ++_glValue;
        const EGLAttrib attribs[] = {EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE, (EGLAttrib)(__bridge void*)_glEvent,
                                     EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE, (EGLAttrib)(value & 0xFFFFFFFFu),
                                     EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_ANGLE, (EGLAttrib)(value >> 32), EGL_NONE};
        EGLSync sync = eglCreateSync(_dpy, EGL_SYNC_METAL_SHARED_EVENT_ANGLE, attribs);
        if (sync == EGL_NO_SYNC) {
            GXXR_LOG("eglCreateSync(metal shared event) failed: 0x%x; falling back to glFinish", eglGetError());
            _sharedEvents = NO;
            _glValue--;
        } else {
            _slotSync[s] = sync;  // kept alive until the slot is acquired again
            {
                std::lock_guard<std::mutex> lock(_lock);
                _slotGLValue[s] = value;
            }
            glFlush();
            return;
        }
    }
    const CFTimeInterval t0 = CACurrentMediaTime();
    glFinish();
    _syncWaitMs += (CACurrentMediaTime() - t0) * 1000.0;
}

- (void)encodeWaitForGLSlot:(NSUInteger)slot into:(id<MTLCommandBuffer>)cb {
    if (slot >= _slotCount || !(_sharedEvents && !_forceGLFinish)) return;
    uint64_t value;
    {
        std::lock_guard<std::mutex> lock(_lock);
        value = _slotGLValue[slot];
    }
    if (value == 0) return;
    [cb encodeWaitForEvent:_glEvent value:value];
}

#pragma mark - Teardown

- (void)teardown {
    if (!_ctx) return;
    [self drain];
    for (NSUInteger s = 0; s < _slotCount; ++s) [self destroySyncForSlot:s];
    for (int t = 0; t < kTargets; ++t) {
        [self releaseTarget:t];
        _reqW[t] = _reqH[t] = 0;
    }
    _frameOpen = NO;
    {
        std::lock_guard<std::mutex> lock(_lock);
        for (NSUInteger s = 0; s < _slotCount; ++s) _refs[s] = 0;
    }
    GXXR_LOG("ring torn down (%llu no-free-slot timeouts over its life)", (unsigned long long)_timeouts);
}

@end
