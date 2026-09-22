import Foundation
import QuartzCore
import simd

/// Test-only input source (launch argument `-testInput`). Off in normal runs.
///
/// Why it exists: the visionOS simulator, driven by automation, delivers look-and-pinch events on the Metal tabletop with
/// a zero selection ray and a zero hand pose, so no code can tell where the "player" looked. This injector builds the same
/// raw events a real pinch produces (GXXRRawSpatialEvent with a gaze ray) and pushes them through the normal input path:
/// GXXRInput -> VisionInteraction -> engine. Only the OS event source is replaced; everything after it is the real code.
///
/// Protocol: write lines to `<app tmp>/gx-test-input.txt`. The file is read and deleted about twice a second.
///   tap  nx ny                    look at (nx, ny) and pinch (indirect pinch)
///   drag nx0 ny0 nx1 ny1 [steps]  pointer drag from one point to the other (box select, pan)
///   mods <bits>                   modifier keys for the next events (1 shift, 4 option)
///   wait <seconds>
/// nx, ny are 0...1 across the simulator screenshot (x right, y down). The ray starts at the head and uses the view's
/// field of view: `GX_TEST_TAN` = "tanX,tanY" (default 1.0,0.5625, the xrOS simulator view).
@MainActor
enum TestInputInjector {
    static let enabled = CommandLine.arguments.contains("-testInput")

    private static var url: URL { URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("gx-test-input.txt") }
    private static var nextID: UInt64 = 0x7E57_0000
    private static var modifiers: UInt32 = 0
    private static var running = false

    private static let tangents: (Float, Float) = {
        let parts = (ProcessInfo.processInfo.environment["GX_TEST_TAN"] ?? "").split(separator: ",").compactMap { Float($0) }
        return parts.count == 2 ? (parts[0], parts[1]) : (1.0, 0.5625)
    }()

    /// Called from the app's status loop.
    static func poll() {
        guard enabled, !running, let text = try? String(contentsOf: url, encoding: .utf8) else { return }
        try? FileManager.default.removeItem(at: url)
        running = true
        Task { @MainActor in
            for line in text.split(whereSeparator: \.isNewline) { await run(String(line)) }
            running = false
        }
    }

    private static func direction(_ nx: Float, _ ny: Float) -> SIMD3<Float> {
        simd_normalize(SIMD3((nx * 2 - 1) * tangents.0, (1 - ny * 2) * tangents.1, -1))
    }

    private static func push(id: UInt64, phase: Int32, kind: Int32, dir: SIMD3<Float>?) {
        var raw = GXXRRawSpatialEvent()
        raw.event_id = id
        raw.timestamp = CACurrentMediaTime()
        raw.kind = kind
        raw.phase = phase
        raw.chirality = Int32(GXXRRawChiralityRight)
        if let d = dir {
            raw.has_ray = true
            raw.ray_origin = (0, 0, 0)
            raw.ray_direction = (d.x, d.y, d.z)
        }
        raw.modifiers_valid = true
        raw.modifiers = modifiers
        GXXRInputPushRawSpatialEvent(&raw)
    }

    private static func run(_ line: String) async {
        let p = line.split(separator: " ").map(String.init)
        guard let cmd = p.first else { return }
        let n = p.dropFirst().compactMap { Float($0) }
        fputs("[test-input] \(line)\n", stderr)
        switch cmd {
        case "tap" where n.count >= 2:
            nextID += 1
            push(id: nextID, phase: Int32(GXXRRawPhaseActive), kind: Int32(GXXRRawKindIndirectPinch), dir: direction(n[0], n[1]))
            try? await Task.sleep(for: .milliseconds(80))
            push(id: nextID, phase: Int32(GXXRRawPhaseEnded), kind: Int32(GXXRRawKindIndirectPinch), dir: nil)
        case "drag" where n.count >= 4:
            nextID += 1
            let steps = n.count >= 5 ? max(2, Int(n[4])) : 12
            for i in 0...steps {
                let t = Float(i) / Float(steps)
                let d = direction(n[0] + (n[2] - n[0]) * t, n[1] + (n[3] - n[1]) * t)
                push(id: nextID, phase: Int32(GXXRRawPhaseActive), kind: Int32(GXXRRawKindPointer), dir: d)
                try? await Task.sleep(for: .milliseconds(40))
            }
            push(id: nextID, phase: Int32(GXXRRawPhaseEnded), kind: Int32(GXXRRawKindPointer), dir: direction(n[2], n[3]))
        case "mods" where n.count >= 1:
            modifiers = UInt32(n[0])
        case "wait" where n.count >= 1:
            try? await Task.sleep(for: .milliseconds(Int(n[0] * 1000)))
        default:
            fputs("[test-input] unknown command: \(line)\n", stderr)
        }
    }
}
