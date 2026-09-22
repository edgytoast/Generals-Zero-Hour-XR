import SwiftUI

/// Order, group, tactic and bookmark controls are drawn from the C++ model (`PanelControlItem`): one view per control, whose look
/// follows the Quest role and state bits. State is never colour alone: it always has a symbol, a filled style and an accessibility
/// value.
struct PanelControlView: View {
    @Environment(PanelStore.self) private var store
    let item: PanelControlItem
    let page: Int

    /// Controls that carry an on / armed / pending state are toggles (the Quest paints them as latched buttons).
    private var isToggle: Bool {
        item.isTargeted
            || (item.role == Int(GX_ROLE_GROUPNUM) && item.id < 40)                   // group keys latch the current group
            || ([8, 30, 31, 32, 40, 41, 42, 43].contains(item.id) && page == Int(GX_PANEL_COMMANDS))
    }

    private var tint: Color {
        if item.isPending { return UITheme.pending }
        if item.isArmed { return UITheme.armed }
        if item.isDanger { return UITheme.danger }
        return UITheme.on
    }

    private var stateWord: String {
        if item.isDisabled { return store.t("unavailable") }
        if item.isPending { return store.t("pending") }
        if item.isArmed { return store.t("armed") }
        if item.isOn { return store.t("On") }
        return ""
    }

    private var symbol: String? {
        if item.isPending { return "hourglass" }
        if item.isArmed { return "scope" }
        if item.isOn { return "checkmark.circle.fill" }
        if item.isTargeted { return "scope" }
        if item.isDanger { return "hand.raised.fill" }
        return nil
    }

    var body: some View {
        let label = content
            .frame(maxWidth: .infinity, minHeight: UITheme.minTarget)
        Group {
            if isToggle {
                Toggle(isOn: Binding(get: { item.isHighlighted }, set: { _ in store.perform(page: page, id: item.id) })) { label }
                    .toggleStyle(.button)
                    .tint(tint)
            } else if item.isDanger {
                Button { store.perform(page: page, id: item.id) } label: { label }
                    .buttonStyle(.borderedProminent)
                    .tint(UITheme.danger)
            } else {
                Button { store.perform(page: page, id: item.id) } label: { label }
                    .buttonStyle(.bordered)
            }
        }
        .disabled(item.isDisabled)
        .hoverEffect(.highlight)
        .accessibilityLabel(item.label)
        .accessibilityValue(accessibilityValue)
        .accessibilityHint(item.explain)
    }

    private var accessibilityValue: String {
        var parts: [String] = []
        if !item.value.isEmpty { parts.append(item.value) }
        if item.role == Int(GX_ROLE_GROUPNUM), item.badge >= 0 { parts.append("\(item.badge) \(store.t("members"))") }
        if !stateWord.isEmpty { parts.append(stateWord) }
        return parts.joined(separator: ", ")
    }

    @ViewBuilder private var content: some View {
        if item.role == Int(GX_ROLE_GROUPNUM) && page == Int(GX_PANEL_COMMANDS) && item.id < 40 {
            VStack(spacing: 2) {
                Text(item.label).font(.title2.weight(.semibold))
                Text(item.badge > 0 ? "\(item.badge)" : "·")
                    .font(.caption.monospacedDigit().weight(.semibold))
                    .foregroundStyle(item.badge > 0 ? Color.primary : Color.secondary)
            }
        } else if item.role == Int(GX_ROLE_GROUPNUM) {
            // Map views A to D: a letter and a filled / empty dot for "saved".
            VStack(spacing: 2) {
                Text(item.label).font(.title2.weight(.semibold))
                Image(systemName: item.badge > 0 ? "circle.fill" : "circle").font(.caption2)
                    .accessibilityHidden(true)
            }
        } else {
            HStack(spacing: 8) {
                if let symbol { Image(systemName: symbol).font(.body).accessibilityHidden(true) }
                Text(item.label).font(.body).multilineTextAlignment(.leading).lineLimit(3)
                Spacer(minLength: 4)
                if !item.value.isEmpty {
                    Text(item.value).font(.callout.weight(.semibold)).monospacedDigit()
                }
            }
        }
    }
}

/// A titled block of controls (section header plus a grid).
struct PanelSectionView<Content: View>: View {
    let title: String
    let caption: String?
    @ViewBuilder let content: () -> Content

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title).font(.headline).accessibilityAddTraits(.isHeader)
            if let caption, !caption.isEmpty {
                Text(caption).font(.footnote).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            content()
        }
    }
}

/// The context card: what the console is doing (mode, selection, group), what to do next, and why controls are unavailable.
struct StatusCardView: View {
    @Environment(PanelStore.self) private var store
    let model: PanelPageModel

    private var reason: String? {
        // The engine's reason (or the "not adjustable" message) of the first control that is disabled with an explanation.
        model.allControls.first { $0.isDisabled && !$0.explain.isEmpty }?.explain
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Image(systemName: "info.circle").accessibilityHidden(true)
                Text(model.status.isEmpty ? store.xr("Kein laufendes Spiel") : model.status)
                    .font(.headline)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if !model.hint.isEmpty {
                Text(model.hint).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            if let reason {
                Label(reason, systemImage: "exclamationmark.circle")
                    .font(.callout)
                    .foregroundStyle(.primary)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityLabel("\(store.t("Why some controls are disabled")): \(reason)")
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
        .accessibilityElement(children: .combine)
    }
}

extension View {
    /// The window background: glass, like every visionOS window, with the scene's colour scheme.
    func appWindowStyle() -> some View {
        self.preferredColorScheme(UILaunchOptions.appearance)
    }
}
