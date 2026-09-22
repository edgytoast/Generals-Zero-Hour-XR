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

    /// True while the launcher window is on screen. The launcher hides itself while a real match is on the table (it is
    /// about a metre tall and stood between the player and the board), and the other windows reopen it from this flag.
    var launcherOpen = false
    @ObservationIgnored var bootstrapped = false
    /// Set between "a window asked to reopen the launcher" and the launcher appearing, so two windows reacting to the same
    /// tabletop close cannot open two launchers.
    @ObservationIgnored private var launcherReopenPending = false

    /// True when the caller should call `openWindow(id: "launcher")`: the launcher is closed and nobody else asked yet.
    func claimLauncherReopen() -> Bool {
        if launcherOpen || launcherReopenPending { return false }
        launcherReopenPending = true
        return true
    }

    func launcherDidAppear() {
        launcherOpen = true
        launcherReopenPending = false
    }
    @ObservationIgnored private var refreshTask: Task<Void, Never>?
    @ObservationIgnored private var memoryWarningObserver: NSObjectProtocol?

    // MARK: Engine (state lives here, logic in AppModel+Engine.swift)

    /// Where the engine host is (GXEngineHost_GetStatus), refreshed with everything else in `refresh()`.
    var enginePhase: GXEngineHostPhase = GX_ENGINE_IDLE
    var engineIsFake = false
    var engineProgress = ""
    var engineError = ""
    var engineLastLogLine = ""
    var engineLogPath = ""
    var engineLogTail = ""
    var engineBootSeconds = 0.0
    var engineFps = 0.0
    var engineLogicHz = 0.0
    var engineFrames: UInt64 = 0
    var engineWaitingForCompositor = false
    var compositorFpsText = ""
    /// Reason the last "Start Game" could not even be attempted (before the engine thread exists).
    var engineStartNotice = ""
    @ObservationIgnored var engineAutoStartDone = false

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

    /// App-lifetime status polling. It belongs to the model, not to a window: a window's `.task` stops when the window
    /// closes, and the launcher closes while the tabletop is in use.
    func startAppServices() {
        guard refreshTask == nil else { return }
        refreshTask = Task { [weak self] in
            while !Task.isCancelled {
                self?.refresh()
                TestInputInjector.poll()
                try? await Task.sleep(for: .milliseconds(500))
            }
        }
        memoryWarningObserver = NotificationCenter.default.addObserver(
            forName: UIApplication.didReceiveMemoryWarningNotification, object: nil, queue: .main) { _ in
            GXXRBridgeNotifyLifecycle(Int32(PLATFORM_LIFECYCLE_MEMORY_WARNING.rawValue))
        }
    }

    /// Hide the launcher while the tabletop shows a real game. Test runs keep it (`-keepLauncher`, `-cycleImmersive`
    /// drives the space from the launcher), and so does the test scene without game data.
    var hidesLauncherOnTabletop: Bool { isGameDataReady && !LaunchOptions.keepLauncher }

    /// Pulls a snapshot from the render thread; called about twice a second.
    func refresh() {
        var st = GXXRBridgeStatus()
        GXXRBridgeGetStatus(&st)
        refreshEngine(compositor: st)
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
            if st.engineMode {
                statusText = String(format: "Tabletop live: compositor %.0f fps, engine %.0f fps, %d view(s), %@ layout, head %@", fps, st.engineFps, views, layout, headTracked ? "tracked" : "fallback pose")
            } else {
                statusText = String(format: "Tabletop live: %.0f fps, %d view(s), %@ layout, head %@", fps, views, layout, headTracked ? "tracked" : "fallback pose")
            }
        }

        var ist = GXXRInputStats()
        GXXRInputGetStats(&ist)
        if ist.raw_events > 0 {
            inputSummary = "Spatial events: \(ist.raw_events) (begin \(ist.begins), drag \(ist.drags), end \(ist.ends)). Active: \(ist.active_pointers)."
        }
    }
}
