# Generals: Zero Hour XR for Apple Vision Pro (visionOS port, early development)

An unofficial, community effort to bring the tabletop edition of *Command & Conquer: Generals – Zero Hour* to Apple Vision Pro
as a native visionOS app. The Meta Quest 3 edition described in the main [README.md](README.md) remains the supported
release; this file covers the visionOS work only. This is not an Electronic Arts product and is not endorsed by EA.

![The visionOS shell in the simulator: a Metal test tabletop over a passthrough room, with the launcher window](docs/media/visionos/shell-tabletop-direct.jpg)

The image above is the current shell running in the visionOS simulator. The checkerboard tabletop and coloured blocks are a
**renderer test scene**, not the game. Simulator capture, 2026-09-20.

## Status: not playable yet

Be clear about where this stands. As of 2026-09-20:

| Area | State |
| --- | --- |
| Native app shell (SwiftUI window, immersive space, Compositor Services, Metal) | Working in the visionOS **simulator** with a test scene at about 60 fps |
| OpenGL ES 3.0 on Metal (ANGLE) for visionOS | Builds for simulator and device; smoke test passes in the simulator |
| The game engine on visionOS | **Not running.** It compiles and links in a probe build; it has never started, loaded a map or drawn a frame |
| Game data import and detection | Not implemented (a placeholder folder check only) |
| Gameplay, input, audio, performance | Not implemented or not tested |
| Physical Apple Vision Pro | **Never tested.** No device has run this app |

The complete, evidence-backed list of every feature and its status is in
[docs/VISIONOS_TEST_MATRIX.md](docs/VISIONOS_TEST_MATRIX.md). The design, decisions and risks are in
[docs/VISIONOS_PORT_ARCHITECTURE.md](docs/VISIONOS_PORT_ARCHITECTURE.md). Nothing in this file should be read as a promise
of a release date or of feature parity with the Quest edition.

## What you need

| Requirement | Version |
| --- | --- |
| A Mac with Apple silicon | development was done on an M5 Mac with 32 GB |
| Xcode | 27.0 with the visionOS SDK 27.0 (`xros` and `xrsimulator`) |
| App deployment target | visionOS 26.0 (set in `visionos/project.yml`) |
| Simulator runtime | visionOS 26.5 is the only runtime the port has been tested on |
| CMake | 3.28 or newer (needed for the `visionOS` system name); 4.4.3 was used |
| vcpkg | a full clone, bootstrapped (shallow clones cannot resolve the manifest baseline) |
| XcodeGen | `brew install xcodegen` (the shell project is generated from `visionos/project.yml`) |
| git and python3 | used by the ANGLE build script |
| ANGLE for visionOS | built once by `scripts/build/visionos/build-angle.sh` (about 10 to 15 minutes per slice) |

Libraries are built for a minimum OS of 2.0; the app itself targets visionOS 26.0. Only the simulator has been used. A
physical Apple Vision Pro is required to check stereo, gaze and pinch, comfort and performance, and none was available.

## Quick start

The full instructions, presets and troubleshooting are in [docs/BUILD/VISIONOS.md](docs/BUILD/VISIONOS.md). The short
version, using only scripts that exist in `scripts/build/visionos/`:

```sh
# 1. Build ANGLE (OpenGL ES 3.0 on Metal) for the simulator and the device, once per machine.
#    Dependencies are checked out outside the repository; pass or export DEPS_ROOT to choose where.
scripts/build/visionos/build-angle.sh all

# 2. Build the app shell for the visionOS simulator (no signing needed).
scripts/build/visionos/build-shell.sh simulator --derived-data build/visionos-dd

# 3. Optional: an unsigned compile check against the device SDK (cannot be installed on a headset).
scripts/build/visionos/build-shell.sh device --derived-data build/visionos-dd-device

# 4. Run the shell in the simulator and capture screenshots. Use your own simulator device.
xcrun simctl create "GXR-mine" com.apple.CoreSimulator.SimDeviceType.Apple-Vision-Pro com.apple.CoreSimulator.SimRuntime.xrOS-26-5
scripts/build/visionos/run-shell-simulator.sh --derived-data build/visionos-dd --out-dir build/visionos-shots \
    --udid <the-udid-printed-by-simctl-create> --wait 20 --shots 2
```

Building the C++ engine as the static library `z_generals` (option `SAGE_BUILD_VISIONOS_LIB`) and linking it into the app
is described in [docs/BUILD/VISIONOS.md](docs/BUILD/VISIONOS.md). The run script defaults to a simulator UUID that belongs
to the original development machine; always pass `--udid` or set `GXX_VISIONOS_SIM_UDID`.

Launch arguments accepted by the shell: `-autoImmersive`, `-externalEyeTextures` (exercises the engine hand-off path),
`-layout layered|shared|dedicated`. See [docs/visionos-shell.md](docs/visionos-shell.md).

## Game data

**No game data ships with this project and none ever will.** You must supply your own, legally owned copy of both
*Command & Conquer: Generals* and *Zero Hour*. The app and the repository contain no retail archives, maps, videos, fonts
or other copyrighted assets.

- Which files are needed, which installs are supported (Steam layout, installed discs) and how the app checks them:
  [docs/GAME_DATA_SETUP.md](docs/GAME_DATA_SETUP.md).
- The original file list for the Quest edition, which uses the same archives: [docs/HOWTO/GETTING_THE_GAME_FILES.md](docs/HOWTO/GETTING_THE_GAME_FILES.md).
- Where files go today: the shell creates `Documents/GameData` inside its app container, which is visible in the Files
  app (`UIFileSharingEnabled`). Picking a folder in place (security-scoped access) and a validating importer are planned.
  Expect about 2.7 GB.

## Running on a physical Apple Vision Pro

This procedure is documented but **has never been executed by the maintainers of this port**, who had no device. Treat it
as a starting point and correct it in the test matrix when you run it.

1. Use a Vision Pro registered to your Apple Developer account, with Developer Mode enabled and trusted by your Mac.
2. Generate the project: `scripts/build/visionos/build-shell.sh simulator` also runs XcodeGen, or run
   `xcodegen generate --spec visionos/project.yml`. This creates `visionos/GeneralsZHXR.xcodeproj` (git-ignored).
3. Open the project in Xcode, select the `GeneralsZHXR` target, and under Signing & Capabilities choose **your own team**.
   Do not commit team identifiers or provisioning files. Bundle identifier: `com.generalsx.zerohour.xr.vision`
   (change it if your team needs a different one).
4. Choose your Vision Pro as the run destination and press Run. From the command line the equivalent is
   `xcodebuild -project visionos/GeneralsZHXR.xcodeproj -scheme GeneralsZHXR -destination 'generic/platform=visionOS' DEVELOPMENT_TEAM=<your team> build`
   followed by `xcrun devicectl list devices` and `xcrun devicectl device install app --device <device> <path to GeneralsZHXR.app>`.
5. Do not use `devicectl device copy to ... --remove-existing-content true` on the app container: it removes all app data,
   including any game data you copied.

The unsigned `device` build in step 3 of the quick start only proves that the code compiles for the device SDK.

## Controls

Intended controls for the tabletop. **None of these commands is connected to a running game yet**; today the shell only
places a test board and logs spatial events. The design and the reasoning (gaze is never continuous on visionOS, so a
ray arrives only when a pinch begins) are in [docs/visionos-interaction.md](docs/visionos-interaction.md), which is the
authoritative reference and may change these.

| Gesture | Intended action | State |
| --- | --- | --- |
| Look at a unit or a point on the map, then pinch | Select a unit, or give the context order (move, attack) to the selected units | planned |
| Pinch, hold and drag across the board | Box selection | planned |
| Grab bar at the board edge, pinch and drag | Move the tabletop | planned |
| Both hands pinching, spread or twist | Scale and rotate the tabletop | planned (events are computed, not connected) |
| Look at the ground and pinch after choosing a building | Preview, then place; a rotate gesture and cancel are part of the design | planned |
| Ground View: choose a ground spot to teleport there | Human-scale view of the battlefield; a matching exit gesture returns to the tabletop | planned, experimental even on Quest |
| Recenter (button in the launcher window) | Place the tabletop in front of you again | button exists; not yet exercised in a test |

## Known problems

- The game does not run. This is a foundation release: shell, ANGLE and design documents only.
- Nothing has been tested on a real Apple Vision Pro. Stereo, comfort, thermals, frame rate and memory are unknown. The
  simulator shows a single view at 60 Hz and has no hand tracking or plane detection.
- Game speed is at risk of following the display rate (risk R1 in the architecture document); the fix is designed but not
  applied because no engine frame runs yet.
- ANGLE has no multiview and no S3TC/DXT texture formats; the engine's textures must be decoded on the CPU. Frame rate and
  texture memory are unmeasured, and route B (a native Metal backend) is the fallback if ANGLE is too slow.
- No text input (chat, save names), no audio, no Ground View, no movie playback and no multiplayer on visionOS. The Quest
  LAN synchronisation problem described in [docs/WORKDIR/planning/MULTIPLAYER_STATUS.md](docs/WORKDIR/planning/MULTIPLAYER_STATUS.md)
  applies here too.
- The ANGLE build uses a vendored copy from WebKit's tree with a small patch. It is a development dependency, not yet a
  reviewed redistributable.
- Distribution outside your own devices (TestFlight, App Store) has not been analysed, including how GPLv3 and the store
  terms interact. Do not assume it is possible.

## Legal and lineage note

This is a community project. It is **not** made, endorsed or supported by Electronic Arts, Westwood Studios or any
rights holder. *Command & Conquer*, *Generals* and *Zero Hour* are trademarks of their owners; the source licence does not
grant rights to those trademarks, and nothing here grants rights to the game's data.

- The engine source was released by EA under the GNU General Public License v3 with additional terms; see
  [LICENSE.md](LICENSE.md). This port is distributed under the same terms.
- Lineage: EA's source release, then the community work of [TheSuperHackers](https://github.com/TheSuperHackers/GeneralsGameCode)
  and [Fighter19](https://github.com/Fighter19/CnC_Generals_Zero_Hour), then [GeneralsX](https://github.com/fbraz3/GeneralsX)
  (macOS and Linux native), the iOS and iPadOS port in the generals-2026 tree, the Android port by
  [tarek369](https://github.com/tarek369/GeneralsZH-Android) and its [Cesarus85 fork](https://github.com/Cesarus85/GeneralsZH-Android),
  and the Quest tabletop edition in this repository. The visionOS port builds on that work. The full description is in
  [docs/VISIONOS_PORT_ARCHITECTURE.md](docs/VISIONOS_PORT_ARCHITECTURE.md), section 1.
- ANGLE (OpenGL ES on Metal) is BSD-3-Clause licensed; its licence text is installed beside the libraries and must ship
  with any binary that includes them.
- Never add, download or fabricate copyrighted assets. Contributions that bundle retail game files will not be accepted.

## Where to look next

- [docs/VISIONOS_TEST_MATRIX.md](docs/VISIONOS_TEST_MATRIX.md): what works, with evidence, and how to update it.
- [docs/VISIONOS_PORT_ARCHITECTURE.md](docs/VISIONOS_PORT_ARCHITECTURE.md): lineage, component map, route decision, threading,
  frame sequence, coordinate systems, interfaces, risk register.
- [docs/visionos-shell.md](docs/visionos-shell.md): the app shell as it exists today.
- [docs/BUILD/VISIONOS.md](docs/BUILD/VISIONOS.md), [docs/GAME_DATA_SETUP.md](docs/GAME_DATA_SETUP.md),
  [docs/visionos-interaction.md](docs/visionos-interaction.md): build, data and controls references.
