import Foundation
import SwiftUI
import os

private let engineLog = Logger(subsystem: "com.generalsx.zerohour.xr.vision", category: "engine")

/// Boot flow of the engine host: log file, "Start Game", the fake engine for tests, status polling.
/// The engine runs on its own thread (GXEngineHost); nothing here blocks on it.
extension AppModel {
    // MARK: Paths

    /// `<Application Support>/GeneralsX`: layout / camera / language cfg files, registry.ini and the engine log.
    static var engineSupportDirectory: String {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSTemporaryDirectory())
        let dir = base.appendingPathComponent("GeneralsX", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.path
    }

    static var engineLogFile: String { engineSupportDirectory + "/generals-xr-stderr.log" }

    // MARK: Start

    /// Starts logging into the engine log file as early as possible (compositor / host lines land in it too, the console
    /// still gets everything), and the fake engine when `-fakeEngine` was given. Called once at launch.
    func startEngineInfrastructure() {
        // A boot marker left behind means the previous run ended the whole process during the engine boot (a fatal engine error
        // calls _exit). Read it BEFORE logging rotates the old log away, then tell the player what the last lines said.
        let marker = Self.engineSupportDirectory + "/engine-boot.marker"
        let crashed = FileManager.default.fileExists(atPath: marker)
        try? FileManager.default.removeItem(atPath: marker)
        GXEngineHost_BeginLogging(Self.engineLogFile)
        engineLogPath = Self.engineLogFile
        if crashed {
            let previous = Self.engineSupportDirectory + "/generals-xr-stderr-prev.log"
            let text = (try? String(contentsOfFile: previous, encoding: .utf8)) ?? ""
            let lines = text.split(separator: "\n").filter { !$0.hasPrefix("[GXXR]") }.suffix(6).joined(separator: "\n")
            engineStartNotice = "The last start ended when the app closed during the engine boot. Last engine log lines:\n" + lines + "\nLog: " + previous
        }
        if LaunchOptions.fakeEngine {
            let ok = GXXRBridgeStartFakeEngine()
            engineLog.info("fake engine start: \(ok, privacy: .public)")
        }
    }

    /// "Start Game": validated paths -> GXEngineHost_Start. Non-blocking; progress and failures show up through `refreshEngine`.
    func startEngine() {
        guard enginePhase == GX_ENGINE_IDLE else { return }
        var zh = [CChar](repeating: 0, count: 1024)
        var base = [CChar](repeating: 0, count: 1024)
        var user = [CChar](repeating: 0, count: 1024)
        guard GXXRGameData_IsReady(), GXXRGameData_GetPaths(&zh, zh.count, &base, base.count, &user, user.count) else {
            engineStartNotice = "Game data is not ready: choose or import your Generals and Zero Hour folders first."
            return
        }
        engineStartNotice = ""
        let support = Self.engineSupportDirectory
        let logPath = Self.engineLogFile
        var cfg = GXEngineHostConfig()
        cfg.policy = UInt32(GX_ENGINE_POLICY_DEFAULT)
        cfg.logicHz = Int32(LaunchOptions.engineLogicHz ?? 0)
        cfg.renderFpsCap = Int32(LaunchOptions.engineFpsCap ?? 0)   // 0 = default 45, negative = uncapped
        cfg.forceAtlas = false
        let started: Bool = zh.withUnsafeBufferPointer { zhP in
            base.withUnsafeBufferPointer { baseP in
                user.withUnsafeBufferPointer { userP in
                    support.withCString { supportP in
                        logPath.withCString { logP in
                            cfg.zhRoot = zhP.baseAddress
                            cfg.baseRoot = baseP.baseAddress
                            cfg.userDataRoot = userP.baseAddress
                            cfg.appSupportRoot = supportP
                            cfg.logPath = logP
                            return GXEngineHost_Start(&cfg)   // copies every string before it returns
                        }
                    }
                }
            }
        }
        if !started {
            engineStartNotice = "The engine could not be started (already started in this process?). Restart the app to boot it again."
        }
        engineLog.info("GXEngineHost_Start -> \(started, privacy: .public)")
    }

    /// Starts the engine automatically once game data is ready (`-autoStartEngine`).
    func autoStartEngineIfRequested() {
        guard LaunchOptions.autoStartEngine, !engineAutoStartDone, isGameDataReady, enginePhase == GX_ENGINE_IDLE else { return }
        engineAutoStartDone = true
        startEngine()
    }

    var canStartEngine: Bool { isGameDataReady && enginePhase == GX_ENGINE_IDLE && !engineIsFake }

    // MARK: Status

    func refreshEngine(compositor st: GXXRBridgeStatus) {
        var hs = GXEngineHostStatus()
        GXEngineHost_GetStatus(&hs)
        enginePhase = hs.phase
        engineIsFake = hs.fake
        engineProgress = Self.string(hs.progress)
        engineError = Self.string(hs.lastError)
        engineLastLogLine = Self.string(hs.lastLogLine)
        let path = Self.string(hs.logPath)
        if !path.isEmpty { engineLogPath = path }
        engineBootSeconds = hs.bootSeconds
        engineFps = hs.engineFps
        engineLogicHz = hs.logicHz
        engineFrames = hs.framesProduced
        engineWaitingForCompositor = hs.waitingForCompositor
        compositorFpsText = st.engineMode ? String(format: "compositor %.0f fps, engine %.0f fps, %.0f new frames/s, %.0f repeats/s", st.fps, hs.engineFps, st.newFramesPerSec, st.repeatFramesPerSec) : ""
        if hs.phase == GX_ENGINE_FAILED {
            var buffer = [CChar](repeating: 0, count: 2048)
            GXEngineHost_ReadLogTail(&buffer, buffer.count, 12)
            engineLogTail = String(cString: buffer)
        }
        autoStartEngineIfRequested()
    }

    private static func string<T>(_ tuple: T) -> String {
        withUnsafeBytes(of: tuple) { raw in
            String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
        }
    }

    /// One line for the launcher headline.
    var engineHeadline: String {
        switch enginePhase {
        case GX_ENGINE_IDLE: return "Engine not started"
        case GX_ENGINE_BOOTING: return String(format: "Booting the engine... %.0f s", engineBootSeconds)
        case GX_ENGINE_RUNNING: return engineWaitingForCompositor ? "Engine ready. Enter the tabletop to see it." : String(format: "Engine running: %.0f fps", engineFps)
        case GX_ENGINE_PAUSED: return "Engine paused"
        case GX_ENGINE_FAILED: return "Engine stopped"
        case GX_ENGINE_STOPPING: return "Engine stopping"
        default: return "Engine"
        }
    }
}
