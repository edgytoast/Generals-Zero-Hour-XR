// See GXXRAudioSession.h.
#import "GXXRAudioSession.h"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#if __has_include(<UIKit/UIKit.h>)
#import <UIKit/UIKit.h>
#endif

#include <mutex>
#include <string>

namespace {

enum PauseReason { kReasonInterruption = 1, kReasonBackground = 2, kReasonInactive = 4, kReasonHost = 8 };

struct State {
	std::mutex mutex;
	bool observing = false;
	int reasons = 0;
	bool paused = false;         // what the callbacks last saw
	bool pauseOnInactive = false;
	bool active = false;
	int spatialMode = GXAUDIO_SESSION_SPATIAL_BYPASSED;
	bool mixWithOthers = false;
	int interruptions = 0, routeChanges = 0, mediaServicesResets = 0;
	std::string lastError;
	GXAudioSessionPauseFn pauseFn = nullptr;
	GXAudioSessionResumeFn resumeFn = nullptr;
	void *ctx = nullptr;
	GXAudioSessionEventFn eventFn = nullptr;
	void *eventCtx = nullptr;
	std::string errorBuffer;     // stable storage for GXAudioSession_LastError
};
State &S() { static State s; return s; }

void setError(NSError *e) {
	State &s = S();
	std::lock_guard<std::mutex> l(s.mutex);
	s.lastError = e ? std::string([[e description] UTF8String]) : std::string();
}

// Recomputes paused from the reason mask and calls the matching callback outside the lock on a real transition.
void applyReasons(int addBits, int clearBits, const char *why) {
	GXAudioSessionPauseFn pf = nullptr;
	GXAudioSessionResumeFn rf = nullptr;
	void *ctx = nullptr;
	int transition = 0;   // +1 pause, -1 resume
	{
		State &s = S();
		std::lock_guard<std::mutex> l(s.mutex);
		s.reasons = (s.reasons | addBits) & ~clearBits;
		const bool nowPaused = s.reasons != 0;
		if (nowPaused != s.paused) {
			s.paused = nowPaused;
			transition = nowPaused ? 1 : -1;
			pf = s.pauseFn; rf = s.resumeFn; ctx = s.ctx;
		}
		NSLog(@"[GXAudioSession] %s -> reasons=0x%x paused=%d", why, s.reasons, (int)s.paused);
	}
	if (transition > 0 && pf) pf(ctx);
	if (transition < 0 && rf) rf(ctx);
}

void fireEvent(int event) {
	GXAudioSessionEventFn fn; void *ctx;
	{
		State &s = S();
		std::lock_guard<std::mutex> l(s.mutex);
		fn = s.eventFn; ctx = s.eventCtx;
	}
	if (fn) fn(event, ctx);
}

void copyStr(char *dst, size_t n, NSString *src) {
	if (!dst || n == 0) return;
	const char *c = src ? [src UTF8String] : "";
	strlcpy(dst, c ? c : "", n);
}

bool applySpatial(GXAudioSessionSpatialMode mode) {
#if TARGET_OS_VISION
	AVAudioSession *session = [AVAudioSession sharedInstance];
	NSError *err = nil;
	BOOL ok = NO;
	switch (mode) {
	case GXAUDIO_SESSION_SPATIAL_FIXED:
		ok = [session setIntendedSpatialExperience:AVAudioSessionSpatialExperienceFixed
		                                   options:@{AVAudioSessionSpatialExperienceOptionSoundStageSize: @(AVAudioSessionSoundStageSizeMedium)}
		                                     error:&err];
		break;
	case GXAUDIO_SESSION_SPATIAL_HEAD_TRACKED:
		ok = [session setIntendedSpatialExperience:AVAudioSessionSpatialExperienceHeadTracked
		                                   options:@{AVAudioSessionSpatialExperienceOptionSoundStageSize: @(AVAudioSessionSoundStageSizeAutomatic),
		                                             AVAudioSessionSpatialExperienceOptionAnchoringStrategy: @(AVAudioSessionAnchoringStrategyAutomatic)}
		                                     error:&err];
		break;
	case GXAUDIO_SESSION_SPATIAL_BYPASSED:
	default:
		ok = [session setIntendedSpatialExperience:AVAudioSessionSpatialExperienceBypassed options:nil error:&err];
		break;
	}
	setError(ok ? nil : err);
	if (ok) { State &s = S(); std::lock_guard<std::mutex> l(s.mutex); s.spatialMode = (int)mode; }
	return ok;
#else
	(void)mode;
	return true;   // no spatial experience API outside visionOS
#endif
}

bool applyCategory(bool mixWithOthers) {
	AVAudioSession *session = [AVAudioSession sharedInstance];
	NSError *err = nil;
	AVAudioSessionCategoryOptions opts = mixWithOthers ? AVAudioSessionCategoryOptionMixWithOthers : 0;
	BOOL ok = [session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:opts error:&err];
	setError(ok ? nil : err);
	return ok;
}

void registerObservers() {
	State &s = S();
	{
		std::lock_guard<std::mutex> l(s.mutex);
		if (s.observing) return;
		s.observing = true;
	}
	NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
	AVAudioSession *session = [AVAudioSession sharedInstance];

	[nc addObserverForName:AVAudioSessionInterruptionNotification object:session queue:nil usingBlock:^(NSNotification *n) {
		const NSUInteger type = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
		if (type == AVAudioSessionInterruptionTypeBegan) {
			{ State &st = S(); std::lock_guard<std::mutex> l(st.mutex); ++st.interruptions; }
			applyReasons(kReasonInterruption, 0, "interruption began");
		} else {
			const NSUInteger opt = [n.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
			// A game is not a media player with a play button: once the interruption is over the game resumes whether or not
			// the system set ShouldResume (the pause menu logic below the engine still decides what is audible).
			NSLog(@"[GXAudioSession] interruption ended, shouldResume=%d", (opt & AVAudioSessionInterruptionOptionShouldResume) ? 1 : 0);
			[[AVAudioSession sharedInstance] setActive:YES error:nil];
			applyReasons(0, kReasonInterruption, "interruption ended");
		}
	}];
	[nc addObserverForName:AVAudioSessionRouteChangeNotification object:session queue:nil usingBlock:^(NSNotification *n) {
		const NSUInteger reason = [n.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
		{ State &st = S(); std::lock_guard<std::mutex> l(st.mutex); ++st.routeChanges; }
		NSLog(@"[GXAudioSession] route change, reason=%lu", (unsigned long)reason);
		fireEvent(GXAUDIO_SESSION_EVENT_ROUTE_CHANGED);
	}];
	[nc addObserverForName:AVAudioSessionMediaServicesWereLostNotification object:session queue:nil usingBlock:^(NSNotification *) {
		NSLog(@"[GXAudioSession] media services lost");
		applyReasons(kReasonInterruption, 0, "media services lost");
		fireEvent(GXAUDIO_SESSION_EVENT_MEDIA_SERVICES_LOST);
	}];
	[nc addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:session queue:nil usingBlock:^(NSNotification *) {
		// mediaserverd restarted: the shared session lost its configuration. Rebuild it, then tell the host to reopen the audio device.
		bool mix; int mode;
		{ State &st = S(); std::lock_guard<std::mutex> l(st.mutex); ++st.mediaServicesResets; mix = st.mixWithOthers; mode = st.spatialMode; }
		applyCategory(mix);
		applySpatial((GXAudioSessionSpatialMode)mode);
		[[AVAudioSession sharedInstance] setActive:YES error:nil];
		fireEvent(GXAUDIO_SESSION_EVENT_MEDIA_SERVICES_RESET);
		applyReasons(0, kReasonInterruption, "media services reset");
	}];

#if __has_include(<UIKit/UIKit.h>)
	[nc addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:nil usingBlock:^(NSNotification *) {
		applyReasons(kReasonBackground, 0, "UIApplicationDidEnterBackground");
	}];
	[nc addObserverForName:UIApplicationWillEnterForegroundNotification object:nil queue:nil usingBlock:^(NSNotification *) {
		[[AVAudioSession sharedInstance] setActive:YES error:nil];
		applyReasons(0, kReasonBackground, "UIApplicationWillEnterForeground");
	}];
#endif
}

}  // namespace

extern "C" {

bool GXAudioSession_Configure(GXAudioSessionSpatialMode spatial, bool mixWithOthers) {
	{ State &s = S(); std::lock_guard<std::mutex> l(s.mutex); s.mixWithOthers = mixWithOthers; s.spatialMode = (int)spatial; }
	registerObservers();
	const bool a = applyCategory(mixWithOthers);
	const bool b = applySpatial(spatial);
	const bool c = GXAudioSession_Activate();
	return a && b && c;
}

bool GXAudioSession_Activate(void) {
	NSError *err = nil;
	const BOOL ok = [[AVAudioSession sharedInstance] setActive:YES error:&err];
	setError(ok ? nil : err);
	{ State &s = S(); std::lock_guard<std::mutex> l(s.mutex); s.active = ok; }
	return ok;
}

void GXAudioSession_Deactivate(void) {
	NSError *err = nil;
	const BOOL ok = [[AVAudioSession sharedInstance] setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:&err];
	setError(ok ? nil : err);
	{ State &s = S(); std::lock_guard<std::mutex> l(s.mutex); s.active = false; }
}

bool GXAudioSession_SetSpatialMode(GXAudioSessionSpatialMode spatial) { return applySpatial(spatial); }

void GXAudioSession_SetCallbacks(GXAudioSessionPauseFn pauseFn, GXAudioSessionResumeFn resumeFn, void *ctx) {
	State &s = S();
	std::lock_guard<std::mutex> l(s.mutex);
	s.pauseFn = pauseFn; s.resumeFn = resumeFn; s.ctx = ctx;
}

void GXAudioSession_SetEventCallback(GXAudioSessionEventFn eventFn, void *ctx) {
	State &s = S();
	std::lock_guard<std::mutex> l(s.mutex);
	s.eventFn = eventFn; s.eventCtx = ctx;
}

void GXAudioSession_NotifyScenePhase(int phase) {
	bool poi;
	{ State &s = S(); std::lock_guard<std::mutex> l(s.mutex); poi = s.pauseOnInactive; }
	switch (phase) {
	case 0: applyReasons(0, kReasonBackground | kReasonInactive, "scenePhase active"); break;
	case 1: if (poi) applyReasons(kReasonInactive, 0, "scenePhase inactive"); else applyReasons(0, kReasonBackground, "scenePhase inactive (audio keeps playing)"); break;
	default: applyReasons(kReasonBackground, 0, "scenePhase background"); break;
	}
}

void GXAudioSession_SetHostPause(bool pausing) {
	if (pausing) applyReasons(kReasonHost, 0, "host pause");
	else applyReasons(0, kReasonHost, "host resume");
}

void GXAudioSession_SetPauseOnInactive(bool pause) {
	State &s = S();
	std::lock_guard<std::mutex> l(s.mutex);
	s.pauseOnInactive = pause;
}

int GXAudioSession_PauseReasons(void) { State &s = S(); std::lock_guard<std::mutex> l(s.mutex); return s.reasons; }
bool GXAudioSession_IsPaused(void) { State &s = S(); std::lock_guard<std::mutex> l(s.mutex); return s.paused; }

void GXAudioSession_GetInfo(GXAudioSessionInfo *out) {
	if (!out) return;
	memset(out, 0, sizeof *out);
	AVAudioSession *session = [AVAudioSession sharedInstance];
	copyStr(out->category, sizeof out->category, session.category);
	copyStr(out->mode, sizeof out->mode, session.mode);
	out->categoryOptions = (unsigned long)session.categoryOptions;
	out->sampleRate = session.sampleRate;
	out->ioBufferDuration = session.IOBufferDuration;
	out->outputChannels = (int)session.outputNumberOfChannels;
	AVAudioSessionPortDescription *port = session.currentRoute.outputs.firstObject;
	if (port) copyStr(out->outputRoute, sizeof out->outputRoute, [NSString stringWithFormat:@"%@:%@", port.portType, port.portName]);
	out->otherAudioPlaying = session.otherAudioPlaying;
#if TARGET_OS_VISION
	out->spatialExperienceRaw = (int)session.intendedSpatialExperience;
#else
	out->spatialExperienceRaw = -1;
#endif
	State &s = S();
	std::lock_guard<std::mutex> l(s.mutex);
	out->active = s.active;
	out->spatialMode = s.spatialMode;
	out->interruptions = s.interruptions;
	out->routeChanges = s.routeChanges;
	out->mediaServicesResets = s.mediaServicesResets;
}

const char *GXAudioSession_LastError(void) {
	State &s = S();
	std::lock_guard<std::mutex> l(s.mutex);
	s.errorBuffer = s.lastError;
	return s.errorBuffer.c_str();
}

}  // extern "C"
