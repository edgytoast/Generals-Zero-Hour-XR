// GXXRANGLEContext.mm - see header. Objective-C++ with ARC.
#import "GXXRANGLEContext.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <cstdio>
#include <string>

namespace {

bool HasExt(const char* list, const char* name) {
    if (!list) return false;
    const std::string l = std::string(" ") + list + " ";
    return l.find(std::string(" ") + name + " ") != std::string::npos;
}

// stderr is what `simctl launch --console-pty` captures; os_log would redact %s arguments.
#define GXXR_LOG(fmt, ...) fprintf(stderr, "[GXXR/ANGLE] " fmt "\n", ##__VA_ARGS__)

}  // namespace

@implementation GXXRANGLEContext {
    EGLDisplay _display;
    EGLContext _context;
    EGLConfig _config;
    id<MTLDevice> _device;
    NSString* _renderer;
    NSString* _version;
    BOOL _sharedEventSync;
    BOOL _metalTextureImport;
}

+ (nullable GXXRANGLEContext*)sharedContext {
    static GXXRANGLEContext* instance = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        GXXRANGLEContext* c = [[GXXRANGLEContext alloc] init];
        instance = [c setUp] ? c : nil;
    });
    return instance;
}

- (id<MTLDevice>)metalDevice { return _device; }
- (void*)eglDisplay { return _display; }
- (void*)eglContext { return _context; }
- (void* _Nullable (*_Nonnull)(const char* _Nonnull))getProcAddress {
    return (void* (*)(const char*))eglGetProcAddress;
}
- (NSString*)rendererString { return _renderer ?: @""; }
- (NSString*)versionString { return _version ?: @""; }
- (BOOL)supportsSharedEventSync { return _sharedEventSync; }
- (BOOL)supportsMetalTextureImport { return _metalTextureImport; }
- (BOOL)isCurrentOnThisThread { return eglGetCurrentContext() == _context && _context != EGL_NO_CONTEXT; }

- (BOOL)setUp {
    const char* clientExt = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!HasExt(clientExt, "EGL_ANGLE_platform_angle_metal")) {
        GXXR_LOG("EGL_ANGLE_platform_angle_metal missing from client extensions (%s)", clientExt ? clientExt : "null");
        return NO;
    }
    auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!getPlatformDisplay) {
        GXXR_LOG("eglGetPlatformDisplayEXT unavailable");
        return NO;
    }
    const EGLint displayAttribs[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE};
    _display = getPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, (void*)EGL_DEFAULT_DISPLAY, displayAttribs);
    if (_display == EGL_NO_DISPLAY) {
        GXXR_LOG("eglGetPlatformDisplayEXT(METAL_ANGLE) failed: 0x%x", eglGetError());
        return NO;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(_display, &major, &minor)) {
        GXXR_LOG("eglInitialize failed: 0x%x", eglGetError());
        return NO;
    }
    const char* dpyExt = eglQueryString(_display, EGL_EXTENSIONS);
    _sharedEventSync = HasExt(dpyExt, "EGL_ANGLE_metal_shared_event_sync");
    _metalTextureImport = HasExt(dpyExt, "EGL_ANGLE_metal_texture_client_buffer") && HasExt(dpyExt, "EGL_KHR_image_base");
    const bool surfaceless = HasExt(dpyExt, "EGL_KHR_surfaceless_context");
    GXXR_LOG("EGL %d.%d vendor=%s; shared_event_sync=%d metal_texture_client_buffer=%d surfaceless=%d", major, minor,
             eglQueryString(_display, EGL_VENDOR), (int)_sharedEventSync, (int)_metalTextureImport, (int)surfaceless);
    if (!_metalTextureImport || !surfaceless) {
        GXXR_LOG("required EGL extensions missing (display extensions: %s)", dpyExt ? dpyExt : "null");
        return NO;
    }

    // ANGLE's MTLDevice, through EGL_ANGLE_device_metal.
    auto queryDisplayAttrib = (PFNEGLQUERYDISPLAYATTRIBEXTPROC)eglGetProcAddress("eglQueryDisplayAttribEXT");
    auto queryDeviceAttrib = (PFNEGLQUERYDEVICEATTRIBEXTPROC)eglGetProcAddress("eglQueryDeviceAttribEXT");
    if (queryDisplayAttrib && queryDeviceAttrib) {
        EGLAttrib eglDevice = 0, mtlDevice = 0;
        if (queryDisplayAttrib(_display, EGL_DEVICE_EXT, &eglDevice) &&
            queryDeviceAttrib((EGLDeviceEXT)eglDevice, EGL_METAL_DEVICE_ANGLE, &mtlDevice) && mtlDevice) {
            _device = (__bridge id<MTLDevice>)(void*)mtlDevice;
        }
    }
    if (!_device) {
        GXXR_LOG("could not query ANGLE's MTLDevice (EGL_ANGLE_device_metal)");
        return NO;
    }

    const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLint numConfigs = 0;
    if (!eglChooseConfig(_display, configAttribs, &_config, 1, &numConfigs) || numConfigs < 1) {
        GXXR_LOG("eglChooseConfig failed: 0x%x", eglGetError());
        return NO;
    }
    const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    _context = eglCreateContext(_display, _config, EGL_NO_CONTEXT, contextAttribs);
    if (_context == EGL_NO_CONTEXT) {
        GXXR_LOG("eglCreateContext(ES 3.0) failed: 0x%x", eglGetError());
        return NO;
    }
    // Make current once on this thread to read the strings, then release again so the caller
    // decides which thread owns the context.
    if (!eglMakeCurrent(_display, EGL_NO_SURFACE, EGL_NO_SURFACE, _context)) {
        GXXR_LOG("eglMakeCurrent(surfaceless) failed: 0x%x", eglGetError());
        return NO;
    }
    _renderer = [NSString stringWithUTF8String:(const char*)glGetString(GL_RENDERER) ?: ""];
    _version = [NSString stringWithFormat:@"%s | EGL %d.%d",
                (const char*)glGetString(GL_VERSION) ?: "", major, minor];
    const char* glExt = (const char*)glGetString(GL_EXTENSIONS);
    const bool eglImage = HasExt(glExt, "GL_OES_EGL_image");
    GXXR_LOG("GL_RENDERER=%s", _renderer.UTF8String);
    GXXR_LOG("GL_VERSION=%s", _version.UTF8String);
    GXXR_LOG("ANGLE MTLDevice: %s (registryID 0x%llx); GL_OES_EGL_image=%d", _device.name.UTF8String,
             (unsigned long long)_device.registryID, (int)eglImage);
    eglMakeCurrent(_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (!eglImage) {
        GXXR_LOG("GL_OES_EGL_image is required for host texture import");
        return NO;
    }
    return YES;
}

- (BOOL)makeCurrent {
    if (self.isCurrentOnThisThread) return YES;
    if (!eglMakeCurrent(_display, EGL_NO_SURFACE, EGL_NO_SURFACE, _context)) {
        GXXR_LOG("eglMakeCurrent failed on %s: 0x%x", [NSThread currentThread].name.UTF8String ?: "thread", eglGetError());
        return NO;
    }
    return YES;
}

- (void)releaseCurrent {
    if (self.isCurrentOnThisThread) eglMakeCurrent(_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

- (BOOL)checkDeviceMatchesCompositorDevice:(id<MTLDevice>)compositorDevice {
    const BOOL same = (compositorDevice == _device) || (compositorDevice.registryID == _device.registryID);
    GXXR_LOG("device check: compositor=\"%s\" (0x%llx) angle=\"%s\" (0x%llx) -> %s", compositorDevice.name.UTF8String,
             (unsigned long long)compositorDevice.registryID, _device.name.UTF8String,
             (unsigned long long)_device.registryID, same ? "SAME DEVICE" : "DIFFERENT DEVICES");
    return same;
}

@end
