import SwiftUI

/// Selected-unit / hover text: the name and status the engine reports for the LAST pinched or hand-pointed target
/// (`XrGameBoot_WorldHoverInfo`, refreshed into the panel snapshot) and, when the interaction layer sets a probe position, the
/// engine's tooltip of the panel under it (`XrGameBoot_HoverInfo`). There is no continuous gaze on visionOS: the system shares
/// where the player looks only when a pinch begins, so between pinches this shows the last target, not what is looked at now.
/// The card says so.
struct HoverInfoCard: View {
    @Environment(PanelStore.self) private var store

    var body: some View {
        let world = cString(store.snapshot.worldHover)
        let panel = cString(store.snapshot.panelHover)
        VStack(alignment: .leading, spacing: 6) {
            if !world.isEmpty {
                let lines = world.split(separator: "\n", maxSplits: 1, omittingEmptySubsequences: false)
                Label(store.t("Last target"), systemImage: "scope").font(.caption).foregroundStyle(.secondary)
                Text(String(lines.first ?? "")).font(.headline)
                if lines.count > 1 { Text(String(lines[1])).font(.callout).foregroundStyle(.secondary) }
            }
            if !panel.isEmpty {
                Label(store.t("Panel tooltip"), systemImage: "info.circle").font(.caption).foregroundStyle(.secondary)
                Text(panel).font(.callout)
            }
            Text(store.t("Shows the last unit or building you pinched or pointed at. The system does not share where you look between pinches."))
                .font(.caption2).foregroundStyle(.secondary)
        }
        .frame(maxWidth: 420, alignment: .leading)
        .padding(14)
        .accessibilityElement(children: .combine)
    }
}

extension View {
    /// The hover / target ornament above the Commands window, visible only while the engine has text for it.
    func commandsHoverOrnament(store: PanelStore) -> some View {
        let has = store.snapshot.worldHover.0 != 0 || store.snapshot.panelHover.0 != 0
        return self.ornament(visibility: has ? .visible : .hidden, attachmentAnchor: .scene(.top), contentAlignment: .bottom) {
            HoverInfoCard()
                .glassBackgroundEffect()
                .environment(store)
        }
    }
}
