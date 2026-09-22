import SwiftUI

/// The controls guide (English / German), mirroring docs/visionos-interaction.md section 4 in player language. Follows the UI
/// language of the app (Settings > Controls & language); the picker at the top switches it here too.
struct HelpWindow: View {
    static let id = "help"

    @Environment(PanelStore.self) private var store

    private struct Entry: Identifiable {
        let id: String
        let title: String
        let body: String
        let symbol: String
    }

    private var entries: [Entry] {
        [
            Entry(id: "tap", title: store.t("Look and pinch (tap)"),
                  body: store.t("Select a unit. With units selected: the engine's contextual command (move, attack, capture, enter, guard, gather)."), symbol: "hand.tap"),
            Entry(id: "drag", title: store.t("Look and pinch, then drag"),
                  body: store.t("On empty terrain: selection box. On the rim: pan the map. On the grab bar: move the board."), symbol: "hand.draw"),
            Entry(id: "two", title: store.t("Both hands"),
                  body: store.t("Pinch with both hands and hold: rotate, zoom and pan the map. On the grab bar: move, turn and scale the board."), symbol: "hands.sparkles"),
            Entry(id: "add", title: store.t("Additive selection"),
                  body: store.t("Additive selection: use the switch in Settings, or tap with the other hand while the first hand pinches. Tap a selected unit to deselect it."), symbol: "plus.circle"),
            Entry(id: "place", title: store.t("Placement"),
                  body: store.t("With a building chosen: pinch the ground to place the preview, drag to move it, twist your wrist to rotate it, release to build. Cancel: tap with the other hand, or use Cancel building."), symbol: "hammer"),
            Entry(id: "ground", title: store.t("Ground View"),
                  body: store.t("Ground View (offline matches): arm it from the toolbar or Settings, then pinch visible open ground to stand there. Hold a still pinch for 1.2 s, or use Leave Ground View, to return to the table."), symbol: "figure.walk"),
            Entry(id: "panels", title: store.t("Panels"),
                  body: store.t("The engine's own panels (build menu, production queue, powers, radar, unit info) are textured panels near the board: look and pinch them like buttons."), symbol: "rectangle.3.group"),
            Entry(id: "windows", title: store.t("Windows"),
                  body: store.t("The Commands window holds orders, groups, waypoints, formations and map views. Settings holds workspace, graphics, audio and language."), symbol: "macwindow"),
            Entry(id: "sim", title: store.t("Simulator"),
                  body: store.t("Mouse click = pinch at the pointer. Shift = additive selection. Option = emulate the second hand. Escape = back in the engine."), symbol: "display"),
            Entry(id: "device", title: store.t("What only a headset shows"),
                  body: store.t("Hand tracking, real gaze targets and the depth impression only exist on a device. The simulator shows the windows and the layout."), symbol: "visionpro"),
        ]
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 22) {
                HStack(alignment: .firstTextBaseline) {
                    Text(store.t("Controls guide")).font(.extraLargeTitle2).accessibilityAddTraits(.isHeader)
                    Spacer()
                    Picker(store.t("Language"), selection: Binding(get: { store.language }, set: { store.setLanguage($0) })) {
                        ForEach(UILang.allCases) { Text($0.nativeName).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    .frame(maxWidth: 260)
                }
                Text(store.t("Look at what you want, then pinch. There is no cursor: the system knows where you look only at the moment you pinch."))
                    .font(.title3)
                    .fixedSize(horizontal: false, vertical: true)
                ForEach(entries) { entry in
                    HStack(alignment: .top, spacing: 16) {
                        Image(systemName: entry.symbol)
                            .font(.title2)
                            .frame(width: 44, height: 44)
                            .accessibilityHidden(true)
                        VStack(alignment: .leading, spacing: 4) {
                            Text(entry.title).font(.headline)
                            Text(entry.body).font(.body).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    .accessibilityElement(children: .combine)
                }
            }
            .padding(28)
        }
        .frame(minWidth: 640, minHeight: 560)
        .appWindowStyle()
    }
}
