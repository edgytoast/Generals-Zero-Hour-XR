// visionOS audio proof 4: AVAudioSession configuration and lifecycle (visionos/Audio/GXXRAudioSession.mm) in the simulator.
// Real: category/mode/spatial-experience calls and read-back, session activation, an OpenAL Soft device opened AFTER the session
// is configured (optional, when built with GX_TEST_WITH_OPENAL). Simulated: interruption, route change, media-services reset,
// UIApplication background/foreground and SwiftUI scene phase are driven by posting the real system notification names with the
// real userInfo keys, so the observers, the pause-reason arithmetic and the callbacks are exercised; what the system itself
// would deliver on a device (a real Siri interruption, a real pod route change) cannot be produced here.
#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <cmath>
#include <cstdio>
#include <unistd.h>

#include "../../visionos/Audio/GXXRAudioSession.h"

#ifdef GX_TEST_WITH_OPENAL
#define AL_ALEXT_PROTOTYPES 1
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#endif

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...)                                                                        \
	do {                                                                                        \
		if (cond) { ++g_pass; printf("  PASS  "); }                                             \
		else { ++g_fail; printf("  FAIL  "); }                                                  \
		printf(__VA_ARGS__);                                                                    \
		printf("\n");                                                                           \
	} while (0)

static int g_pauses = 0, g_resumes = 0, g_events = 0, g_lastEvent = 0;
static void onPause(void *ctx) { ++g_pauses; (void)ctx; }
static void onResume(void *ctx) { ++g_resumes; (void)ctx; }
static void onEvent(int e, void *) { ++g_events; g_lastEvent = e; }
static void reset() { g_pauses = g_resumes = g_events = g_lastEvent = 0; }

static void post(NSNotificationName name, id object, NSDictionary *info) {
	[[NSNotificationCenter defaultCenter] postNotificationName:name object:object userInfo:info];
}

#ifdef GX_TEST_WITH_OPENAL
static void openalTone(const char *label) {
	ALCdevice *dev = alcOpenDevice(nullptr);
	if (!dev) { CHECK(false, "%s: alcOpenDevice", label); return; }
	ALCcontext *ctx = alcCreateContext(dev, nullptr);
	alcMakeContextCurrent(ctx);
	ALCint freq = 0;
	alcGetIntegerv(dev, ALC_FREQUENCY, 1, &freq);
	short pcm[9600];
	for (int i = 0; i < 9600; ++i) pcm[i] = (short)(9000 * sinf(2 * (float)M_PI * 440.0f * i / 48000.0f));
	ALuint b, s;
	alGenBuffers(1, &b); alBufferData(b, AL_FORMAT_MONO16, pcm, sizeof pcm, 48000);
	alGenSources(1, &s); alSourcei(s, AL_BUFFER, (ALint)b); alSourcei(s, AL_SOURCE_RELATIVE, AL_TRUE); alSourcePlay(s);
	usleep(120 * 1000);
	ALfloat off = 0; ALint st = 0;
	alGetSourcef(s, AL_SEC_OFFSET, &off); alGetSourcei(s, AL_SOURCE_STATE, &st);
	CHECK(alGetError() == AL_NO_ERROR && off > 0.03f, "%s: OpenAL Soft tone runs on the configured session (device %d Hz, offset %.3f s)", label, freq, off);
	alSourceStop(s); alDeleteSources(1, &s); alDeleteBuffers(1, &b);
	alcMakeContextCurrent(nullptr); alcDestroyContext(ctx); alcCloseDevice(dev);
}
#endif

static void dumpInfo(const char *tag) {
	GXAudioSessionInfo i;
	GXAudioSession_GetInfo(&i);
	printf("  [%s] category=%s mode=%s options=0x%lx sampleRate=%.0f ioBuffer=%.4fs outputChannels=%d route=\"%s\" otherAudioPlaying=%d active=%d requestedSpatial=%d intendedSpatialExperience(raw)=%d\n",
	       tag, i.category, i.mode, i.categoryOptions, i.sampleRate, i.ioBufferDuration, i.outputChannels, i.outputRoute, i.otherAudioPlaying, i.active, i.spatialMode, i.spatialExperienceRaw);
}

int main() {
	@autoreleasepool {
		setvbuf(stdout, nullptr, _IOLBF, 0);
		printf("vision-audio-session-test\n");
		AVAudioSession *session = [AVAudioSession sharedInstance];
		printf("== configuration (real AVAudioSession in the simulator) ==\n");
		dumpInfo("before");
		GXAudioSession_SetCallbacks(onPause, onResume, nullptr);
		GXAudioSession_SetEventCallback(onEvent, nullptr);
		bool ok = GXAudioSession_Configure(GXAUDIO_SESSION_SPATIAL_BYPASSED, false);
		CHECK(ok, "Configure(bypassed, mixWithOthers=false) succeeded (%s)", GXAudioSession_LastError());
		dumpInfo("bypassed");
		CHECK([session.category isEqualToString:AVAudioSessionCategoryPlayback], "category is playback");
		CHECK([session.mode isEqualToString:AVAudioSessionModeDefault], "mode is default");
		CHECK((session.categoryOptions & AVAudioSessionCategoryOptionMixWithOthers) == 0, "mixing off: the game is the primary audio app");
		CHECK(session.intendedSpatialExperience == AVAudioSessionSpatialExperienceBypassed, "intended spatial experience reads back as Bypassed (%ld)", (long)session.intendedSpatialExperience);
#ifdef GX_TEST_WITH_OPENAL
		openalTone("bypassed");
#endif
		const struct { GXAudioSessionSpatialMode m; const char *n; AVAudioSessionSpatialExperience e; } modes[] = {
		    {GXAUDIO_SESSION_SPATIAL_FIXED, "fixed (medium sound stage)", AVAudioSessionSpatialExperienceFixed},
		    {GXAUDIO_SESSION_SPATIAL_HEAD_TRACKED, "head tracked (automatic anchoring)", AVAudioSessionSpatialExperienceHeadTracked},
		    {GXAUDIO_SESSION_SPATIAL_BYPASSED, "bypassed", AVAudioSessionSpatialExperienceBypassed},
		};
		for (auto &m : modes) {
			const bool r = GXAudioSession_SetSpatialMode(m.m);
			CHECK(r, "SetSpatialMode(%s) accepted by the system (%s)", m.n, GXAudioSession_LastError());
			CHECK(session.intendedSpatialExperience == m.e, "read back matches (%ld)", (long)session.intendedSpatialExperience);
#ifdef GX_TEST_WITH_OPENAL
			openalTone(m.n);
#endif
		}
		dumpInfo("after mode sweep");
		CHECK(GXAudioSession_Configure(GXAUDIO_SESSION_SPATIAL_BYPASSED, false), "Configure is idempotent");
		GXAudioSession_Deactivate();
		CHECK(GXAudioSession_Activate(), "deactivate then activate again");
		reset();

		printf("== interruption (posted AVAudioSessionInterruptionNotification) ==\n");
		post(AVAudioSessionInterruptionNotification, session, @{AVAudioSessionInterruptionTypeKey: @(AVAudioSessionInterruptionTypeBegan)});
		CHECK(g_pauses == 1 && g_resumes == 0 && GXAudioSession_IsPaused() && (GXAudioSession_PauseReasons() & 1), "interruption began -> pause once (reasons=0x%x)", GXAudioSession_PauseReasons());
		post(AVAudioSessionInterruptionNotification, session, @{AVAudioSessionInterruptionTypeKey: @(AVAudioSessionInterruptionTypeBegan)});
		CHECK(g_pauses == 1, "a second 'began' does not pause twice");
		post(AVAudioSessionInterruptionNotification, session, @{AVAudioSessionInterruptionTypeKey: @(AVAudioSessionInterruptionTypeEnded), AVAudioSessionInterruptionOptionKey: @(AVAudioSessionInterruptionOptionShouldResume)});
		CHECK(g_pauses == 1 && g_resumes == 1 && !GXAudioSession_IsPaused(), "interruption ended (shouldResume) -> resume once");
		reset();
		post(AVAudioSessionInterruptionNotification, session, @{AVAudioSessionInterruptionTypeKey: @(AVAudioSessionInterruptionTypeBegan)});
		post(AVAudioSessionInterruptionNotification, session, @{AVAudioSessionInterruptionTypeKey: @(AVAudioSessionInterruptionTypeEnded)});
		CHECK(g_pauses == 1 && g_resumes == 1, "interruption ended WITHOUT shouldResume still resumes (game policy)");

		printf("== app lifecycle (posted UIApplication notifications) ==\n");
		reset();
		post(UIApplicationDidEnterBackgroundNotification, nil, nil);
		CHECK(g_pauses == 1 && (GXAudioSession_PauseReasons() & 2), "didEnterBackground -> pause (reasons=0x%x)", GXAudioSession_PauseReasons());
		post(AVAudioSessionInterruptionNotification, session, @{AVAudioSessionInterruptionTypeKey: @(AVAudioSessionInterruptionTypeBegan)});
		post(UIApplicationWillEnterForegroundNotification, nil, nil);
		CHECK(g_resumes == 0 && GXAudioSession_IsPaused(), "foreground while still interrupted stays paused (reasons=0x%x)", GXAudioSession_PauseReasons());
		post(AVAudioSessionInterruptionNotification, session, @{AVAudioSessionInterruptionTypeKey: @(AVAudioSessionInterruptionTypeEnded)});
		CHECK(g_pauses == 1 && g_resumes == 1 && !GXAudioSession_IsPaused(), "resume only when the LAST reason clears (pauses=%d resumes=%d)", g_pauses, g_resumes);

		printf("== SwiftUI scene phase (GXAudioSession_NotifyScenePhase) ==\n");
		reset();
		GXAudioSession_NotifyScenePhase(1);
		CHECK(g_pauses == 0, "inactive does not pause by default (system overlays, looking away)");
		GXAudioSession_NotifyScenePhase(2);
		CHECK(g_pauses == 1, "background pauses");
		GXAudioSession_NotifyScenePhase(0);
		CHECK(g_resumes == 1 && !GXAudioSession_IsPaused(), "active resumes");
		GXAudioSession_SetPauseOnInactive(true);
		GXAudioSession_NotifyScenePhase(1);
		CHECK(g_pauses == 2, "inactive pauses when opted in");
		GXAudioSession_NotifyScenePhase(2);
		GXAudioSession_NotifyScenePhase(0);
		CHECK(g_resumes == 2 && !GXAudioSession_IsPaused() && GXAudioSession_PauseReasons() == 0, "background then active clears both inactive and background (reasons=0x%x)", GXAudioSession_PauseReasons());
		GXAudioSession_SetPauseOnInactive(false);
		reset();
		GXAudioSession_SetHostPause(true);
		GXAudioSession_SetHostPause(true);
		CHECK(g_pauses == 1, "host pause (immersive space closed) pauses once");
		GXAudioSession_SetHostPause(false);
		CHECK(g_resumes == 1, "host resume resumes once");

		printf("== route change and media services ==\n");
		reset();
		post(AVAudioSessionRouteChangeNotification, session, @{AVAudioSessionRouteChangeReasonKey: @(AVAudioSessionRouteChangeReasonNewDeviceAvailable)});
		CHECK(g_events == 1 && g_lastEvent == GXAUDIO_SESSION_EVENT_ROUTE_CHANGED && g_pauses == 0, "route change -> event callback, no pause");
		post(AVAudioSessionMediaServicesWereLostNotification, session, nil);
		CHECK(g_lastEvent == GXAUDIO_SESSION_EVENT_MEDIA_SERVICES_LOST && g_pauses == 1, "media services lost -> pause + event");
		post(AVAudioSessionMediaServicesWereResetNotification, session, nil);
		CHECK(g_lastEvent == GXAUDIO_SESSION_EVENT_MEDIA_SERVICES_RESET && g_resumes == 1 && !GXAudioSession_IsPaused(), "media services reset -> session rebuilt, event (host reopens the device), resumed");
		CHECK([session.category isEqualToString:AVAudioSessionCategoryPlayback], "category still playback after the rebuild");
		GXAudioSessionInfo info;
		GXAudioSession_GetInfo(&info);
		CHECK(info.interruptions >= 4 && info.routeChanges == 1 && info.mediaServicesResets == 1, "counters: interruptions=%d routeChanges=%d mediaServicesResets=%d", info.interruptions, info.routeChanges, info.mediaServicesResets);
		dumpInfo("final");
		printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
		return g_fail ? 1 : 0;
	}
}
