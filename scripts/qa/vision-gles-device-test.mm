// GeneralsX @test visionOS port - device-level test of the native d3d8gles backend running on
// ANGLE's Metal backend with host-supplied render targets.
//
// This is the Objective-C++ host half: it owns the ANGLE EGL display, a surfaceless ES 3.0
// context, and a set of Private-storage MTLTextures created on ANGLE's own MTLDevice that it wraps
// as GL textures through EGL_ANGLE_metal_texture_client_buffer (eglCreateImageKHR +
// glEGLImageTargetTexture2DOES) -- exactly the plumbing the visionOS host performs each frame.
// The cases themselves (D3D8 API + d3d8gles_* calls) live in vision-gles-device-cases.cpp.
//
// Rules this file follows (see docs/visionos-gles-backend.md):
//   * host MTLTextures come from eglQueryDeviceAttribEXT(EGL_METAL_DEVICE_ANGLE);
//   * they are MTLStorageModePrivate (what Compositor Services hands out) and are NEVER read
//     with glReadPixels: every readback is a Metal blit into a Shared buffer;
//   * GPU-to-GPU synchronisation uses EGL_ANGLE_metal_shared_event_sync.
//
// Build/run: scripts/qa/vision-gles-device-test.sh
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <TargetConditionals.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <GLES2/gl2ext_angle.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "vision-gles-harness.h"

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static id<MTLDevice> g_dev = nil;
static id<MTLCommandQueue> g_queue = nil;
static id<MTLSharedEvent> g_event = nil;
static uint64_t g_eventValue = 0;
static PFNEGLCREATEIMAGEKHRPROC g_createImage = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC g_destroyImage = nullptr;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_targetTex = nullptr;
static std::vector<id<MTLTexture>> g_keepAlive;

static unsigned g_bindFboCalls = 0, g_drawCalls = 0;
typedef void (GL_APIENTRY *PFN_BindFramebuffer)(GLenum, GLuint);
typedef void (GL_APIENTRY *PFN_DrawElements)(GLenum, GLsizei, GLenum, const void *);
typedef void (GL_APIENTRY *PFN_DrawArrays)(GLenum, GLint, GLsizei);
static PFN_BindFramebuffer g_realBindFramebuffer = nullptr;
static PFN_DrawElements g_realDrawElements = nullptr;
static PFN_DrawArrays g_realDrawArrays = nullptr;
static void GL_APIENTRY countedBindFramebuffer(GLenum t, GLuint f) { ++g_bindFboCalls; g_realBindFramebuffer(t, f); }
static void GL_APIENTRY countedDrawElements(GLenum m, GLsizei c, GLenum ty, const void *i) { ++g_drawCalls; g_realDrawElements(m, c, ty, i); }
static void GL_APIENTRY countedDrawArrays(GLenum m, GLint f, GLsizei c) { ++g_drawCalls; g_realDrawArrays(m, f, c); }

// The resolver the backend receives. Everything is forwarded to eglGetProcAddress; the three
// counted entry points are interposed so the cases can assert how the backend uses them.
static void *hostGetProcAddress(const char *name)
{
	void *real = (void *)eglGetProcAddress(name);
	if (!strcmp(name, "glBindFramebuffer")) { g_realBindFramebuffer = (PFN_BindFramebuffer)real; return (void *)&countedBindFramebuffer; }
	if (!strcmp(name, "glDrawElements")) { g_realDrawElements = (PFN_DrawElements)real; return (void *)&countedDrawElements; }
	if (!strcmp(name, "glDrawArrays")) { g_realDrawArrays = (PFN_DrawArrays)real; return (void *)&countedDrawArrays; }
	return real;
}

static bool makeTexture(int w, int h, HostTex *out)
{
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
	                                                                               width:w height:h mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModePrivate; // what Compositor Services drawables are
	id<MTLTexture> tex = [g_dev newTextureWithDescriptor:td];
	if (!tex) return false;
	g_keepAlive.push_back(tex);
	EGLImageKHR img = g_createImage(g_dpy, EGL_NO_CONTEXT, EGL_METAL_TEXTURE_ANGLE, (__bridge EGLClientBuffer)tex, nullptr);
	if (img == EGL_NO_IMAGE_KHR) { printf("  eglCreateImageKHR failed 0x%x\n", eglGetError()); return false; }
	GLuint name = 0;
	glGenTextures(1, &name);
	glBindTexture(GL_TEXTURE_2D, name);
	g_targetTex(GL_TEXTURE_2D, (GLeglImageOES)img);
	g_destroyImage(g_dpy, img); // the GL texture holds its own reference
	glBindTexture(GL_TEXTURE_2D, 0);
	out->gl = name; out->width = w; out->height = h; out->mtl = (__bridge void *)tex;
	return glGetError() == GL_NO_ERROR;
}

static bool readTexture(const HostTex &t, int x, int y, int w, int h, std::vector<uint8_t> *rgba)
{
	id<MTLTexture> tex = (__bridge id<MTLTexture>)t.mtl;
	const size_t bpr = (size_t)w * 4;
	id<MTLBuffer> buf = [g_dev newBufferWithLength:bpr * h options:MTLResourceStorageModeShared];
	id<MTLCommandBuffer> cb = [g_queue commandBuffer];
	id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
	[be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(w, h, 1)
	           toBuffer:buf destinationOffset:0 destinationBytesPerRow:bpr destinationBytesPerImage:bpr * h];
	[be endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	if (cb.status != MTLCommandBufferStatusCompleted) return false;
	rgba->assign((const uint8_t *)buf.contents, (const uint8_t *)buf.contents + bpr * h);
	return true;
}

static void fillTexture(const HostTex &t, uint8_t value)
{
	id<MTLTexture> tex = (__bridge id<MTLTexture>)t.mtl;
	const size_t bpr = (size_t)t.width * 4;
	id<MTLBuffer> buf = [g_dev newBufferWithLength:bpr * t.height options:MTLResourceStorageModeShared];
	memset(buf.contents, value, bpr * t.height);
	id<MTLCommandBuffer> cb = [g_queue commandBuffer];
	id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
	[be copyFromBuffer:buf sourceOffset:0 sourceBytesPerRow:bpr sourceBytesPerImage:bpr * t.height sourceSize:MTLSizeMake(t.width, t.height, 1)
	         toTexture:tex destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
	[be endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
}

static bool finishFrame()
{
	// glFlush + a signalled shared event, exactly the frame protocol of the host.
	const uint64_t v = ++g_eventValue;
	EGLAttrib attrs[] = {
		EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE, (EGLAttrib)(__bridge void *)g_event,
		EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE, (EGLAttrib)(v & 0xFFFFFFFFu),
		EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_ANGLE, (EGLAttrib)(v >> 32),
		EGL_NONE };
	EGLSync sync = eglCreateSync(g_dpy, EGL_SYNC_METAL_SHARED_EVENT_ANGLE, attrs);
	if (sync == EGL_NO_SYNC) { printf("  eglCreateSync(METAL_SHARED_EVENT) failed 0x%x\n", eglGetError()); return false; }
	glFlush();
	const bool ok = [g_event waitUntilSignaledValue:v timeoutMS:10000];
	eglDestroySync(g_dpy, sync);
	return ok;
}

static bool readGLTexture(unsigned glTexture, int w, int h, std::vector<uint8_t> *rgba)
{
	GLint prevFbo = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTexture, 0);
	bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	if (ok) {
		rgba->assign((size_t)w * h * 4, 0);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba->data());
		ok = glGetError() == GL_NO_ERROR;
		// Rows stay in GL order (row 0 == GL window y 0); the cases index them that way.
	}
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
	glDeleteFramebuffers(1, &fbo);
	return ok;
}

static unsigned bindFramebufferCalls() { return g_bindFboCalls; }
static unsigned drawCalls() { return g_drawCalls; }
static void resetCounters() { g_bindFboCalls = 0; g_drawCalls = 0; }
static const char *deviceName() { return g_dev ? [[g_dev name] UTF8String] : "(none)"; }
static const char *glStringFn(unsigned name) { return (const char *)glGetString((GLenum)name); }

// ---------------------------------------------------------------------------------------------
// Precision probe. Draws a quad whose interpolated texture coordinate u runs 0..64 across 512
// pixels and writes fract(u) into the red channel (8 bit). If u is interpolated in fp16 (ANGLE
// translating `mediump` to half), fract(u) develops steps far coarser than 1/512 ... 1/8: the
// returned value is the largest |sampled - expected| in units of "u steps per pixel" (0 = exact).
// ---------------------------------------------------------------------------------------------
static GLuint compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, nullptr, log); printf("  shader log: %s\n", log); }
	return s;
}

static double probeVaryingPrecision(bool useHighp)
{
	const int W = 512, H = 4;
	HostTex target;
	if (!makeTexture(W, H, &target)) return -1.0;
	const char *vs = "#version 300 es\nprecision highp float;\nlayout(location=0) in vec2 p;layout(location=1) in vec2 uv;out vec2 vUV;\n"
	                 "void main(){vUV=uv;gl_Position=vec4(p,0.0,1.0);}\n";
	std::string fs = "#version 300 es\nprecision mediump float;\n";
	fs += useHighp ? "in highp vec2 vUV;\n" : "in vec2 vUV;\n";
	fs += "out vec4 o;void main(){o=vec4(fract(vUV.x),0.0,0.0,1.0);}\n";
	GLuint prog = glCreateProgram();
	GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs.c_str());
	glAttachShader(prog, v); glAttachShader(prog, f); glLinkProgram(prog);
	GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked) return -1.0;
	GLuint fbo; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.gl, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return -1.0;
	const float u1 = 64.0f;
	const float verts[] = { -1.f, -1.f, 0.f, 0.f,  1.f, -1.f, u1, 0.f,  -1.f, 1.f, 0.f, 1.f,  1.f, 1.f, u1, 1.f };
	GLuint vao, vbo; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
	glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
	glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
	glViewport(0, 0, W, H); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
	glUseProgram(prog);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	finishFrame();
	std::vector<uint8_t> px;
	readTexture(target, 0, 0, W, 1, &px);
	double worst = 0;
	for (int x = 0; x < W; ++x) {
		const double u = (x + 0.5) / W * u1;
		const double expected = u - floor(u);
		const double got = px[x * 4] / 255.0;
		double d = fabs(got - expected);
		d = fmin(d, 1.0 - d); // wrap-around at the repeat boundary
		worst = fmax(worst, d);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo); glDeleteProgram(prog); glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao);
	return worst; // fraction of a repeat; 1/255 quantisation alone gives <= 0.005
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		setvbuf(stdout, nullptr, _IONBF, 0);
		printf("vision-gles-device-test: d3d8gles on ANGLE-Metal, host-supplied render targets\n");
		g_dev = MTLCreateSystemDefaultDevice();
		printf("MTLCreateSystemDefaultDevice: %s\n", g_dev ? [[g_dev name] UTF8String] : "(none)");
		if (!g_dev) return 2;

		auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
		EGLint dattr[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE };
		g_dpy = getPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, (void *)EGL_DEFAULT_DISPLAY, dattr);
		EGLint maj = 0, min = 0;
		if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, &maj, &min)) { printf("FAIL: EGL display\n"); return 2; }
		auto qDpyAttr = (PFNEGLQUERYDISPLAYATTRIBEXTPROC)eglGetProcAddress("eglQueryDisplayAttribEXT");
		auto qDevAttr = (PFNEGLQUERYDEVICEATTRIBEXTPROC)eglGetProcAddress("eglQueryDeviceAttribEXT");
		EGLAttrib devAttr = 0, mtlPtr = 0;
		if (!qDpyAttr(g_dpy, EGL_DEVICE_EXT, &devAttr) || !qDevAttr((EGLDeviceEXT)devAttr, EGL_METAL_DEVICE_ANGLE, &mtlPtr)) {
			printf("FAIL: cannot query ANGLE's MTLDevice\n"); return 2;
		}
		g_dev = (__bridge id<MTLDevice>)(void *)mtlPtr; // textures MUST come from ANGLE's device
		g_queue = [g_dev newCommandQueue];
		g_event = [g_dev newSharedEvent];
		printf("ANGLE MTLDevice: %s (EGL %d.%d)\n", [[g_dev name] UTF8String], maj, min);

		EGLint cfgAttr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		                     EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
		EGLConfig cfg; EGLint ncfg = 0;
		if (!eglChooseConfig(g_dpy, cfgAttr, &cfg, 1, &ncfg) || ncfg == 0) { printf("FAIL: eglChooseConfig\n"); return 2; }
		EGLint ctxAttr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
		g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
		// Surfaceless: the backend never draws to framebuffer 0 in XR mode.
		if (g_ctx == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_ctx)) {
			printf("FAIL: surfaceless ES3 context (egl 0x%x)\n", eglGetError()); return 2;
		}
		printf("GL_RENDERER: %s\nGL_VERSION: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
		g_createImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
		g_destroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
		g_targetTex = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
		if (!g_createImage || !g_destroyImage || !g_targetTex) { printf("FAIL: EGLImage entry points\n"); return 2; }

		Harness h;
		h.makeTexture = makeTexture;
		h.fillTexture = fillTexture;
		h.readTexture = readTexture;
		h.finishFrame = finishFrame;
		h.readGLTexture = readGLTexture;
		h.getProcAddress = hostGetProcAddress;
		h.bindFramebufferCalls = bindFramebufferCalls;
		h.drawCalls = drawCalls;
		h.resetCounters = resetCounters;
		h.probeVaryingPrecision = probeVaryingPrecision;
		h.deviceName = deviceName;
		h.glString = glStringFn;
#if TARGET_OS_VISION
		h.visionOS = true;
#endif

		int checks = 0;
		const int failures = RunDeviceCases(&h, &checks);
		printf("\n%s: %d check(s), %d failure(s)\n", failures ? "FAILED" : "PASSED", checks, failures);
		return failures ? 1 : 0;
	}
}
