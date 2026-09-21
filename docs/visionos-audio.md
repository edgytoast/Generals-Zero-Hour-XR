# visionOS audio

Audio on Apple Vision Pro for the Zero Hour XR port: what runs, the spatial model for the tabletop, the C API the host uses,
the `AVAudioSession` decisions and the evidence for each, and what only a headset can prove.

Sources: `visionos/Audio/**`, the visionOS-gated block in
`Core/GameEngineDevice/Source/OpenALAudioDevice/OpenALAudioManager.cpp` (and its header), tests in `scripts/qa/vision-audio-*`.
Nothing here changes game rules and nothing changes desktop, Android or iOS behaviour: every engine edit is additive and inside
`#if defined(GX_PLATFORM_VISIONOS)`. Section 6.2 is the audio-session part of the recon note `visionos-api.md` (section 4, which was not available in
this worktree), with the evidence; the SDK header quotes are also in `visionos/Audio/GXXRAudioSession.h`.

## 1. Status

| Area | State | Evidence |
|---|---|---|
| OpenAL Soft (static, CoreAudio, `TARGET_OS_VISION` patch) on xrsimulator | Works **only with the semaphore fix in section 2** | `vision-audio-openal-test`: 63 passed, 0 failed (with the fix); aborts in `alcCreateContext` without it |
| Tone on the default device, device pause/resume | Verified in the simulator | same test, part A |
| Listener/source geometry, distance model, yaw | Verified numerically through `ALC_SOFT_loopback` | same test, parts B and C |
| FFmpeg decode chain (WAV PCM/ADPCM, OGG Vorbis, FLAC, MP3) | Verified in the simulator | `vision-audio-ffmpeg-test`: 32 passed, 0 failed |
| Board -> world listener transform, tabletop attenuation, API state | Verified on the host, cross-checked against the real `xrWorldToBoard()` | `vision-audio-listener-test`: 71 passed, 0 failed |
| `AVAudioSession` category, spatial experience, activation | Verified in the simulator and inside a real app process | `vision-audio-session-test`: 36 passed, 0 failed, plus the scratch app launch (section 6.4) |
| Interruption, route change, media-services reset, background, scene phase | Logic verified by posting the real notification names with real `userInfo`; real system-generated events need a device | same test |
| Engine integration (`gxApplyHostAudio`, source retuning, C API in the engine object) | Compiles for xrsimulator with the engine's real flags and exports all 18 `GXAudio_*` symbols; **not run**: no game data, no engine host yet | section 9 |
| How it sounds (spatial impression, HRTF on the audio pods, `.bypassed` vs `.fixed`) | **Device only** | section 10 |

## 2. Blocker found and fixed: OpenAL Soft aborts on visionOS (`alsem.h`)

`common/alsem.h` in openal-soft 1.24.2 uses libdispatch semaphores only for `TARGET_OS_IOS || TARGET_OS_TV` (or a macOS minimum
version above 10.5). On visionOS neither holds, so the header falls back to POSIX `sem_init`, which Darwin does not implement.
The first `al::semaphore` (created in `ContextBase::ContextBase`, so in `alcCreateContext`) throws
`std::system_error(EAGAIN)` and the process aborts:

```
THROW NSt3__112system_errorE
  _ZN2al9semaphoreC1Ej
  _ZN11ContextBaseC2EP10DeviceBase
  _ZN10ALCcontextC2E...
  alcCreateContext
Child process terminated with signal 6: Abort trap
```
(`nm libopenal.a` shows `U _sem_init`; the log is `scratchpad/au/vision-audio-openal-test-UNPATCHED-openal.log`.)

The fix is one condition: add `|| TARGET_OS_VISION` (`scripts/qa/vision-audio-openal-alsem.patch`). With it the archive references
`_dispatch_semaphore_create` and everything below passes. **`cmake/patches/openal-soft-1.24.2-visionos.patch` (package A) must
receive this hunk**; until then the engine build produces a `libopenal.a` that crashes at audio start-up.
`scripts/qa/vision-audio-build-openal.sh` builds a corrected `libopenal.a` from the already fetched source for the tests.

## 3. Spatial model for the tabletop

### 3.1 How the engine positions sounds

* Positional sound effects (`AudioEventRTS::isPositionalAudio()`): `playSample3D` puts the source at the object's game position,
  `AL_POSITION = (x, y, z)` in **game units**, Z up; every frame `processPlayingList` refreshes it. `AL_SOURCE_RELATIVE` is false.
  Per source: `AL_REFERENCE_DISTANCE = MinDistance`, `AL_MAX_DISTANCE = MaxDistance` (or the global min/max range for `ST_GLOBAL`
  events), `AL_ROLLOFF_FACTOR = 0.5`, model `AL_INVERSE_DISTANCE_CLAMPED` (`OpenALAudioManager::init`).
* The listener (`AudioManager::update`, `GameAudio.cpp`) is a "microphone": the point on the line from the ground look-at point to the
  game camera, at `MicrophoneDesiredHeightAboveTerrain` or at most `MicrophoneMaxPercentageBetweenGroundAndCamera` of the way to
  the camera; orientation is the camera forward vector. `setDeviceListenerPosition` sends it to OpenAL with up = +Z.
* Non-positional sounds (UI, music, speech, multichannel fallbacks) are `AL_SOURCE_RELATIVE = TRUE` (or streams) and are never attenuated.

On the tabletop this listener is wrong in two ways: the game camera is not where the user's head is, and the game's ranges
(hundreds of units) are meaningless at table scale: with the head 1 to 2 metres from a battlefield that is 2000 units wide, the head
is *far outside* every source's maximum distance, so with the original parameters every sound collapses to the same clamped level
(measured below: a constant 0.500 gain, 6 dB down) and carries no distance information.

### 3.2 Conventions

* **Game world**: game units, Z up, terrain plane XY. Sources keep these coordinates. Unchanged.
* **Board space** (what the host passes): metres, origin at the centre of the board (the world point `centerWorld` that
  `xrWorldToBoard` puts there), +X right, +Y toward the far side of the board (the map's "up"), +Z out of the board. This is the
  local frame of `XrSurface.pose`: `xrPoseInverse(board.pose)` applied to a room-space point gives board-space metres.
* **Mapping** = inverse of `xrWorldToBoard` (`GeneralsMD/Code/Main/XrWorld.h`): with `(rx, ry)` the unit map "right" vector,
  board X axis = world `(rx, ry, 0)`, board Y axis = world `(-ry, rx, 0)`, Z = world Z: a proper rotation (no mirroring).
  `metersPerWorldUnit = board.width / span` (`span` = `s_worldSpan`, world units across the board width).
  `vision-audio-listener-test` checks this against the real `xrWorldToBoard` for 300 random frames.

### 3.3 Listener

When the host listener is active the AL listener is the user's head: position `boardPointToWorld(head)` in game units,
orientation from the head forward/up expressed through the board axes, and `AL_METERS_PER_UNIT = metersPerWorldUnit`. Sources are
untouched. Active means: host enabled **and** the "Spatial battlefield audio" switch on **and** a pose received since the host was
enabled. Otherwise the original microphone code runs unchanged (the visionOS block returns early only when active).

Numerics (loopback, `vision-audio-openal-test` part C; head 0.9 m in front of a 0.8 m board, 0.55 m up, looking at the centre):

```
board frame yaw +37 deg -> listener world pos (2354.1, -796.9, 1390.0) forward (-0.514, 0.681, -0.521)
  left-far   d=1.257 m  L=0.2484 R=0.1337  ILD=+5.38 dB
  right-far  d=1.257 m  L=0.1337 R=0.2484  ILD=-5.38 dB
  ... identical L/R for board-frame yaws 0, 37, 90, 180 and -121 degrees (0.00 dB difference)
```

Rotating the map view (the game camera yaw) changes the world coordinates of everything but not what the head hears, because
sources and listener move together through the same mapping: that is the "yaw invariance" the test asserts.

### 3.4 Attenuation for table scale

Physical reasoning, in metres (`TabletopTuning`, defaults in `GXAudioListenerMath.h`):

* Any table point is between about 0.3 and 1.6 m from the head (0.6 to 1.2 m board, seen from 0.4 to 2 m).
* `AL_REFERENCE_DISTANCE = 1.0 m`: at or inside it, full level. `AL_ROLLOFF_FACTOR = 0.35`, `AL_MAX_DISTANCE = 8 m`
  (level stops falling; a sound never becomes silent, matching the clamped model the engine already uses).
* Measured spread across the table: gain 1.000 at 0.3 to 1.0 m down to 0.826 at 1.6 m, that is **1.66 dB** (loopback: 0.2978 at
  0.5 m vs 0.2461 at 1.6 m). A battle across the table is audible at natural level and is panned by real geometry; walking away
  from the table (up to 8 m) still fades it gently.
* Per event: the event's own maximum range scales the values by `clamp(eventMax / 600 u, 0.5, 2)` so a big explosion carries
  farther than a footstep, without recreating the original numbers.
* Values are converted to game units with the board scale (`ref_units = 1.0 m / metersPerWorldUnit`), so the level for a given
  physical distance is independent of the board scale (unit-tested).

Original parameters for comparison (min 200, max 600 units, rolloff 0.5), head 1400 to 2700 units from the sources:
constant gain 0.500 across the whole table, i.e. flat and 6 dB down; the tabletop values replace that.

**The scale is a setting**: `GXAudio_SetTabletopAttenuation(refMeters, rolloff, maxMeters)` (values <= 0 restore the defaults) retunes
every playing source at the next audio update.

### 3.5 What stays as the original

* UI, music, speech, multichannel positional fallbacks: `AL_SOURCE_RELATIVE`, untouched (`gxApplyDistanceModel` and the retune loop
  skip relative sources).
* "Spatial battlefield audio" OFF: nothing of the above runs; `gxRetuneAllSources(false)` writes back exactly the values
  `playSample3D` sets (event min/max or global min/max, rolloff 0.5) for sources already playing.

## 4. C API (`visionos/Audio/GXAudioListener.h`, defined in the engine)

| Function | Purpose |
|---|---|
| `GXAudio_SetHostListener(bool)` | Host starts/stops supplying head poses (immersive space presenting). Enabling clears any old pose. |
| `GXAudio_SetBoardFrame(center[3], rightX, rightY)` | The numbers `xrWorldToBoard` was built from. |
| `GXAudio_SetListenerPose(scale, headPos[3], headFwd[3], headUp[3])` | Head in board space (metres) + `metersPerWorldUnit`. Call every frame on the engine thread before the engine frame. Invalid input is ignored. |
| `GXAudio_SetListenerPoseWorld(scale, pos[3], fwd[3], up[3])` | Same, but the head is already in game-world units (host holds the room->world mapping; also the ground-level observer view). |
| `GXAudio_SetSpatialBattlefieldAudio(bool)` / `Get` | The user switch. **Default ON.** OFF = original camera-relative behaviour exactly. |
| `GXAudio_IsHostListenerActive()` | Diagnostics. |
| `GXAudio_SetTabletopAttenuation(ref, rolloff, max)` | Attenuation scale in metres. |
| `GXAudio_SetBinauralMode(0 auto / 1 HRTF on / 2 off)` | OpenAL Soft HRTF (device reset applied on the engine thread; honoured at device creation when set earlier). |
| `GXAudio_SetMasterVolume` / `Get` | `AL_GAIN` on the listener. |
| `GXAudio_SetCategoryVolume(cat, v)` / `Get` | Music, speech, SFX 2D, SFX 3D (section 5). |
| `GXAudio_SetEffectsVolume(v)` | One Effects slider, using the same `Relative2DVolume` rule as the in-game options menu. |
| `GXAudio_EnginePause()` / `EngineResume()` / `IsEnginePaused()` | Lifecycle (section 7). Thread-safe. |
| `GXAudio_EngineReopenDevice()` | `alcReopenDeviceSOFT` in place, for media-services resets. |

Thread rules: everything is callable from any thread (mutex/atomics; volume, HRTF and pause changes are applied by the engine thread in
`update()`, except the device-level pause which is applied at once). Do not call after the engine is shut down.

### 4.1 Hook for the engine host (package C1; not done here)

In `GX_XR_BeginStereoWorld()` (`XrGameBoot.cpp`), after `xrPrepareWorldMapping()` succeeded and `head`/`position` are known
(the head-midpoint code already computes `position = xrInversePoint(room, head)` = the head in game units):

```cpp
// every frame, engine thread, before TheGameEngine->execute():
GXAudio_SetHostListener(true);   // once when the immersive space starts presenting; false when it closes
if (!s_worldFrame.observer) {
    const float centre[3] = {center.x, center.y, center.z};                 // the values passed to xrWorldToBoard
    GXAudio_SetBoardFrame(centre, axis.x, axis.y);
    // room-space head -> board-space metres: xrPoseInverse(s_worldFrame.board.pose) * head
    GXAudio_SetListenerPose(s_worldFrame.board.width / s_worldSpan, headBoard, forwardBoard, upBoard);
} else {   // ground-level view: no board, the room is the world
    GXAudio_SetListenerPoseWorld(1.0f / kXrObserverUnitsPerMetre, headWorld, forwardWorld, upWorld);
}
```
Head forward is the head orientation's -Z (OpenXR/visionOS convention, the same axis `camera[]` is built from); up is +Y.
The settings window calls `GXAudio_SetSpatialBattlefieldAudio(...)`; persist the choice in `AppStorage` (default true).

## 5. Sound categories and volumes

How the engine categorises sound (from `AudioEventInfo.h`, `INIAudioEventInfo.cpp`, `OpenALAudioManager.cpp`; retail INI data is not
on this machine, so which retail events fall in which INI block is **not verified**):

| Category | INI block / type | Path in `OpenALAudioManager` | Gain | Host category |
|---|---|---|---|---|
| Music | `MusicTrack` -> `AT_Music` | streamed (`OpenALAudioStream`) | `m_musicVolume` | `GXAUDIO_CATEGORY_MUSIC` |
| Speech / dialogue | `DialogEvent` -> `AT_Streaming` | streamed | `m_speechVolume` | `GXAUDIO_CATEGORY_SPEECH` |
| Sound effects, non-positional (UI, cameos, money, EVA/global stingers if declared without a position) | `AudioEvent` -> `AT_SoundEffect` | 2D sample, `AL_SOURCE_RELATIVE` | `m_soundVolume` | `GXAUDIO_CATEGORY_SFX_2D` |
| Sound effects, positional (weapons, explosions, engines, footsteps, world ambience) | `AudioEvent` -> `AT_SoundEffect` | 3D sample | `m_sound3DVolume` | `GXAUDIO_CATEGORY_SFX_3D` |

Per-event flags (`Type =`): `ST_UI`, `ST_WORLD`, `ST_SHROUDED`, `ST_GLOBAL` (global range instead of the event's), `ST_VOICE`,
`ST_PLAYER`/`ST_ALLIES`/`ST_ENEMIES`/`ST_EVERYONE` (who hears it); control flags `AC_LOOP`, `AC_RANDOM`, `AC_ALL`, `AC_POSTDELAY`,
`AC_INTERRUPT`; priorities `AP_LOWEST`..`AP_CRITICAL`. Per-event gain = event volume x volume shift x category volume.

Volume chain: `Options.ini` (`SFXVolume`, `SFX3DVolume`, `VoiceVolume`, `MusicVolume`, 0 to 100, read by `OptionPreferences`) ->
`AudioSettings::m_preferred*Volume` -> `AudioManager::init` sets the **system** volumes -> effective volume = script volume (mission
scripts, reset per mission) x system volume. The in-game options menu writes the same system volumes with
`setVolume(v, AudioAffect_X | AudioAffect_SystemSetting)`, one SFX slider feeding both SFX categories through `Relative2DVolume`.

The host API (section 4) uses exactly that `setVolume(... | AudioAffect_SystemSetting)` call from the engine thread, so script
volumes keep multiplying on top and no game rule is involved; `GXAudio_SetMasterVolume` is `AL_GAIN` on the listener and scales
everything (including music) independently. When the in-game menu and the host both change a volume the last writer wins, and
`GXAudio_GetCategoryVolume` always reports the engine's current system value.

## 6. `AVAudioSession` (`visionos/Audio/GXXRAudioSession.{h,mm}`)

### 6.1 Configuration

Category `playback`, mode `default`, no mixing (the game is the primary audio app; other apps are interrupted and the game pauses/
resumes through the interruption path), activated by `GXAudioSession_Configure`. Call it before the engine opens its device: OpenAL
Soft's visionOS backend is a `kAudioUnitSubType_RemoteIO` output AudioUnit (`coreaudio.cpp`) and never touches `AVAudioSession`, so
without the call the app is in the default `SoloAmbient` category (measured: `category=AVAudioSessionCategorySoloAmbient` before
`Configure`). `UIBackgroundModes` `audio` is deliberately not needed: the game pauses in the background.

### 6.2 Spatial experience: `.bypassed` (evidence)

Because the game supplies its own head-driven listener, the system must not spatialise on top of it.

1. **The system default is head tracked.** In the simulator, before any call:
   `intendedSpatialExperience(raw)=0` = `AVAudioSessionSpatialExperienceHeadTracked`. The system would therefore head-track the
   engine's stereo output as a sound stage and counter-rotate our head-driven listener (double spatialisation).
2. **SDK header** (`AVAudioSessionTypes.h`, xros 27.0): `AVAudioSessionSpatialExperienceHeadTracked` = "a fully head-tracked spatial
   experience parameterized by a sound stage size and anchoring strategy"; `...Fixed` = "an unanchored, non-head-tracked spatial
   experience parameterized by a sound stage size"; `...Bypassed` = "an experience that bypasses any system-provided spatialization
   and instead mixes the application's sound straight to the output".
3. **Only the session-level API applies.** `AVAudioOutputNode.intendedSpatialExperience` / `CASpatialAudioExperience` (visionOS 26) is
   for `AVAudioEngine` graphs; OpenAL Soft renders through a raw AudioUnit, so the per-session setting is the only control.
4. **Simulator**: `setIntendedSpatialExperience` accepts all three modes and reads them back (`2`, `1`, `0`), and an OpenAL Soft device
   opened afterwards plays under each (`vision-audio-session-test`). Inside a real app process (scratch shell build of the
   real `GeneralsZHXR` target with `visionos/Audio` added): `configure=1 category=AVAudioSessionCategoryPlayback sr=48000 ch=2
   route=Speaker:Speaker spatialRaw=2`.

Choice: **`.bypassed`, with OpenAL Soft's HRTF on** (`GXXRAudioHost_Start` sets `GXAudio_SetBinauralMode(1)`), so exactly one
binaural stage exists (ours). OpenAL's automatic HRTF selection is not reliable here: the "CoreAudio Default" device reported
`ALC_HRTF_STATUS_SOFT = 0` (disabled) on the simulator's speaker output.
Fallbacks, switchable at run time for an A/B listening test on the headset: `GXAudioSession_SetSpatialMode(GXAUDIO_SESSION_SPATIAL_FIXED)`
(medium stage) with HRTF off, or `HEAD_TRACKED` with `GXAudio_SetSpatialBattlefieldAudio(false)` (the original camera-relative
listener, system does the head tracking).
Whether `.bypassed` sounds better than `.fixed` on the audio pods cannot be judged here: section 10.

### 6.3 Lifecycle -> one pause/resume decision

Reasons form a bitmask (interruption 1, background 2, inactive 4, host request 8); pause on the first, resume when the last clears.
Callbacks fire only on real transitions, outside any lock, on the thread that delivered the notification.

| Input | Effect |
|---|---|
| `AVAudioSessionInterruptionNotification` began / ended | pause / resume. Ended resumes even without `ShouldResume` (a game has no play button; the game's own pause state still decides what is audible, section 7). |
| `UIApplicationDidEnterBackground` / `WillEnterForeground` | pause / resume (observed automatically) |
| `GXAudioSession_NotifyScenePhase(0/1/2)` (SwiftUI `ScenePhase`) | active clears background+inactive; background pauses; inactive does **not** pause unless `SetPauseOnInactive(true)` |
| `GXAudioSession_SetHostPause(bool)` | host reasons, e.g. immersive space closed (`GXXRAudioHost_SetImmersiveOpen`) |
| Route change | `GXAUDIO_SESSION_EVENT_ROUTE_CHANGED` event only (the AudioUnit follows the route); no pause |
| Media services lost / reset | lost: pause + event; reset: session rebuilt (category + spatial experience + activate), event -> host calls `GXAudio_EngineReopenDevice()`, resume |

Observed on the simulator: launching another app (Settings) over the game does **not** deliver `UIApplicationDidEnterBackground`
(visionOS keeps the app running in the Shared Space), so `scenePhase` and the immersive-space state are the inputs that matter;
only the launch-time `WillEnterForeground` was received in that run.

### 6.4 Tests (`scripts/qa/vision-audio-session-test.sh --udid <sim>`)

36 checks: category/mode/mixing read back, spatial experience set and read back for all three modes with an OpenAL Soft tone running
under each, idempotence, deactivate/activate, interruption began/ended (with and without `ShouldResume`) counting exactly one
pause and one resume, multi-reason arithmetic (background + interruption resumes only when both clear), scene-phase cases,
host pause, route change (event, no pause), media services lost/reset (rebuild, event, resume), counters. Notification names and
`userInfo` keys are the real ones; the system did not generate them.

## 7. Engine-side pause/resume

`GXAudio_EnginePause()` (any thread): pauses the output device immediately with `alcDevicePauseSOFT` (thread-safe; silence even if the
engine is not running frames) and records the request. On its next `update()` the engine thread runs `pauseAudio(AudioAffect_All)`;
`GXAudio_EngineResume()` resumes the device immediately and the engine thread runs `resumeAudio(gamePaused ? AudioAffect_Music :
AudioAffect_All)` -- the same policy as `SDL3GameEngine` for mobile (`SDL_EVENT_WILL_ENTER_BACKGROUND` / `DID_ENTER_FOREGROUND`):
returning into an open pause menu does not restart the battlefield. A pause that is resumed before the engine thread ran cancels
out (unit-tested). The device pause/resume behaviour is verified (source clock frozen for 300 ms and continues afterwards).

## 8. Host wiring summary (for packages C1 / F)

1. Early in launch, before the engine starts: `GXXRAudioHost_Start(GXAUDIO_SESSION_SPATIAL_BYPASSED, false)` (from `GXXRAudioHost.h`).
2. SwiftUI: `.onChange(of: scenePhase)` -> `GXXRAudioHost_SetScenePhase(0/1/2)`; immersive space opened/closed -> `GXXRAudioHost_SetImmersiveOpen`.
3. Engine host each frame: section 4.1.
4. Settings window: `GXAudio_SetSpatialBattlefieldAudio`, `GXAudio_SetMasterVolume`, `GXAudio_SetCategoryVolume` / `SetEffectsVolume`,
   optionally `GXAudio_SetTabletopAttenuation`; persist in `AppStorage`.
5. `visionos/project.yml` needs `visionos/Audio` (two lines, section 9.2).
6. No Info.plist audio entries are needed (no microphone, no background audio).

## 9. How to run the tests, files, project.yml lines

### 9.1 Tests

```
scripts/qa/vision-audio-listener-test.sh                       # host only, no simulator
scripts/qa/vision-audio-build-openal.sh                        # libopenal.a with the alsem fix -> prints its path
xcrun simctl create GXR-audio com.apple.CoreSimulator.SimDeviceType.Apple-Vision-Pro-4K com.apple.CoreSimulator.SimRuntime.xrOS-26-5
GX_OPENAL_LIB=<that libopenal.a> GX_OPENAL_INCLUDE=<openal-src>/include scripts/qa/vision-audio-openal-test.sh --udid <udid>
scripts/qa/vision-audio-ffmpeg-test.sh --udid <udid>            # needs the engine build's vcpkg_installed (GX_BUILD_DIR)
GX_OPENAL_LIB=... GX_OPENAL_INCLUDE=... scripts/qa/vision-audio-session-test.sh --udid <udid>
```
(`xcrun simctl create` with the plain `Apple-Vision-Pro` device type fails on this machine, use the `-4K` type. Delete the device
with `xcrun simctl delete <udid>` afterwards.)

FFmpeg test coverage: mirrors `FFmpegFile::open` / `decodePacket` and `OpenALAudioFileCache::decodeFFmpeg` call for call (custom AVIO
read callback, `avformat_open_input(NULL)`, `avcodec_send_packet` / `receive_frame`, planar->interleaved copy, no flush at EOF),
for WAV PCM s16 stereo (exact sample count, correlation 1.0000), WAV PCM u8, WAV IMA ADPCM (the retail SFX format; +1622 samples
of block padding, correlation 0.9998), OGG Vorbis (+26 samples), FLAC, and a synthetic MP3 (40 valid frames decode to exactly
46080 = 40 x 1152 samples, `mp3float`, fltp -> `AL_FORMAT_STEREO_FLOAT32`). The vcpkg FFmpeg has MP3 *decoding* only (no encoder), so
the MP3 input is generated frame headers around silent payload; real music needs the checklist below. Not flushing the decoder at EOF
loses no samples for any of these codecs in this build (flush adds +0).

### 9.2 Files and the `project.yml` addition

`visionos/Audio/`: `GXAudioListener.h` (C API), `GXAudioListenerMath.h` (pure math), `GXAudioHostState.h` (state holders),
`GXXRAudioSession.{h,mm}`, `GXXRAudioHost.{h,mm}`. The engine includes the two header-only files by relative path
(`../../../../visionos/Audio/...`), so no CMake change is needed.

`visionos/project.yml` (owned by package D1/C1; add under `targets.GeneralsZHXR`): in `sources:`
```
      - path: Audio
```
and in `HEADER_SEARCH_PATHS`:
```
          - $(SRCROOT)/Audio
```
`GXXRAudioHost.mm` references the engine's `GXAudio_*` symbols, so the app must link the engine (or exclude that one file while the
engine is not linked, as the scratch verification build did).

## 10. What only a device (or real data) can prove

* Spatial impression: HRTF over the Vision Pro audio pods, `.bypassed` vs `.fixed`, whether OpenAL Soft's default HRTF suits the
  pods, perceived level at table scale, the `0.35` rolloff and `1.0 m` reference (all are runtime settings for that reason).
* Real interruptions (Siri, calls, other app audio), real route changes (pods <-> Bluetooth), `mediaserverd` reset, Shared Space
  audio ducking.
* Whether `alcReopenDeviceSOFT` is sufficient after a media-services reset.
* Retail asset behaviour (formats, ranges): no game data on the build machine.
* Latency/underruns of the RemoteIO unit under load (the simulator's default device is the Mac's).

### Real-data checklist (music, voices, effects)

With the user's own Generals + Zero Hour data installed and the engine hosted:
1. Menu music plays (MP3 through FFmpeg): starts, loops, changes track; in-game battle music. Music volume slider works, master at 0 mutes.
2. Unit voice lines (select/move/attack) and EVA/mission dialogue: audible, not cut off, respect the Voice volume; `disallowSpeech` self-heals.
3. Effects: click a unit, fire weapons, explosions, engines, construction: 3D sounds come from the unit's place on the board; walk around
   the table and hear them move; lean toward one end: it gets louder, the far end gently quieter; nothing silent, nothing mono.
4. Turn the map (game camera yaw): sounds stay attached to their units (yaw invariance).
5. "Spatial battlefield audio" OFF: back to the original camera-relative mix; toggling mid-battle retunes playing sounds without a restart.
6. Close the immersive space / background the app / trigger an interruption: silence at once; return: continues (paused battle stays
   quiet, pause-menu music resumes).
7. UI/menu clicks and money ticks are unattenuated.
8. Check the log for `Failed to open ALC device`, `Failed 'avcodec_*'`, `Audio Cache is full` and missing-file lines.
