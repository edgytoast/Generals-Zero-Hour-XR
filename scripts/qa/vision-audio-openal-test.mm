// visionOS audio proof 1: OpenAL Soft (the static libopenal.a the engine build produces: CoreAudio backend,
// TARGET_OS_VISION patch) opens the default device on xrsimulator, plays a generated tone, and renders through
// ALC_SOFT_loopback so listener/source geometry can be asserted numerically (no ears needed).
//
// Build + run: scripts/qa/vision-audio-openal-test.sh   (runs inside a simulator device via xcrun simctl spawn)
//
// Conventions match the engine: Z is up, the listener orientation is {at, up} = {(lookX,lookY,lookZ),(0,0,1)}
// (OpenALAudioManager::setDeviceListenerPosition) and AL_INVERSE_DISTANCE_CLAMPED is the distance model.
#import <Foundation/Foundation.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <execinfo.h>
#include <exception>

#define AL_ALEXT_PROTOTYPES 1
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include "../../visionos/Audio/GXAudioListenerMath.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                                                        \
	do {                                                                                        \
		if (cond) { ++g_pass; printf("  PASS  "); }                                             \
		else { ++g_fail; printf("  FAIL  "); }                                                  \
		printf(__VA_ARGS__);                                                                    \
		printf("\n");                                                                           \
	} while (0)

static const char *alErrName(ALenum e) {
	switch (e) {
	case AL_NO_ERROR: return "AL_NO_ERROR";
	case AL_INVALID_NAME: return "AL_INVALID_NAME";
	case AL_INVALID_ENUM: return "AL_INVALID_ENUM";
	case AL_INVALID_VALUE: return "AL_INVALID_VALUE";
	case AL_INVALID_OPERATION: return "AL_INVALID_OPERATION";
	case AL_OUT_OF_MEMORY: return "AL_OUT_OF_MEMORY";
	}
	return "?";
}

static std::vector<short> makeSine(int rate, float hz, float seconds, float amp) {
	const int n = (int)(rate * seconds);
	std::vector<short> s(n);
	for (int i = 0; i < n; ++i) s[i] = (short)lrintf(amp * 32767.0f * sinf(2.0f * (float)M_PI * hz * i / rate));
	return s;
}

// ---- Part A: default device, real tone ---------------------------------------------------------------------
static void partA() {
	printf("== A. default device + generated tone ==\n");
	ALCdevice *dev = alcOpenDevice(nullptr);
	CHECK(dev != nullptr, "alcOpenDevice(NULL) returned %p", (void *)dev);
	if (!dev) return;
	const ALCchar *name = alcGetString(dev, ALC_ALL_DEVICES_SPECIFIER);
	if (!name || alcGetError(dev) != ALC_NO_ERROR) name = alcGetString(dev, ALC_DEVICE_SPECIFIER);
	printf("  device name: %s\n", name ? name : "(null)");
	const ALCchar *list = alcGetString(nullptr, ALC_ALL_DEVICES_SPECIFIER);
	int count = 0;
	for (const ALCchar *p = list; p && *p; p += strlen(p) + 1) { printf("  enumerated[%d]: %s\n", count, p); ++count; }
	printf("  enumerated devices: %d\n", count);
	ALCint attrs[] = {ALC_FREQUENCY, 44100, 0};
	ALCcontext *ctx = alcCreateContext(dev, attrs);
	CHECK(ctx && alcMakeContextCurrent(ctx), "context created and made current");
	if (!ctx) { alcCloseDevice(dev); return; }
	ALCint freq = 0, mono = 0, stereo = 0, refresh = 0, hrtfStatus = 0;
	alcGetIntegerv(dev, ALC_FREQUENCY, 1, &freq);
	alcGetIntegerv(dev, ALC_MONO_SOURCES, 1, &mono);
	alcGetIntegerv(dev, ALC_STEREO_SOURCES, 1, &stereo);
	alcGetIntegerv(dev, ALC_REFRESH, 1, &refresh);
	if (alcIsExtensionPresent(dev, "ALC_SOFT_HRTF")) alcGetIntegerv(dev, ALC_HRTF_STATUS_SOFT, 1, &hrtfStatus);
	printf("  AL_VENDOR=%s AL_RENDERER=%s AL_VERSION=%s\n", alGetString(AL_VENDOR), alGetString(AL_RENDERER), alGetString(AL_VERSION));
	printf("  output frequency=%d Hz (requested 44100) mono_sources=%d stereo_sources=%d refresh=%d hrtf_status=0x%x\n", freq, mono, stereo, refresh, hrtfStatus);
	printf("  ALC extensions: %s\n", alcGetString(dev, ALC_EXTENSIONS));
	CHECK(freq > 0, "device reports a non-zero output frequency (%d Hz)", freq);
	CHECK(alcIsExtensionPresent(dev, "ALC_SOFT_loopback") == ALC_TRUE, "ALC_SOFT_loopback present");
	CHECK(alcIsExtensionPresent(dev, "ALC_SOFT_pause_device") == ALC_TRUE, "ALC_SOFT_pause_device present");
	CHECK(alcIsExtensionPresent(dev, "ALC_SOFT_HRTF") == ALC_TRUE, "ALC_SOFT_HRTF present");
	CHECK(alIsExtensionPresent("AL_EXT_FLOAT32") == AL_TRUE, "AL_EXT_FLOAT32 present (engine uses float buffers)");

	auto tone = makeSine(44100, 440.0f, 0.7f, 0.3f);
	ALuint buf = 0, src = 0;
	alGenBuffers(1, &buf);
	alBufferData(buf, AL_FORMAT_MONO16, tone.data(), (ALsizei)(tone.size() * sizeof(short)), 44100);
	alGenSources(1, &src);
	alSourcei(src, AL_BUFFER, (ALint)buf);
	alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
	alSourcePlay(src);
	ALenum err = alGetError();
	CHECK(err == AL_NO_ERROR, "alSourcePlay -> %s", alErrName(err));
	usleep(250 * 1000);
	ALint state = 0;
	ALfloat off1 = 0, off2 = 0;
	alGetSourcei(src, AL_SOURCE_STATE, &state);
	alGetSourcef(src, AL_SEC_OFFSET, &off1);
	CHECK(state == AL_PLAYING && off1 > 0.05f, "tone is playing, offset advanced to %.3f s after 250 ms", off1);
	// Interruption path used by GXAudio_HostPause/Resume: device level pause must freeze the source clock.
	alcDevicePauseSOFT(dev);
	usleep(120 * 1000);
	alGetSourcef(src, AL_SEC_OFFSET, &off1);
	usleep(300 * 1000);
	alGetSourcef(src, AL_SEC_OFFSET, &off2);
	CHECK(fabsf(off2 - off1) < 0.01f, "alcDevicePauseSOFT freezes playback (%.4f -> %.4f s over 300 ms)", off1, off2);
	alcDeviceResumeSOFT(dev);
	usleep(250 * 1000);
	alGetSourcef(src, AL_SEC_OFFSET, &off2);
	CHECK(off2 > off1 + 0.05f, "alcDeviceResumeSOFT resumes playback (%.4f -> %.4f s)", off1, off2);
	usleep(500 * 1000);
	alGetSourcei(src, AL_SOURCE_STATE, &state);
	CHECK(state == AL_STOPPED, "tone ran to completion (state=0x%x)", state);
	CHECK(alGetError() == AL_NO_ERROR, "no AL error after the run");
	alDeleteSources(1, &src);
	alDeleteBuffers(1, &buf);
	alcMakeContextCurrent(nullptr);
	alcDestroyContext(ctx);
	alcCloseDevice(dev);
}

// ---- Part B: loopback rendering ---------------------------------------------------------------------------
struct Loopback {
	ALCdevice *dev = nullptr;
	ALCcontext *ctx = nullptr;
	ALuint buf = 0, src = 0;
	int rate = 48000;
	bool open(bool hrtf) {
		dev = alcLoopbackOpenDeviceSOFT(nullptr);
		if (!dev) return false;
		if (!alcIsRenderFormatSupportedSOFT(dev, rate, ALC_STEREO_SOFT, ALC_FLOAT_SOFT)) return false;
		ALCint attrs[] = {ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT, ALC_FORMAT_TYPE_SOFT, ALC_FLOAT_SOFT, ALC_FREQUENCY, rate,
		                  ALC_HRTF_SOFT, hrtf ? ALC_TRUE : ALC_FALSE, 0};
		ctx = alcCreateContext(dev, attrs);
		if (!ctx || !alcMakeContextCurrent(ctx)) return false;
		alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);  // exactly what OpenALAudioManager::init() selects
		auto sine = makeSine(rate, 1000.0f, 1.0f, 0.5f);
		alGenBuffers(1, &buf);
		alBufferData(buf, AL_FORMAT_MONO16, sine.data(), (ALsizei)(sine.size() * sizeof(short)), rate);
		alGenSources(1, &src);
		alSourcei(src, AL_BUFFER, (ALint)buf);
		alSourcei(src, AL_LOOPING, AL_TRUE);
		alSourcei(src, AL_SOURCE_RELATIVE, AL_FALSE);
		return alGetError() == AL_NO_ERROR;
	}
	void close() {
		alcMakeContextCurrent(nullptr);
		if (ctx) alcDestroyContext(ctx);
		if (dev) alcCloseDevice(dev);
	}
	// Listener in the engine's convention: up is world +Z.
	void listener(gxaudio::Vec3 pos, gxaudio::Vec3 at, gxaudio::Vec3 up = gxaudio::make(0, 0, 1)) {
		alListener3f(AL_POSITION, pos.x, pos.y, pos.z);
		ALfloat o[6] = {at.x, at.y, at.z, up.x, up.y, up.z};
		alListenerfv(AL_ORIENTATION, o);
	}
	void place(gxaudio::Vec3 p, float ref, float maxd, float rolloff) {
		alSourcef(src, AL_REFERENCE_DISTANCE, ref);
		alSourcef(src, AL_MAX_DISTANCE, maxd);
		alSourcef(src, AL_ROLLOFF_FACTOR, rolloff);
		alSource3f(src, AL_POSITION, p.x, p.y, p.z);
	}
	// Renders and returns RMS of left/right over `frames` after a warm-up.
	void render(double *rmsL, double *rmsR, int frames = 9600) {
		alSourceStop(src);
		alSourceRewind(src);
		alSourcePlay(src);
		std::vector<float> warm(4800 * 2), pcm((size_t)frames * 2);
		alcRenderSamplesSOFT(dev, warm.data(), 4800);  // lets the ~3 ms gain/HRTF ramp settle
		alcRenderSamplesSOFT(dev, pcm.data(), frames);
		double sl = 0, sr = 0;
		for (int i = 0; i < frames; ++i) { sl += (double)pcm[2 * i] * pcm[2 * i]; sr += (double)pcm[2 * i + 1] * pcm[2 * i + 1]; }
		*rmsL = sqrt(sl / frames);
		*rmsR = sqrt(sr / frames);
	}
};

static double db(double a, double b) { return 20.0 * log10((a + 1e-12) / (b + 1e-12)); }

static void partB() {
	using namespace gxaudio;
	printf("== B. loopback rendering (ALC_SOFT_loopback, stereo float 48 kHz, HRTF off = plain amplitude panning) ==\n");
	Loopback lb;
	bool ok = lb.open(false);
	CHECK(ok, "loopback device + context opened");
	if (!ok) return;
	ALCint hrtf = -1;
	alcGetIntegerv(lb.dev, ALC_HRTF_SOFT, 1, &hrtf);
	printf("  ALC_HRTF_SOFT reported %d (0 = off)\n", hrtf);
	double L, R;
	const Vec3 origin = make(0, 0, 0);
	const Vec3 faceY = make(0, 1, 0);   // engine listener "at" vector
	const float ref = 1.0f, maxd = 100.0f;

	printf(" -- azimuth (listener at origin facing +Y, up +Z; right = +X) --\n");
	lb.listener(origin, faceY);
	lb.place(make(-2, 0.5f, 0), ref, maxd, 1.0f); lb.render(&L, &R);
	printf("  source at left  (-2,0.5,0): L=%.4f R=%.4f  ILD=%+.2f dB\n", L, R, db(L, R));
	CHECK(L > R * 1.5, "left source is louder in the LEFT channel");
	const double ildLeft = db(L, R);
	lb.place(make(2, 0.5f, 0), ref, maxd, 1.0f); lb.render(&L, &R);
	printf("  source at right ( 2,0.5,0): L=%.4f R=%.4f  ILD=%+.2f dB\n", L, R, db(L, R));
	CHECK(R > L * 1.5, "right source is louder in the RIGHT channel");
	CHECK(fabs(db(L, R) + ildLeft) < 0.3, "mirror symmetry: |ILD| left %.2f dB vs right %.2f dB", ildLeft, db(L, R));
	lb.place(make(0, 2, 0), ref, maxd, 1.0f); lb.render(&L, &R);
	printf("  source in front (0,2,0):    L=%.4f R=%.4f  ILD=%+.2f dB\n", L, R, db(L, R));
	CHECK(fabs(db(L, R)) < 0.3, "centre source is balanced (%.3f dB)", db(L, R));

	printf(" -- listener yaw --\n");
	// Exactly 90 degrees to the side so the yaw-180 mirror image is exact (front/back are not symmetric in a stereo panner).
	lb.listener(origin, faceY);
	lb.place(make(-2, 0, 0), ref, maxd, 1.0f); lb.render(&L, &R);
	const double sideL = L, sideR = R;
	printf("  yaw 0,   source at (-2,0,0): L=%.4f R=%.4f\n", sideL, sideR);
	lb.listener(origin, make(0, -1, 0));  // yaw 180 degrees
	lb.render(&L, &R);
	printf("  yaw 180, source at (-2,0,0): L=%.4f R=%.4f\n", L, R);
	CHECK(R > L * 1.5, "yaw 180: the source that was on the left is now on the RIGHT (channels swapped)");
	CHECK(fabs(L - sideR) < 0.02 * sideL && fabs(R - sideL) < 0.02 * sideL,
	      "yaw 180: output is the exact channel swap of yaw 0 within 2%% (L %.4f vs %.4f, R %.4f vs %.4f)", L, sideR, R, sideL);
	lb.listener(origin, make(1, 0, 0));  // yaw -90: facing +X, listener right = at x up = (0,-1,0)
	lb.place(make(0, -2, 0), ref, maxd, 1.0f); lb.render(&L, &R);
	printf("  yaw -90 (facing +X), source at (0,-2,0): L=%.4f R=%.4f ILD=%+.2f dB\n", L, R, db(L, R));
	CHECK(R > L * 1.5, "yaw -90: source at world -Y is on the listener's right");
	lb.place(make(0, 2, 0), ref, maxd, 1.0f); lb.render(&L, &R);
	CHECK(L > R * 1.5, "yaw -90: source at world +Y is on the listener's left (L=%.4f R=%.4f)", L, R);

	printf(" -- distance attenuation, AL_INVERSE_DISTANCE_CLAMPED --\n");
	struct Cfg { float ref, maxd, roll; const char *label; } cfgs[] = {
	    {1.0f, 100.0f, 1.0f, "ref 1, rolloff 1.0 (pure 1/d)"},
	    {200.0f, 600.0f, 0.5f, "engine defaults: ref 200, max 600, rolloff 0.5"},
	    {2500.0f, 20000.0f, 0.35f, "tabletop tuning in game units (ref 1.0 m = 2500 u at 0.0004 m/u, rolloff 0.35)"},
	};
	for (auto &c : cfgs) {
		lb.listener(origin, faceY);
		const float dists[] = {0.25f * c.ref, 1.0f * c.ref, 2.0f * c.ref, 3.0f * c.ref, c.maxd * 0.5f, c.maxd, c.maxd * 4.0f};
		double first = 0;
		double worst = 0;
		printf("  %s\n", c.label);
		for (size_t i = 0; i < sizeof(dists) / sizeof(dists[0]); ++i) {
			lb.place(make(0, dists[i], 0), c.ref, c.maxd, c.roll);
			lb.render(&L, &R);
			const double tot = sqrt(L * L + R * R);
			if (i == 0) first = tot;
			const double expect = inverseDistanceClamped(dists[i], c.ref, c.maxd, c.roll) / inverseDistanceClamped(dists[0], c.ref, c.maxd, c.roll);
			const double meas = tot / first;
			worst = fmax(worst, fabs(meas / expect - 1.0));
			printf("    d=%9.1f measured gain %.4f  model %.4f\n", dists[i], meas, expect);
		}
		CHECK(worst < 0.02, "measured attenuation follows the model within 2%% (worst deviation %.3f%%)", worst * 100.0);
	}
	lb.close();
}

// End-to-end: head pose in BOARD SPACE -> gxaudio::computeListener -> AL -> rendered numerics.
static void partC() {
	using namespace gxaudio;
	printf("== C. tabletop mapping end to end (board-space head pose -> game-world listener -> rendered stereo) ==\n");
	Loopback lb;
	if (!lb.open(false)) { CHECK(false, "loopback open"); return; }
	const float metersPerUnit = 0.8f / 2000.0f;  // 0.8 m board shows a 2000 unit wide map
	const TabletopTuning tune = defaultTabletopTuning();
	const SourceAttenuation att = tabletopAttenuation(tune, metersPerUnit, 600.0f);
	printf("  tabletop attenuation for a 600 u event at %.5f m/u: ref=%.1f u max=%.1f u rolloff=%.2f\n", metersPerUnit, att.refDistance, att.maxDistance, att.rolloff);

	// Head 0.9 m south of board centre, 0.55 m above it, looking at the board centre, up = board +Z.
	const Vec3 head = make(0.0f, -0.9f, 0.55f);
	const Vec3 fwd = make(0.0f, 0.9f, -0.55f);
	struct Case { float yawDeg; } yaws[] = {{0}, {37}, {90}, {180}, {-121}};
	// Sources are defined in BOARD space so that the expected side is computed independently of the mapping code:
	// headRight = normalize(fwd x up); a source is on the left when dot(headRight, src - head) < 0.
	const Vec3 srcBoard[] = {make(-0.3f, 0.2f, 0.02f), make(0.3f, 0.2f, 0.02f), make(-0.25f, -0.2f, 0.02f), make(0.1f, 0.35f, 0.02f)};
	const char *names[] = {"left-far", "right-far", "left-near", "right-ish-far"};
	double refL[4], refR[4];
	for (size_t yi = 0; yi < sizeof(yaws) / sizeof(yaws[0]); ++yi) {
		const float yaw = yaws[yi].yawDeg * (float)M_PI / 180.0f;
		BoardFrame f;
		f.centerWorld = make(1000, 1000, 15);
		f.rightX = cosf(yaw);
		f.rightY = sinf(yaw);
		ListenerWorld lw;
		bool ok = computeListener(f, metersPerUnit, head, fwd, make(0, 0, 1), &lw);
		if (!ok) { CHECK(false, "computeListener"); continue; }
		lb.listener(lw.position, lw.forward, lw.up);
		alListenerf(AL_METERS_PER_UNIT, lw.metersPerUnit);
		printf("  board frame yaw %+.0f deg -> listener world pos (%.1f, %.1f, %.1f) forward (%.3f, %.3f, %.3f)\n", yaws[yi].yawDeg,
		       lw.position.x, lw.position.y, lw.position.z, lw.forward.x, lw.forward.y, lw.forward.z);
		Vec3 headRight;
		normalize(cross(fwd, make(0, 0, 1)), make(1, 0, 0), &headRight);
		for (int s = 0; s < 4; ++s) {
			const Vec3 w = boardPointToWorld(f, metersPerUnit, srcBoard[s]);  // sources KEEP game coordinates
			lb.place(w, att.refDistance, att.maxDistance, att.rolloff);
			double L, R;
			lb.render(&L, &R);
			const float side = dot(headRight, sub(srcBoard[s], head));
			const bool expectLeft = side < 0;
			const float dMeters = length(sub(srcBoard[s], head));
			const float dUnits = length(sub(w, lw.position));
			const double tot = sqrt(L * L + R * R);
			const double modelGain = inverseDistanceClamped(dUnits, att.refDistance, att.maxDistance, att.rolloff);
			printf("    %-13s d=%.3f m (%.0f u) L=%.4f R=%.4f ILD=%+.2f dB total=%.4f model_gain=%.4f\n", names[s], dMeters, dUnits, L, R,
			       db(L, R), tot, modelGain);
			CHECK((expectLeft ? L > R : R > L), "yaw %+.0f: %s is on the %s in board space and renders louder in the %s channel", yaws[yi].yawDeg,
			      names[s], expectLeft ? "left" : "right", expectLeft ? "left" : "right");
			if (yi == 0) { refL[s] = L; refR[s] = R; }
			else {
				// Rotating the board frame rotates the world under the head but the head->source geometry is
				// defined in board space, so the render must be identical for every frame yaw.
				CHECK(fabs(db(L, refL[s])) < 0.05 && fabs(db(R, refR[s])) < 0.05,
				      "yaw invariance: L/R identical to yaw 0 within 0.05 dB (dL=%.4f dB dR=%.4f dB)", db(L, refL[s]), db(R, refR[s]));
			}
		}
	}
	// Tabletop level: every source on a 0.8 m table seen from 0.9-1.1 m must sit within ~2 dB of each other.
	printf(" -- tabletop natural level: spread across the table (should be a small fraction of a dB per 10 cm) --\n");
	lb.listener(make(0, 0, 0), make(0, 1, 0));
	lb.place(make(0, 0.5f / metersPerUnit, 0), att.refDistance, att.maxDistance, att.rolloff);  // 0.5 m
	double L, R;
	lb.render(&L, &R);
	const double near = sqrt(L * L + R * R);
	lb.place(make(0, 1.6f / metersPerUnit, 0), att.refDistance, att.maxDistance, att.rolloff);  // 1.6 m (far corner)
	lb.render(&L, &R);
	const double far = sqrt(L * L + R * R);
	printf("  0.5 m: %.4f   1.6 m (far corner): %.4f   spread %.2f dB\n", near, far, db(near, far));
	CHECK(db(near, far) < 3.0 && db(near, far) > 0.0, "far corner within 3 dB of a near source (%.2f dB): audible at natural level, still falling with distance", db(near, far));
	lb.close();

	printf("== D. HRTF (informational: OpenAL Soft's built-in default HRTF, loopback, 48 kHz) ==\n");
	Loopback h;
	if (!h.open(true)) { printf("  HRTF loopback context could not be created\n"); return; }
	ALCint status = 0;
	alcGetIntegerv(h.dev, ALC_HRTF_STATUS_SOFT, 1, &status);
	printf("  ALC_HRTF_STATUS_SOFT=0x%x (0x%x = ALC_HRTF_ENABLED_SOFT)\n", status, ALC_HRTF_ENABLED_SOFT);
	if (status == ALC_HRTF_ENABLED_SOFT) {
		h.listener(make(0, 0, 0), make(0, 1, 0));
		h.place(make(-2, 0.5f, 0), 1, 100, 1);
		double hl, hr;
		h.render(&hl, &hr);
		printf("  HRTF, source at left: L=%.4f R=%.4f ILD=%+.2f dB\n", hl, hr, db(hl, hr));
		CHECK(hl > hr, "HRTF renders the left source louder in the left ear");
	} else {
		printf("  HRTF not enabled by the loopback device here (status above); the amplitude-panning results above stand.\n");
	}
	h.close();
}

int main() {
	@autoreleasepool {
		setvbuf(stdout, nullptr, _IOLBF, 0);
		std::set_terminate([] { void *f[40]; int n = backtrace(f, 40); fprintf(stderr, "terminate() backtrace:\n"); backtrace_symbols_fd(f, n, 2); abort(); });
		printf("vision-audio-openal-test\n");
		partA();
		partB();
		partC();
		printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
		return g_fail ? 1 : 0;
	}
}
