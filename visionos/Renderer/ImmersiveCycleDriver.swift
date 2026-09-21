import CompositorServices
import SwiftUI
import os

private let cycleLog = Logger(subsystem: "com.generalsx.zerohour.xr.vision", category: "cycle")

/// Test driver: closes and re-opens the immersive space N times (`-cycleImmersive N`, optional
/// `-cycleHold <seconds>` for the time the space stays open each round, default 8) to prove the render
/// loop, the ANGLE context and the GL/Metal target ring survive layer invalidation without crashes
/// or leaks. It is inert unless `-cycleImmersive` is on the command line.
///
/// Wiring (one line in a SwiftUI view that has run `openImmersiveSpace` before, e.g. LauncherView's
/// body): `.modifier(ImmersiveCycleDriver())`. It does not use AppModel: the launcher notices the
/// dismissal through the bridge status and the CompositorLayer closure re-attaches on re-open.
struct ImmersiveCycleDriver: ViewModifier {
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace

    private static let spaceID = "tabletop"  // AppModel.spaceID

    private static var cycles: Int {
        let a = CommandLine.arguments
        guard let i = a.firstIndex(of: "-cycleImmersive"), i + 1 < a.count else { return 0 }
        return Int(a[i + 1]) ?? 0
    }

    private static var holdSeconds: Double {
        let a = CommandLine.arguments
        guard let i = a.firstIndex(of: "-cycleHold"), i + 1 < a.count else { return 8 }
        return Double(a[i + 1]) ?? 8
    }

    func body(content: Content) -> some View {
        content.task {
            let n = Self.cycles
            guard n > 0 else { return }
            await run(cycles: n, hold: Self.holdSeconds)
        }
    }

    private func status() -> GXXRBridgeStatus {
        var st = GXXRBridgeStatus()
        GXXRBridgeGetStatus(&st)
        return st
    }

    private func waitUntil(timeout: Double, _ predicate: () -> Bool) async -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if predicate() { return true }
            try? await Task.sleep(for: .milliseconds(100))
        }
        return predicate()
    }

    @MainActor
    private func run(cycles: Int, hold: Double) async {
        for round in 1...cycles {
            // Wait for the space (opened by -autoImmersive or the user) to be live.
            let live = await waitUntil(timeout: 90) { let s = status(); return s.attached && s.layerState == 2 && s.framesRendered > 10 }
            guard live else {
                cycleLog.error("cycle \(round): immersive space never became live; giving up")
                fputs("[GXXR/cycle] round \(round): space never became live; giving up\n", stderr)
                return
            }
            let gen = status().loopGeneration
            fputs("[GXXR/cycle] round \(round)/\(cycles): live (loop generation \(gen), \(status().framesRendered) frames); holding \(hold)s\n", stderr)
            try? await Task.sleep(for: .seconds(hold))
            fputs("[GXXR/cycle] round \(round)/\(cycles): dismissing\n", stderr)
            await dismissImmersiveSpace()
            let gone = await waitUntil(timeout: 30) { !status().attached }
            fputs("[GXXR/cycle] round \(round)/\(cycles): render loop exited=\(gone)\n", stderr)
            try? await Task.sleep(for: .seconds(1.5))
            if round == cycles { break }
            fputs("[GXXR/cycle] round \(round)/\(cycles): reopening\n", stderr)
            let result = await openImmersiveSpace(id: Self.spaceID)
            fputs("[GXXR/cycle] round \(round)/\(cycles): openImmersiveSpace -> \(result)\n", stderr)
        }
        // Leave the final round open so screenshots / memory checks can run after the last cycle.
        let result = await openImmersiveSpace(id: Self.spaceID)
        fputs("[GXXR/cycle] all \(cycles) cycles done; final reopen -> \(result)\n", stderr)
    }
}
