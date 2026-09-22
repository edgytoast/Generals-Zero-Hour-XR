import SwiftUI
import UniformTypeIdentifiers

struct LauncherView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace

    var body: some View {
        @Bindable var model = model
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Generals: Zero Hour XR")
                        .font(.extraLargeTitle2)
                    Text("Apple Vision Pro tabletop")
                        .font(.title3)
                        .foregroundStyle(.secondary)
                }

                GameDataSection()

                EngineSection()

                GroupBox("Tabletop") {
                    VStack(alignment: .leading, spacing: 8) {
                        Text(model.statusText).font(.headline)
                        if model.spaceState == .open {
                            Text("Frames: \(model.frames)  |  \(model.formats)  |  board placed: \(model.placed ? "yes" : "no")")
                                .font(.callout).foregroundStyle(.secondary)
                        }
                        if !model.lastMessage.isEmpty {
                            Text(model.lastMessage).font(.footnote).foregroundStyle(.secondary)
                        }
                        Text(model.inputSummary).font(.footnote).foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }

                HStack(spacing: 16) {
                    Button {
                        Task { await enter() }
                    } label: {
                        Label(model.isGameDataReady ? "Enter Tabletop" : "Enter Tabletop (test scene)", systemImage: "cube.transparent")
                    }
                    .disabled(model.spaceState != .closed || !model.canEnterTabletop)

                    Button {
                        Task { await leave() }
                    } label: {
                        Label("Leave Tabletop", systemImage: "xmark.circle")
                    }
                    .disabled(model.spaceState != .open)

                    Button {
                        GXXRBridgeRecenter()
                    } label: {
                        Label("Recenter", systemImage: "scope")
                    }
                    .disabled(model.spaceState != .open)
                }
                if !model.canEnterTabletop {
                    Text("Enter Tabletop unlocks when your game data is ready.")
                        .font(.footnote).foregroundStyle(.secondary)
                }

                Toggle("Test engine hand-off path (offscreen per-eye textures)", isOn: $model.useFakeEngineTextures)
                    .disabled(model.spaceState != .closed)
            }
            .padding(28)
        }
        .frame(minWidth: 560, minHeight: 520)
        .modifier(ImmersiveCycleDriver())
        .textInputBridge()
        .fileImporter(isPresented: $model.filePickerPresented,
                      allowedContentTypes: [.folder],
                      allowsMultipleSelection: false) { result in
            model.handlePickerResult(result)
        }
        .onAppear { model.launcherOpen = true }
        .onDisappear { model.launcherOpen = false }
        .task {
            // Once per app run: the launcher can close and reopen, the start-up must not repeat. Status polling and the
            // scene-phase pause live at app level (AppModel.startAppServices, GeneralsZHXRApp) so they keep working
            // while this window is closed.
            guard !model.bootstrapped else { return }
            model.bootstrapped = true
            model.startAppServices()
            model.startEngineInfrastructure()
            await model.startGameData()
            model.autoStartEngineIfRequested()
            if LaunchOptions.autoImmersive { await enter() }
        }
    }

    private func enter() async {
        guard model.spaceState == .closed else { return }
        model.spaceState = .opening
        let result = await openImmersiveSpace(id: AppModel.spaceID)
        switch result {
        case .opened: break  // spaceState becomes .open in layerRendererReady
        case .userCancelled, .error:
            model.spaceState = .closed
            model.lastMessage = "Immersive space did not open (\(result))."
        @unknown default: model.spaceState = .closed
        }
    }

    private func leave() async {
        guard model.spaceState == .open else { return }
        model.spaceState = .closing
        await dismissImmersiveSpace()
        model.spaceState = .closed
    }
}

// MARK: - Engine section

/// "Start Game", booting progress, failure reasons (with the log path and the last log lines) and the running state.
/// The engine boots on its own thread and never blocks the UI or the compositor.
private struct EngineSection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        GroupBox("Engine") {
            VStack(alignment: .leading, spacing: 10) {
                switch model.enginePhase {
                case GX_ENGINE_IDLE:
                    StatusHeadline(symbol: "play.circle", tint: .secondary, title: model.isGameDataReady ? "Ready to start" : "Waiting for game data",
                                   subtitle: model.isGameDataReady ? "Start Game boots the engine in the background (about a minute). You can enter the tabletop while it boots." : "The engine needs your validated game data.")
                    if !model.engineStartNotice.isEmpty {
                        Label(model.engineStartNotice, systemImage: "exclamationmark.triangle.fill").font(.callout).foregroundStyle(.orange).textSelection(.enabled)
                    }
                    Button {
                        model.startEngine()
                    } label: {
                        Label("Start Game", systemImage: "play.fill")
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!model.canStartEngine)
                case GX_ENGINE_BOOTING:
                    HStack(spacing: 12) {
                        ProgressView()
                        VStack(alignment: .leading, spacing: 2) {
                            Text(model.engineHeadline).font(.headline)
                            Text(model.engineProgress).font(.callout).foregroundStyle(.secondary)
                        }
                    }
                    if !model.engineLastLogLine.isEmpty {
                        Text(model.engineLastLogLine).font(.caption.monospaced()).foregroundStyle(.secondary).lineLimit(2)
                    }
                case GX_ENGINE_FAILED:
                    StatusHeadline(symbol: "xmark.octagon.fill", tint: .red, title: "The engine could not start", subtitle: model.engineError.isEmpty ? "No reason was recorded. See the log." : model.engineError)
                    if !model.engineLogTail.isEmpty {
                        Text(model.engineLogTail).font(.caption.monospaced()).foregroundStyle(.secondary).lineLimit(12).textSelection(.enabled)
                    }
                    Text("Log file: \(model.engineLogPath)").font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                    Text("Fix the reason above (usually missing or damaged game files), then restart the app: the engine can be started once per launch.")
                        .font(.footnote).foregroundStyle(.secondary)
                default:
                    StatusHeadline(symbol: model.enginePhase == GX_ENGINE_PAUSED ? "pause.circle.fill" : "checkmark.circle.fill",
                                   tint: model.enginePhase == GX_ENGINE_PAUSED ? .yellow : .green,
                                   title: model.engineIsFake ? "Fake engine (test scene)" : model.engineHeadline,
                                   subtitle: model.engineProgress)
                    if !model.compositorFpsText.isEmpty {
                        Text(model.compositorFpsText).font(.caption.monospaced()).foregroundStyle(.secondary)
                    }
                    if model.engineLogicHz > 0 {
                        Text(String(format: "Simulation: %.1f Hz (target 30)", model.engineLogicHz)).font(.caption.monospaced()).foregroundStyle(.secondary)
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

// MARK: - Game data section

/// Status, folder picker, import progress, validation results and the "where do I get the
/// files" help. Everything is driven by `AppModel.gameData`.
private struct GameDataSection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        GroupBox("Game data") {
            VStack(alignment: .leading, spacing: 12) {
                if let notice = model.gameDataNotice {
                    Label(notice, systemImage: model.gameDataNoticeIsError ? "exclamationmark.triangle.fill" : "info.circle.fill")
                        .font(.callout)
                        .foregroundStyle(model.gameDataNoticeIsError ? Color.orange : Color.secondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }

                switch model.gameData {
                case .notConfigured: NotConfiguredView()
                case .validating: ValidatingView()
                case .importing(let progress): ImportingView(progress: progress)
                case .ready(let paths): ReadyView(paths: paths)
                case .invalid(let report): InvalidView(report: report)
                }

                if let rejected = model.rejectedSelection {
                    Divider()
                    Label("The folder you just chose was not used. Your working game data is unchanged.", systemImage: "arrow.uturn.backward.circle")
                        .font(.callout).foregroundStyle(.secondary)
                    InvalidView(report: rejected)
                }

                if let interrupted = model.interruptedImport, !model.isGameDataBusy {
                    InterruptedView(interrupted: interrupted)
                }

                if !model.isGameDataBusy { PickerControls() }

                WhereToGetFiles()
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .confirmationDialog("Remove the imported game data?", isPresented: $model.confirmRemoveImported, titleVisibility: .visible) {
            Button("Remove imported data", role: .destructive) { model.removeImportedData() }
            Button("Keep it", role: .cancel) {}
        } message: {
            Text("This deletes only the copy stored inside this app. Your original files are not touched.")
        }
    }
}

private struct StatusHeadline: View {
    let symbol: String
    let tint: Color
    let title: String
    let subtitle: String?

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: symbol).font(.title2).foregroundStyle(tint)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.headline)
                if let subtitle { Text(subtitle).font(.callout).foregroundStyle(.secondary) }
            }
        }
    }
}

private struct NotConfiguredView: View {
    var body: some View {
        StatusHeadline(symbol: "folder.badge.questionmark", tint: .secondary, title: "Not configured",
                       subtitle: "Choose the folder with your own Generals and Zero Hour files. The app copies them once into its private storage. About 2.7 GB of free space is needed.")
    }
}

private struct ValidatingView: View {
    @Environment(AppModel.self) private var model
    var body: some View {
        HStack(spacing: 12) {
            ProgressView()
            VStack(alignment: .leading, spacing: 2) {
                Text("Checking files...").font(.headline)
                if !model.validationSubject.isEmpty {
                    Text(model.validationSubject).font(.callout).foregroundStyle(.secondary)
                }
            }
        }
    }
}

private struct ImportingView: View {
    @Environment(AppModel.self) private var model
    let progress: ImportProgress

    private var phaseTitle: String {
        switch progress.phase {
        case .planning: return "Reading the folder..."
        case .checkingSpace: return "Checking free storage..."
        case .copying: return "Copying game data"
        case .verifying: return "Verifying the copy..."
        case .committing: return "Finishing..."
        case .cleaning, .done: return "Cleaning up..."
        }
    }

    private func bytes(_ v: UInt64) -> String { ByteCountFormatter.string(fromByteCount: Int64(v), countStyle: .file) }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(progress.cancelRequested ? "Stopping..." : phaseTitle).font(.headline)
            ProgressView(value: progress.fraction)
                .progressViewStyle(.linear)
            HStack {
                Text("\(bytes(progress.bytesDone)) of \(bytes(progress.bytesTotal))  (\(Int(progress.fraction * 100)) %)")
                Spacer()
                if progress.bytesPerSecond > 1_000_000 {
                    Text("\(bytes(UInt64(progress.bytesPerSecond)))/s")
                }
            }
            .font(.callout).foregroundStyle(.secondary)
            Text("File \(progress.filesDone) of \(progress.filesTotal)" + (progress.currentFile.isEmpty ? "" : "  |  \(progress.currentFile)"))
                .font(.footnote.monospaced()).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
            if progress.bytesReused > 0 {
                Text("Resumed: \(bytes(progress.bytesReused)) from the earlier attempt was reused.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            Button(role: .cancel) { model.cancelImport() } label: {
                Label("Cancel import", systemImage: "xmark.circle")
            }
            .disabled(progress.cancelRequested)
            Text("You can cancel safely. Files already copied are kept so the import can continue later.")
                .font(.footnote).foregroundStyle(.secondary)
        }
    }
}

private struct ReadyView: View {
    @Environment(AppModel.self) private var model
    let paths: GameDataPaths

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            StatusHeadline(symbol: "checkmark.seal.fill", tint: .green, title: "Ready",
                           subtitle: paths.isComplete ? "Complete game data found." : "Minimal game data found (the game is playable).")
            VStack(alignment: .leading, spacing: 2) {
                Text(paths.source.title).font(.callout)
                Text("Zero Hour: \(paths.zhRoot)").font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                if !paths.baseMerged {
                    Text("Generals:  \(paths.baseRoot)").font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                }
                Text(details).font(.caption).foregroundStyle(.secondary)
            }
            ForEach(paths.optionalNotes, id: \.self) { note in
                Label(note, systemImage: "info.circle").font(.footnote).foregroundStyle(.secondary)
            }
            if paths.source == .imported {
                Button(role: .destructive) { model.confirmRemoveImported = true } label: {
                    Label("Remove imported data", systemImage: "trash")
                }
            }
        }
    }

    private var details: String {
        var parts: [String] = []
        if paths.bytes > 0 { parts.append(ByteCountFormatter.string(fromByteCount: Int64(paths.bytes), countStyle: .file)) }
        if !paths.languages.isEmpty { parts.append("Language: " + paths.languages.joined(separator: ", ")) }
        return parts.joined(separator: "  |  ")
    }
}

private struct InvalidView: View {
    @Environment(AppModel.self) private var model
    let report: GXGDReport

    private var headline: String {
        switch report.verdict {
        case .noSelection: return "No folder chosen"
        case .unreadable: return "This folder cannot be read"
        case .installerOnly: return "This is an installer, not installed game data"
        case .noZeroHour: return "No Zero Hour data found here"
        case .ambiguous: return "More than one Zero Hour install found"
        case .incomplete: return "Some required files are missing"
        case .ready: return "Ready"
        @unknown default: return "Cannot use this folder"
        }
    }

    private var explanation: String? {
        switch report.verdict {
        case .unreadable: return "The folder could not be listed. Check that it is still available, then choose it again."
        case .installerOnly: return "An .iso, .cab, .msi or setup.exe was found. Install or fully extract Generals and Zero Hour on a computer first, then choose the installed folder."
        case .noZeroHour: return "Choose the Zero Hour folder (the one containing INIZH.big) or its parent folder. Folders up to two levels deeper are searched."
        case .ambiguous: return "Choose the one install you want to use:"
        default: return nil
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            StatusHeadline(symbol: "exclamationmark.triangle.fill", tint: .orange, title: headline, subtitle: explanation)

            if report.verdict == .ambiguous {
                ForEach(report.zhChoices, id: \.self) { choice in
                    Text(choice).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                }
            }

            if report.verdict == .incomplete {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Zero Hour: \(report.zhRoot)").font(.caption.monospaced()).foregroundStyle(.secondary)
                    Text("Generals:  \(report.baseFolderMissing ? "not found" : report.baseRoot)")
                        .font(.caption.monospaced()).foregroundStyle(.secondary)
                }
                if report.baseFolderMissing {
                    Text("The base Generals folder (the one containing Terrain.big) was not found next to or inside the Zero Hour folder. Choose the parent folder that holds both games, or choose the Generals folder separately.")
                        .font(.callout)
                }
                let zh = report.missing.filter { !$0.isBaseGenerals }
                let base = report.baseFolderMissing ? [] : report.missing.filter { $0.isBaseGenerals }
                if !zh.isEmpty { MissingList(title: "Missing Zero Hour files", items: zh) }
                if !base.isEmpty { MissingList(title: "Missing Generals files", items: base) }
                if !report.damaged.isEmpty {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Damaged or incomplete files (copy them again)").font(.subheadline.bold())
                        ForEach(report.damaged, id: \.self) { d in
                            Text(d).font(.caption.monospaced()).foregroundStyle(.secondary)
                        }
                    }
                }
                if !report.weatherFound {
                    Text("Missing: Data/INI/Default/Weather.ini (normally inside INI.big).").font(.callout)
                }
                if !report.languageFound {
                    Text("Missing: Zero Hour language text (Data/<Language>/generals.csf, normally inside EnglishZH.big or the same for your language). A Generals-only string table is not enough.")
                        .font(.callout)
                }
            }

            ForEach(report.warnings, id: \.self) { w in
                Label(w, systemImage: "info.circle").font(.footnote).foregroundStyle(.secondary)
            }
        }
    }
}

private struct MissingList: View {
    let title: String
    let items: [GXGDMissingItem]
    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).font(.subheadline.bold())
            ForEach(items, id: \.display) { item in
                HStack(alignment: .top, spacing: 6) {
                    Text(item.name).font(.caption.monospaced())
                    Text("- " + item.reason).font(.caption).foregroundStyle(.secondary)
                }
            }
        }
    }
}

private struct InterruptedView: View {
    @Environment(AppModel.self) private var model
    let interrupted: InterruptedImport

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            StatusHeadline(symbol: "arrow.clockwise.circle.fill", tint: .blue, title: "An import was interrupted",
                           subtitle: "\(interrupted.filesStaged) of \(interrupted.filesTotal) files (\(ByteCountFormatter.string(fromByteCount: Int64(interrupted.bytesStaged), countStyle: .file))) from \(interrupted.sourceName) were already copied. Resuming skips them.")
            HStack(spacing: 12) {
                Button { model.resumeInterruptedImport() } label: { Label("Resume import", systemImage: "play.circle") }
                    .disabled(!interrupted.canResumeAutomatically)
                Button(role: .destructive) { model.discardInterruptedImport() } label: { Label("Discard partial import", systemImage: "trash") }
            }
            if !interrupted.canResumeAutomatically {
                Text("To resume, choose the same folder again.").font(.footnote).foregroundStyle(.secondary)
            }
        }
    }
}

private struct PickerControls: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 12) {
                Button {
                    model.presentFolderPicker(.gameFolder)
                } label: {
                    Label(model.isGameDataReady ? "Replace game data..." : "Choose game folder...", systemImage: "folder.badge.plus")
                }
                .buttonStyle(.borderedProminent)

                if model.awaitingBaseFolder {
                    Button {
                        model.presentFolderPicker(.baseFolder)
                    } label: {
                        Label("Choose separate Generals folder...", systemImage: "folder")
                    }
                }

                Button { model.recheck() } label: { Label("Check again", systemImage: "arrow.clockwise") }
            }
            Toggle(isOn: Binding(get: { model.importInPlace }, set: { model.setImportInPlace($0) })) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Use files in place (advanced)")
                    Text("Skips the copy and reads your folder where it is. It must stay available; the copy is safer and faster to load.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            Text("Tip: files you place in this app's Documents/GameData folder with the Files app are detected automatically.")
                .font(.footnote).foregroundStyle(.secondary)
        }
    }
}

private struct WhereToGetFiles: View {
    var body: some View {
        DisclosureGroup("Where do I get the files?") {
            VStack(alignment: .leading, spacing: 8) {
                Text("This app does not include any game files. You need your own legally owned copy of Command & Conquer: Generals and Command & Conquer: Generals - Zero Hour, installed on a computer.")
                Text("Steam: Zero Hour is app ID 2732960 and the base game Generals is app ID 2229870. Install them with the Steam client, then copy the game folders to this headset (Files app, AirDrop or iCloud Drive) or choose them from a connected drive. Keep the folders exactly as installed: Steam keeps the base game inside a ZH_Generals folder, and that layout is supported.")
                Text("Discs (CD/ISO): install both games on a computer first and copy the installed folders. Raw ISO, CAB, MSI and setup.exe files cannot be used.")
                Text("Copy everything, including the Data folder and the videos. Both games are needed because Zero Hour builds on Generals. See docs/GAME_DATA_SETUP.md in the project for the full list of required files.")
            }
            .font(.callout)
            .foregroundStyle(.secondary)
            .padding(.top, 6)
        }
    }
}
