import SwiftUI

/// The colours the panels set explicitly (everything else is a system style over the window glass). Each fill carries white label
/// text, so each is chosen for at least 4.5:1 against white (WCAG AA, normal text); `scripts/qa/vision-ui-contrast.py` computes the
/// ratios from these exact values and fails below 4.5. Keep the hex values in sync with that script (it parses this file).
enum UITheme {
    /// Order armed and waiting for a target on the table.
    static let armed = Color(red: 0xB4 / 255.0, green: 0x53 / 255.0, blue: 0x09 / 255.0)      // #B45309
    /// A switch that is on (waypoints, formation, save view, current group).
    static let on = Color(red: 0x0B / 255.0, green: 0x63 / 255.0, blue: 0xCE / 255.0)         // #0B63CE
    /// A group operation waiting for its number.
    static let pending = Color(red: 0x7A / 255.0, green: 0x3E / 255.0, blue: 0xB8 / 255.0)    // #7A3EB8
    /// Stop.
    static let danger = Color(red: 0xC2 / 255.0, green: 0x1F / 255.0, blue: 0x1F / 255.0)     // #C21F1F
    /// Badge capsule of a group with members / a saved map view.
    static let badge = Color(red: 0x1F / 255.0, green: 0x2A / 255.0, blue: 0x37 / 255.0)      // #1F2A37
    /// Unavailable reason banner.
    static let warning = Color(red: 0x92 / 255.0, green: 0x40 / 255.0, blue: 0x0E / 255.0)    // #92400E

    /// Minimum size of every tappable control, in points (visionOS asks for 44 pt; the console uses 60 pt like the Quest panel).
    static let minTarget: CGFloat = 60
    static let controlSpacing: CGFloat = 12
}
