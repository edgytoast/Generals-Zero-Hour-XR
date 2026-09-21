// GeneralsX @test visionOS port - shared interface between the two translation units of the
// d3d8gles-on-ANGLE-Metal device test:
//   vision-gles-device-test.mm     Objective-C++: ANGLE EGL/Metal host, MTLTexture ring, readback
//   vision-gles-device-cases.cpp   C++: the D3D8 API + d3d8gles_* calls under test
// They are separate TUs because the D3D8/Win32 compatibility headers and Objective-C's BOOL
// cannot live in one translation unit. Nothing in this header mentions Metal, EGL or D3D8.
#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>

// A host render target: a GL texture name bound (through an EGLImage) to a Private-storage
// MTLTexture created on ANGLE's own MTLDevice, exactly what the visionOS host builds.
struct HostTex {
	unsigned gl = 0;
	int width = 0, height = 0;
	void *mtl = nullptr; // id<MTLTexture>, retained by the harness
};

struct Harness {
	// Host targets ------------------------------------------------------------------------
	bool (*makeTexture)(int width, int height, HostTex *out) = nullptr;
	// Clears the texture on the Metal side to a known pattern (0xAA fill) so an untouched
	// target is distinguishable from a rendered one.
	void (*fillTexture)(const HostTex &t, uint8_t value) = nullptr;
	// RGBA8 readback of a sub-rectangle through a Metal blit into a Shared buffer. Rows are in
	// Metal texel order: row 0 is the TOP row of the MTLTexture. Never glReadPixels.
	bool (*readTexture)(const HostTex &t, int x, int y, int w, int h, std::vector<uint8_t> *rgba) = nullptr;
	// GPU-to-GPU hand-off of the frame just rendered: eglCreateSync(EGL_SYNC_METAL_SHARED_EVENT)
	// + glFlush, then a CPU wait on the event (the host waits on its own queue instead).
	bool (*finishFrame)() = nullptr;
	// GL access for the tests that must inspect backend-allocated textures (allowed: those are
	// ANGLE's own, not host-wrapped). Reads a whole texture through a scratch FBO.
	bool (*readGLTexture)(unsigned glTexture, int w, int h, std::vector<uint8_t> *rgba) = nullptr;
	// Resolver handed to d3d8gles_SetXRConfig: eglGetProcAddress, but with glBindFramebuffer
	// (and a few draw calls) counted so tests can assert the P6 FBO behaviour.
	void *(*getProcAddress)(const char *) = nullptr;
	unsigned (*bindFramebufferCalls)() = nullptr;
	unsigned (*drawCalls)() = nullptr;
	void (*resetCounters)() = nullptr;
	// Raw-GL probe of the shader precision question, executed on the harness' own program
	// (independent of d3d8gles): returns the largest deviation, in texels, between the expected
	// and the sampled position along a 64-repeat gradient. `useHighp` selects the varying qualifier.
	double (*probeVaryingPrecision)(bool useHighp) = nullptr;
	// True when built for visionOS/xrsimulator (assertions about ANGLE's visionOS behaviour are only
	// made there; a macOS ANGLE-Metal exposes S3TC, for instance).
	bool visionOS = false;
	// Metal-side device facts for the log.
	const char *(*deviceName)() = nullptr;
	const char *(*glString)(unsigned name) = nullptr;
};

// The cases return the number of failed checks; `checks` is incremented per check.
int RunDeviceCases(Harness *h, int *checks);
