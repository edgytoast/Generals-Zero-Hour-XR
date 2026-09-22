import Foundation
import Observation
import os

private let gfxLog = Logger(subsystem: "com.generalsx.zerohour.xr.vision", category: "ui-graphics")

/// The graphics settings the Settings window edits: render scale, render frame-rate cap, shadow mode and the stereo eye size tier
/// (the set specified for package C2's `GXGraphicsSettings`, GXEngineHost.h). This file is the ONLY place that knows how they
/// reach the engine, see `GraphicsBackend`.
struct GraphicsSettings: Equatable, Codable {
    enum ShadowMode: Int, Codable, CaseIterable, Identifiable {
        case off = 0, decals = 1, volumes = 2
        var id: Int { rawValue }
    }
    enum EyeTier: Int, Codable, CaseIterable, Identifiable {
        case balanced = 0, high = 1, ultra = 2   // XrLayout.resolutionTier
        var id: Int { rawValue }
    }

    /// 1.0 = the compositor's recommended eye size. Clamped to 0.5...1.5 by the store.
    var renderScale: Double = 1.0
    /// Engine frames per second the frame limiter allows: 30, 45, 60, or 0 for uncapped.
    var renderFpsCap: Int = 45
    var shadowMode: ShadowMode = .volumes
    var eyeTier: EyeTier = .balanced

    static let fpsChoices = [30, 45, 60, 0]
    static let renderScaleRange = 0.5...1.5
}

/// Where the settings go. Until package C2's `GXEngineHost_SetGraphics(const GXGraphicsSettings*)` is merged the values are stored
/// (UserDefaults) and mirrored into the preferences the panel state already carries (`GXPanelPrefs.volumeShadows`,
/// `GXPanelPrefs.resolutionTier`, both the Quest's own XrPerformance / XrLayout fields). When C2 lands, replace the body of
/// `applyToEngine` with:
///
///     var g = GXGraphicsSettings()            // size field first (the struct is extendable)
///     g.size = UInt32(MemoryLayout<GXGraphicsSettings>.size)
///     g.renderScale = Float(s.renderScale); g.renderFpsCap = Int32(s.renderFpsCap)
///     g.shadowMode = UInt32(s.shadowMode.rawValue); g.eyeTier = UInt32(s.eyeTier.rawValue)
///     GXEngineHost_SetGraphics(&g)
///
/// (field names as in C2's header; the mapping is one-to-one with `GraphicsSettings`.)
enum GraphicsBackend {
    static let engineSetterAvailable = false

    @MainActor
    static func applyToEngine(_ s: GraphicsSettings) {
        var prefs = GXPanelPrefs()
        GXEnginePanelState_GetPrefs(&prefs)
        prefs.volumeShadows = s.shadowMode == .volumes
        prefs.resolutionTier = Int32(s.eyeTier.rawValue)
        GXEnginePanelState_SetPrefs(&prefs)
        gfxLog.info("graphics: scale \(s.renderScale, privacy: .public) cap \(s.renderFpsCap, privacy: .public) shadows \(s.shadowMode.rawValue, privacy: .public) tier \(s.eyeTier.rawValue, privacy: .public) (engine setter: \(engineSetterAvailable, privacy: .public))")
    }
}

@MainActor
@Observable
final class GraphicsSettingsStore {
    private static let key = "ui.graphics.v1"

    var settings: GraphicsSettings {
        didSet {
            var s = settings
            s.renderScale = min(GraphicsSettings.renderScaleRange.upperBound, max(GraphicsSettings.renderScaleRange.lowerBound, s.renderScale))
            if s != settings { settings = s; return }
            if let data = try? JSONEncoder().encode(settings) { UserDefaults.standard.set(data, forKey: Self.key) }
            GraphicsBackend.applyToEngine(settings)
        }
    }

    init() {
        if let data = UserDefaults.standard.data(forKey: Self.key), let s = try? JSONDecoder().decode(GraphicsSettings.self, from: data) {
            settings = s
        } else {
            settings = GraphicsSettings()
        }
        GraphicsBackend.applyToEngine(settings)
    }
}
