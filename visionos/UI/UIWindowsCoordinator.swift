import SwiftUI

/// Attached to the launcher window: opens the Commands window together with the immersive space (and closes it with it), opens
/// the windows named by `-openWindow`, routes host-side panel effects and adds the toolbar ornament. Kept out of LauncherView.swift
/// so the launcher stays untouched.
///
/// `model`/`store` are passed in by the caller (`GeneralsZHXRApp.swift`, which already has them from `@State`), not read
/// through `@Environment` here: a `ViewModifier` whose `body(content:)` installs an ornament (`.hudOrnament` below) was
/// observed to crash on this SDK the moment ANY of its own `@Environment(SomeObservableType.self)` properties are read —
/// not only reads textually inside the ornament's content closure — with the same `TransformOrnament.updateValue()` ->
/// "No Observable object of type ... found" fatal error documented on `HudOrnament` (visionos/App/HudOrnament.swift).
/// Reproduced here with `AppModel` before this fix (xrOS 27.0 simulator). System `EnvironmentValues` keys
/// (`\.openWindow`, `\.dismissWindow`) are unaffected and stay as `@Environment`.
struct UIWindowsCoordinator: ViewModifier {
    let model: AppModel
    let store: PanelStore
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
                case .open:
                    // The small controls strip, not the big Commands window (it opens from the strip's Windows menu). The
                    // system restores windows that were open when the app last quit, so close a restored Commands window:
                    // the table starts clear.
                    dismissWindow(id: CommandsWindow.id)
                    openWindow(id: TabletopControlsWindow.id)
                    // The launcher is about a metre tall and stands between the player and the board. With a real game it
                    // steps aside; the controls strip reopens it (Windows > Open Launcher, and Leave Tabletop). The short
                    // wait lets the strip be placed beside the launcher first.
                    if model.hidesLauncherOnTabletop {
                        Task { @MainActor in
                            try? await Task.sleep(for: .milliseconds(800))
                            if model.spaceState == .open { dismissWindow(id: "launcher") }
                        }
                    }
                case .closed:
                    dismissWindow(id: CommandsWindow.id)
                    dismissWindow(id: TabletopControlsWindow.id)
                default: break
                }
            }
    }
}
