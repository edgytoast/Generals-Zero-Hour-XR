import CompositorServices
import SwiftUI

@main
struct GeneralsZHXRApp: App {
    @State private var model = AppModel()
    @State private var panelStore = PanelStore()
    @State private var graphicsStore = GraphicsSettingsStore()
    @State private var audioStore = AudioSettingsStore()
    @State private var immersionStyle: ImmersionStyle = .mixed
    /// App-level phase: `.active` while any scene (a window or the immersive space) is active. It must not come from one
    /// window, because the launcher closes while the tabletop runs.
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup(id: "launcher") {
            LauncherView()
                .environment(model)
                .environment(panelStore)
                .modifier(UIWindowsCoordinator(model: model, store: panelStore))
        }
        .defaultSize(width: 760, height: 1000)
        .onChange(of: scenePhase) { _, phase in
            // The engine thread parks (and silences audio) while the app is not active, exactly like the mobile
            // background handling of the 2D port; the compositor keeps presenting the last frame meanwhile.
            GXEngineHost_Pause(GX_PAUSE_SCENE, phase != .active)
            switch phase {
            case .background: GXXRBridgeNotifyLifecycle(Int32(PLATFORM_LIFECYCLE_SUSPEND.rawValue))
            case .active:
                GXXRBridgeNotifyLifecycle(Int32(PLATFORM_LIFECYCLE_RESUME.rawValue))
                Task { await model.gameDataBecameActive() }
            default: break
            }
        }

        // Native SwiftUI windows of the spatial UI (docs/visionos-ui.md). visionOS 26 window placement: `defaultWindowPlacement`
        // positions a NEW window relative to a window that is already open (`WindowPlacement.Position.trailing / leading /
        // below(WindowProxy)`, found through `context.windows`); the system picks the depth and height, and an app cannot give a
        // window an absolute position in a Full Space. When the reference window is closed the system default placement applies.
        // The tabletop controls strip: the one small window that stays open during a match (TabletopControlsWindow.swift).
        WindowGroup(id: TabletopControlsWindow.id) {
            TabletopControlsWindow(model: model, store: panelStore)
                .environment(model)
                .environment(panelStore)
        }
        .windowResizability(.contentSize)
        .defaultWindowPlacement { content, context in
            let size = content.sizeThatFits(.unspecified)
            if let launcher = context.windows.first(where: { $0.id == "launcher" }) { return WindowPlacement(.trailing(launcher), size: size) }
            return WindowPlacement(.utilityPanel, size: size)
        }

        WindowGroup(id: CommandsWindow.id) {
            CommandsWindow(appModel: model, store: panelStore)
                .environment(model)
                .environment(panelStore)
        }
        .defaultSize(width: 580, height: 760)
        .defaultWindowPlacement { content, context in
            let size = content.sizeThatFits(.unspecified)
            if let launcher = context.windows.first(where: { $0.id == "launcher" }) { return WindowPlacement(.trailing(launcher), size: size) }
            return WindowPlacement(.utilityPanel, size: size)
        }

        WindowGroup(id: SettingsWindow.id) {
            SettingsWindow()
                .environment(model)
                .environment(panelStore)
                .environment(graphicsStore)
                .environment(audioStore)
        }
        .defaultSize(width: 980, height: 720)
        .defaultWindowPlacement { content, context in
            let size = content.sizeThatFits(.unspecified)
            if let commands = context.windows.first(where: { $0.id == CommandsWindow.id }) { return WindowPlacement(.trailing(commands), size: size) }
            if let launcher = context.windows.first(where: { $0.id == "launcher" }) { return WindowPlacement(.below(launcher), size: size) }
            return WindowPlacement(.utilityPanel, size: size)
        }

        WindowGroup(id: HelpWindow.id) {
            HelpWindow()
                .environment(model)
                .environment(panelStore)
        }
        .defaultSize(width: 760, height: 820)
        .defaultWindowPlacement { content, context in
            let size = content.sizeThatFits(.unspecified)
            if let launcher = context.windows.first(where: { $0.id == "launcher" }) { return WindowPlacement(.leading(launcher), size: size) }
            return WindowPlacement(.utilityPanel, size: size)
        }

        // Stereo tabletop rendered with Metal through Compositor Services.
        // .mixed keeps passthrough visible; the layer's alpha channel (premultiplied)
        // decides what the player sees through the scene.
        ImmersiveSpace(id: AppModel.spaceID) {
            CompositorLayer(configuration: TabletopLayerConfiguration()) { layerRenderer in
                model.layerRendererReady(layerRenderer)
            }
        }
        .immersionStyle(selection: $immersionStyle, in: .mixed)
    }
}
