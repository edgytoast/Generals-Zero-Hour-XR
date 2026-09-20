import SwiftUI

struct LauncherView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        @Bindable var model = model
        VStack(alignment: .leading, spacing: 20) {
            VStack(alignment: .leading, spacing: 4) {
                Text("Generals: Zero Hour XR")
                    .font(.extraLargeTitle2)
                Text("Apple Vision Pro tabletop shell (renderer validation build)")
                    .font(.title3)
                    .foregroundStyle(.secondary)
            }

            GroupBox("Status") {
                VStack(alignment: .leading, spacing: 6) {
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
                    Label("Enter Tabletop", systemImage: "cube.transparent")
                }
                .disabled(model.spaceState != .closed)

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

            GroupBox("Game data") {
                VStack(alignment: .leading, spacing: 6) {
                    // Placeholder for the future data-setup UI (folder picker / importer / validator).
                    Text(model.gameDataPresent ? "Game data folder has content." : "No game data yet. Data setup arrives in a later build.")
                        .font(.callout)
                    Text(model.gameDataPath).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
                    Text("Copy your own legally owned Generals / Zero Hour files to this folder with the Files app. No game data ships with this app.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }

            Toggle("Test engine hand-off path (offscreen per-eye textures)", isOn: $model.useFakeEngineTextures)
                .disabled(model.spaceState != .closed)
        }
        .padding(28)
        .frame(minWidth: 560, minHeight: 520)
        .task {
            if LaunchOptions.autoImmersive { await enter() }
            while !Task.isCancelled {
                model.refresh()
                try? await Task.sleep(for: .milliseconds(500))
            }
        }
        .onChange(of: scenePhase) { _, phase in
            switch phase {
            case .background: GXXRBridgeNotifyLifecycle(Int32(PLATFORM_LIFECYCLE_SUSPEND.rawValue))
            case .active: GXXRBridgeNotifyLifecycle(Int32(PLATFORM_LIFECYCLE_RESUME.rawValue))
            default: break
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.didReceiveMemoryWarningNotification)) { _ in
            GXXRBridgeNotifyLifecycle(Int32(PLATFORM_LIFECYCLE_MEMORY_WARNING.rawValue))
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
