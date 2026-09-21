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
| `GeneralsMD/Code/Main/visionos/xr_shim/openxr/openxr.h` | POD stand-in for `<openxr/openxr.h>` so the pure Quest `Xr*.h` headers compile off OpenXR |
| `visionos/Platform/XRInteraction.h` | platform-neutral event stream (extended compatibly, section 8) |
| `visionos/Input/GXXRInput.{h,mm}` | raw spatial events -> `XRInteractionEvent` queue (balanced, coalesced, thread safe) |
| `visionos/Input/SpatialHandTracker.swift` | ARKit hand samples (device only; degrades cleanly in the simulator) |
| `visionos/App/SpatialEventForwarder.swift` | LayerRenderer spatial events -> raw events; `InteractionControls` UI commands |
| `scripts/qa/vision-interaction-test.{cpp,sh}` | scenario tests with a recording fake bridge |

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

The order 1-5 is the Quest order (`XrHello.cpp` runLoop: input step, `SetWorldFrame`, `XrGameBoot_Frame`).

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

## 7. Engine bridge table (package C)

`VisionEngineBridge` method -> `XrGameBoot` function of the same name (`GeneralsMD/Code/Main/XrGameBoot.h`):

| Method | XrGameBoot |
| --- | --- |
| `Pointer(active,x,y,select,secondary,wheel)` | `XrGameBoot_Pointer` |
| `Key(VisionKey, down)` | `XrGameBoot_Key((XrGameKey)key, down)` |
| `RoutePointer(target)` | `XrGameBoot_RoutePointer` (0 composed, 1 world, 2 windows) |
| `PickWorld(board, aim, hit)` | `XrGameBoot_PickWorld` |
| `SpatialPointer(active)` / `SpatialTrigger(down,available,additive)` / `SpatialClick(cancel)` | `XrGameBoot_SpatialPointer/Trigger/Click` |
| `CancelTarget()` / `TacticalAction(id)` | `XrGameBoot_CancelTarget` / `XrGameBoot_TacticalAction` |
| `AdjustCamera(yaw,pitch)` / `NavigateWorld(r,f,z)` / `CanAdjustWorld()` | same names |
| `CanRotatePlacement()` / `RotatePlacement(rad)` / `PlacementDegrees()` | same names |
| `CanObserveGround()` / `PickObserverGround(...)` / `ObserverStep(...)` | same names |
| `IsInteractiveGame()` / `CanStereoWorld()` / `ExpandedUI()` / `HasUIAt(x,y)` / `GameWidth()` / `GameHeight()` | same names |
| `TacticalState`, `HoverInfo`, `WorldHoverInfo` | same names |
| **new, optional** `PlacementPending()` | `TheInGameUI->getPendingPlaceType() != nullptr` (walls/anchored builds cannot rotate, so `CanRotatePlacement` is not enough) |
| **new, optional** `PlacementLegal()` | `TheBuildAssistant->isLocationLegalToBuild` on the preview icon at its position (same call `InGameUI.cpp:2065` makes each other frame): -1 unknown, 0 illegal, 1 legal |
| **new, optional** `HasArmedCommand()` | `TouchInput::hasArmedCommand()` |

The three optional ones have safe defaults; without them a superweapon/ability target is committed on the press ray through the
deferred trigger (Quest behaviour) and walls place with a click.

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

### Package C (engine bridge)

1. Implement `VisionXrGameBootBridge : VisionEngineBridge` forwarding to the functions in section 7 (thin, no logic), including
   the three optional additions if you can.
2. Per frame on the render thread, in the Quest order: drain `XRInteraction_PollEvent` into an array; fill `VisionHostState`;
   `visionCaptureEngineFlags(bridge, host.engine)`; `interaction.update(host, events, n, out)`; then build the `XrWorldFrame`
   from the output (below) and call `XrGameBoot_SetWorldFrame`; then `XrGameBoot_Frame()`.
3. Do not call the pointer / trigger / click functions from anywhere else while the interaction layer is active: it keeps them
   balanced. Panels of the Commands console keep their own host state (`XrCommandUI.h` semantics); apply the
   `out.activations[]` you do not handle in `applyCommandAction`.
4. Compile flags for `VisionInteraction.cpp`: `-I GeneralsMD/Code/Main -I GeneralsMD/Code/Main/visionos/xr_shim -I
   Core/Libraries/Source/d3d8gles/include` (package A: the `visionos/*.cpp` glob picks the file up; no other CMake change). Never
   put `xr_shim` on the path of a target that uses the real OpenXR SDK.

### Package D1 (renderer / frame loop)

Fill each frame: `host.time_s = predicted display time`, `host.head` (from the device anchor, `XRFrameInfo.head_pose` ->
`XrPosef`), `host.headTracked`, `host.sessionFocused` (false when the layer state is not running), `host.board` /
`boardPlaced` (adopt `out.board`; the layer proposes the initial placement when `boardPlaced == false`, replacing the shell's own
`kFallbackPlacementFrames` placement), `host.boardAspect`, `host.tableHeight` (ARKit planes, optional), `host.worldZoom`,
`host.panels[]` (world pose, aspect, crop rect of each visible texture quad), and `host.engine`.

Consume `VisionInteractionOutput`:

| Field | Use |
| --- | --- |
| `board`, `boardChanged` | adopt as the physical board; call `XRInteraction_SetBoardTransform` with `visionBoardTransformForShell` |
| `worldZoom`, `worldZoomChanged` | store; `XrWorldFrame::coverage = xrMapCoverage(worldZoom, board.width)` |
| `box` | draw the rubber band on the board (corners are world space); tint by `additive` |
| `grabBar` | draw the bar (pose, length, thickness); highlight when `active` |
| `regions[]` | tracking areas / hover (section 5.10) |
| `placement` | legality tint hint, cancel-armed cue, degrees readout |
| `ground` | `XrWorldFrame::observer = (mode == Active)` with `observerGround/Head/Forward`; drop passthrough while Active; black veil with `fadeAlpha`; hold ring `holdProgress`; reticle `hasTarget/targetRoom/targetValid` |
| `cursorVisible/cursorWorld/rayStart/rayEnd`, `additive`, `mode`, `events` | cursor dot, ray ribbon, badges, audio/haptic cues |

Also: on `cp_layer_renderer_state` paused/invalidated call `XRInteraction_Flush(2)` (or `SpatialEventForwarder.reset(.focus)`).

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
`--existing` additionally builds and runs the shared Quest host tests that compile against the stand-in header.

## 11. Limitations, unverified items, next steps

* All device behaviour is **[UNVERIFIED]**: event coordinate space, two simultaneous pinch delivery for a `CompositorLayer`, the
  `pose3D` orientation convention behind the twist axis, `selectionRay` availability after the start, `trackingAreaIdentifier`
  population, hand tracking latency. Each is isolated behind one config value or one Swift function.
* No engine is attached: the rotate sign, the pan sign and the token pixel/board mapping are derived from `XrGameBoot.cpp` /
  `XrWorld.h` and cross-checked in tests against a fake, not against a running game.
* Line-build placement (walls: press-drag-release) needs a bridge flag (`isLineBuildTemplate`) so the aim flow can send press at
  the start and release at the end instead of one click.
* Unit-level hover needs an engine object-id pass (section 5.10).
* No smooth locomotion or snap turn in Ground View by design; a GameController stick path is the next step if wanted.
* Simulator: mouse/trackpad pinch delivery and whether `pose3D` moves for it were not measured here; the layer supports both a
  moving pose and an absolute pointing ray.
