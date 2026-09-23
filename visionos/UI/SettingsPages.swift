import SwiftUI

/// Content of the pages of the Settings window.
struct SettingsPageView: View {
    let page: SettingsWindow.Page

    var body: some View {
        switch page {
        case .workspace: WorkspacePage()
        case .presentation: PresentationPage()
        case .graphics: GraphicsPage()
        case .audio: AudioPage()
        case .controls: ControlsPage()
        case .data: DataPage()
        case .diagnostics: DiagnosticsPage()
        }
    }
}

private struct Row<Content: View>: View {
    @ViewBuilder let content: () -> Content
    var body: some View { content().frame(minHeight: UITheme.minTarget) }
}

private struct Caption: View {
    let text: String
    var body: some View { Text(text).font(.footnote).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true) }
}

// MARK: Workspace

private struct WorkspacePage: View {
    @Environment(PanelStore.self) private var store

    var body: some View {
        Form {
            Section(store.t("Board")) {
                Row { Button { InteractionControls.recenterBoard(); GXXRBridgeRecenter() } label: {
                    Label(store.t("Recenter board"), systemImage: "scope").frame(maxWidth: .infinity, alignment: .leading)
                } }
                Row { Button { InteractionControls.resetWorkspace() } label: {
                    Label(store.t("Reset workspace"), systemImage: "arrow.counterclockwise").frame(maxWidth: .infinity, alignment: .leading)
                } }
                Caption(text: store.t("Recenter puts the board 1.0 m ahead of you and keeps its size. Reset also restores the default size and map zoom."))
                Caption(text: store.t("Move the board with a pinch-drag on the grab bar at its near edge. Use both hands to move, turn and scale it at once. Pan the map with a pinch-drag on the rim."))
            }
            Section(store.t("Board size, distance and height presets")) {
                Caption(text: store.t("Not available yet: the interaction layer only exposes Recenter and Reset. Resize with the two-hand pinch on the grab bar."))
            }
        }
        .formStyle(.grouped)
    }
}

// MARK: Presentation

private struct PresentationPage: View {
    @Environment(PanelStore.self) private var store

    private func toggle(_ german: String, _ id: Int, _ value: Bool) -> some View {
        Row {
            Toggle(store.xr(german), isOn: Binding(get: { value }, set: { _ in store.perform(page: Int(GX_PANEL_MENU_VIEW), id: id) }))
        }
    }

    var body: some View {
        let prefs = store.snapshot.session.prefs
        let ground = store.page(Int(GX_PANEL_MENU_VIEW)).control(16)
        Form {
            Section(store.t("Screen")) {
                Caption(text: store.t("Matches always play on the tabletop. Menus, briefings and videos use the upright screen above the far edge of the board."))
            }
            Section(store.t("Ground View")) {
                Row {
                    let on = store.groundViewRequested || store.snapshot.groundViewMode != 0
                    Toggle(isOn: Binding(get: { on }, set: { _ in store.toggleGroundView() })) {
                        Label(on ? store.t("Leave Ground View") : store.t("Enter Ground View"), systemImage: "figure.walk")
                    }
                    .toggleStyle(.button)
                    .disabled(!on && (ground?.isDisabled ?? true))
                }
                Caption(text: ground?.explain.isEmpty == false ? ground!.explain : store.t("Ground View is available in offline matches while the world can be adjusted."))
                Caption(text: store.t("Then pinch visible open ground to stand there. Leave returns to the unchanged table."))
            }
            Section(store.t("Selection indicators")) {
                toggle("Lebenspunkte", 4, prefs.healthBars)
                toggle("Einheitenringe", 5, prefs.unitRings)
                toggle("Brettkörper", 6, prefs.boardFrame)
                Caption(text: store.t("Health bars, unit rings and the board frame are drawn by the engine; these switches are stored for the presentation layer."))
            }
            Section(store.t("Camera presets")) {
                let cams: [(String, Int)] = [("Classic", 0), ("Table 78°", 1), ("Oblique 65°", 2), ("Top 85°", 3), ("Favorite", 4)]
                ForEach(cams, id: \.1) { cam in
                    Row { Button { store.performExtra(Int(GX_EXTRA_CAMERA_PRESET), value: cam.1) } label: {
                        HStack {
                            Text(store.t(cam.0))
                            Spacer()
                            if Int(store.snapshot.defaultCameraPreset) == cam.1 { Image(systemName: "star.fill").accessibilityLabel(store.t("Favorite")) }
                        }
                    } }
                    .disabled(!store.snapshot.canAdjustWorld)
                }
                Row { Button { store.performExtra(Int(GX_EXTRA_SAVE_CAMERA_DEFAULT)) } label: {
                    Label(store.t("Save current view as favorite"), systemImage: "star").frame(maxWidth: .infinity, alignment: .leading)
                } }.disabled(!store.snapshot.canAdjustWorld)
                Row { Button { store.performExtra(Int(GX_EXTRA_VIEW_BASE)) } label: {
                    Label(store.t("Home base view"), systemImage: "house").frame(maxWidth: .infinity, alignment: .leading)
                } }.disabled(!store.snapshot.canAdjustWorld)
                if !store.refusedNotice.isEmpty { Caption(text: store.refusedNotice) }
            }
        }
        .formStyle(.grouped)
    }
}

// MARK: Graphics

private struct GraphicsPage: View {
    @Environment(PanelStore.self) private var store
    @Environment(GraphicsSettingsStore.self) private var graphics

    var body: some View {
        @Bindable var graphics = graphics
        Form {
            Section(store.t("Graphics")) {
                Row {
                    VStack(alignment: .leading) {
                        HStack { Text(store.t("Render scale")); Spacer(); Text(String(format: "%.2f×", graphics.settings.renderScale)).monospacedDigit() }
                        Slider(value: $graphics.settings.renderScale, in: GraphicsSettings.renderScaleRange, step: 0.05)
                            .accessibilityLabel(store.t("Render scale"))
                    }
                }
                Row {
                    Picker(store.t("Render frame rate cap"), selection: $graphics.settings.renderFpsCap) {
                        ForEach(GraphicsSettings.fpsChoices, id: \.self) { fps in
                            Text("\(fps)").tag(fps)
                        }
                        Text(store.t("Uncapped")).tag(GraphicsSettings.uncappedFps)
                    }
                    .pickerStyle(.segmented)
                }
                Row {
                    Picker(store.t("Shadows"), selection: $graphics.settings.shadowMode) {
                        Text(store.t("Off (fastest)")).tag(GraphicsSettings.ShadowMode.off)
                        Text(store.t("Decals")).tag(GraphicsSettings.ShadowMode.decals)
                        Text(store.t("Volumes (original)")).tag(GraphicsSettings.ShadowMode.volumes)
                    }
                    .pickerStyle(.segmented)
                }
                Row {
                    Picker(store.t("Eye size"), selection: $graphics.settings.eyeTier) {
                        Text(store.t("Balanced")).tag(GraphicsSettings.EyeTier.balanced)
                        Text(store.t("High")).tag(GraphicsSettings.EyeTier.high)
                        Text(store.t("Ultra")).tag(GraphicsSettings.EyeTier.ultra)
                    }
                    .pickerStyle(.segmented)
                }
                Row {
                    Picker(store.t("UI resolution"), selection: $graphics.settings.uiResolution) {
                        Text(store.t("720p")).tag(GraphicsSettings.UIResolution.p720)
                        Text(store.t("1080p")).tag(GraphicsSettings.UIResolution.p1080)
                    }
                    .pickerStyle(.segmented)
                    .accessibilityHint(store.t("Read once at engine start. Takes effect the next time the tabletop is entered."))
                }
                Toggle(store.t("Comfort fade"), isOn: $graphics.settings.comfortFade)
                    .accessibilityHint(store.t("A brief dark fade during Ground View enter, exit and teleport."))
                Toggle(store.t("Focus marker"), isOn: $graphics.settings.focusMarker)
                    .accessibilityHint(store.t("A pointer dot on panels and highlights on the grab bar while pinching."))
                Caption(text: store.t("Applied on the engine thread. Changes to the render scale, eye size and UI resolution take effect when the tabletop is entered again."))
            }
        }
        .formStyle(.grouped)
    }
}

// MARK: Audio

private struct AudioPage: View {
    @Environment(PanelStore.self) private var store
    @Environment(AudioSettingsStore.self) private var audio

    private func slider(_ key: String, _ value: Binding<Double>) -> some View {
        Row {
            VStack(alignment: .leading) {
                HStack { Text(store.t(key)); Spacer(); Text("\(Int((value.wrappedValue * 100).rounded())) %").monospacedDigit() }
                Slider(value: value, in: 0...1).accessibilityLabel(store.t(key))
            }
        }
    }

    var body: some View {
        @Bindable var audio = audio
        Form {
            Section(store.t("Audio")) {
                slider("Master volume", $audio.values.master)
                slider("Music", $audio.values.music)
                slider("Speech", $audio.values.speech)
                slider("Interface sounds", $audio.values.interface2D)
                slider("Battlefield effects", $audio.values.battlefield3D)
            }
            Section(store.t("Spatial battlefield audio")) {
                Row { Toggle(store.t("Spatial battlefield audio"), isOn: $audio.values.spatial) }
                Caption(text: store.t("On: sounds come from where the units are on the table and follow your head. Off: the original camera-relative mix."))
            }
        }
        .formStyle(.grouped)
    }
}

// MARK: Controls & language

private struct ControlsPage: View {
    @Environment(PanelStore.self) private var store
    @State private var additive = false

    var body: some View {
        Form {
            Section(store.t("Controls")) {
                Row {
                    Toggle(store.t("Additive selection"), isOn: $additive)
                        .onChange(of: additive) { _, on in InteractionControls.setAdditive(on) }
                }
                Caption(text: store.t("Every pinch adds to or removes from the selection (Shift in the simulator)."))
                Row { Button { store.performExtra(Int(GX_EXTRA_CANCEL_TARGET)); InteractionControls.cancelAll() } label: {
                    Label(store.t("Cancel targeting"), systemImage: "xmark.circle").frame(maxWidth: .infinity, alignment: .leading)
                } }
                Row { Button { InteractionControls.cancelPlacement() } label: {
                    Label(store.t("Cancel building"), systemImage: "hammer").frame(maxWidth: .infinity, alignment: .leading)
                } }
            }
            Section(store.t("Rotate building")) {
                Row {
                    HStack(spacing: UITheme.controlSpacing) {
                        Button { InteractionControls.rotatePlacement(degrees: -15) } label: {
                            Label(store.t("Rotate left 15°"), systemImage: "rotate.left").frame(maxWidth: .infinity)
                        }
                        Button { InteractionControls.rotatePlacement(degrees: 15) } label: {
                            Label(store.t("Rotate right 15°"), systemImage: "rotate.right").frame(maxWidth: .infinity)
                        }
                    }
                }
                Caption(text: store.t("Rotates the building preview while a placement is pending."))
            }
            Section(store.t("Language")) {
                Row {
                    Picker(store.t("Language"), selection: Binding(get: { store.language }, set: { store.setLanguage($0) })) {
                        ForEach(UILang.allCases) { Text($0.nativeName).tag($0) }
                    }
                    .pickerStyle(.segmented)
                }
                Caption(text: store.t("Switches this app at once. The game's own text is staged and needs a restart of the app."))
                if !store.snapshot.languageStatusText.isEmpty { Caption(text: store.snapshot.languageStatusText) }
            }
        }
        .formStyle(.grouped)
    }
}

extension GXPanelSnapshot {
    var languageStatusText: String { cString(languageStatus) }
}

// MARK: Data

private struct DataPage: View {
    @Environment(AppModel.self) private var model
    @Environment(PanelStore.self) private var store
    @Environment(\.openWindow) private var openWindow
    @State private var confirmRemove = false

    var body: some View {
        Form {
            Section(store.t("Game data")) {
                Row {
                    Label(model.isGameDataReady ? store.t("Ready") : store.t("Not ready"),
                          systemImage: model.isGameDataReady ? "checkmark.circle.fill" : "exclamationmark.triangle")
                }
                Text(model.gameDataStatusLine).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                Row { Button {
                    openWindow(id: "launcher")
                    model.presentFolderPicker(.gameFolder)
                } label: {
                    Label(store.t("Choose game folder…"), systemImage: "folder").frame(maxWidth: .infinity, alignment: .leading)
                } }
                Caption(text: store.t("The folder picker lives in the launcher window."))
                Row { Button(role: .destructive) { confirmRemove = true } label: {
                    Label(store.t("Remove imported data"), systemImage: "trash").frame(maxWidth: .infinity, alignment: .leading)
                } }
                .disabled(model.isGameDataBusy)
                Caption(text: store.t("Removes only the copy inside this app. Your original files stay untouched."))
            }
            Section(store.t("Documentation")) {
                Row { Link(destination: URL(string: "https://github.com/Cesarus85/Generals-Zero-Hour-XR/tree/main/docs")!) {
                    Label(store.t("Open the documentation"), systemImage: "book").frame(maxWidth: .infinity, alignment: .leading)
                } }
            }
        }
        .formStyle(.grouped)
        .confirmationDialog(store.t("Remove the imported game data?"), isPresented: $confirmRemove, titleVisibility: .visible) {
            Button(store.t("Remove imported data"), role: .destructive) { model.removeImportedData() }
            Button(store.t("Keep it"), role: .cancel) {}
        } message: {
            Text(store.t("Removes only the copy inside this app. Your original files stay untouched."))
        }
    }
}

// MARK: Diagnostics

private struct DiagnosticsPage: View {
    @Environment(PanelStore.self) private var store
    @State private var status = GXEngineHostStatus()
    @State private var logTail = ""

    private func phaseName(_ p: GXEngineHostPhase) -> String {
        switch p {
        case GX_ENGINE_IDLE: return store.t("Idle")
        case GX_ENGINE_BOOTING: return store.t("Booting")
        case GX_ENGINE_RUNNING: return store.t("Running")
        case GX_ENGINE_PAUSED: return store.t("Paused")
        case GX_ENGINE_FAILED: return store.t("Failed")
        case GX_ENGINE_STOPPING: return store.t("Stopping")
        default: return "?"
        }
    }

    private func line(_ key: String, _ value: String) -> some View {
        HStack { Text(store.t(key)); Spacer(); Text(value).monospacedDigit().foregroundStyle(.secondary) }
            .accessibilityElement(children: .combine)
    }

    var body: some View {
        let logPath = cString(status.logPath)
        Form {
            Section(store.t("Engine")) {
                line("Phase", phaseName(status.phase))
                line("Engine frames per second", String(format: "%.1f", status.engineFps))
                line("Simulation rate", status.logicHz > 0 ? String(format: "%.1f Hz", status.logicHz) : store.t("n/a"))
                line("Frames produced", "\(status.framesProduced)")
                line("Frames skipped", "\(status.framesSkipped)")
                line("Last frame", String(format: "%.1f ms", status.lastFrameMs))
                line("Longest frame", String(format: "%.0f ms", status.longestFrameMs))
            }
            Section(store.t("Render targets")) {
                line("Render targets", "\(status.ringSlotsInUse)/\(status.ringSlots) \(store.t("slots in use")), \(String(format: "%.0f MB", status.ringMegabytes))")
                line("Synchronisation", cString(status.syncMode).isEmpty ? store.t("n/a") : cString(status.syncMode))
                line("Renderer", cString(status.renderer).isEmpty ? store.t("n/a") : cString(status.renderer))
            }
            Section(store.t("Engine log")) {
                ScrollView(.horizontal) {
                    Text(logTail.isEmpty ? store.t("No log yet") : logTail)
                        .font(.caption.monospaced())
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(minHeight: 120)
                if !logPath.isEmpty, FileManager.default.fileExists(atPath: logPath) {
                    Row { ShareLink(item: URL(fileURLWithPath: logPath)) {
                        Label(store.t("Share the log"), systemImage: "square.and.arrow.up").frame(maxWidth: .infinity, alignment: .leading)
                    } }
                }
                Caption(text: store.t("The log lists file paths of your game data and technical details of your device. Read it before you share it."))
            }
        }
        .formStyle(.grouped)
        .task {
            while !Task.isCancelled {
                GXEngineHost_GetStatus(&status)
                var buffer = [CChar](repeating: 0, count: 4096)
                GXEngineHost_ReadLogTail(&buffer, buffer.count, 10)
                logTail = String(cString: buffer)
                try? await Task.sleep(for: .milliseconds(700))
            }
        }
    }
}
