import Foundation
import Observation
import os

private let gfxLog = Logger(subsystem: "com.generalsx.zerohour.xr.vision", category: "ui-graphics")

/// The graphics settings the Settings window edits: render scale, render frame-rate cap, shadow mode, the stereo eye size tier,
/// the UI backbuffer resolution and the two comfort/feedback flags. Field names, ordinal values and ranges mirror
/// `GXGraphicsSettings` in `GeneralsMD/Code/Main/visionos/GXEngineHost.h` exactly (package C2, commit 77aee73 on
/// vp/c2-presentation: "visionOS: graphics settings API in GXEngineHost.h ..."), so `GraphicsBackend.applyToEngine` below is a
/// straight field-for-field copy. This file is the ONLY place that knows how the settings reach the engine.
struct GraphicsSettings: Equatable, Codable {
    /// == GXShadowMode.
    enum ShadowMode: Int32, Codable, CaseIterable, Identifiable {
        case off = 0, decals = 1, volumes = 2   // GX_SHADOWS_OFF / _DECALS / _VOLUMES
        var id: Int32 { rawValue }
    }
    /// == GXEyeSizeTier.
    enum EyeTier: Int32, Codable, CaseIterable, Identifiable {
        case balanced = 0, high = 1, ultra = 2   // GX_EYE_BALANCED / _HIGH / _ULTRA (XrLayout.resolutionTier)
        var id: Int32 { rawValue }
    }
    /// == GXUIResolution. Read once at engine boot; changing it afterwards only stores the value for the next launch.
    enum UIResolution: Int32, Codable, CaseIterable, Identifiable {
        case p720 = 0, p1080 = 1   // GX_UI_720P / _1080P
        var id: Int32 { rawValue }
    }

    /// 1.0 = the tier's own eye extent. Clamped to 0.5...1.5 (GXGraphicsSettings.renderScale) by the store.
    var renderScale: Double = 1.0
    /// Engine frames per second the frame limiter allows: 30...120, or -1 for uncapped (GXGraphicsSettings.renderFpsCap;
    /// the engine treats 0 as "use the default", so the UI never offers 0 — it offers the default value, 45, directly).
    var renderFpsCap: Int32 = 45
    var shadowMode: ShadowMode = .decals
    var eyeTier: EyeTier = .balanced
    var uiResolution: UIResolution = .p720
    /// GX_GFX_COMFORT_FADE: the black veil during Ground View enter / exit / teleport.
    var comfortFade: Bool = true
    /// GX_GFX_FOCUS_MARKER: the pointer dot on panels and hover / grab-bar highlights.
    var focusMarker: Bool = true

    static let fpsChoices: [Int32] = [30, 45, 60, 90, 120]
    static let uncappedFps: Int32 = -1
    static let renderScaleRange = 0.5...1.5
}

/// Where the settings go: `GXEngineHost_SetGraphics` (GXEngineHost.h), applied on the engine thread at the start of the next
/// engine frame (`uiResolution` is boot-time only). The struct is extendable via its leading `size` field; this file always
/// builds the full struct it was compiled against, so a caller built against an older header cannot clobber newer fields and
/// vice versa (see the header's own comment on `GXEngineHost_SetGraphics`).
enum GraphicsBackend {
    @MainActor
    static func applyToEngine(_ s: GraphicsSettings) {
        var g = GXGraphicsSettings()
        g.size = UInt32(MemoryLayout<GXGraphicsSettings>.size)
        g.renderScale = Float(s.renderScale)
        g.renderFpsCap = s.renderFpsCap
        g.shadowMode = s.shadowMode.rawValue
        g.eyeTier = s.eyeTier.rawValue
        g.uiResolution = s.uiResolution.rawValue
        var flags: UInt32 = 0
        if s.comfortFade { flags |= UInt32(GX_GFX_COMFORT_FADE) }
        if s.focusMarker { flags |= UInt32(GX_GFX_FOCUS_MARKER) }
        g.flags = flags
        let accepted = GXEngineHost_SetGraphics(&g)
        gfxLog.info("graphics: scale \(s.renderScale, privacy: .public) cap \(s.renderFpsCap, privacy: .public) shadows \(s.shadowMode.rawValue, privacy: .public) tier \(s.eyeTier.rawValue, privacy: .public) ui \(s.uiResolution.rawValue, privacy: .public) flags \(flags, privacy: .public) accepted \(accepted, privacy: .public)")
    }

    /// The engine's own documented defaults (GXEngineHost_GetGraphicsDefaults), used to seed the store on first launch so the
    /// UI's defaults never drift from the engine's.
    @MainActor
    static func engineDefaults() -> GraphicsSettings {
        var g = GXGraphicsSettings()
        g.size = UInt32(MemoryLayout<GXGraphicsSettings>.size)
        GXEngineHost_GetGraphicsDefaults(&g)
        var s = GraphicsSettings()
        s.renderScale = Double(g.renderScale)
        s.renderFpsCap = g.renderFpsCap == 0 ? 45 : g.renderFpsCap
        s.shadowMode = GraphicsSettings.ShadowMode(rawValue: g.shadowMode) ?? .decals
        s.eyeTier = GraphicsSettings.EyeTier(rawValue: g.eyeTier) ?? .balanced
        s.uiResolution = GraphicsSettings.UIResolution(rawValue: g.uiResolution) ?? .p720
        s.comfortFade = (g.flags & UInt32(GX_GFX_COMFORT_FADE)) != 0
        s.focusMarker = (g.flags & UInt32(GX_GFX_FOCUS_MARKER)) != 0
        return s
    }
}

@MainActor
@Observable
final class GraphicsSettingsStore {
    private static let key = "ui.graphics.v2"

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
            settings = GraphicsBackend.engineDefaults()
        }
        GraphicsBackend.applyToEngine(settings)
    }
}
