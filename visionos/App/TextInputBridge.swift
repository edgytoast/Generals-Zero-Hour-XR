// TextInputBridge.swift - package C2. There is no hardware keyboard on Vision Pro and the engine has no window to type
// into directly (GX_XR_OffscreenBoot): while a game text entry gadget owns the focus (chat, save-game name, lobby name)
// GXEngineHostStatus.textFieldFocused is true (GeneralsMD/Code/Main/visionos/GXEngineHost.h) and this shows a system
// TextField in a sheet; what the player types is sent back with GXEngineHost_SubmitText, which queues it on the engine
// thread as backspace-to-clear (optional) + the new text + Enter (docs/visionos-presentation.md section 9).
//
// Self-contained: this file owns no other file's body. Attach it with the `.textInputBridge()` one-line modifier from
// any SwiftUI view that stays around while the engine runs (the launcher window does); see the "Hookup" note at the
// bottom of this file for the exact line and where package G/F should add it.
import SwiftUI

/// Polls `GXEngineHost_TextFieldFocused` and presents a sheet with a system `TextField` while a game text entry has
/// the focus. Polling (not a push callback) matches every other piece of engine status in this app (AppModel+Engine's
/// `refreshEngine`, driven by the compositor's frame ticks) and keeps this file from needing its own thread or timer
/// wired into anything else.
private struct TextInputBridgeModifier: ViewModifier {
    /// How often to check `GXEngineHost_TextFieldFocused` while nothing is focused. Once the sheet is up, typing is
    /// local to the TextField (no polling needed) until Cancel/Send hands control back to the engine.
    private static let pollInterval: TimeInterval = 0.2

    @State private var focused = false
    @State private var text = ""
    @State private var didAppearWithFocus = false

    func body(content: Content) -> some View {
        content
            .onReceive(Timer.publish(every: Self.pollInterval, on: .main, in: .common).autoconnect()) { _ in
                guard !focused else { return }  // the sheet owns `text` once it is up; do not clobber player keystrokes
                pollFocus()
            }
            .sheet(isPresented: $focused) {
                TextInputBridgeSheet(text: $text, onCancel: cancel, onSend: send)
            }
    }

    private func pollFocus() {
        var buffer = [CChar](repeating: 0, count: 192)
        let isFocused = buffer.withUnsafeMutableBufferPointer { ptr in
            GXEngineHost_TextFieldFocused(ptr.baseAddress, ptr.count)
        }
        guard isFocused else { return }
        text = String(cString: buffer)
        focused = true
    }

    /// The player dismissed without sending: nothing is typed into the game (no partial text either), the entry
    /// gadget keeps whatever it already had. `false, false` submits an empty, no-op edit purely to let the engine
    /// know the sheet closed; it changes no text and presses no Enter.
    private func cancel() {
        focused = false
        _ = GXEngineHost_SubmitText("", false, false)
    }

    /// Erases the gadget's existing content (it may have changed since `pollFocus` populated `text`, e.g. autocomplete
    /// in game chat) and types the sheet's text, then presses Enter (chat: sends the line; a name field: confirms it).
    private func send() {
        focused = false
        _ = text.withCString { GXEngineHost_SubmitText($0, true, true) }
    }
}

private struct TextInputBridgeSheet: View {
    @Binding var text: String
    let onCancel: () -> Void
    let onSend: () -> Void
    @FocusState private var fieldFocused: Bool

    var body: some View {
        NavigationStack {
            Form {
                TextField("", text: $text)
                    .focused($fieldFocused)
                    .submitLabel(.send)
                    .onSubmit(onSend)
            }
            .navigationTitle("Enter Text")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel", action: onCancel) }
                ToolbarItem(placement: .confirmationAction) { Button("Send", action: onSend) }
            }
        }
        .frame(minWidth: 420, minHeight: 220)
        .onAppear { fieldFocused = true }
    }
}

public extension View {
    /// Shows the visionOS text-entry sheet (package C2) whenever the engine reports a focused text field. Attach to a
    /// view that stays mounted for the life of the app, such as the launcher window's root view.
    func textInputBridge() -> some View { modifier(TextInputBridgeModifier()) }
}

// Hookup (one additive line; C2 does not own visionos/App/LauncherView.swift so cannot add it directly — see
// handoffNotesForLead): in LauncherView's `body`, add `.textInputBridge()` to the outermost view, e.g.
//
//     var body: some View {
//         NavigationStack { ... }
//             .textInputBridge()
//             ...existing modifiers...
//     }
