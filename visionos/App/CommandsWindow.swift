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
                    sections(model)
                }
            }
            .padding(28)
        }
        .frame(minWidth: 620, idealWidth: 720, minHeight: 640)
        .appWindowStyle()
        .commandsHoverOrnament(store: store)
        .hudOrnament(location: .commands, model: appModel, store: store)
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(store.t("Commands")).font(.extraLargeTitle2).accessibilityAddTraits(.isHeader)
            Spacer()
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

    // MARK: sections

    @ViewBuilder private func sections(_ model: PanelPageModel) -> some View {
        ForEach(model.sections) { section in
            switch section.id {
            case -12: groupsSection(section)
            case -14: PanelSectionView(title: section.title, caption: section.controls.first?.explain) { grid(section.controls, columns: 4) }
            case -11: PanelSectionView(title: section.title, caption: nil) { grid(section.controls, columns: 3) }
            case -13: PanelSectionView(title: section.title, caption: nil) { grid(section.controls, columns: 2) }
            default: PanelSectionView(title: section.title, caption: nil) { grid(section.controls, columns: 2) }
            }
        }
    }

    private func grid(_ controls: [PanelControlItem], columns: Int) -> some View {
        LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: UITheme.controlSpacing), count: columns),
                  spacing: UITheme.controlSpacing) {
            ForEach(controls) { PanelControlView(item: $0, page: page) }
        }
    }

    /// Groups: ten number keys with member counts, the three operations (replace / new-extend / center), then Help and the tactics foldout.
    private func groupsSection(_ section: PanelSectionItem) -> some View {
        let keys = section.controls.filter { $0.role == Int(GX_ROLE_GROUPNUM) }
        let ops = section.controls.filter { $0.role == Int(GX_ROLE_OP) }
        let rest = section.controls.filter { $0.role != Int(GX_ROLE_GROUPNUM) && $0.role != Int(GX_ROLE_OP) }
        return PanelSectionView(title: section.title, caption: keys.first?.explain) {
            VStack(alignment: .leading, spacing: UITheme.controlSpacing) {
                grid(ops, columns: 3)
                grid(keys, columns: 5)
                grid(rest, columns: 2)
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
