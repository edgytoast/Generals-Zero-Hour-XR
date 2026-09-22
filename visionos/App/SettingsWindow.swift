import SwiftUI

/// The Quest 'Menu' console (Windows / View tabs) as native settings, extended with what only the visionOS port has:
/// Workspace, Presentation, Graphics, Audio, Controls & language, Data and Diagnostics. Page content lives in UI/SettingsPages.swift.
struct SettingsWindow: View {
    static let id = "settings"

    enum Page: String, CaseIterable, Identifiable {
        case workspace, presentation, graphics, audio, controls, data, diagnostics
        var id: String { rawValue }

        var symbol: String {
            switch self {
            case .workspace: return "square.grid.3x3.topleft.filled"
            case .presentation: return "eye"
            case .graphics: return "sparkles.tv"
            case .audio: return "speaker.wave.3"
            case .controls: return "hand.point.up.left"
            case .data: return "externaldrive"
            case .diagnostics: return "waveform.path.ecg"
            }
        }
        var titleKey: String {
            switch self {
            case .workspace: return "Workspace"
            case .presentation: return "Presentation"
            case .graphics: return "Graphics"
            case .audio: return "Audio"
            case .controls: return "Controls & language"
            case .data: return "Data"
            case .diagnostics: return "Diagnostics"
            }
        }
    }

    @Environment(PanelStore.self) private var store
    @State private var selection: Page? = Page(rawValue: UILaunchOptions.settingsPage ?? "") ?? .workspace

    var body: some View {
        NavigationSplitView {
            List(selection: $selection) {
                ForEach(Page.allCases) { page in
                    Label(store.t(page.titleKey), systemImage: page.symbol)
                        .frame(minHeight: UITheme.minTarget - 16)
                        .tag(page)
                }
            }
            .navigationTitle(store.t("Settings"))
            .navigationSplitViewColumnWidth(min: 250, ideal: 280, max: 340)
        } detail: {
            NavigationStack {
                SettingsPageView(page: selection ?? .workspace)
                    .navigationTitle(store.t((selection ?? .workspace).titleKey))
            }
        }
        .frame(minWidth: 860, minHeight: 640)
        .appWindowStyle()
    }
}
