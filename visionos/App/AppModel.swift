import CompositorServices
import Observation
import SwiftUI
import os

private let modelLog = Logger(subsystem: "com.generalsx.zerohour.xr.vision", category: "app")

@MainActor
@Observable
final class AppModel {
    static let spaceID = "tabletop"

    enum SpaceState: String { case closed, opening, open, closing }

    var spaceState: SpaceState = .closed
    var statusText = "Ready. Tabletop not started."
    var useFakeEngineTextures = LaunchOptions.externalEyeTextures

    var fps = 0.0
    var frames: UInt64 = 0
    var layout = "-"
    var formats = "-"
    var headTracked = false
    var placed = false
    var views = 0
    var lastMessage = ""

    var inputSummary = "No spatial events yet."

    // MARK: Game data (logic lives in GameData/AppModel+GameData.swift)

    /// One state for the launcher and the engine bridge. Change it only through
    /// `setGameData(_:)`, which keeps the C snapshot (GXXRGameData_*) in sync.
    var gameData: GameDataStatus = .notConfigured
    /// Human-readable message from the last data operation (import result, error, recovery).
    var gameDataNotice: String?
    var gameDataNoticeIsError = false
    /// What the "checking" spinner is looking at.
    var validationSubject = ""
    /// An interrupted import that can be resumed or discarded.
    var interruptedImport: InterruptedImport?
    /// Advanced: use the picked folder where it is instead of copying it into the app.
    var importInPlace = false
    var filePickerPresented = false
    var filePickerPurpose: FilePickerPurpose = .gameFolder
    var confirmRemoveImported = false
    /// Set once the Zero Hour folder was read but no base Generals folder was found; the next
    /// pick is then treated as the separate Generals folder.
    var awaitingBaseFolder = false
    /// The last folder the player chose that could not be used, shown next to the (preserved)
    /// working configuration. A rejected pick never discards data that is already ready.
    var rejectedSelection: GXGDReport?

    @ObservationIgnored var gameDataStartupDone = false
    @ObservationIgnored var heldScopes: [URL] = []
    @ObservationIgnored var firstPick: URL?
    @ObservationIgnored var importTask: Task<Void, Never>?
    @ObservationIgnored var cancelToken: GXGDCancelToken?

    /// Called by CompositorLayer once the layer renderer exists (space is open).
    func layerRendererReady(_ renderer: LayerRenderer) {
        modelLog.info("layerRendererReady")
        GXXRBridgeSetBoolOption("externalEyeTextures", useFakeEngineTextures)
        renderer.onSpatialEvent = { events in
            SpatialEventForwarder.forward(events)
        }
        GXXRBridgeAttachLayerRenderer(renderer)
        spaceState = .open
    }

    /// Pulls a snapshot from the render thread; called about twice a second.
    func refresh() {
        var st = GXXRBridgeStatus()
        GXXRBridgeGetStatus(&st)
        fps = st.fps
        frames = st.framesRendered
        views = Int(st.viewCount)
        headTracked = st.headTracked
        placed = st.placementValid
        layout = withUnsafePointer(to: &st.layout) { $0.withMemoryRebound(to: CChar.self, capacity: 16) { String(cString: $0) } }
        let color = withUnsafePointer(to: &st.colorFormat) { $0.withMemoryRebound(to: CChar.self, capacity: 24) { String(cString: $0) } }
        let depth = withUnsafePointer(to: &st.depthFormat) { $0.withMemoryRebound(to: CChar.self, capacity: 24) { String(cString: $0) } }
        formats = "\(color) / \(depth)"
        lastMessage = withUnsafePointer(to: &st.lastMessage) { $0.withMemoryRebound(to: CChar.self, capacity: 160) { String(cString: $0) } }

        if spaceState == .open && !st.attached && st.layerState == 3 {
            spaceState = .closed  // system dismissed the space (Digital Crown, etc.)
        }
        switch spaceState {
        case .closed: statusText = "Tabletop not started."
        case .opening: statusText = "Opening immersive space..."
        case .closing: statusText = "Closing immersive space..."
        case .open:
            statusText = String(format: "Tabletop live: %.0f fps, %d view(s), %@ layout, head %@", fps, views, layout, headTracked ? "tracked" : "fallback pose")
        }

        var ist = GXXRInputStats()
        GXXRInputGetStats(&ist)
        if ist.raw_events > 0 {
            inputSummary = "Spatial events: \(ist.raw_events) (begin \(ist.begins), drag \(ist.drags), end \(ist.ends)). Active: \(ist.active_pointers)."
        }
    }
}
