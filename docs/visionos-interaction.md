# visionOS interaction layer (gaze + pinch + hands)

How the native Apple Vision Pro port turns gaze, pinch and hand input into the SAME engine calls the Quest host
makes. Scope of this document: the portable state machine (`VisionInteraction`), its engine seam
(`VisionEngineBridge`), the per-frame host snapshot (`VisionHostState`), the Objective-C++/Swift input glue, the
behaviour of every gesture, and exactly what the engine bridge and the renderer must call or consume (section 9).

Status legend used below: **[tested]** covered by `scripts/qa/vision-interaction-test.sh` against a recording fake
engine; **[built]** compiled for the visionOS simulator/device SDK but not run against real input; **[UNVERIFIED]**
needs a physical Vision Pro or a real engine (none is attached here, and no game data exists on this machine).

## 1. Files

| File | Role |
| --- | --- |
| `GeneralsMD/Code/Main/visionos/VisionInteraction.{h,cpp}` | pure state machine + pure helpers (board math, initial placement, clamps) |
| `GeneralsMD/Code/Main/visionos/VisionEngineBridge.h` | abstract engine seam mirroring the `XrGameBoot_*` surface, plus an inert `VisionNullEngineBridge` |
| `GeneralsMD/Code/Main/visionos/VisionHostState.h` | per-frame snapshot: head, board, panels, engine flags |
| `GeneralsMD/Code/Main/visionos/VisionEngineBridgeXr.{h,cpp}` | the forwarding `VisionEngineBridge`: one line per method into `XrGameBoot_*`, compiled by the engine library (`GX_XR_HOST`) |
| `GeneralsMD/Code/Main/visionos/VisionFrameDriver.h` | per-frame glue (header only, pure): compositor `XRFrameInfo` -> `VisionHostState`, event drain, `update`, board adoption, `XrWorldFrame` |
| `GeneralsMD/Code/Main/visionos/xr_shim/openxr/openxr.h` | POD stand-in for `<openxr/openxr.h>` so the pure Quest `Xr*.h` headers compile off OpenXR |
| `visionos/Platform/XRInteraction.h` | platform-neutral event stream (extended compatibly, section 8) |
| `visionos/Input/GXXRInput.{h,mm}` | raw spatial events -> `XRInteractionEvent` queue (balanced, coalesced, thread safe) |
| `visionos/Input/SpatialHandTracker.swift` | ARKit hand samples (device only; degrades cleanly in the simulator) |
| `visionos/App/SpatialEventForwarder.swift` | LayerRenderer spatial events -> raw events; `InteractionControls` UI commands |
| `scripts/qa/vision-interaction-test.{cpp,sh}` | scenario tests with a recording fake bridge (the `.sh` also runs the forwarding test and, with `--existing`, the shared Quest host tests) |
| `scripts/qa/vision-bridge-forward-test.{cpp,sh}` | the forwarding bridge against recording `XrGameBoot_*` stand-ins built from the real `XrGameBoot.h` declarations |

The Quest headers (`XrTactics.h`, `XrPlacement.h`, `XrWorld.h`, `XrLayers.h`, `XrPanelLayout.h`, `XrCommands.h`,
`XrMath.h`) are used **unchanged** and are not modified by this package. Reused pieces: `XrTriggerGesture` (tap vs drag,
2 cm), `XrSurfaceGrab` (one/two hand board grab), `XrObserverState` (Ground View), `xrSelectionContains` (box rule),
`panelRayUV` + `xrCommandHit`/`xrCommandAction` (panel picking), `xrMapCoverage` semantics for zoom, `surfaceMatrix`,
`xrFromTo`, `yawForwardFromQuat`.

## 2. What the platform gives us (input facts the design rests on)

Sources: recon report `visionos-api.md` (SDK 27.0 swiftinterfaces, visionOS 26.5 simulator probe) and the shell docs.

* **No continuous gaze.** The app gets one gaze-derived `selectionRay` when an indirect pinch **begins**; afterwards
  only the hand pose (`inputDevicePose.pose3D`) moves. There is no hover, no dwell, no parked cursor. Every pinch is
  therefore classified once, from that ray, and then follows the hand relative to its start point. [SDK, DOC]
* `event.location` is always (0,0) in a `CompositorLayer`; it is never used. There is no `.began` phase: the first event
  with a new `id` is the begin; `.ended` / `.cancelled` end it. `chirality` says which hand. [SDK, DOC]
* Two simultaneous pinches arrive as two events with different ids (Apple documents this for `SpatialEventGesture`; for a
  `CompositorLayer` it is **[UNVERIFIED]** on device).
* `modifierKeys` and (visionOS 26) `trackingAreaIdentifier` exist on each event and are forwarded. [SDK]
* `HandTrackingProvider.isSupported == false` in the simulator (probe), so hand joints are device-only. World tracking
  (head pose) works everywhere. [PROBE]
* Coordinate space of `selectionRay` / `pose3D` is assumed to be the immersive-space origin, the same space as the ARKit
  device anchor. **[UNVERIFIED]**; the assumption lives in `SpatialEventForwarder.swift` only.

## 3. Data flow

```
 main thread                      any thread                     render thread (one per process, owns the engine)
 ------------                     ----------                     -----------------------------------------------
 LayerRenderer.onSpatialEvent     ARKit hand task                  1. drain XRInteraction_PollEvent()
   SpatialEventForwarder            SpatialHandTracker             2. visionCaptureEngineFlags(bridge, host.engine)
        |                               |                          3. VisionInteraction::update(host, events, out)
        v                               v                                 |  engine calls (VisionEngineBridge)
   GXXRInputPushRawSpatialEvent   GXXRInputPushHandSample                 |    PickWorld / Pointer / SpatialTrigger ...
   GXXRInputPostCommand (UI)      GXXRInputFlush (scene phase)            v
        \_____________ GXXRInput.mm: one mutexed, bounded, ordered queue -+--> VisionInteractionOutput
                                                                          4. adopt out.board / out.worldZoom, fill
                                                                             XrWorldFrame, XrGameBoot_SetWorldFrame
                                                                          5. XrGameBoot_Frame()  (engine frame)
                                                                          6. renderer draws box, grab bar, veil, ghost hints
```

The order 1-5 is the Quest order (`XrHello.cpp` runLoop: input step, `SetWorldFrame`, `XrGameBoot_Frame`). Steps 1-4 (everything
before `XrGameBoot_Frame`) are one call, `VisionFrameDriver::step` (section 9).

## 4. Controls

Terms: **look** = the gaze ray captured at pinch start; **pinch** = indirect pinch (or direct pinch with the fingers on the
target); **board** = the physical tabletop map; **rim** = the pan-handle band around the map edge; **grab bar** = the visible
bar in front of the near edge of the board.

| You do | Where you look | Result |
| --- | --- | --- |
| Look + pinch (tap) | a unit | select it (engine hit test through `PickWorld` + deferred `SpatialTrigger` click) |
| Look + pinch (tap) | terrain / enemy / building with units selected | the engine's contextual command: move, attack, capture, enter, guard, gather, deploy (all decided by `evaluateContextCommand`) |
| Additive select / deselect | as above | enable the SwiftUI toggle, or hold Shift (simulator keyboard), or tap with the **other** hand while the first hand is pinching (toggles per gesture); tapping an already selected unit deselects it (engine rule) |
| Pinch and drag | empty terrain on the map | box selection; the rectangle is exposed in board coordinates and as four world corners |
| Second-hand tap during a box | - | cancels the box (nothing selected) |
| Pinch and drag | the rim (pan handle) | pan the map; the map follows the hand |
| Two-hand pinch, both held > 0.35 s | the map | rotate (hand axis yaw about the board centre), zoom (hand distance ratio: spread = zoom in) and pan (midpoint) |
| Pinch and drag | the grab bar | move the board (one hand, translation only) |
| Two-hand pinch | the grab bar | move + yaw + uniform scale of the board (level, clamped) |
| Recenter button / `XR_CMD_RECENTER_BOARD` | - | board goes back 0.9 m ahead of the head, keeps its size; `XR_CMD_RESET_WORKSPACE` also restores the default size and zoom |
| Building placement pending: pinch | the ground | the ghost jumps to the gaze target; dragging moves it; wrist twist rotates it; release confirms |
| Placement: cancel | second-hand tap, SwiftUI cancel button, or look away from the board + pinch | `CancelTarget`; two consecutive confirms on illegal ground also cancel |
| Ground View: button, or hold a still pinch on the rim for 1.5 s | - / the rim | arm (`ground.holdProgress` fills a ring); then look at visible open ground + pinch (tap) = teleport there; pinch again elsewhere = teleport again |
| Ground View: leave | hold a still pinch for 1.2 s, or button, or engine refusal | back to the unchanged tabletop |
| Look + pinch | the engine UI panel (control bar, dialogs) | pixel pointer through `Pointer`: hover, press one frame later, release one frame after that (a click) |
| Look + pinch | the Commands console / button | host panel: hit table from `XrPanelLayout.h`; activation on release over the same control; tactic buttons call `TacticalAction` |

Simulator (mouse click = pinch at the pointer): Shift = additive, Option = emulate the second hand (a mirror image of the
pinch about a pivot 15 cm to the side, so moving the mouse toward/away from the pivot zooms and moving around it rotates).
Escape / rotate steps / cancel are SwiftUI commands (`InteractionControls`).

## 5. Behaviour reference

### 5.1 Classification at pinch start [tested]

Order: Ground View state -> nearest hit among {visible panels, grab bar, rim, map} along the gaze ray.

* Panels: `panelRayUV(surfaceMatrix(panel), aspect, aim)` per visible panel; the transparent HUD panel only counts where
  `HasUIAt(pixel)` is true (no invisible input wall); the Commands console is a host panel and consumes its own dead space.
  Nearest hit wins against the board plane hit, exactly like the Quest slot loop.
* Board regions in board space (metres, x right, y toward the far edge): grab bar = a bar centred 0.11 m in front of the near
  edge (length 30 % of the board width clamped 0.30-0.60 m, thickness 0.05 m, +0.02 m gaze pad); rim = the band from 0.01 m
  inside to 0.07 m outside the map edge; map = the rectangle. Precedence grab bar > rim > map. A ray that misses the
  plane rectangle but hits the board volume (a tall unit above the edge) still counts as map (asks `PickWorld`).
* A tracking-area id delivered with the event names a region when the ray classified as nothing (section 7).
* Direct pinch (fingers on the target, no ray) probes straight into the panel/board face from the pinch point.
* Anything the engine forbids (script-locked camera, native dialog open) makes the world regions inert; the pinch is
  swallowed and its release never becomes a click.

### 5.2 Cursor model [tested]

A pinch has a start point on a plane (board plane or panel plane): the gaze ray hit. After the start the point follows the
hand with **head->hand ray amplification**: `cursor = start + (hit(eye -> hand_now) - hit(eye -> hand_at_start))` on the
plane, with the eye at the gaze ray origin. A hand movement of `d` at ~0.4 m moves the cursor by about `d * plane
distance / hand distance` (typically 2-3x), so a 35 cm arm sweep covers a 1 m board. Fallback when no eye ray exists: constant
gain 2.5. A mouse/trackpad pointer with an absolute pointing ray uses that ray directly (no hand motion needed).

### 5.3 Select, command, additive [tested]

Tap = release before 2 cm of hand travel (`XrTriggerGesture`, the same class and threshold the engine uses). Engine calls:

```
begin  : RoutePointer(world) if needed, PickWorld(gaze aim), SpatialPointer(true), Pointer(true, tokenX, tokenY, false)
release: PickWorld(gaze aim again), SpatialPointer(true), Pointer(true,...),
         SpatialTrigger(false,true,add)  arm    <- the engine trigger needs a released sample before it accepts a press
         SpatialTrigger(true ,true,add)  press  <- no order yet
         SpatialTrigger(false,true,add)  release -> engine Click -> XrGameBoot_SpatialClick -> TouchInput::tap /
                                                    evaluateContextCommand (select or contextual order)
next frame: SpatialPointer(false), Pointer(false)
```

The press and release use the **start** ray again, so hand jitter at release never changes the target (the Quest host does
the same with `s_triggerRayStart`). `add` is evaluated at release, so a second-hand tap that arrives after the first hand
pinched still counts. The layer never calls `SpatialClick(false)` for taps and never re-implements a rule.

### 5.4 Box selection [tested]

Once hand travel passes 2 cm the pinch becomes a box: `SpatialTrigger` arm + press are sent at the **frozen start ray** (the
engine records the ground under the gaze as its first corner), then every frame `PickWorld(eye -> clamped cursor)`,
`SpatialPointer(true)`, `Pointer(true, x, y, false)`, `SpatialTrigger(true,true,add)`; release sends
`SpatialTrigger(false,true,add)` (the engine `Drop` selects everything inside). The cursor is clamped to the map (inset 5 mm)
so an overshoot cannot make `PickWorld` fail, which would make the engine cancel the gesture.

`VisionInteractionOutput::box`: `minX/minY/maxX/maxY` in board metres and `corners[4]` in world space (counter-clockwise,
2 mm above the surface). It is the rectangle the engine selects with: the press hit and the cursor hit as the engine reported
them (`hit.room` mapped back into board space), so it matches `xrSelectionContains` on the engine side. `visionBoxContains`
applies the same rule for the renderer. `additive` shows the latched mode.

### 5.5 Camera on the map [tested]

* **Pan (rim, or two-hand midpoint).** Content follows the hand: dragging the map by `d` board-fractions moves the camera by
  `-d`. `NavigateWorld(right, forward, 0)` acts in the board basis; the engine converts `value * span / 2` game units and
  clamps `|right|`, `|forward|` and the diagonal to 0.05 per call, so `right = -2 * dx / boardWidth`, `forward = -2 * dy /
  boardWidth`, sent in <= 24 chunks per frame with the remainder carried over. A refused call (script lock) drops the residual.
* **Rotate (two-hand).** Yaw of the hand axis about the board normal (CCW seen from above positive), 3 degree dead zone
  (continuous, no snap, exact undo), `AdjustCamera(rotateSign * dθ, 0)`. Sign derivation: `xrWorldToBoard` uses
  `axis = (cos a, sin a)` with `a = TheTacticalView->getAngle()`, so a game point at angle φ sits at board angle φ - a; a
  positive `a` step rotates the map clockwise seen from above. Content follows hands turning CCW, so `rotateSign = -1`.
  **[UNVERIFIED on a running engine]**; one config value to flip.
* **Zoom (two-hand).** Hand distance ratio `r`: `worldZoom = clamp(zoom0 * exp(-(ln r - deadzone)), 0.5, 3.0)`, deadzone
  |ln r| = 0.06. It is the Quest host-side zoom (`XrWorldFrame::coverage = xrMapCoverage(worldZoom, board.width)`), so spreading
  the hands (r > 1) shrinks the coverage multiplier = zoom in. The host stores `out.worldZoom`.
* Two-hand starts when a second pinch is held past 0.35 s or moves 1.5 cm; a quicker still second pinch is a tap (additive
  latch / box cancel / placement cancel). While a second pinch is undecided the first hand's box decision waits, so a hand
  that starts moving slightly early cannot steal the gesture. When one hand lets go the other is swallowed (it must not click).

### 5.6 Workspace: the board in the room [tested]

* **Initial placement** (`visionInitialBoard`): yaw only from the head, flat (normal up), 0.9 m ahead, width 1.0 m. Height:
  ARKit table if found; else, when the origin is on the floor (head between 1.0 and 2.3 m, the shell's heuristic) 0.8 m
  (`XR_TABLETOP_SURFACE_HEIGHT_M`); else (simulator, origin at head height) 0.45 m below the eyes and 1.25 m ahead so the
  whole board fits the simulator's narrow field of view. It is proposed when the host has not placed a board yet.
* **Grab bar.** One hand: the board follows the pinch translation only (pinch pose orientation is ignored, so the wrist can
  never tilt the board), gain 1.5 about the pinch start. Two hands: `XrSurfaceGrab` mode 3 (rebased on every one/two hand
  transition, no snap) on hand positions whose axis is flattened to the horizontal plane, so the board yaws and scales but stays
  level. Releasing one hand continues as a one-hand move without a jump.
* **Clamps**: width 0.45-2.0 m (0.45 is the `XrSurfaceGrab` floor; 2.0 m is 95 degrees wide at 0.9 m), distance from the head
  0.35-3.0 m, surface 0.10-1.40 m below the eyes. Recenter re-runs the initial placement with the current width.
* Ownership: the host owns the pose between frames. The layer re-reads `host.board` every frame and proposes changes in
  `out.board` (`out.boardChanged`).

### 5.7 Building placement [tested]

The ghost is the engine's own preview. While `placementPending` (or an armed superweapon/ability, `armedCommand`) the pinch
does **not** use the deferred trigger; it is an aim gesture:

```
begin  : RoutePointer(world), PickWorld(gaze), SpatialPointer(true), Pointer(true,x,y,false)   -> ghost jumps to the gaze target
frame  : PickWorld(eye -> cursor), SpatialPointer(true), Pointer(true,x,y,false), RotatePlacement(dθ)
release: PickWorld, SpatialPointer(true), Pointer(true,x,y,false), SpatialClick(false)        -> native PlaceEventTranslator commits
next frame: SpatialPointer(false), Pointer(false)
```

* **Twist**: wrist rotation about the hand-local Z axis of the pinch pose (swing-twist decomposition), 8 degree continuous dead
  zone, `RotatePlacement(twistSign * (twist - deadzone))` in radians. Derivation: engine angle is CCW seen from above; a clockwise
  wrist roll seen from behind is `-θ` about local +Z (forward is -Z), and the ghost should turn clockwise, so
  `radians = +twist about Z`. **[UNVERIFIED]**: the `pose3D` orientation convention. `twistAxis`/`twistSign` are config;
  `XR_CMD_ROTATE_PLACEMENT_STEP` (buttons/keys) is the always-available alternative.
* **Feedback for the renderer**: `out.placement` = {active, ghostFollowing, rotating, cancelArmed (this pinch began away from
  the board), legal (-1/0/1 from the optional engine query), degrees, roomPoint}. The engine already tints an illegal ghost red.
* **Cancel**: `XR_CMD_CANCEL_PLACEMENT`, a second-hand tap, or a tap that began away from the board (`AimMiss`) call
  `CancelTarget`. Dragging off the map before release keeps the placement pending. When `legal == 0`, the first confirm is still
  forwarded (the engine explains why), the second consecutive one cancels (`invalidConfirmCancelsAfter`).
* Line-build placements (walls) that the engine builds with press-drag-release are **not** modelled: they get the same aim flow
  (single click at release). See section 11.

### 5.8 Ground View [tested]

`XrObserverState` (Quest) drives the state: `Off -> Armed -> Active -> Off`.

1. Enter: command, or a still pinch held 1.5 s on the pan handle (only if `canObserveGround`); any pinch in flight is cancelled; the engine gets a neutral controller.
2. Armed: tabletop gestures are off. While a pinch is held the engine's verdict on the gaze point is exposed
   (`ground.hasTarget/targetValid/targetRoom`, `PickObserverGround`). Release as a tap (< 0.6 s, < 2 cm): `PickObserverGround(board,
   gazeAim)`, then `observer.choose(ground, head, headYawForward)`; the ground plane is anchored 1.65 m below the head.
   Refusal raises `kVisionEventGroundInvalid` and stays armed.
3. Active: **teleport-style only, no smooth locomotion, no smooth turning** (turn your body; the world is 1:1). Tap: the gaze ray
   meets the virtual ground plane; the observer walks there in `ObserverStep` chunks of <= 1.8 game units (engine limit 2),
   stopping at the first refused step (slope, cliff, shroud, collision), <= 6 m per jump. The head pose is re-anchored, so player
   height always comes from the current head position.
4. Comfort: `ground.fadeAlpha` is 1 at every mode change and teleport and ramps to 0 in 0.18 s (the Quest black veil); the
   renderer draws a black full-eye veil with that alpha.
5. Exit: hold a still pinch 1.2 s (`ground.holdProgress` 0..1 for a ring), the command, tracking/focus loss, or the engine
   dropping `canObserveGround`. The board pose and size are never touched in Ground View, so the tabletop returns unchanged.

### 5.9 Panels [tested]

Engine UI panels (control bar, dialogs, the composed screen, the transparent HUD) forward **pixels** through
`XrGameBoot_Pointer`: `x = (rect.x + u * rect.w) * (W - 1)`, `y = (1 - rect.y - v * rect.h) * (H - 1)` with `(u, v)` the
GL-native panel UV, identical to `updateControls`. Route: composed 0, planar world 1, UI and HUD 2. Script per pinch: hover in
the first frame, press one frame later, release one frame after the press (the GUI needs a move before a press); dragging moves
the pointer with the button held; a cancel or flush releases the button. Host panels (Commands button 100, console table) never
touch the engine except through `TacticalAction(xrCommandAction(id))` (and 40-42) on activation; help, groups, bookmarks
and close are reported in `out.activations[]` for the host UI to apply (`applyCommandAction` in `XrCommandUI.h` is host state).

### 5.10 Hover feedback without gaze [data model implemented; renderer side documented]

The app cannot know what the player is looking at, so hover must be done by the system. visionOS 26 **tracking areas**:
the compositor draws the highlight out of process from a second render target the app writes; the app never sees gaze, and
the pinch event carries `trackingAreaIdentifier`.

Implemented here: stable region ids (`VisionRegionId`: board 1, grab bar 2, pan handle 3, panels 16 + index) and
`out.regions[]` with world-space quads every frame; the identifier of a pinch is forwarded and used as a routing hint for the grab
bar and rim (section 5.1). What the renderer (package D1) must do, next concrete step: set `configuration.trackingAreasFormat =
.r8Uint`, per frame call `drawable.addTrackingArea(identifier:)` + `addHoverEffect(.automatic)` for each region, write the render
value into `colorAttachments[1]` while drawing that region's quad. **Unit-level hover** (highlight the unit you look at) needs an
engine object-id buffer mapped to tracking-area values; that is not available and is not implemented. Fallbacks: hand-ray hover
(from the pinching hand) or a head-ray reticle. Whether the simulator draws the highlight is **[UNVERIFIED]**.

### 5.11 Cancellation, balance and flush [tested]

* Every engine press has a release: trigger presses end in a release (drop/click) or `SpatialTrigger(false,false,false)`
  (engine cancel); pointer buttons end in a release or an inactive pointer. A 400-run randomized test (begins, drags, ends,
  cancels, flushes, tracking loss, focus loss, commands, modifier changes) asserts after every frame that at most one trigger
  and one button is held, that a held trigger always has an active gesture that can release it, and that after a final flush
  nothing is held.
* Tracking loss (`headTracked == false`), focus loss (`sessionFocused == false`), `XR_EVENT_TRACKING_LOST`, a hand that a fresh ARKit
  sample reports untracked while it is pinching: everything is cancelled without click/order, `CancelTarget` is called (never a
  deselect), Ground View exits. Events of the interrupted pinch that still arrive are ignored (a `DRAG` never starts a
  pinch, and the glue swallows the remainder of flushed pinches), so a stale drag cannot restart as a new gesture.
  `XR_EVENT_FLUSH` resets gesture state but leaves engine targets (placement) pending.
* The glue queue is bounded (256): drag runs are coalesced; if only begin/end/command events fill it the whole queue is replaced by
  one FLUSH.
* Route change hazard: a queued button-up must reach its original recipient, so a pinch that needs a different pointer route
  is deferred one frame when the engine was already touched in the same update.

## 6. Configuration

`VisionConfig` (all values in `VisionInteraction.h`, unit-tested defaults): `dragThresholdM 0.02`, `secondHandTapSeconds 0.35`,
`secondHandTapTravelM 0.015`, `cursorFallbackGain 2.5`, region geometry (`rimInnerM 0.01`, `rimOuterM 0.07`, `grabBarOffsetM
0.11`, `grabBarThicknessM 0.05`, `grabBarPadM 0.02`), workspace (`boardMinWidthM 0.45`, `boardMaxWidthM 2.0`,
`initialDistanceM 0.9`, `workspaceOneHandGain 1.5`, distance/height clamps), camera (`panChunk 0.05`, `rotateSign -1`,
`rotateDeadzoneRad 3 deg`, `zoomDeadzone 0.06`, `zoomMin/Max 0.5/3.0`), placement (`twistDeadzoneRad 8 deg`, `twistSign`,
`twistAxis 2`, `invalidConfirmCancelsAfter 2`, `missCancelsPlacement`), Ground View (`groundTapMaxSeconds 0.6`,
`groundExitHoldSeconds 1.2`, `groundEnterHoldSeconds 1.5`, `comfortFadeSeconds 0.18`, `teleportMaxMetres 6`, `teleportStepUnits 1.8`), simulator
(`emulateSecondHandWithOption`, `simSecondHandSeparationM 0.30`), `applyCommandActions`.

## 7. Engine bridge table

`VisionXrGameBootBridge` (`GeneralsMD/Code/Main/visionos/VisionEngineBridgeXr.cpp`, obtained with `VisionCreateXrGameBootBridge()`)
implements `VisionEngineBridge` by forwarding each method to the `XrGameBoot` function of the same name
(`GeneralsMD/Code/Main/XrGameBoot.h`). The forwarding body is compiled only where that API is declared and defined: under
`__ANDROID__`, or when `GX_XR_HOST` **and** `GX_XRGAMEBOOT_HOST` are defined (package C defines the latter, in `XrGameBoot.h` or as a
compile definition, when it makes the header host neutral). In every other build `VisionCreateXrGameBootBridge()` returns `nullptr`, so
the file can sit in the `visionos/*.cpp` glob of `z_generals` before package C lands without breaking that build.

| Method | XrGameBoot | Definition today (`XrGameBoot.cpp`) |
| --- | --- | --- |
| `Pointer(active,x,y,select,secondary,wheel)` | `XrGameBoot_Pointer` | 1491 |
| `Key(VisionKey, down)` | `XrGameBoot_Key((XrGameKey)key, down)` (same enumerator order) | 1532 |
| `RoutePointer(target)` | `XrGameBoot_RoutePointer` (0 composed, 1 world, 2 windows) | 1262 |
| `PickWorld(board, aim, hit)` | `XrGameBoot_PickWorld` | 738 |
| `SpatialPointer(active)` | `XrGameBoot_SpatialPointer` | 853 |
| `SpatialTrigger(down,available,additive)` | `XrGameBoot_SpatialTrigger` | 966 |
| `SpatialClick(cancel)` | `XrGameBoot_SpatialClick` | 900 |
| `CancelTarget()` / `TacticalAction(id)` | `XrGameBoot_CancelTarget` / `XrGameBoot_TacticalAction` | 1080 / 1008 |
| `AdjustCamera(yaw,pitch)` | `XrGameBoot_AdjustCamera` | 1387 |
| `NavigateWorld(r,f,z)` / `CanAdjustWorld()` | `XrGameBoot_NavigateWorld` / `XrGameBoot_CanAdjustWorld` | 1397 / 531 |
| `CanRotatePlacement()` / `RotatePlacement(rad)` / `PlacementDegrees()` | same names | 840 / 844 / 848 |
| `CanObserveGround()` | `XrGameBoot_CanObserveGround` | 536 |
| `PickObserverGround(board,aim,ground,room*)` / `ObserverStep(cur,delta,next)` | same names | 769 / 813 |
| `IsInteractiveGame()` / `CanStereoWorld()` / `ExpandedUI()` | same names | 1416 / 522 / 473 |
| `HasUIAt(x,y)` / `GameWidth()` / `GameHeight()` | same names | 1291 / 1550 / 1558 |
| `TacticalState(mode,group,queue)` | `XrGameBoot_TacticalState` | 1156 |
| `HoverInfo(x,y)` / `WorldHoverInfo()` | `XrGameBoot_HoverInfo` / `XrGameBoot_WorldHoverInfo` | 1298 / 1164 |
| optional `PlacementPending()` | `TheInGameUI->getPendingPlaceType() != nullptr` (direct engine read, guarded so the host test builds without engine headers) | - |
| optional `HasArmedCommand()` | `TouchInput::hasArmedCommand()` (direct engine read) | - |
| optional `PlacementLegal()` | not forwarded: returns -1 (unknown). See section 9, item 5 | - |

Those 27 `XrGameBoot_*` functions are the complete link surface of the forwarder. The same file defines `XrGameBoot_Init`
(220), `_Frame` (439), `_SetWorldFrame` (625) and `_Shutdown` (1566), which the engine host calls itself.

`scripts/qa/vision-bridge-forward-test.sh` proves the forwarding (109 checks): every method reaches the function of the same
name with the same arguments and return value, against stand-ins compiled from the real `XrGameBoot.h` declarations (so a
signature change in the engine host breaks the build of this test).

## 8. Event API v2 (`visionos/Platform/XRInteraction.h`)

Extended **compatibly**: existing enumerators and fields are unchanged; new enumerators (`XR_EVENT_TRACKING_LOST 8`,
`FLUSH 9`, `COMMAND 10`, `HAND_UPDATE 11`, `MODIFIERS 12`) and new fields are appended (`modifiers`, `has_hand_pose`/`hand_pose`,
`has_current_ray`/`current_ray`, `tracking_area_id`, `command`/`command_value`, `hand_tracked/pinching/palm_up`). New entry points:
`XRInteraction_PostCommand`, `XRInteraction_Flush`, `XRInteraction_SetModifiers`. `XR_EVENT_TWO_HAND_*` remain advisory and are
ignored by `VisionInteraction`, which derives two-hand data from the two pointers itself.

**Include hazard:** on a case-insensitive filesystem (macOS default) `#include "XRInteraction.h"` also matches the Quest header
`GeneralsMD/Code/Main/XrInteraction.h`. `VisionInteraction.h` includes it through a relative path; any target that has both
`GeneralsMD/Code/Main` and `visionos/Platform` on its include path must do the same (or put `Platform` first).

## 9. Hookup

### Package C (engine host)

**1. Build.** `VisionInteraction.cpp` and `VisionEngineBridgeXr.cpp` are picked up by the `GeneralsMD/Code/Main/visionos/*.cpp`
glob of `z_generals`; no CMake edit is needed for the files themselves. The target needs these include directories (both
files, and every other file that includes the pure `Xr*.h` headers):

| Include directory | Why |
| --- | --- |
| `GeneralsMD/Code/Main` | `XrTactics.h`, `XrPlacement.h`, `XrWorld.h`, `XrLayers.h`, `XrPanelLayout.h`, `XrCommands.h`, `XrMath.h`, `XrGameBoot.h` |
| `GeneralsMD/Code/Main/visionos/xr_shim` | stand-in `<openxr/openxr.h>` (POD types and flag bits only). Never on the path of a target that uses the real OpenXR SDK |
| `Core/Libraries/Source/d3d8gles/include` | `XRBoardBounds.h` (included by `XrWorld.h`) |
| `Core/Libraries/Source/WWVegas/WWLib` | `gx_backend.h` (`GX_XR_HOST`); already on the engine include path |

Package A's `z_generals` block does not add the second and third directories yet. With the `visionos-device` flags of `z_generals` alone both
`VisionInteraction.cpp` and `VisionEngineBridgeXr.cpp` stop at `openxr/openxr.h`; with these two lines they compile (checked with `-fsyntax-only` against
A's `compile_commands.json`):

```cmake
target_include_directories(z_generals PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/visionos/xr_shim
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/d3d8gles/include)
```

`visionos/Platform` does not need to be on the path: `VisionInteraction.h` includes `XRInteraction.h` by relative path (the
case-insensitive-filesystem hazard of section 8 does not apply).

Verified: `VisionEngineBridgeXr.cpp` compiles (`-fsyntax-only`, with the engine-query branch, i.e. `TheInGameUI`/`TouchInput`) against
the real engine headers with the flags of package A's `visionos-device` configuration plus the three include directories above,
and against a copy of `XrGameBoot.h` with the Android/JNI wrapper removed.

**2. Make `XrGameBoot.h` host neutral, define the 27 functions of section 7 and define `GX_XRGAMEBOOT_HOST`.** Today the whole header is inside
`#ifdef __ANDROID__` and includes `<jni.h>`, and `XrGameBoot_Init` takes `JNIEnv*`. Either widen the guard to `GX_XR_HOST`
(move the `JNIEnv` declaration behind `__ANDROID__`) and compile `XrGameBoot.cpp` on visionOS with its Android-only parts gated
(JNI, `__android_log`, `/sdcard`), or write `VisionGameBoot.cpp` as the twin the visionos README describes. The 27 functions
listed in section 7 (line numbers given there) contain the engine logic (tactics state, world mapping, picking, spatial
trigger); they must keep their behaviour because the interaction layer is tuned to it. `XrGameBoot.cpp` and `XrGameBoot.h`
are read-only for package E. When both are ready define `GX_XRGAMEBOOT_HOST` (for example `#define GX_XRGAMEBOOT_HOST 1` at the top of the widened
guard in `XrGameBoot.h`): that switches `VisionEngineBridgeXr.cpp` from the `nullptr` factory to the real forwarder.

**3. Construct once, on the render thread:**

```cpp
static VisionFrameDriver driver(VisionCreateXrGameBootBridge());   // VisionFrameDriver.h; nullptr bridge = shell without engine
```

**4. Every rendered frame** (inside the frame client's `-renderFrame:` / XRPresentation frame callback, after
`d3d8gles_SetXRHostTargets`, before the engine frame; D1's "Engine attach guide" in `docs/visionos-shell.md` step 4 to 5):

```cpp
const bool running = XRPresentation_GetSessionState() == XR_SESSION_RUNNING;
VisionPanel panels[kVisionMaxPanels]; int n = 0;                       // the SAME quads the renderer composites this frame
panels[n++] = visionMakePanel(kVisionPanelGameUI, uiQuadPose, uiWidthM, uiHeightM / uiWidthM, cropRect);
// ... kVisionPanelGameHud (transparent overlay), kVisionPanelGameScreen (menus/movies), Commands button/console
XrWorldFrame world = ...;                                               // sizes, toggles, healthBars ... (Quest values)
const VisionInteractionOutput &out = driver.step(*frame, running, panels, n, world);
XrGameBoot_SetWorldFrame(world);                                        // board, coverage, observer, eyes and fov are set by step()
XrGameBoot_Frame();
```

`step()` does, in order: `visionFillHostFromFrame` (time, frame index, head pose, `head_tracked`, `sessionFocused`), copies the panel
table, `visionCaptureEngineFlags(bridge)` (the engine flags below), drains `XRInteraction_PollEvent` (<= 256 events per frame),
`VisionInteraction::update`, adopts a proposed board (`XRInteraction_SetBoardTransform` with `visionBoardTransformForShell`) and zoom for the
next frame, then `visionApplyToWorldFrame` (board, `coverage = xrMapCoverage(worldZoom, board.width)`, observer) and
`visionFillWorldFrameEyes` (eye poses and OpenXR-style fov from the compositor frame; a mono frame mirrors eye 0).

How `VisionHostState` is filled:

| Field | Source |
| --- | --- |
| `time_s`, `frame` | `XRFrameInfo.predicted_display_time_s`, `.frame_index` |
| `head`, `headTracked` | `XRFrameInfo.head_pose` (device anchor, room space), `.head_tracked` |
| `sessionFocused` | `XRPresentation_GetSessionState() == XR_SESSION_RUNNING` (false: input suspended, everything in flight cancelled) |
| `board`, `boardPlaced` | owned by the driver: the layer proposes the initial placement on the first tracked frame; `driver.adoptBoard(...)` if the shell restored one |
| `boardAspect` | `driver.setBoardAspect(h / w)` of the tactical view (default 9/16) |
| `tableHeightKnown`, `tableHeight` | `driver.setTableHeight(...)` from ARKit plane detection (optional; device only) |
| `worldZoom` | owned by the driver (adopts `out.worldZoom`) |
| `panels[]`, `panelCount` | the argument of `step()`; `visionMakePanel(kind, worldPose, widthM, heightM / widthM, cropRect)` for each visible textured quad: engine UI (`kVisionPanelGameUI`), transparent HUD (`kVisionPanelGameHud`, hit only where `HasUIAt`), composed screen (`kVisionPanelGameScreen`), Commands button and console (`kVisionPanelCommandsButton` / `Console`) |
| `engine` | `visionCaptureEngineFlags(bridge)`: `IsInteractiveGame`, `CanStereoWorld`, `CanAdjustWorld`, `CanObserveGround`, `ExpandedUI`, `PlacementPending`/`CanRotatePlacement`, `HasArmedCommand`, `PlacementLegal`, `PlacementDegrees`, `GameWidth/Height` |

The panel table is the single source of truth for both gaze picking and drawing: the pose given to `visionMakePanel` must be the pose of
the `GXXRCompositeLayer` the renderer draws for that texture (D1 guide step 8), otherwise the player pinches where a panel is not.

**5. Do not call `Pointer` / `SpatialPointer` / `SpatialTrigger` / `SpatialClick` / `RoutePointer` / `CancelTarget` from anywhere else
while the layer is active**: it keeps them balanced. Panels of the Commands console keep their own host state (`XrCommandUI.h`
semantics); apply the `out.activations[]` entries you do not handle (help, groups, bookmarks, close) with `applyCommandAction`.

**6. Optional engine queries that need an engine change** (both small; the layer works without them):

* `PlacementLegal()`: add `bool W3DInGameUI::xrPlacementLegal()` next to `rotateXrPlacement` (it can read `m_placeIcon[0]`, which is
  private to `InGameUI`) wrapping `TheBuildAssistant->isLocationLegalToBuild(&pos, m_pendingPlaceType, angle, <the flag set of
  InGameUI.cpp:2068>, builder, nullptr) == LBC_OK` on the preview's position and angle, export `XrGameBoot_PlacementLegal()` and return
  `-1/0/1` from `VisionXrGameBootBridge::PlacementLegal` (the one line marked in `VisionEngineBridgeXr.cpp`). It feeds `out.placement.legal`
  (tint hint) and the "two confirms on illegal ground cancel" rule.
* Line builds (walls): see section 11.

**7. Session state.** In `XRPresentation_SetSessionStateCallback`, for every state except `XR_SESSION_RUNNING` call `driver.reset(kVisionResetFocus)` (or
post `XRInteraction_Flush(2)`), so a pinch that was in flight when the layer paused releases the engine (balanced) and cannot continue as a click.

**8. Compile-time flags.** Nothing else: the interaction sources have no platform ifdefs.

### Package D1 (renderer)

`docs/visionos-shell.md` "Engine attach guide" lists the GL/Metal calls. What the interaction layer adds, per frame, from `out`:

| Field | What to draw / do |
| --- | --- |
| `board`, `boardChanged` | the physical board pose (already applied to `XrWorldFrame.board` by `step()`); the shell's own fallback placement (`kFallbackPlacementFrames`, `XRPresentation_GetTabletopPlacement`) is superseded once the layer proposes a board: use `out.board` / `driver.host().board` for anything anchored to the table |
| `worldZoom` | nothing (already in `XrWorldFrame.coverage`) |
| `box` | rubber band on the board: `corners[4]` are world space (2 mm above the surface), tint by `additive`; `visionBoxContains` if you need membership |
| `grabBar` | the bar under the board: `pose`, `length`, `thickness`, highlight when `active`; hide when `!visible` |
| `regions[]` | tracking areas / hover (section 5.10): `id`, world quad `quad[4]`, `active` |
| `placement` | `legal` tint hint (0 = illegal), `cancelArmed` cue (the pinch began off the board: releasing cancels), `degrees` readout near `roomPoint` |
| `ground` | black veil over both eyes with alpha `fadeAlpha` (the comfort fade); ring at the gaze target with `holdProgress`; reticle at `targetRoom` coloured by `targetValid`; while `mode == Active` `XrWorldFrame.observer` is already set by `step()` and the passthrough should be dropped |
| `cursorVisible`, `cursorWorld`, `cursorOnBoard`, `rayVisible`, `rayStart/rayEnd` | cursor dot on the board or panel and an optional eye-to-cursor ribbon |
| `additive`, `mode`, `events` (`VisionEventBits`) | badges and audio/haptic cues (tap, box committed, teleport, placement confirmed/cancelled, board moved) |
| `panelPointerPanel`, `panelU`, `panelV` | a pointer dot on the panel a pinch is driving |

All overlays are drawn by the renderer in Metal (or as GL layers in the ring); none of them touches the engine.

### Package G (launcher / SwiftUI)

Use `InteractionControls` (`SpatialEventForwarder.swift`): additive toggle, cancel placement, recenter/reset, Ground View
enter/exit, rotate steps, Escape; `.onChange(of: scenePhase)` -> `SpatialEventForwarder.reset(.focus)`. Optionally call
`SpatialHandTracker.shared.startIfNeeded()` when the immersive space opens so the ARKit permission prompt does not appear
mid-pinch (the forwarder also starts it lazily on the first spatial event).

## 10. Tests

`scripts/qa/vision-interaction-test.sh` (macOS clang++, ASan+UBSan, no SDK, no device):
`vision-interaction-test.cpp` drives `VisionInteraction` exactly like the render thread against a recording fake
`VisionEngineBridge` (a realistic `PickWorld` against the board plane) and also exercises `GXXRInput.mm` (compiled as Obj-C++)
end to end. Scenarios: helpers/initial placement, select, additive (toggle, Shift, second-hand tap), contextual command
sequence, box rectangle in board coordinates on a translated and yawed board, box additive/cancel/clamp, rim pan (chunking,
refusals), two-hand rotate/zoom/pan with dead zones and clamps, workspace grab clamps/level/scale/recenter, placement
(drag, twist, confirm, cancel by second hand/button/look-away/illegal streak, pending-but-not-rotatable, armed command), Ground
View (enter, teleport, wall, head height, fade, hold-exit, forced exit), tracking/focus loss, balanced press/release under
flush (400 randomized runs), panels (pixel mapping, HUD pass-through, occlusion, console hit table, activation), simulator
fallback (Option second hand, absolute pointer ray, direct pinch), robustness, regions/hover, API compatibility, glue.
`--existing` additionally builds and runs the shared Quest host tests that compile against the stand-in header (16 `xr-*-test.cpp`,
the `xr-*-test.sh` bridge tests that extract production engine functions, and the ground-observer source test; tests that need
the real OpenXR SDK types, the Android SDK/NDK or a GL device are skipped). The per-frame glue (`VisionFrameDriver`) is covered by
`testFrameDriver`: host state from a compositor frame, initial board proposal, adoption by the shell input layer (a ray through the board
centre hits it), panel table, engine flags, Ground View and unfocused-session handling, mono frames, a pre-placed board, and a null bridge.
`scripts/qa/vision-bridge-forward-test.sh` (run by the same script) checks the forwarding bridge.

## 11. Limitations, unverified items, next steps

* All device behaviour is **[UNVERIFIED]**: event coordinate space, two simultaneous pinch delivery for a `CompositorLayer`, the
  `pose3D` orientation convention behind the twist axis, `selectionRay` availability after the start, `trackingAreaIdentifier`
  population, hand tracking latency. Each is isolated behind one config value or one Swift function.
* No engine is attached: the rotate sign, the pan sign and the token pixel/board mapping are derived from `XrGameBoot.cpp` /
  `XrWorld.h` and cross-checked in tests against a fake, not against a running game.
* Line-build placement (walls: press-drag-release) needs a bridge flag (`isLineBuildTemplate`, an engine read like
  `PlacementPending`) so the aim flow can send `Pointer(select=true)` at the start, keep the button held while the ghost end follows the
  hand, and release at the end, instead of the single click (one wall segment) that `SpatialClick(false)` produces. Not implemented because
  `XrGameBoot_Pointer` on the world route was not verified to drive `PlaceEventTranslator` anchors, and there is no engine to verify it
  against here.
* Unit-level hover needs an engine object-id pass (section 5.10).
* No smooth locomotion or snap turn in Ground View by design; a GameController stick path is the next step if wanted.
* Simulator: mouse/trackpad pinch delivery and whether `pose3D` moves for it were not measured here; the layer supports both a
  moving pose and an absolute pointing ray.
