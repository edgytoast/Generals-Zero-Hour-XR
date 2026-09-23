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

    /// Quick orders within reach (the most used Commands-window controls, same ids and actions): stop, attack-move, guard,
    /// scatter, all units; then groups 1 to 5 with their member counts. Native buttons: the system highlights the one the
    /// player looks at, which the engine-drawn control bar cannot do.
    private static let quickOrders = [5, 2, 4, 6, 10]
    private static let quickGroups = [20, 21, 22, 23, 24]
    private let page = Int(GX_PANEL_COMMANDS)

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HudOrnament(location: .controls, model: model, store: store)
            Divider()
            HStack(spacing: UITheme.controlSpacing) {
                ForEach(Self.quickOrders.compactMap { store.commands.control($0) }) { item in
                    PanelControlView(item: item, page: page).frame(width: 150)
                }
                Divider().frame(height: UITheme.minTarget)
                ForEach(Self.quickGroups.compactMap { store.commands.control($0) }) { item in
                    PanelControlView(item: item, page: page).frame(width: 64)
                }
                Button { openWindow(id: CommandsWindow.id) } label: {
                    Label(store.t("More commands"), systemImage: "list.bullet.rectangle").frame(minHeight: UITheme.minTarget)
                }
                .buttonStyle(.bordered)
                .hoverEffect(.highlight)
            }
            .accessibilityElement(children: .contain)
            .accessibilityLabel(store.t("Quick orders"))
        }
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
