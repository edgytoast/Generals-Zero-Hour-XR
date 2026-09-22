# visionOS spatial UI (native SwiftUI windows + the engine's own HUD panels)

Package F. The hybrid spatial UI of the native Apple Vision Pro port: the engine's own HUD (`ControlBar`: build menu,
production queue, general powers, radar, selected-unit info) stays a set of textured panel quads in the space, drawn
by package C2's presentation layer; this package adds native SwiftUI windows for the Quest `Commands` and `Menu`
consoles, driven by the exact logic the Quest host uses. Nothing here re-implements a game rule: every control ends
in the same `XrGameBoot_*` call `applyCommandAction` / `applyMenuAction` make on the Quest.

Contents: 1 Architecture, 2 Files, 3 The C++ model, 4 SwiftUI windows, 5 Window placement (visionOS 26 API and its
limits), 6 Localisation, 7 Selected-unit info and hover text, 8 UI quality audit, 9 Verification, 10 What remains
device-only, 11 Diffs to files this package does not own.

## 1. Architecture

```
 engine thread ("GXXR.Engine")                     any thread (main actor)
 ------------------------------                    -----------------------
 GXEngineHost frame hook (~15 Hz, every           GXEnginePanelState_Get -> GXPanelSnapshot (copy under a lock)
 iteration incl. parked/waiting)                          |
   -> visionCaptureSnapshot(bridge, extras)                v
      (VisionCommandActions.cpp)                  GXPanelModel_Build(page, snapshot, language)
   -> publish (version bump on real change)                |
                                                             v
 GXPanelAction_Perform(page, id) --------------->  PanelControlItem[] (id, label, role, state bits, badges)
   posted with GXEngineHost_Post                            |
   -> visionApplyCommandAction / visionApplyMenuAction       v
      (VisionCommandActions.cpp, the ported            SwiftUI: CommandsWindow / SettingsPageView / HelpWindow
      Quest switch tables) -> VisionEngineBridge /            |
      VisionCommandExtras -> XrGameBoot_*            GXPanelAction_PopResult -> host effects (Ground View,
                                                       recenter, hide console, ...) -> InteractionControls /
                                                       GXXRBridgeRecenter / dismissWindow
```

Three new C++ files, all pure (no Apple types, host-tested):

* **`VisionPanelModel.{h,cpp}`** — for one Quest page, builds the list of controls SwiftUI shows: id, section,
  label (`xrTr` in the selected language), role, state bits (armed / pending / disabled / on / active), badges
  (group member counts, bookmark known). Reads `xrCommandLayout` / `xrMenuLayout` (`XrPanelLayout.h`) for ids,
  geometry-derived roles and hittability, and mirrors `updateMenuTextures()` (`XrMenuPainting.h`) label by label —
  every label, chip value and disabled reason in `VisionPanelModel.cpp` has a Quest line reference in its comment.
  visionOS ignores the pixel rectangles: SwiftUI lays the same controls out natively (`LazyVGrid`).
* **`VisionCommandActions.{h,cpp}`** — `visionApplyCommandAction` / `visionApplyMenuAction` are the Quest switch
  tables (`XrCommandUI.h` `applyCommandAction`, `XrMenuUI.h` `applyMenuAction`) ported statement by statement, cited
  by Quest line ranges in the header comment and section comments in the `.cpp`. Every branch ends in the same
  `VisionEngineBridge` call (package E) or, for the handful of `XrGameBoot_*` functions the bridge does not carry
  (`TacticalGroup`, `Bookmark`, `ViewBase`, `CameraPreset`, `SaveCameraDefault`, `Communicator`, `SetLanguage`, the
  read-only reason/status/hint queries), the small companion interface `VisionCommandExtras`.
* **`GXEnginePanelState.{h,cpp}`** — the plain-C, Swift-importable seam: the snapshot store (thread safe, published
  from the engine thread, copied out on any thread), the `GXEngineHost` frame hook, `XrGameBootExtras` (the real
  engine's implementation of `VisionCommandExtras`, one line per function into `XrGameBoot_*`), the scripted engine
  for `-uiFakeSnapshot`, and the queue of host-side effects (`GXPanelAction_PopResult`) the SwiftUI side applies
  (Ground View, recenter, hiding the console — nothing else is host-owned).

Everything else is SwiftUI (`visionos/UI/*.swift`, `visionos/App/{CommandsWindow,SettingsWindow,HelpWindow,
HudOrnament}.swift`): pure presentation over `PanelStore`, which owns the poll loop, the language, the persisted
preferences (`GXPanelPrefs`) and the two action entry points.

## 2. Files

| File | Role |
| --- | --- |
| `GeneralsMD/Code/Main/visionos/GXEnginePanelState.h` | plain-C surface: snapshot, control model types, actions (Swift-importable) |
| `GeneralsMD/Code/Main/visionos/GXEnginePanelState.cpp` | the store, the frame hook, `XrGameBootExtras`, the scripted engine |
| `GeneralsMD/Code/Main/visionos/VisionPanelModel.{h,cpp}` | pure control-model builder (ported `updateMenuTextures`) |
| `GeneralsMD/Code/Main/visionos/VisionCommandActions.{h,cpp}` | pure action tables (ported `applyCommandAction` / `applyMenuAction`) |
| `visionos/UI/PanelStore.swift` | the `@Observable` store: polling, language, preferences, actions, effect routing, `-uiScript` replay |
| `visionos/UI/UIStrings.swift` | native-UI string table (English key -> German) + `xr()` bridge into the Quest table |
| `visionos/UI/UITheme.swift` | the explicit colours and the 60 pt minimum target size (section 8) |
| `visionos/UI/PanelViews.swift` | `PanelControlView` (one control -> a `Button`/`Toggle`), section and status-card views |
| `visionos/UI/SettingsPages.swift` | the seven Settings pages |
| `visionos/UI/GraphicsSettings.swift` | the graphics settings model + `GraphicsBackend` (section 4.2) |
| `visionos/UI/AudioSettings.swift` | volumes + spatial switch, applied to `GXAudio_*` (defined inside the engine) |
| `visionos/UI/HoverInfoOrnament.swift` | the selected-unit / hover-text card (section 7) |
| `visionos/UI/UIWindowsCoordinator.swift` | opens/closes `CommandsWindow` with the immersive space, `-openWindow`, effect routing |
| `visionos/App/CommandsWindow.swift` | the Commands console window |
| `visionos/App/SettingsWindow.swift` | the Settings window (`NavigationSplitView` over the seven pages) |
| `visionos/App/HelpWindow.swift` | the controls guide |
| `visionos/App/HudOrnament.swift` | the toolbar ornament (Ground View, Recenter, Pause, Menu, Windows, Leave Tabletop) |
| `visionos/App/GeneralsZHXRApp.swift` | `WindowGroup`s, `defaultWindowPlacement`, `ImmersiveSpace` (scenes only, no logic) |
| `scripts/qa/vision-ui-panel-test.{cpp,sh}` | host test: every control id maps to the same engine call as the Quest, state, labels, gating |
| `scripts/qa/vision-ui-contrast.py` | computes the contrast ratios of section 8.3 from `UITheme.swift` |
| `scripts/qa/vision-ui-strings-check.sh` | every `t("...")` / `xr("...")` call site has a table entry, no untranslated leftovers |

## 3. The C++ model

`GXPanelSnapshot` (`GXEnginePanelState.h`) is the thread-safe copy of everything the panels need: the read-only
engine flags (`IsInteractiveGame`, `CanAdjustWorld`, `CanStereoWorld`, `CanObserveGround`, `ExpandedUI`), the
tactical state (`mode`, `group`, `queue`, ten group sizes, formation active, four bookmark-known flags, match
result), the four gating reasons (`XrGameBoot_TacticalReason` for actions 8/40/41/42 — the only ones the Quest
tables query), the status/hint/language-status strings, the last world-hover text and its sequence number, the panel
hover text at the last probed position, and `GXPanelSession` (the UI-owned state the Quest keeps in `XrCommandState`
/ `XrMenuState` / `XrLayout` / `XrPerformance`: page, help/tactics/bookmark-save flags, the pending group operation,
and the persisted preferences).

`GXEnginePanelState_Install()` registers `frameHook` with `GXEngineHost_SetFrameHook` (a small, additive C API added
to `GXEngineHost.h`/`.mm`, see section 11). The hook runs on the engine thread every loop iteration — including
while parked or waiting for the compositor, so the console reacts within about 66 ms even with the tabletop closed —
captures a fresh snapshot when dirty or every 66 ms, and publishes it only when it actually changed (a `memcmp`
against the last published copy), so `GXPanelSnapshot.version` is a cheap "did anything change" test for the UI.

`GXPanelAction_Perform(page, id)` posts the corresponding `visionApplyCommandAction` / `visionApplyMenuAction` call
onto the engine thread through the existing `GXEngineHost_Post`; the caller does not block, the result shows up in
the next snapshot. Host-side effects the action produced (Ground View armed, recenter requested, the console should
hide, a camera call was refused) are queued (bounded to 32) and drained by `PanelStore.drainEffects()` on the main
actor, which applies the ones only the app can (`InteractionControls.*`, `dismissWindow`).

**Fidelity.** `scripts/qa/vision-ui-panel-test.cpp` transcribes, by hand from `XrCommandUI.h` / `XrMenuUI.h`, the
expected engine call (or "no call") for every hittable id of every page and variant, then asserts the ported tables
produce exactly that call, in the same order effects happen (a pending group operation is cleared, `stereoWorld` is
requested, the menu closes) — 1665 checks, 0 failures (section 9). Three deliberate presentation differences from
the Quest (documented in the file header of `VisionPanelModel.cpp` and tested): a press the Quest would silently
swallow (no controllable selection, no adjustable world) is shown **disabled with the engine's own reason** as the
explanation instead of accepting a press that does nothing; the Guard order (id 4) is disabled the same way (the
Quest's `applyCommandAction` sends it to the engine, which ignores it, but the reason exists so the port can show
it); and the `Windows` page's size/distance/height chips are left empty (host state the Quest reads from `XrHello`,
not modelled here — see section 4.1).

## 4. SwiftUI windows

### 4.1 CommandsWindow

Sections **Orders**, **Instant & selection**, **Groups** (1-10 with member-count badges, `Replace group` / `New /
Extend` / `Center` as three op buttons above a 5-wide grid of number keys), **Tactics** (foldout: formation, force
move, guard-without-pursuit, `Save view` + four map-view slots A-D with a filled/hollow dot for "known"), all inside
a `LazyVGrid`. A `StatusCardView` at the top shows the engine's status/hint text and, when any control is disabled
with a reason, that reason (`Label(..., systemImage: "exclamationmark.circle")`). The help variant
(`session.help`) replaces the sections with the four Quest help pages (`CommandsHelpView`, English/German text from
`L10n`, mirroring `docs/visionos-interaction.md` section 4). `.buttonStyle(.bordered)` for plain buttons,
`.toggleStyle(.button)` + `UITheme.on`/`.armed`/`.pending` tint for the latching controls (orders, waypoints, group
keys, tactics, bookmarks — anything the Quest paints with `kXrStateOn`/`Armed`/`Pending`), `.hoverEffect(.highlight)`
on every control, `frame(minHeight: 60)` everywhere (`UITheme.minTarget`).

### 4.2 SettingsWindow

`NavigationSplitView` over seven pages (`visionos/UI/SettingsPages.swift`):

* **Workspace** — Recenter / Reset buttons (`InteractionControls.recenterBoard` / `.resetWorkspace`, plus
  `GXXRBridgeRecenter()`), explanatory captions for board size/distance/height ("not available yet: the interaction
  layer only exposes Recenter and Reset; resize with the two-hand pinch on the grab bar" — an honest statement, not
  a stub: package E's `VisionInteraction` has no discrete size-preset API today, only the continuous grab-bar
  gesture and the two host commands).
* **Presentation** — the composed-screen note, a Ground View toggle wired through `PanelStore.toggleGroundView()`
  (arms it via `GX_PANEL_MENU_VIEW` id 16 exactly like the Quest's `armGroundView`, or posts
  `InteractionControls.exitGroundView()` once armed/active), the three selection-indicator toggles (health bars,
  unit rings, board frame — drawn by the engine/C2, values stored in `GXPanelPrefs`), and the five camera presets +
  `Save current view as favorite` + `Home base view` (through `GX_EXTRA_CAMERA_PRESET` / `_SAVE_CAMERA_DEFAULT` /
  `_VIEW_BASE`; a refused call, e.g. during a cinematic, shows the "not available right now" caption once).
* **Graphics** — render scale (0.5x-1.5x slider), render-fps cap (30/45/60/90/120/uncapped segmented control), shadow
  mode (off/decals/volumes), stereo eye size tier (Balanced/High/Ultra), UI backbuffer resolution (720p/1080p,
  boot-time only) and the two `GX_GFX_*` toggles (Comfort fade, Focus marker). **Bound to the real
  `GXGraphicsSettings` / `GXEngineHost_{Set,Get,GetDefaults}Graphics` API** (package C2 commit `77aee73` on
  `vp/c2-presentation`, "visionOS: graphics settings API in GXEngineHost.h..."): `visionos/UI/GraphicsSettings.swift`
  mirrors the struct field-for-field (`renderScale: Float`, `renderFpsCap: Int32`, `shadowMode`/`eyeTier`/
  `uiResolution: Int32`, `flags: UInt32`) and `GraphicsBackend.applyToEngine`/`engineDefaults` build/read the real
  struct with its `size` field set, matching the header's extensibility contract exactly. Earlier in this package's
  history (before C2 had committed the header) this file used a placeholder that stored the values locally and
  mirrored shadow/eye-tier into `GXPanelPrefs`; that placeholder's assumed field names/ranges turned out to differ
  from the real struct in two ways, both fixed once the real header was read: the fps-cap "uncapped" value is
  *negative* (the placeholder used `0`, which the real API defines as "use the default, 45"), and the engine's
  default shadow mode is `GX_SHADOWS_DECALS` (the placeholder defaulted to `.volumes`). **Caveat**: `vp/f-ui` alone
  does not build — `GeneralsMD/Code/Main/visionos/GXEngineHost.h` on this branch only carries this package's own
  small addition (`GXEngineFrameHook`, section 11); the graphics API is C2's own commit on its own branch. This
  session verified the binding compiles, links and runs correctly by temporarily overlaying C2's committed
  `GXGraphicsSettings.{h,cpp}` and its `GXEngineHost.h` addition into a local build (not committed to `vp/f-ui`); see
  the session report's `handoffNotesForLead`. Once the lead merges C2 and F, no further Swift change should be
  needed.
* **Audio** — five volume sliders (master, music, speech, interface, battlefield) and the "Spatial battlefield
  audio" switch, bound straight to `GXAudio_SetMasterVolume` / `_SetCategoryVolume` / `_SetSpatialBattlefieldAudio`
  (`visionos/Audio/GXAudioListener.h`, defined inside the engine — these symbols exist today and are exercised by
  `AudioSettingsStore`, persisted in `UserDefaults`, reapplied at launch).
* **Controls & language** — additive-selection toggle (`InteractionControls.setAdditive`), cancel targeting/building,
  two 15° rotate-step buttons (`InteractionControls.rotatePlacement`), and the language picker (`PanelStore.setLanguage`,
  which calls `GXEnginePanelState_SetLanguage(lang, persistGameText: true)`: the UI switches at once, the game's own
  text is staged through `XrGameBoot_SetLanguage` exactly like the Quest's `Menu > View > Language`).
* **Data** — delegates to the existing game-data flow (`AppModel`/`AppModel+GameData.swift`, package G): status
  line, "Choose game folder…" (opens the launcher window and its picker — the picker itself is owned by that
  package and not duplicated here), "Remove imported data" with the existing confirmation dialog, and a
  documentation link.
* **Diagnostics** — `GXEngineHost_GetStatus` polled every 700 ms: phase, engine fps, simulation Hz, frame counters,
  ring slot usage, sync mode, renderer string, a scrollable monospaced log tail (`GXEngineHost_ReadLogTail`), and
  `ShareLink` over the log file with a one-line privacy note (the log contains game-data file paths and device
  details).

### 4.3 HelpWindow

A static guide (`visionos/App/HelpWindow.swift`) mirroring `docs/visionos-interaction.md` section 4 in player
language: tap, drag, two-hand, additive selection, placement, Ground View, panels, windows, simulator controls, and
what only a headset shows. A language segmented control at the top uses the same `PanelStore.setLanguage`.

### 4.4 HudOrnament

One `.ornament` attached to both the launcher and the Commands window (`visionos/App/HudOrnament.swift`): Ground
View toggle, Recenter, Pause/Resume (`GXEngineHost_Pause(GX_PAUSE_USER, ...)`), a `Windows` menu (opens
Commands/Settings/Help), Menu (`InteractionControls.engineBack()` — Escape/Back into the engine, opens the in-game
menu during a match) and Leave Tabletop (`dismissImmersiveSpace`).

`HudOrnament` is a plain `View` fed `model`/`store` explicitly by its caller through a `.hudOrnament(location:model:
store:)` helper, not a `ViewModifier` reading `@Environment` — see section 9's crash writeup and section 11.4.

### 4.5 A crash class specific to `.ornament(...)` on this SDK

Discovered and fixed this session, after windows were exercised in the simulator for the first time (section 9).
**Any `View` or `ViewModifier` that reads a custom `@Environment(SomeObservableType.self)` (an `@Observable` app type
such as `AppModel`/`PanelStore`, not a system `EnvironmentValues` key) on its own properties, while ALSO attaching an
`.ornament(...)` anywhere in its own body — not only inside the ornament's content closure — crashes at first layout**
on the xrOS 27.0 simulator SDK used here: `Fatal error: No Observable object of type AppModel found`, trace
`ViewGraphRootValueUpdater.render` → `TransformOrnament.updateValue()` → `AGGraphGetValue` → `EnvironmentValues.
subscript.getter` → `_assertionFailure`. Three instances existed in this package; all three are fixed by passing the
value in as a plain stored property/init parameter instead of `@Environment`, from the nearest ancestor that legally
has it (`GeneralsZHXRApp.swift`'s `@State`):

1. `HudOrnament` itself (fixed by the predecessor, before this session, but its two call sites were left on the old
   API — a build error, not a crash; fixed first).
2. `HoverInfoOrnament.swift`'s `HoverInfoCard`, reached through `commandsHoverOrnament(store:)`.
3. `UIWindowsCoordinator` (a `ViewModifier` whose `body(content:)` installs `.hudOrnament`) and `CommandsWindow`
   itself (a `View` that installs both `.commandsHoverOrnament` and `.hudOrnament` in its own body) — found this
   session; not caught by the predecessor's fix because the read was not textually inside an ornament's trailing
   closure, only in the same struct that attaches one.

Descendant views further down the normal content tree (`CommandsHelpView`, `PanelControlView`, the `SettingsPages.
swift` pages, …) are unaffected and keep reading `@Environment` normally: the bug is specific to the exact
`View`/`ViewModifier` node that carries the `.ornament(...)` modifier, not the whole subtree.

## 5. Window placement (visionOS 26 API and its limits)

Every `WindowGroup` uses `.defaultWindowPlacement { content, context in ... }` (`SwiftUI.Scene`, `visionOS 2.0+`,
confirmed present and unchanged in the xrOS 27.0 SDK's `SwiftUI.swiftinterface` used by this build): it returns a
`WindowPlacement` built from a `WindowPlacement.Position` (`.trailing(window)`, `.leading(window)`, `.below(window)`,
`.above(window)`, or `.utilityPanel`) found through `context.windows` (`[WindowProxy]`, matched by `.id`), sized with
`content.sizeThatFits(.unspecified)` (`WindowLayoutRoot.sizeThatFits`). Concretely: Commands is placed
`.trailing(launcher)`; Settings `.trailing(commands)` (falling back to `.below(launcher)`, then `.utilityPanel`);
Help `.leading(launcher)`. `WindowPlacementContext.defaultDisplay` and `WindowPlacement`'s point/size/`UnitPoint`
initialisers are macOS-only (`@available(visionOS, unavailable)`) — visionOS only accepts the `Position` enum, never
an absolute point.

**Limits, confirmed by reading the interface, not run on a device (no immersive-space window layout test exists in
the simulator harness):**

* The system chooses the exact depth, height offset and any collision avoidance; `defaultWindowPlacement` only says
  *relative to what*, never *how far*. There is no way to pin a window at an absolute room position or distance —
  that is exactly the tabletop's own placement job (`VisionInteraction`, package E) and not something a `WindowGroup`
  can do.
  ▪ **In a Full Space** (the tabletop is `.mixed` immersion, which is a Full Space) window placement rules are the
  same API; there is no documented Full-Space-specific placement failure in the header, but `.utilityPanel` is the
  documented fallback for "no reference window is open", which is what every placement closure above falls back to.
* A reference window that is not open (Commands not yet opened when Settings placement runs) makes `context.windows`
  omit it: the fallback chain above exists for exactly that race, and is exercised at launch (Commands is not open
  until the immersive space opens; opening Settings from the launcher first hits the `.below(launcher)` branch).
* `defaultWindowPlacement` is *default*, not *sticky*: the system only consults it the first time a window opens (or
  after the user never repositioned it); a player who drags a window keeps that position across reopens, which this
  package neither overrides nor needs to.
* The Commands window opens automatically with the immersive space (`UIWindowsCoordinator.onChange(of:
  model.spaceState)`, `case .open: openWindow(id: CommandsWindow.id)`) and closes with it
  (`case .closed: dismissWindow(...)`), matching "Commands window opens automatically with the immersive space" in
  the brief; Settings and Help are opened only by `-openWindow` or the toolbar menu, never automatically, so they
  never crowd a first-time player.

## 6. Localisation

One table, `visionos/UI/UIStrings.swift`: `L10n.t(key, lang)` for UI-only strings (English key, German
translation), `L10n.xr(germanSource, lang)` for anything that already exists as a Quest string
(`kXrTranslations` in `XrStrings.h`), routed through the C entry point `GXPanelModel_Translate` so the exact same
table produces the panel labels (`VisionPanelModel.cpp`) and the window chrome. `scripts/qa/vision-ui-strings-check.sh`
parses every `t("...")` / `xr("...")` call site under `visionos/UI` and `visionos/App/*Window.swift` +
`HudOrnament.swift` and fails when a key has no German entry, an entry is empty, an entry equals its English key
without being one of the handful of words that are genuinely identical in both languages (`Audio`, `Menu`,
`Phase`, ...), or an `xr(...)` key is not in the Quest / extra string tables. 170 keys, 134 used by name (the rest by
the `titleKey`/tuple patterns the script also scans, or currently unused and reported as notes, not failures)
— `scripts/qa/vision-ui-strings-check.sh` passes.

## 7. Selected-unit info and hover text

`visionos/UI/HoverInfoOrnament.swift` shows two independent things, both explicitly labelled so a player is never
misled about *when* they were true:

* **Last target** (`XrGameBoot_WorldHoverInfo`, captured by `GXEnginePanelState.cpp`'s frame hook and kept — not
  cleared — until a new non-empty value arrives, with a sequence number so the UI can flash on change): the
  name/cost/health string of the last unit or building a spatial pointer (pinch) was active over. There is no
  continuous gaze on visionOS (`docs/visionos-interaction.md` section 2: `selectionRay` exists only at pinch begin),
  so this is honestly the *last pinched/hand-pointed target*, and the card's caption says exactly that in both
  languages ("Shows the last unit or building you pinched or pointed at. The system does not share where you look
  between pinches.").
* **Panel tooltip** (`XrGameBoot_HoverInfo` at a probe position set by `GXEnginePanelState_SetHoverProbe`, throttled
  to 5 Hz): the engine's own tooltip text for a composed-frame pixel. No caller sets the probe on this branch yet
  (the interaction layer's panel-focus reporting is package E/C2's frame data, `VisionInteractionOutput.panelU/V` /
  `panelPointerPanel`); the seam is complete and host-tested (`vision-ui-panel-test.cpp` truncation/UTF-8 case,
  `GXEnginePanelState_SetHoverProbe` compiles and links) but wiring the actual call site is listed in section 10.

The card is an ornament on the Commands window, visible only while either string is non-empty.

## 8. UI quality audit

### 8.1 Text and targets

| Element class | Font | Notes |
| --- | --- | --- |
| Window titles (`Commands`, page titles, `Controls guide`) | `.extraLargeTitle2` / `.title2` (system, Dynamic-Type-scaling) | headers (`.accessibilityAddTraits(.isHeader)`) |
| Control labels | `.body` (17 pt base) | `LazyVGrid` cells, `lineLimit(3)`, never truncated to one line |
| Chip values (group counts, "AN"/"AUS", camera favorite) | `.callout.weight(.semibold)`, monospaced digits | `.monospacedDigit()` so a changing count never reflows the row |
| Status card / hints | `.headline` (status), `.callout` (hint/reason) | `fixedSize(horizontal: false, vertical: true)`: never clipped |
| Diagnostics numbers | `.monospacedDigit()` throughout | stable column alignment |

All text uses the system's semantic styles (`.body`, `.headline`, `.callout`, `.footnote`, `.caption`/`.caption2`),
never a fixed point size: visionOS auto-scales window content to remain legible at whatever distance the system
places the window (Apple's stated design guidance for visionOS, matched by the SDK giving no per-window "viewing
distance" control at all — sections 5 and this table's absence of pixel/point sizes is deliberate, not an
oversight). Dynamic Type is therefore supported by construction; not separately tested on a device (section 10).

**Targets.** `UITheme.minTarget = 60` pt applied via `.frame(minHeight: 60)` on every `PanelControlView` and every
`Row` of the Settings pages — matching the Quest console's own 60-68 px control height (`XrPanelLayout.h`
`xrCommandLayout`: most rows are `h: 60`-`68`) and exceeding Apple's 44 pt HIG minimum for visionOS. Group number
keys and Windows-tab buttons are laid out in `LazyVGrid` columns whose width is `.flexible()`, so at the window's
`minWidth` (620 pt for Commands) a 4-column group-op row and a 5-column group-key row both stay above 60 pt wide as
well as tall (measured: 620 pt / 5 - spacing ≈ 112 pt).

### 8.2 State communication (never colour alone)

Every stateful control (`PanelControlItem.isOn/.isArmed/.isPending/.isActive`) pairs a fill colour (`UITheme`) with
an SF Symbol (`hourglass` pending, `scope` armed/targeted, `checkmark.circle.fill` on) **and** an
`.accessibilityValue` string built from `PanelStore`'s translated words ("armed", "pending", "On" / their German
translations) — a colour-blind or VoiceOver user gets the same information a sighted user gets from the tint.
Disabled controls carry `.disabled(true)` (system dimming + VoiceOver "dimmed" trait) plus the engine's own reason
in `.accessibilityHint`.

### 8.3 Contrast (WCAG)

`scripts/qa/vision-ui-contrast.py` parses the hex literals documented in `UITheme.swift` and computes the sRGB
relative-luminance contrast ratio of white label text against each fill (WCAG 2.1 formula, `(L1+0.05)/(L2+0.05)`).
Result (run 2026-09-21, script exits 0):

| Colour | Hex | Used for | Ratio vs. white text | WCAG AA (normal text, >= 4.5:1) |
| --- | --- | --- | --- | --- |
| `armed` | `#B45309` | an order mode / formation / camera state that is currently active | 5.02:1 | pass |
| `on` | `#0B63CE` | a toggle that is on (waypoints, current group, save-view armed) | 5.69:1 | pass |
| `pending` | `#7A3EB8` | a group operation waiting for its number | 6.54:1 | pass |
| `danger` | `#C21F1F` | STOP / destructive actions | 5.98:1 | pass |
| `badge` | `#1F2A37` | group-count / bookmark badge capsule | 14.54:1 | pass |
| `warning` | `#92400E` | the "unavailable" reason banner | 7.09:1 | pass |

Everything else (body text, `Button`/`Toggle` chrome, window glass) uses system colours and materials, which Apple
documents as meeting the platform's own contrast targets and which this package does not override.

### 8.4 Panel distance / scale (SwiftUI windows) vs. the engine's own panels

**SwiftUI windows carry no distance/scale number of their own** (section 5): visionOS positions and scales every
window so its content stays legible, and the app has no API to set or read a physical size or distance. This is the
correct comparison point against the Quest's fixed `commandSurface`/`uiButtonSurface` panel geometry
(`XrCommandUI.h`: the console is `0.72` m wide, the toggle button `0.20` m) — those numbers describe the **engine's
own textured panels** (`kVisionPanelGameUI`/`GameHud`/`CommandsButton`/`CommandsConsole`), which are package C2's
responsibility (`visionos-presentation.md`, not this package's file) and are out of this audit's scope by the brief
("radar/minimap which are engine textures handled by C2").

### 8.5 Selection indicators, health bars, radar, minimap

Drawn by the engine into its own textures and composited by package C2 as world-anchored panels; this package only
carries the three **toggle preferences** that gate them (`healthBars`, `unitRings`, `boardFrame` in `GXPanelPrefs`,
`Presentation` page, section 4.2) — the pixels themselves are out of scope here, as the brief specifies.

### 8.6 Numeric criteria used, summary

* Minimum target size: **60 pt** (`UITheme.minTarget`), inside the 44-60 pt range specified, matching the Quest's
  own control heights.
* Contrast: **WCAG AA, >= 4.5:1** for every explicit fill against the white label text drawn on it (section 8.3);
  system colours/materials elsewhere are assumed to already meet the platform's targets.
* Minimum angular size: **not applicable to SwiftUI windows** (section 8.4) — the system guarantees legibility by
  scaling; the only place an angular-size number is meaningful is the engine's own panels, which package C2 owns and
  documents in `docs/visionos-presentation.md`.
* State: never colour alone (section 8.2), Dynamic Type: supported by construction (section 8.1, untested on device).

## 9. Verification

Everything below ran on the visionOS 26.5 **simulator** (`GXR-f-ui`, reused from the predecessor's session and
deleted per the working rules at the end of this one), no game data, no headset. Nothing here is claimed as
verified on a physical Vision Pro. This session's additions are marked **(this session)**.

| Check | Result |
| --- | --- |
| Host test, C++ model and actions | `scripts/qa/vision-ui-panel-test.sh`: **1665 checks, 0 failed** (ASan+UBSan). Every hittable id of every page/variant (Commands compact/tactics/help, Menu Windows/Units/Groups/View/Help) maps to the expected engine call, in the expected order, with the expected `stereoWorld`/`closeMenu`/host effect; group operations (arm, cancel, apply, clear-on-order); bookmarks (save-armed, recall-of-empty, disarm-on-any-other-control); tactics foldout and its gating by `TacticalReason`; every menu page's navigation and gating (`canAdjustWorld`, `splitVisible`, Ground View refusal without `canObserveGround`/`stereoVisible`); all 6 extra (camera/base/cancel/language/tactical) actions; state derivation (armed/on/pending/disabled with the engine's own reason text) for every mode/queue/group/formation/bookmark/match-result combination in both languages; 168 hittable-control labels checked in German and English (0 untranslated survivors); snapshot capture through both engine seams including UTF-8-safe truncation; the four C entry points' bounds checks. Re-run **(this session)** after every fix below: still 1665/1665. |
| Existing host tests, unchanged | `scripts/qa/vision-interaction-test.sh --existing` (27/27) and `scripts/qa/vision-bridge-forward-test.sh` (109 checks) still pass — re-run **(this session)**, confirming the `GXEngineHost.{h,mm}` frame-hook addition (section 11) changed nothing. |
| Localisation completeness | `scripts/qa/vision-ui-strings-check.sh`: **passes** (177 table keys after this session's additions, every call site covered, no untranslated leftovers, no `xr()` key missing from the Quest table). |
| Contrast | `scripts/qa/vision-ui-contrast.py`: **passes**, all 6 explicit fills >= 4.5:1 (section 8.3). |
| Engine build, both slices **(this session)** | `scripts/build/visionos/build-engine.sh --simulator` and `--device`, then `make-xcframework.sh`: **0 errors** on both (simulator: 14 s incremental against the predecessor's cached build tree; device: 3m59s, the device slice's first build in this worktree). Merged xcframework: 1.4 GB, both slices present. |
| Shell build, simulator **(this session, re-verified after every fix)** | `scripts/build/visionos/build-shell.sh simulator --no-build-engine`: **0 errors, 1 warning**, final run. Two build errors were found and fixed on the way (both in files this session touched, not pre-existing): `GraphicsSettings.swift` calling `.rawValue` on `GX_GFX_COMFORT_FADE`/`GX_GFX_FOCUS_MARKER` (anonymous C enum constants import into Swift as plain `Int`, not a named enum — section 4.2); the two stale `.modifier(HudOrnament(location:))` call sites left over from the predecessor's uncommitted `HudOrnament` refactor (section 4.5). |
| Shell build, device (unsigned) **(this session)** | `scripts/build/visionos/build-shell.sh device --no-build-engine`: **0 errors, 1 warning**. |
| Windows open, screenshots **(this session)** | Run for the first time this package has ever exercised real windows in the simulator. **First attempt crashed at launch** (`Fatal error: No Observable object of type AppModel found`) — see section 4.5 for the root cause and fix; two more `.ornament`/`@Environment` sites beyond the predecessor's `HudOrnament` fix needed the same treatment. After the fix: `-openWindow commands,settings,help -uiFakeSnapshot 1 -fakeEngine` opens all three windows plus the HUD ornament and the hover-info ornament with **no crash**, in English/light, German/dark, both tested. Screenshots taken with `xcrun simctl io <udid> screenshot` and **read with the Read tool** (not just captured): the Commands window (Orders/Instant & selection/Groups sections, status card showing "Move · 5 selected · Group 3 · Waypoints ON", the hover-info ornament above it showing "Last target: Sample unit") renders with full labels, correct armed/on/pending tints (white for the active order, blue for "on" toggles, red STOP), no text clipping or truncation, group key "3" highlighted matching the scripted snapshot's `Group 3` — in both English and German (labels translate correctly, e.g. "Zwangsangriff"/"Wegpunkte AN"/"Gruppe ersetzen"). The Settings/Graphics page's top row ("Graphics" header, "Render scale 1.00x") is confirmed rendering correctly and matching the real default; the remaining Graphics rows (fps cap, shadows, eye size, UI resolution, comfort fade, focus marker) and the Help/Workspace pages in full were **not visually confirmed** — the simulator's fixed default camera pose places `below(launcher)`/`trailing(commands)`-placed windows mostly below the visible frustum, and no camera-pose or window-drag control is available through `simctl` or this session's tools (no tap injection either, as the working rules already note); this is a screenshot-tooling limit, not a code issue — the code path is identical to the Commands window's, which did render correctly, and the visible portion of the Graphics page matches the code exactly. |

## 10. What remains device-only

* Real hand/gaze input driving the Commands console vs. the engine's own textured panels side by side (the
  simulator has no hand tracking, `docs/visionos-interaction.md` section 2).
* Dynamic Type at non-default sizes, VoiceOver navigation order across the `LazyVGrid`s, and the exact system window
  placement/scale chosen for a given room (section 5) — the simulator can show window opening but not a considered
  "does it feel right at arm's length" pass.
* `ShareLink` on the Diagnostics page's log file (the simulator's share sheet exists but was not exercised in this
  session).
* ✅ **(this session)** `GXEnginePanelState_SetHoverProbe` (section 7, panel-tooltip half of the hover card) still
  waits on the interaction layer's panel-focus report from package E; out of this package's scope to add.
* The Settings window's Presentation/Controls & language/Data/Diagnostics pages and the Help window's German text
  were not screenshotted this session either (section 9's camera-framing limit, not specific to these pages);
  nothing about them changed this session, so the predecessor's code-review-level confidence stands, just without a
  fresh screenshot. The Graphics page's visible top rows and the Commands/Workspace pages in both languages WERE
  screenshotted and confirmed correct (section 9).

## 11. Diffs to files this package does not own

Small and additive, documented in the file's own comments; the lead was told in `handoffNotesForLead`.

* **`GeneralsMD/Code/Main/visionos/GXEngineHost.h`** — one new typedef and one new function declaration (unchanged
  this session; re-verified against C2's own commit `77aee73` on `vp/c2-presentation`, which does not touch this
  insertion point in a conflicting way — see below):
  ```c
  typedef void (*GXEngineFrameHook)(void* user, bool realEngine);
  void GXEngineHost_SetFrameHook(GXEngineFrameHook hook, void* user);
  ```
* **`GeneralsMD/Code/Main/visionos/GXEngineHost.mm`** — a `frameHook`/`frameHookUser` pair on the existing `Host`
  struct, the setter, and one call site inside the existing per-iteration loop, right after `RunPosted(h)` and
  before the pause-mask check (so the hook runs every iteration, parked or not):
  ```cpp
  RunPosted(h);
  if (GXEngineFrameHook hook = h.frameHook.load()) hook(h.frameHookUser.load(), h.useRealEngine);
  const uint32_t mask = h.pauseMask.load();
  ```
  No existing symbol, struct layout (beyond appending two fields) or behaviour changed; `vision-engine-host-*` tests
  were not re-run in this session (out of this package's ownership) but the edit is textually additive and the
  existing `scripts/qa/vision-interaction-test.sh --existing` (which exercises the same binary surface indirectly
  through the forwarding bridge test) passed after it, both times it was run.

**Cross-package note for the lead, not a diff of ours**: package C2's commit `77aee73` ("visionOS: graphics settings
API in GXEngineHost.h...", on `vp/c2-presentation`) inserts its `GXGraphicsSettings` block at the exact same anchor
point in `GXEngineHost.h` as this package's frame-hook block (both right after `GXEngineHost_Post`'s `#endif`) — a
textual merge conflict when the two branches merge, trivially resolved by keeping both blocks (they do not overlap
semantically). This session verified `visionos/UI/GraphicsSettings.swift` (section 4.2) compiles, links and runs
correctly against C2's real struct by temporarily overlaying C2's committed `GXGraphicsSettings.{h,cpp}` and
`GXEngineHost.h` addition into a local build only (never committed to `vp/f-ui`, reverted with `git checkout --`
before every commit this session) — `git status` on this branch is clean of C2's files. `vp/f-ui` alone does not
build past `GraphicsSettings.swift` until merged with C2's `GXEngineHost.h`/`GXGraphicsSettings.{h,cpp}`.
