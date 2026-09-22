import SwiftUI

/// Attached to the launcher window: opens the Commands window together with the immersive space (and closes it with it), opens
/// the windows named by `-openWindow`, routes host-side panel effects and adds the toolbar ornament. Kept out of LauncherView.swift
/// so the launcher stays untouched.
struct UIWindowsCoordinator: ViewModifier {
    @Environment(AppModel.self) private var model
    @Environment(PanelStore.self) private var store
    @Environment(\.openWindow) private var openWindow
    @Environment(\.dismissWindow) private var dismissWindow
    @State private var started = false

    func body(content: Content) -> some View {
        content
            .hudOrnament(location: .launcher, model: model, store: store)
            .task {
                guard !started else { return }
                started = true
                store.effectHandler = { result in
                    if Int(result.host) == Int(GX_HOST_HIDE_COMMANDS) { dismissWindow(id: CommandsWindow.id) }
                }
                for name in UILaunchOptions.openWindows {
                    switch name {
                    case "commands": openWindow(id: CommandsWindow.id)
                    case "settings": openWindow(id: SettingsWindow.id)
                    case "help": openWindow(id: HelpWindow.id)
                    default: break
                    }
                }
            }
            .onChange(of: model.spaceState) { _, state in
                switch state {
                case .open: openWindow(id: CommandsWindow.id)
                case .closed: dismissWindow(id: CommandsWindow.id)
                default: break
                }
            }
    }
}
