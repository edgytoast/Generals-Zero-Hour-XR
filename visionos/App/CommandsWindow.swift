import SwiftUI

/// The Quest 'Commands' console as a native window: orders, instant actions and selection, groups 1 to 10 with member counts,
/// waypoints, formations, tactics and map views A to D, plus a context card. Controls, labels, states and counts come from the
/// C++ panel model (GXPanelModel_Build over xrCommandLayout); presses go through GXPanelAction_Perform, i.e. the ported
/// applyCommandAction on the engine thread. See docs/visionos-ui.md.
/// `appModel`/`store` are init parameters, not `@Environment`, even though the window's environment also carries them
/// (`GeneralsZHXRApp.swift`'s `WindowGroup` sets both, for the descendants below that still read `@Environment(PanelStore.self)`
/// normally, e.g. `CommandsHelpView`, `PanelControlView`). `CommandsWindow` itself attaches two ornaments in its own body
/// (`commandsHoverOrnament`, `hudOrnament`), and a top-level view that both attaches an ornament and reads
/// `@Environment(SomeObservableType.self)` on itself was observed to crash on this SDK: see the comment on
/// `HudOrnament` (visionos/App/HudOrnament.swift) and `UIWindowsCoordinator` (visionos/UI/UIWindowsCoordinator.swift).
struct CommandsWindow: View {
    static let id = "commands"

    let appModel: AppModel
    let store: PanelStore
    private let page = Int(GX_PANEL_COMMANDS)

    var body: some View {
        let model = store.commands
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                header
                if store.snapshot.engineKind == Int32(GX_ENGINE_KIND_NONE) || !store.snapshot.interactiveGame {
                    noMatchBanner
                }
                StatusCardView(model: model)
                if store.snapshot.session.help {
                    CommandsHelpView()
                } else {
                    organized(model)
                }
            }
            .padding(28)
        }
        .frame(minWidth: 520, idealWidth: 580, minHeight: 520)
        .appWindowStyle()
        .commandsHoverOrnament(store: store)
        .hudOrnament(location: .commands, model: appModel, store: store)
        .onChange(of: appModel.spaceState) { _, state in
            // The launcher hides during a match. If the tabletop closes while only this window is left (the player closed
            // the controls strip), bring the launcher back so the app never ends up with no window.
            if state == .closed {
                if appModel.claimLauncherReopen() { openWindow(id: "launcher") }
                dismissWindow(id: CommandsWindow.id)
            }
        }
    }

    @Environment(\.openWindow) private var openWindow
    @Environment(\.dismissWindow) private var dismissWindow

    private var header: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(store.t("Commands")).font(.extraLargeTitle2).accessibilityAddTraits(.isHeader)
            Spacer()
            if !store.snapshot.session.help, let help = store.commands.control(34) {
                Button { store.perform(page: page, id: 34) } label: { Label(help.label, systemImage: "questionmark.circle") }
                    .buttonStyle(.bordered)
                    .hoverEffect(.highlight)
            }
            if store.snapshot.engineKind == Int32(GX_ENGINE_KIND_SCRIPTED) {
                Label(store.t("Scripted demo state (no engine)"), systemImage: "wand.and.stars")
                    .font(.footnote).foregroundStyle(.secondary)
            }
        }
    }

    private var noMatchBanner: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label(store.t("No match is running"), systemImage: "moon.zzz").font(.headline)
            Text(store.t("The console lights up once a match is running. Start a skirmish or a mission from the game menu."))
                .font(.callout).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    }

    // MARK: layout

    // The Quest console order (orders, "instant & selection" mixed, groups, tactics) is regrouped by what the player wants
    // to do. Same controls and ids from the C++ panel model, same actions (GXPanelAction_Perform); only the arrangement and
    // the section wording are the window's own:
    //   Orders      pick one, then pinch the target on the table (move, attack-move, guard, force attack) + waypoints
    //   Right now   act at once on the selection (stop, scatter, cancel / deselect)
    //   Select      whole-army and find-a-unit shortcuts
    //   Groups      number keys with member counts, then replace / add / centre
    //   Tactics and map views (fold-out, the engine's tactics toggle 37): formation, force move, no-pursuit guard, views A-D
    @ViewBuilder private func organized(_ model: PanelPageModel) -> some View {
        PanelSectionView(title: store.t("Orders"), caption: store.t("Pick an order, then pinch the spot or target on the table.")) {
            grid(controls(model, [1, 2, 4, 3]), columns: 2)
            grid(controls(model, [8]), columns: 1)
        }
        PanelSectionView(title: store.t("Right now"), caption: store.t("Acts at once on the selected units.")) {
            grid(controls(model, [5, 6, 15]), columns: 3)
        }
        PanelSectionView(title: store.t("Select"), caption: nil) {
            grid(controls(model, [10, 9, 11, 12]), columns: 2)
            grid(controls(model, [7, 14, 13]), columns: 3)
            grid(controls(model, [0]), columns: 1)
        }
        groupsSection(model)
        tacticsSection(model)
        grid(controls(model, [35]), columns: 1)
    }

    private func controls(_ model: PanelPageModel, _ ids: [Int]) -> [PanelControlItem] {
        ids.compactMap { model.control($0) }
    }

    private func grid(_ controls: [PanelControlItem], columns: Int) -> some View {
        LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: UITheme.controlSpacing), count: columns),
                  spacing: UITheme.controlSpacing) {
            ForEach(controls) { PanelControlView(item: $0, page: page) }
        }
    }

    /// Groups: ten number keys with member counts (a number alone recalls the group), then the three operations.
    private func groupsSection(_ model: PanelPageModel) -> some View {
        PanelSectionView(title: store.t("Groups"), caption: store.t("A number selects its group. To save one: select units, press Replace or Add, then a number.")) {
            VStack(alignment: .leading, spacing: UITheme.controlSpacing) {
                grid(controls(model, Array(20...29)), columns: 5)
                grid(controls(model, [30, 31, 32]), columns: 3)
            }
        }
    }

    /// The engine's tactics fold-out (control 37 toggles it; its controls exist only while it is open).
    @ViewBuilder private func tacticsSection(_ model: PanelPageModel) -> some View {
        let open = store.snapshot.session.tactics
        VStack(alignment: .leading, spacing: UITheme.controlSpacing) {
            Button { store.perform(page: page, id: 37) } label: {
                Label(store.t("Tactics and map views"), systemImage: open ? "chevron.down" : "chevron.right")
                    .font(.headline)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .buttonStyle(.plain)
            .hoverEffect(.highlight)
            if open {
                grid(controls(model, [40, 41, 42]), columns: 3)
                Text(store.t("Map views: turn on Save view, then press a letter. A letter alone jumps there."))
                    .font(.footnote).foregroundStyle(.secondary)
                grid(controls(model, [43, 44, 45, 46, 47]), columns: 5)
            }
        }
    }
}

/// The in-window help of the Commands window (visionOS wording, English / German).
private struct CommandsHelpView: View {
    @Environment(PanelStore.self) private var store

    private var pages: [(title: String, body: String)] {
        [(store.t("Selection and orders"), store.t("Select your units with a pinch on the table. Pick an order here, then pinch the target on the table. Stop and Scatter act at once.")),
         (store.t("Groups"), store.t("Replace group: select units, tap it, then a number. New / Extend adds the selected units to that number. A number alone recalls the group. Center view only moves the camera.")),
         (store.t("Waypoints"), store.t("Turn Waypoints on, pinch the destinations in order, turn it off again. Cancel or Stop ends the mode.")),
         (store.t("Tactics and map views"), store.t("Formation keeps at least two selected units together. Save view stores the camera in A to D for this match, A to D alone recalls it."))]
    }

    var body: some View {
        let index = min(3, max(0, Int(store.snapshot.session.helpPage)))
        let current = pages[index]
        VStack(alignment: .leading, spacing: 16) {
            Text("\(store.t("Commands help")) · \(index + 1)/4").font(.headline)
            Text(current.title).font(.title3.weight(.semibold))
            Text(current.body).font(.body).fixedSize(horizontal: false, vertical: true)
            HStack(spacing: UITheme.controlSpacing) {
                if let back = store.commands.control(34) { PanelControlView(item: back, page: Int(GX_PANEL_COMMANDS)) }
                if let next = store.commands.control(36) { PanelControlView(item: next, page: Int(GX_PANEL_COMMANDS)) }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(20)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    }
}
