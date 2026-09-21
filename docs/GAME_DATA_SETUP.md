# Game data setup for the Apple Vision Pro app

The visionOS app is an engine and a launcher. It contains **no game data**. You bring your own
legally owned copy of *Command & Conquer: Generals* and *Command & Conquer: Generals - Zero Hour*,
the same way as for the Quest and desktop ports. Nothing in this repository, in the app bundle or
in these instructions provides, downloads or reproduces retail game files, and the app never
fetches game data from the network. The test fixtures used by the automated tests are tiny
synthetic files generated on the fly; they are not game assets.

For where to obtain the games and the Steam layout, see
[HOWTO/GETTING_THE_GAME_FILES.md](HOWTO/GETTING_THE_GAME_FILES.md). The Quest counterpart of this
page is the "Select game data" section of [HOWTO/INSTALLATION_XR.md](HOWTO/INSTALLATION_XR.md); the
visionOS rules are the same because the validator is a port of the Quest one.

## What the app needs

Zero Hour is an expansion, so **both** games are required: the Zero Hour archives *and* the base
Generals archives. The app checks these rules before it accepts a folder. All file names are
matched case-insensitively.

### Zero Hour folder (marker file `INIZH.big`)

| Required archive | Required archive |
| --- | --- |
| `INIZH.big` | `WindowZH.big` |
| `TerrainZH.big` | `MapsZH.big` |
| `TexturesZH.big` | `AudioZH.big` |
| `W3DZH.big` | `SpeechZH.big` |
| `ShadersZH.big` | `MusicZH.big` |

### Base Generals folder (marker file `Terrain.big`)

| Required archive | Required archive |
| --- | --- |
| `INI.big` | `Shaders.big` |
| `Terrain.big` | `Audio.big` |
| `Textures.big` | `Speech.big` |
| `W3D.big` | `Maps.big` |
| `Window.big` | `Music.big` |

### Text and configuration data

* A Zero Hour language string table: `Data/<Language>/generals.csf` (or `generals.str`), either
  loose on disk or inside a top-level Zero Hour archive (Steam ships it inside `EnglishZH.big`).
  A string table that exists only in the base Generals folder does not count, because Zero Hour adds
  interface text of its own.
* `Data/INI/Default/Weather.ini`, either loose or inside any top-level archive of either game
  (Steam ships it inside `INI.big`). Zero-byte files never count.

### Archive integrity

Every top-level `.big` file is opened and its BIGF table is checked against the real file length.
This catches transfers that were cut short even when the file header survived. Only headers and
tables are read, not the payload, so it takes seconds. It is not a checksum: a corrupted payload with
an intact table is not detected. Patch archives such as `PatchZH.big` (which contain an empty
placeholder entry) are accepted.

The parser follows the engine's own reader
(`Core/GameEngineDevice/Source/StdDevice/Common/StdBIGFileSystem.cpp`, `openArchiveFile`): the magic
`BIGF`, a 4 byte size field the engine ignores, a big-endian file count at offset 8, and entries
from offset `0x10`, each `{u32 big-endian offset, u32 big-endian size, NUL-terminated path}` with
`\` or `/` separators. The host test compares the validator's parse with a transcription of the
engine's loader on generated and randomised archives.

## Supported folder layouts

You can choose the Zero Hour folder itself or a parent folder. The search is bounded: at most two
folder levels below the folder you choose and at most 128 folders in total, and folders whose name
starts with a dot are skipped. It is never a scan of your whole storage.

| Layout | What it looks like | How the base game is found |
| --- | --- | --- |
| **Steam** | `Command and Conquer Generals Zero Hour/` containing `INIZH.big` and a `ZH_Generals/` folder that holds the base game | nested inside the Zero Hour folder |
| **Installed from CD/ISO** | two folders side by side, for example `Zero Hour/` and `Generals/` | the only sibling folder that contains `Terrain.big` |
| **Merged** | one folder that contains both the Zero Hour and the base archives | the same folder |
| **Separate folders** | anywhere | you choose the Generals folder separately (`Choose separate Generals folder...`) |

If several Zero Hour installs are found under the folder you chose, the app lists them and asks you to
choose one explicitly. It never guesses.

**Not supported:** a raw `.iso` image, `.cab` archives, `.msi` installers and `setup.exe`. The app does
not mount disc images or run Windows installers. Install both games on a computer (or fully extract
them) and copy the resulting installed folders. If you choose a folder that contains only such files,
the launcher tells you so.

## Minimal versus complete

* **Minimal (playable)**: every archive above, the Zero Hour string table and `Weather.ini`. The
  launcher shows "Ready" and unlocks Enter Tabletop.
* **Complete**: minimal plus intro/campaign videos (`*.bik` loose under `Data/`, or inside an
  archive) and the language voice archives (for example `AudioEnglishZH.big`,
  `SpeechEnglishZH.big`). "Complete" is a heuristic that never blocks play: it only changes the
  message. The voice-archive names are typical of Steam installs and were not verified against a retail
  copy while this was written. Copy the whole installed folders, including `Data`, the videos and
  the language archives, so nothing is missing.

## Getting the files onto the headset

Any of these works. All of them end with either the picker (import) or the drop folder
(`Documents/GameData`).

1. **Files app / AirDrop / iCloud Drive / USB drive (recommended).** Copy the installed game
   folder(s) from your Mac or PC to iCloud Drive, or AirDrop them to the headset, or put them on
   an external drive. In the app choose **Choose game folder...** and pick the folder. The app
   copies it into its private storage. Make iCloud Drive folders available offline first ("Download
   Now"); otherwise reading them has to download every file, which is slow and can fail on a weak
   connection.
2. **Drop folder in the Files app.** The app enables file sharing, so it appears in the Files app
   under *On My Apple Vision Pro > Generals ZH XR*. Copy the game folder(s) into the `GameData` folder
   there. The app detects them without a picker (on launch, when you return to the app, and when
   you press **Check again**) and uses them where they are, so no second copy is made. The app
   excludes that folder from device backup.
3. **From a Mac with Xcode.** With the headset connected, open *Window > Devices and Simulators*,
   select the Vision Pro and the app, and download/replace the app container (`Documents/GameData`
   inside it). This is the developer route and replaces the whole container, so download first, add
   the files, then replace. (Not verified on hardware for this project.)
4. **Simulator.** `xcrun simctl get_app_container <udid> com.generalsx.zerohour.xr.vision data`
   prints the app's data container; copy the folders into `<container>/Documents/GameData`.

### Import versus use in place

* **Import (default).** The folder is validated, then copied into
  `Library/Application Support/GeneralsX/GameData/` inside the app, excluded from backup. The
  engine reads only that copy. It works when the original is disconnected, and it is the fastest to
  load because the copy is on the headset's own storage. It uses about the same amount of space again.
* **Use files in place (advanced).** Turn on the switch before choosing the folder. The app keeps
  a persisted security-scoped bookmark to your folder and reads it where it is: no copy, no extra space.
  The folder must stay available (drive connected, file provider online and downloaded). Loading
  from a Files provider can be much slower than from app storage. Choosing a different folder
  later, or importing, replaces the bookmark.
* **Drop folder.** `Documents/GameData` is always used in place.

If both an imported copy and a drop folder exist, the source you used last is preferred; otherwise
the imported copy wins.

## What the import does

* Validates the folder first. Nothing is copied unless the data would be accepted.
* Copies **every** file below the Zero Hour folder (and the base Generals folder) except: hidden
  files and folders (names starting with `.`), `__MACOSX`, `_CommonRedist`, Windows executables and
  installers (`.exe .dll .msi .cab .iso .bat .cmd .lnk .url .scr .com`), `Thumbs.db`,
  `desktop.ini` and symbolic links. When the base game is nested (Steam), it goes to its own folder
  and is not copied twice.
* Result on the headset:

  ```
  Library/Application Support/GeneralsX/
    GameData/
      ZH/               Zero Hour tree   (CNC_GENERALS_ZH_PATH, the working directory)
      Generals/         base Generals    (CNC_GENERALS_PATH; absent when both were merged)
      install.manifest  the authority that an install exists
    GeneralsZH/         saves, Options.ini, maps, logs (user data; never touched by an import)
  ```

* **Space check first.** It needs the size of the files still to be copied plus headroom (256 MB or
  2 %, whichever is larger). If a previous install exists it stays in place until the new copy is
  verified, so a re-import temporarily needs room for both. When space runs short, the launcher
  says how much is needed and how much is free; use **Remove imported data** if you are replacing
  an old copy.
* **Progress and cancel.** The launcher shows bytes, files, speed and the current file. Cancel is
  safe: files already copied are kept.
* **Verification.** Each copied file is checked by size, and each `.big` also by its BIGF header and
  table. After the last file, the whole copied tree goes through the same validator as the source.
* **Resumable.** A file only gets its final name after it was verified; partial files are
  `*.gxpart` and are deleted. If the import is cancelled, the app is killed, or the headset restarts,
  the launcher offers **Resume import** (or **Discard partial import**). Choosing the same folder
  again also resumes: files that are already present with the right size are reused, and the
  launcher shows how many bytes were reused. A different source folder starts fresh.
* **Atomic.** The new trees are staged in `.staging`, then swapped in with renames. The previous
  install is parked in `.backup` during the swap. If the app dies during the swap, the next launch
  finishes it (the new, fully verified install), so you never end up with a half old, half new
  install. Until the swap the previous install stays usable.
* **Backup.** The whole `GameData` folder is marked "excluded from backup" so 2.7 GB of game data
  does not go to iCloud backups.

Size and time: the two games are about 2.7 GB together for a typical Steam install; the launcher
shows the exact figure. Copying from local storage or a USB drive is a matter of minutes; over iCloud
Drive or a network share it depends on the connection. These durations are estimates, not
measurements on a Vision Pro.

## Reading the validation result

The **Game data** box in the launcher shows one of five states.

| State | Meaning | What to do |
| --- | --- | --- |
| **Not configured** | No data chosen and nothing in `Documents/GameData`. | Choose your game folder. |
| **Checking files...** | Archive tables are being read (seconds). | Wait. |
| **Copying game data** | Import in progress with a progress bar. | Wait, or cancel (safe). |
| **Ready** | Data validated and available to the engine. Shows the source, both folders, size, language and whether the set is complete. | Enter Tabletop. |
| **Cannot use this folder** (headline varies) | The folder was inspected and rejected; the box explains why. | See below. |

Headlines of the rejection state:

| Headline | Meaning | Fix |
| --- | --- | --- |
| This folder cannot be read | The folder could not be listed (not a folder, provider offline, no permission). | Make it available and choose it again. |
| This is an installer, not installed game data | Only `.iso`, `.cab`, `.msi` or `setup.exe` found. | Install/extract on a computer, copy the installed folders. |
| No Zero Hour data found here | No `INIZH.big` within two levels. | Choose the Zero Hour folder or its parent. |
| More than one Zero Hour install found | Several folders contain `INIZH.big`. The paths are listed. | Choose the one you want. |
| Some required files are missing | A Zero Hour folder was found, but files are missing or damaged. The lists name each file with a reason. | Copy the missing files again; re-copy the damaged ones. |

Details in the "Some required files are missing" state:

* **Missing Zero Hour files / Missing Generals files**: archives from the tables above that were not
  found next to their marker file, each with a reason.
* **The base Generals folder was not found**: no folder with `Terrain.big` was found nested, as a
  neighbour, or merged. Choose the parent folder that contains both games, or use **Choose separate
  Generals folder...** (it appears in this state).
* **Damaged or incomplete files**: an archive header or table is invalid, or an entry points beyond the
  end of the file (typically an interrupted copy). The reason is one of `BIGF header`, `BIGF table`,
  `BIGF name`, `BIGF payload bounds`.
* **Missing `Weather.ini` / Zero Hour language text**: see "Text and configuration data".
* Grey notes (warnings) never block: for example an empty placeholder in a patch archive.

## Troubleshooting

* **"No Zero Hour data found" but I copied the game.** The folder you chose has no `INIZH.big` within
  two folder levels, or the file provider did not really copy it (iCloud placeholders). Open the
  folder in the Files app and confirm the archives have their full size.
* **The picker cannot see my folder.** Use a location the Files app can browse: On My Apple Vision Pro,
  iCloud Drive, or an external drive.
* **Base Generals not found on a Steam install.** Keep the `ZH_Generals` folder inside the Zero Hour
  folder. If you separated them, choose the Generals folder separately.
* **Not enough storage.** Free up space, or use *Use files in place*, or remove an older imported copy
  first.
* **Import keeps failing at the same file.** The message names it. Copy that file to the headset
  again (a source file that changes while it is copied, or one that is corrupt, is reported).
* **Data validated but the game does not start.** Validation is structural (archives present, tables
  in bounds, text and weather found). It is not a compatibility guarantee for every retail edition
  or mod; the developer's verified dataset is a Steam-derived install with a nested `ZH_Generals`
  folder. Send the log, not your game files.
* **Start over.** *Remove imported data* deletes only the copy inside the app. Your original files
  and `Documents/GameData` are never touched by the app.

## For developers

* **Sources.** `visionos/GameData/`: `GXGameDataValidator.{h,cpp}` and `GXGameDataInstall.{h,cpp}` are
  portable C++17 with no Apple dependencies; `GXGameDataService.{h,mm}` is the Objective-C++ glue
  (container locations, coordinated reads with `NSFileCoordinator`, free-space query, backup
  exclusion); `AppModel+GameData.swift` is the state machine; `GXXRGameData.h` is the C surface.
* **State model.** `AppModel.gameData` is `notConfigured`, `validating`, `importing(progress)`,
  `ready(paths)` or `invalid(report)`. An interrupted import is reported separately
  (`AppModel.interruptedImport`) so those five states stay exact.
* **Engine bridge.** Before booting the engine:

  ```c
  if (GXXRGameData_IsReady()) {
      char zh[1024], base[1024], user[1024];
      GXXRGameData_GetPaths(zh, sizeof zh, base, sizeof base, user, sizeof user);
      setenv("CNC_GENERALS_ZH_PATH", zh, 1);       // primary root, mounted first
      setenv("CNC_GENERALS_PATH", base, 1);        // base Generals (== zh when merged)
      setenv("GENERALSX_USERDATA_DIR", user, 1);   // optional: same dir the Apple branch derives
      chdir(zh);
  }
  ```

  `userDataRoot` is `<Application Support>/GeneralsX/GeneralsZH`, the directory the engine's Apple
  branch already uses (`GlobalData::BuildUserDataPathFromRegistry`).
* **Launch arguments** (simulator, CI): `-autoImmersive` (opens the tabletop with the built-in test
  scene, unchanged), `-allowNoData` (enables Enter Tabletop without data, test scene),
  `-importFrom <path>` (runs the flow on a folder without the picker), `-importBaseFrom <path>`,
  `-importInPlace`, `-importCancelAfter <seconds>` (presses Cancel that long after an import starts),
  `-resetGameData` (removes the app's own imported copy and saved preferences).
* **Host tests.** `scripts/qa/vision-gamedata-test.sh` builds and runs the validator and importer
  tests with synthetic fixtures: the Android validator scenarios, the engine-parity BIGF checks, the
  import/resume/cancel/space/verification tests and a crash-injection matrix over every step of the
  transaction. `scripts/qa/vision-gamedata-test.sh <ZERO_HOUR> [GENERALS]` additionally validates a
  real folder read-only and prints the report. The binary also has `--make-fixtures <dir>`, which
  writes the fixture trees used for the simulator checks.
* **Unverified.** The picker on a real headset, the copy speed from Files providers, iCloud
  materialisation through coordinated reads, and the "complete" heuristic against retail data have
  not been exercised: no headset and no retail data were available.
