import SwiftUI

/// The small strip of tabletop buttons (Ground View, Recenter, Pause, Menu, Leave Tabletop, Windows) that stays open during a
/// match. It replaces the big Commands window as the window that opens with the tabletop: the launcher steps aside so the
/// board is clear, and this strip is the one window the player always has. The Commands, Settings and Help windows open
/// from its Windows menu.
///
/// It also owns the "never end up with no window" rule: when the tabletop closes (Leave Tabletop, or the system dismissed the
/// space) it reopens the launcher and closes itself and the Commands window.
///
/// `model`/`store` are stored properties, not `@Environment` reads (see the ornament crash note on HudOrnament).
struct TabletopControlsWindow: View {
    static let id = "tabletop-controls"

    let model: AppModel
    let store: PanelStore
    @Environment(\.openWindow) private var openWindow
    @Environment(\.dismissWindow) private var dismissWindow

    var body: some View {
        HudOrnament(location: .controls, model: model, store: store)
            .padding(16)
            .fixedSize()
            .onChange(of: model.spaceState) { _, state in
                guard state == .closed else { return }
                if model.claimLauncherReopen() { openWindow(id: "launcher") }
                dismissWindow(id: CommandsWindow.id)
                dismissWindow(id: Self.id)
            }
            .textInputBridge(enabled: !model.launcherOpen)
    }
}
