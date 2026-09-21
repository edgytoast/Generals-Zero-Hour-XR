// Header-only state holders behind the GXAudio_* C API (GXAudioListener.h). Pure C++11, no engine or OpenAL
// dependencies: the engine (OpenALAudioManager.cpp) owns one instance of each and the host-side unit test
// (scripts/qa/vision-audio-listener-test.cpp) exercises them directly.
#ifndef GX_AUDIO_HOST_STATE_H
#define GX_AUDIO_HOST_STATE_H

#include "GXAudioListenerMath.h"

#include <atomic>
#include <cmath>
#include <mutex>

namespace gxaudio {

// Everything the engine thread needs, copied out under the lock.
struct HostListenerSnapshot {
	bool hostEnabled;
	bool spatialSetting;
	bool poseValid;
	bool active;                  // hostEnabled && spatialSetting && poseValid
	ListenerWorld listener;
	TabletopTuning tuning;
	unsigned generation;          // bumps whenever `active` flips or the tuning/scale changes: sources must be retuned
	int binauralMode;             // 0 auto, 1 on, 2 off
	unsigned binauralGeneration;  // bumps whenever binauralMode changes
};

class HostListenerState {
public:
	HostListenerState() {
		m_hostEnabled = false;
		m_spatial = true;   // default ON on visionOS
		m_poseValid = false;
		m_frame = defaultBoardFrame();
		m_tuning = defaultTabletopTuning();
		m_listener.position = make(0, 0, 0);
		m_listener.forward = make(0, 1, 0);
		m_listener.up = make(0, 0, 1);
		m_listener.metersPerUnit = 1.0f;
		m_generation = 0;
		m_binaural = 0;
		m_binauralGeneration = 0;
		m_lastActive = false;
	}

	void setHostEnabled(bool enabled) {
		std::lock_guard<std::mutex> l(m_mutex);
		if (m_hostEnabled != enabled) m_poseValid = false;   // a stale pose must never survive a re-enable
		m_hostEnabled = enabled;
		refreshActive();
	}
	void setSpatial(bool on) {
		std::lock_guard<std::mutex> l(m_mutex);
		m_spatial = on;
		refreshActive();
	}
	bool spatial() const {
		std::lock_guard<std::mutex> l(m_mutex);
		return m_spatial;
	}
	void setBoardFrame(const float center[3], float rightX, float rightY) {
		if (!center || !std::isfinite(center[0] + center[1] + center[2] + rightX + rightY)) return;
		if (!(std::sqrt(rightX * rightX + rightY * rightY) > 1e-4f)) return;
		std::lock_guard<std::mutex> l(m_mutex);
		m_frame.centerWorld = make(center[0], center[1], center[2]);
		m_frame.rightX = rightX;
		m_frame.rightY = rightY;
	}
	// Returns whether the pose was accepted. The pose is converted to world space immediately with the frame that is
	// current now (so SetBoardFrame + SetListenerPose in either order within one frame both work on the next pose).
	bool setPose(float metersPerWorldUnit, const float pos[3], const float fwd[3], const float up[3]) {
		if (!pos || !fwd || !up) return false;
		ListenerWorld w;
		std::lock_guard<std::mutex> l(m_mutex);
		if (!computeListener(m_frame, metersPerWorldUnit, make(pos[0], pos[1], pos[2]), make(fwd[0], fwd[1], fwd[2]),
		                     make(up[0], up[1], up[2]), &w))
			return false;
		if (std::fabs(w.metersPerUnit - m_listener.metersPerUnit) > 1e-9f * (1.0f + w.metersPerUnit)) ++m_generation;
		m_listener = w;
		m_poseValid = true;
		refreshActive();
		return true;
	}
	void setTuning(float refMeters, float rolloff, float maxMeters) {
		std::lock_guard<std::mutex> l(m_mutex);
		const TabletopTuning d = defaultTabletopTuning();
		TabletopTuning t = m_tuning;
		t.refDistanceMeters = (refMeters > 0.0f && std::isfinite(refMeters)) ? refMeters : d.refDistanceMeters;
		t.rolloff = (rolloff > 0.0f && std::isfinite(rolloff)) ? rolloff : d.rolloff;
		t.maxDistanceMeters = (maxMeters > 0.0f && std::isfinite(maxMeters)) ? maxMeters : d.maxDistanceMeters;
		if (t.maxDistanceMeters < t.refDistanceMeters) t.maxDistanceMeters = t.refDistanceMeters;
		m_tuning = t;
		++m_generation;
	}
	void setBinaural(int mode) {
		std::lock_guard<std::mutex> l(m_mutex);
		if (mode < 0 || mode > 2) mode = 0;
		if (mode != m_binaural) { m_binaural = mode; ++m_binauralGeneration; }
	}
	HostListenerSnapshot snapshot() const {
		std::lock_guard<std::mutex> l(m_mutex);
		HostListenerSnapshot s;
		s.hostEnabled = m_hostEnabled;
		s.spatialSetting = m_spatial;
		s.poseValid = m_poseValid;
		s.active = m_hostEnabled && m_spatial && m_poseValid;
		s.listener = m_listener;
		s.tuning = m_tuning;
		s.generation = m_generation;
		s.binauralMode = m_binaural;
		s.binauralGeneration = m_binauralGeneration;
		return s;
	}
	bool active() const { return snapshot().active; }

private:
	void refreshActive() {   // caller holds the lock
		const bool a = m_hostEnabled && m_spatial && m_poseValid;
		if (a != m_lastActive) { m_lastActive = a; ++m_generation; }
	}
	mutable std::mutex m_mutex;
	bool m_hostEnabled, m_spatial, m_poseValid, m_lastActive;
	BoardFrame m_frame;
	TabletopTuning m_tuning;
	ListenerWorld m_listener;
	unsigned m_generation;
	int m_binaural;
	unsigned m_binauralGeneration;
};

// Master + per-category volumes set by the host. `dirty` makes the engine thread apply a value once, at its next
// update, instead of the host thread poking the audio manager's lists.
class HostMixState {
public:
	enum { kCategories = 4 };
	HostMixState() {
		m_master.store(1.0f);
		m_masterDirty.store(false);
		for (int i = 0; i < kCategories; ++i) { m_value[i].store(1.0f); m_set[i].store(false); m_dirty[i].store(false); }
	}
	static float clamp01(float v) { return !(v > 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v); }   // NaN -> 0
	void setMaster(float v) { m_master.store(clamp01(v)); m_masterDirty.store(true); }
	float master() const { return m_master.load(); }
	// Returns true (and clears the flag) when the engine thread should push the master gain to the device.
	bool takeMasterDirty() { return m_masterDirty.exchange(false); }
	void setCategory(int c, float v) {
		if (c < 0 || c >= kCategories) return;
		m_value[c].store(clamp01(v));
		m_set[c].store(true);
		m_dirty[c].store(true);
	}
	bool categoryWasSet(int c) const { return c >= 0 && c < kCategories && m_set[c].load(); }
	float category(int c) const { return (c >= 0 && c < kCategories) ? m_value[c].load() : 0.0f; }
	bool takeCategoryDirty(int c, float *value) {
		if (c < 0 || c >= kCategories || !m_dirty[c].exchange(false)) return false;
		*value = m_value[c].load();
		return true;
	}

private:
	std::atomic<float> m_master;
	std::atomic<bool> m_masterDirty;
	std::atomic<float> m_value[kCategories];
	std::atomic<bool> m_set[kCategories];
	std::atomic<bool> m_dirty[kCategories];
};

// Engine pause request tracking (see GXAudio_EnginePause). Two flags: what the host wants, and what the engine thread
// has applied to its source lists. All transitions are lock-free.
class HostPauseState {
public:
	HostPauseState() { m_wanted.store(false); m_applied.store(false); }
	// Host thread: returns true if the request changed (the caller then pauses/resumes the device).
	bool request(bool paused) { return m_wanted.exchange(paused) != paused; }
	bool wanted() const { return m_wanted.load(); }
	bool applied() const { return m_applied.load(); }
	enum Action { None, ApplyPause, ApplyResume };
	// Engine thread, once per audio update: says what to do to the source lists.
	Action nextAction() {
		const bool w = m_wanted.load(), a = m_applied.load();
		if (w && !a) { m_applied.store(true); return ApplyPause; }
		if (!w && a) { m_applied.store(false); return ApplyResume; }
		return None;
	}

private:
	std::atomic<bool> m_wanted, m_applied;
};

}  // namespace gxaudio

#endif
