// GXXRANGLEContext.h - the process-wide ANGLE (GLES 3.0 on Metal) display and context.
//
// One EGLDisplay + one surfaceless ES 3.0 EGLContext are created on first use and live for
// the rest of the process (the engine's singletons are not restart-safe, so its GL objects
// must survive immersive-space close/open). Only ONE thread may have the context current at
// a time; the compositor render thread makes it current when its frame loop starts and
// releases it when the loop exits, so the next loop (a re-opened immersive space) can take it
// over with every GL object still alive.
//
// This header deliberately exposes only Foundation/Metal types and opaque pointers so it can
// be imported without the EGL/GLES headers.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN

@interface GXXRANGLEContext : NSObject

/// Creates the display and context on first call (blocking, thread-safe). Returns nil, and logs
/// why, when ANGLE cannot provide a Metal-backed ES 3.0 context.
+ (nullable GXXRANGLEContext*)sharedContext;

/// ANGLE's own MTLDevice (eglQueryDeviceAttribEXT(EGL_METAL_DEVICE_ANGLE)). Every host
/// texture imported into GL must be created from this device.
@property(nonatomic, readonly) id<MTLDevice> metalDevice;

/// Opaque EGLDisplay / EGLContext, for D3D8GLES_XRConfig.eglDisplay / eglContext.
@property(nonatomic, readonly) void* eglDisplay;
@property(nonatomic, readonly) void* eglContext;

/// eglGetProcAddress: the resolver handed to the engine (D3D8GLES_XRConfig.getProcAddress).
@property(nonatomic, readonly) void* _Nullable (*_Nonnull getProcAddress)(const char* _Nonnull);

/// glGetString(GL_RENDERER) / GL_VERSION / EGL_VERSION, captured when the context was created.
@property(nonatomic, readonly, copy) NSString* rendererString;
@property(nonatomic, readonly, copy) NSString* versionString;

/// EGL_ANGLE_metal_shared_event_sync (GPU-GPU fence with an MTLSharedEvent) is available.
@property(nonatomic, readonly) BOOL supportsSharedEventSync;
/// EGL_ANGLE_metal_texture_client_buffer + GL_OES_EGL_image (MTLTexture import) is available.
@property(nonatomic, readonly) BOOL supportsMetalTextureImport;

/// Makes the context current on the calling thread (no-op when it already is). Returns NO on failure.
- (BOOL)makeCurrent;
/// Releases the context from the calling thread. Call before a render thread exits.
- (void)releaseCurrent;
/// YES when the calling thread currently owns the context.
@property(nonatomic, readonly) BOOL isCurrentOnThisThread;

/// Compares ANGLE's device with the Compositor Services device and logs the result.
/// Returns YES when they are the same MTLDevice (same object or same registryID).
- (BOOL)checkDeviceMatchesCompositorDevice:(id<MTLDevice>)compositorDevice;

@end

NS_ASSUME_NONNULL_END
