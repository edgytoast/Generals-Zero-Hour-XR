import SwiftUI

/// The toolbar under the launcher and the Commands window: Ground View, Recenter, Pause, Menu and Leave Tabletop. They are the
/// actions a player needs at arm's reach while the immersive space is open; each maps to an existing command
/// (InteractionControls / GXEngineHost / dismissImmersiveSpace), none re-implements a rule.
struct HudOrnament: ViewModifier {
    enum Location { case launcher, commands }
    let location: Location

    @Environment(AppModel.self) private var model
    @Environment(PanelStore.self) private var store
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @Environment(\.openWindow) private var openWindow
    @State private var paused = false

    func body(content: Content) -> some View {
        content.ornament(attachmentAnchor: .scene(.bottom), contentAlignment: .top) {
            bar
                .padding(12)
                .glassBackgroundEffect()
        }
    }

    private var bar: some View {
        let groundOn = store.groundViewRequested || store.snapshot.groundViewMode != 0
        let groundAvailable = store.snapshot.canObserveGround && store.snapshot.stereoVisible
        return HStack(spacing: 12) {
            Toggle(isOn: Binding(get: { groundOn }, set: { _ in store.toggleGroundView() })) {
                Label(store.t("Ground View"), systemImage: "figure.walk")
            }
            .toggleStyle(.button)
            .disabled(!groundOn && !groundAvailable)
            .accessibilityHint(store.t("Ground View is available in offline matches while the world can be adjusted."))

            Button { InteractionControls.recenterBoard(); GXXRBridgeRecenter() } label: {
                Label(store.t("Recenter"), systemImage: "scope")
            }
            .disabled(model.spaceState != .open)

            Toggle(isOn: Binding(get: { paused }, set: { on in
                paused = on
                GXEngineHost_Pause(GX_PAUSE_USER, on)
            })) {
                Label(store.t("Pause"), systemImage: paused ? "play.fill" : "pause.fill")
            }
            .toggleStyle(.button)
            .disabled(!GXEngineHost_IsActive())

            Button { InteractionControls.engineBack() } label: {
                Label(store.t("Menu"), systemImage: "line.3.horizontal")
            }
            .accessibilityHint(store.t("Send Back to the engine (opens the game menu in a match)"))

            Button(role: .destructive) {
                Task {
                    guard model.spaceState == .open else { return }
                    model.spaceState = .closing
                    await dismissImmersiveSpace()
                    model.spaceState = .closed
                }
            } label: {
                Label(store.t("Leave Tabletop"), systemImage: "xmark.circle")
            }
            .disabled(model.spaceState != .open)

            Menu {
                Button { openWindow(id: CommandsWindow.id) } label: { Label(store.t("Open Commands"), systemImage: "list.bullet.rectangle") }
                Button { openWindow(id: SettingsWindow.id) } label: { Label(store.t("Open Settings"), systemImage: "gearshape") }
                Button { openWindow(id: HelpWindow.id) } label: { Label(store.t("Open Help"), systemImage: "questionmark.circle") }
            } label: {
                Label(store.t("Windows"), systemImage: "macwindow.on.rectangle")
            }
        }
        .labelStyle(.titleAndIcon)
        .frame(minHeight: UITheme.minTarget)
    }
}
