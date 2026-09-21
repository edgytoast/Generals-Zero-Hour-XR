import ARKit
import Foundation
import simd
import os

private let handLog = Logger(subsystem: "com.generalsx.zerohour.xr.vision", category: "hand-tracking")

/// Continuous ARKit hand samples for the interaction layer (two-hand gestures, tracking-loss detection, and a
/// pinch-position fallback when a spatial event carries no pose).
///
/// Availability (verified in the visionOS 26.5 simulator with the probe app): `HandTrackingProvider.isSupported`
/// is FALSE in the simulator, so this class degrades to `.unsupported` there and everything else keeps working
/// with the simulator fallback (mouse click = pinch, Shift = additive, Option = second hand). On a device the
/// provider needs the `NSHandsTrackingUsageDescription` Info.plist key (present) and an OPEN immersive space
/// before `session.run`; the authorization prompt appears at `run` / `requestAuthorization`.
///
/// The system's own pinch (SpatialEventCollection) stays the source of truth for press/release and the gaze
/// ray. Samples from here are only supplementary: `XR_EVENT_HAND_UPDATE`.
final class SpatialHandTracker: @unchecked Sendable {
    static let shared = SpatialHandTracker()

    enum Status: String { case idle, starting, running, unsupported, denied, failed }

    private let lock = NSLock()
    private var _status: Status = .idle
    private var task: Task<Void, Never>?
    private let session = ARKitSession()
    private let provider = HandTrackingProvider()
    private var pinching: [Bool] = [false, false]  // [left, right]

    var status: Status { lock.withLock { _status } }
    private func set(_ s: Status) { lock.withLock { _status = s }; handLog.info("hand tracking status: \(s.rawValue)") }

    /// Start once (idempotent). Call after the immersive space is open.
    func startIfNeeded() {
        let go: Bool = lock.withLock {
            guard _status == .idle else { return false }
            _status = .starting
            return true
        }
        guard go else { return }
        guard HandTrackingProvider.isSupported else {
            set(.unsupported)  // simulator, or hardware without hand tracking
            return
        }
        task = Task.detached { [weak self] in await self?.run() }
    }

    func stop() {
        task?.cancel()
        task = nil
        session.stop()
        set(.idle)
        pinching = [false, false]
    }

    private func run() async {
        let auth = await session.requestAuthorization(for: [.handTracking])
        guard auth[.handTracking] == .allowed else {
            set(.denied)  // no hands: gaze+pinch through spatial events keeps working
            return
        }
        do {
            try await session.run([provider])
        } catch {
            handLog.error("ARKit session run failed: \(String(describing: error))")
            set(.failed)
            return
        }
        set(.running)
        for await update in provider.anchorUpdates {
            if Task.isCancelled { break }
            handle(update.anchor, removed: update.event == .removed)
        }
    }

    private func position(_ anchor: HandAnchor, _ name: HandSkeleton.JointName) -> SIMD3<Float>? {
        guard let skeleton = anchor.handSkeleton else { return nil }
        let joint = skeleton.joint(name)
        guard joint.isTracked else { return nil }
        let m = anchor.originFromAnchorTransform * joint.anchorFromJointTransform
        return SIMD3<Float>(m.columns.3.x, m.columns.3.y, m.columns.3.z)
    }

    private func handle(_ anchor: HandAnchor, removed: Bool) {
        var sample = GXXRRawHandSample()
        let index = anchor.chirality == .left ? 0 : 1
        sample.chirality = Int32(anchor.chirality == .left ? GXXRRawChiralityLeft : GXXRRawChiralityRight)
        sample.timestamp = ProcessInfo.processInfo.systemUptime
        let wrist = anchor.originFromAnchorTransform.columns.3
        sample.wrist_position = (wrist.x, wrist.y, wrist.z)
        if removed || !anchor.isTracked {
            sample.tracked = false
            pinching[index] = false
            GXXRInputPushHandSample(&sample)
            return
        }
        guard let thumb = position(anchor, .thumbTip), let tip = position(anchor, .indexFingerTip) else {
            sample.tracked = false
            GXXRInputPushHandSample(&sample)
            return
        }
        sample.tracked = true
        let d = simd_distance(thumb, tip)
        // hysteresis: engage at 1.5 cm, release at 3 cm (heuristic; the system pinch is authoritative for clicks)
        let now = pinching[index] ? d < 0.03 : d < 0.015
        pinching[index] = now
        sample.pinching = now
        let mid = (thumb + tip) * 0.5
        sample.pinch_position = (mid.x, mid.y, mid.z)
        // palm normal: the anchor's +Y axis points out of the palm for the left hand and into it for the right;
        // "up" when the outward palm normal has a large +Y world component (heuristic for a menu gesture).
        let up = SIMD3<Float>(anchor.originFromAnchorTransform.columns.1.x, anchor.originFromAnchorTransform.columns.1.y, anchor.originFromAnchorTransform.columns.1.z)
        let outward = anchor.chirality == .left ? up : -up
        sample.palm_up = outward.y > 0.8
        GXXRInputPushHandSample(&sample)
    }
}
