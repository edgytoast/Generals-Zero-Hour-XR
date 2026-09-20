# visionOS shell (Apple Vision Pro)

Native visionOS app shell for the Generals: Zero Hour XR tabletop port. It renders stereoscopically through
Compositor Services (Metal) with gaze + pinch input. At this stage it contains a temporary Metal test tabletop
(checkerboard board, colored units, floor grid) used to validate the render path; the real engine will replace it.

Sources live in `visionos/`. Build and run scripts live in `scripts/build/visionos/`.

## Build

Requires Xcode with the visionOS SDK and `xcodegen` (`brew install xcodegen`).

```sh
# visionOS simulator build (no signing)
scripts/build/visionos/build-shell.sh simulator --derived-data /tmp/GeneralsZHXR-DD

# Unsigned device compile check (cannot be installed on a headset)
scripts/build/visionos/build-shell.sh device --derived-data /tmp/GeneralsZHXR-DD-device
```

Both print the built `.app` path on the last line. The generated `visionos/GeneralsZHXR.xcodeproj` is git-ignored.
For a signed device build open that project in Xcode and select your own team. Do not commit team IDs.

## Run in the simulator

```sh
scripts/build/visionos/run-shell-simulator.sh --derived-data /tmp/GeneralsZHXR-DD --out-dir /tmp/shots --wait 20 --shots 2
```

Boots the Vision Pro simulator (UDID overridable with `GXX_VISIONOS_SIM_UDID`), installs the app, launches it with
`-autoImmersive`, waits, and saves screenshots. App console output (frame-loop fps lines) goes to `<out-dir>/app-console.log`.

Launch arguments: `-autoImmersive`, `-externalEyeTextures` (exercise the engine hand-off path), `-layout layered|shared|dedicated`.

## Engine integration seam

The engine targets the four C headers in `visionos/Platform/` (no Apple types):

| Header | Purpose |
| --- | --- |
| `XRPresentation.h` | frame callback, per-eye pose/view/projection/fov/viewport, head pose, submit-eye-texture, alpha mode, recenter, tabletop placement |
| `XRInteraction.h` | pinch/drag/two-hand event stream in world and board space |
| `PlatformFilesystem.h` | app data dir, game data dir, security-scoped access |
| `PlatformLifecycle.h` | pause / resume / suspend / memory / terminate |

To replace the test scene, register a frame callback with `XRPresentation_SetFrameCallback`.
Return `XR_FRAME_SUBMITTED_TEXTURES` after calling `XRPresentation_SubmitEyeTexture` for each eye and the shell
composites the textures into the compositor drawable. Design notes and verified API signatures are in the
recon report `shell-skeleton.md`.

## Game data

No game data ships with the app. The player copies a legally owned Generals / Zero Hour install into the app's
`Documents/GameData` folder (visible in the Files app because `UIFileSharingEnabled` is set), or a later build will
let them pick a folder (`LSSupportsOpeningDocumentsInPlace`).
