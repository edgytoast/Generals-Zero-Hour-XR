import Foundation
import SwiftUI
import Spatial
import os

private let inputLog = Logger(subsystem: "com.generalsx.zerohour.xr.vision", category: "input-swift")

/// Copies LayerRenderer spatial events into plain C structs for the Objective-C++
/// input layer (Input/GXXRInput.mm), which normalizes them to XRInteraction.h.
///
/// Verified (SDK 27.0 SwiftUI/_CompositorServices_SwiftUI interfaces):
///   LayerRenderer.onSpatialEvent : @MainActor (SpatialEventCollection) -> Void
///   SpatialEventCollection.Event: id, timestamp, kind, location3D, phase, selectionRay?,
///   inputDevicePose?.pose3D, chirality?
/// The coordinate space of location3D / selectionRay for CompositorLayer is assumed to be the
/// immersive-space origin (the same world space as the ARKit device anchor). That is unverified
/// on a physical device; the tabletop math flows through one place (GXXRInput.mm) so it is a
/// one-line fix if the space differs.
@MainActor
enum SpatialEventForwarder {
    private static var received: UInt64 = 0

    static func forward(_ events: SpatialEventCollection) {
        for e in events {
            var raw = GXXRRawSpatialEvent()
            raw.event_id = UInt64(bitPattern: Int64(e.id.hashValue))
            raw.timestamp = e.timestamp

            switch e.kind {
            case .touch: raw.kind = Int32(GXXRRawKindTouch)
            case .directPinch: raw.kind = Int32(GXXRRawKindDirectPinch)
            case .indirectPinch: raw.kind = Int32(GXXRRawKindIndirectPinch)
            case .pointer: raw.kind = Int32(GXXRRawKindPointer)
            default: raw.kind = Int32(GXXRRawKindOther)
            }

            switch e.phase {
            case .active: raw.phase = Int32(GXXRRawPhaseActive)
            case .ended: raw.phase = Int32(GXXRRawPhaseEnded)
            case .cancelled: raw.phase = Int32(GXXRRawPhaseCancelled)
            @unknown default: raw.phase = Int32(GXXRRawPhaseCancelled)
            }

            if let c = e.chirality {
                raw.chirality = Int32(c == .left ? GXXRRawChiralityLeft : GXXRRawChiralityRight)
            }

            let loc = e.location3D
            raw.has_location3d = true
            raw.location3d = (Float(loc.x), Float(loc.y), Float(loc.z))

            if let ray = e.selectionRay {
                raw.has_ray = true
                raw.ray_origin = (Float(ray.origin.x), Float(ray.origin.y), Float(ray.origin.z))
                raw.ray_direction = (Float(ray.direction.vector.x), Float(ray.direction.vector.y), Float(ray.direction.vector.z))
            }

            if let pose = e.inputDevicePose?.pose3D {
                raw.has_pose = true
                raw.pose_position = (Float(pose.position.x), Float(pose.position.y), Float(pose.position.z))
                let q = pose.rotation.quaternion.vector
                raw.pose_rotation = (Float(q.x), Float(q.y), Float(q.z), Float(q.w))
            }

            received += 1
            if received <= 5 || received % 60 == 0 {
                inputLog.info("spatial event #\(received) kind=\(raw.kind) phase=\(raw.phase) ray=\(raw.has_ray) pose=\(raw.has_pose)")
            }
            GXXRInputPushRawSpatialEvent(&raw)
        }
    }
}
