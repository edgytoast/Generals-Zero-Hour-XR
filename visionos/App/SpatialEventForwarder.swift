import Foundation
import SwiftUI
import Spatial
import os

private let inputLog = Logger(subsystem: "com.generalsx.zerohour.xr.vision", category: "input-swift")

/// Copies LayerRenderer spatial events into plain C structs for the Objective-C++ input layer
/// (Input/GXXRInput.mm), which normalizes them to XRInteraction.h for the VisionInteraction state machine.
///
/// Verified (SDK 27.0 SwiftUI/_CompositorServices_SwiftUI interfaces):
///   LayerRenderer.onSpatialEvent : @MainActor (SpatialEventCollection) -> Void
///   SpatialEventCollection.Event: id, timestamp, kind, location3D, phase, selectionRay?,
///   inputDevicePose?.pose3D, chirality?, modifierKeys, trackingAreaIdentifier (visionOS 26).
/// Facts the input model is built on (docs/visionos-interaction.md):
///   * the app never receives a continuous gaze ray: `selectionRay` is the gaze ray of the pinch START
///     (nil afterwards); afterwards only the hand pose (`inputDevicePose`) moves;
///   * `event.location` is always (0,0) in a CompositorLayer and is never used;
///   * there is no `.began` phase: the first event with a new id is the begin, `.ended`/`.cancelled` end it.
/// The coordinate space of location3D / selectionRay / pose3D is assumed to be the immersive-space origin (the
/// same world space as the ARKit device anchor). That is unverified on a physical device; the interaction math
/// flows through one place (VisionInteraction) so it is a one-line fix if the space differs.
@MainActor
enum SpatialEventForwarder {
    private static var received: UInt64 = 0

    static func forward(_ events: SpatialEventCollection) {
        // The system may deliver ARKit hand tracking permission only once an immersive space is open; a first
        // pinch proves it is. Start lazily unless the app already started the tracker explicitly.
        SpatialHandTracker.shared.startIfNeeded()

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

            // Keyboard modifiers ride on the event: Shift = additive selection, Option = emulate the second hand
            // (simulator: mouse click = pinch at the pointer, modifier keys stand in for the second hand).
            raw.modifiers_valid = true
            raw.modifiers = InteractionControls.modifierBits(e.modifierKeys)

            if #available(visionOS 26.0, *) {
                raw.tracking_area_id = UInt32(truncatingIfNeeded: e.trackingAreaIdentifier.rawValue)
            }

            received += 1
            if received <= 5 || received % 60 == 0 {
                inputLog.info("spatial event #\(received) kind=\(raw.kind) phase=\(raw.phase) ray=\(raw.has_ray) pose=\(raw.has_pose) mods=\(raw.modifiers) area=\(raw.tracking_area_id)")
            }
            GXXRInputPushRawSpatialEvent(&raw)
        }
    }

    /// Cancel everything in flight (scene phase change, layer paused/invalidated, headset removed, modal).
    /// Balanced: each active pinch is delivered as a cancel and its remaining events are swallowed.
    static func reset(_ reason: InteractionControls.FlushReason = .flush) {
        GXXRInputFlush(reason.rawValue)
    }
}

/// UI-facing commands for the interaction layer. They travel in the same ordered queue as the pinch events, so a
/// button press can never overtake or be overtaken by a gesture that started earlier.
///
/// Suggested wiring (owned by the launcher/app files, not by this package):
///   Toggle("Additive select", isOn: ...)   -> InteractionControls.setAdditive(_:)
///   Button("Cancel build")                 -> InteractionControls.cancelPlacement()
///   Button("Recenter board")               -> InteractionControls.recenterBoard()
///   Button("Ground View")                  -> InteractionControls.enterGroundView() / exitGroundView()
///   .onModifierKeysChanged / .onKeyPress   -> InteractionControls.setModifiers(_:), .engineBack()
///   .onChange(of: scenePhase)              -> SpatialEventForwarder.reset(.focus) when it leaves .active
@MainActor
enum InteractionControls {
    enum FlushReason: Int32 {
        case flush = 0, trackingLost = 1, focus = 2
    }

    static func setAdditive(_ on: Bool) { GXXRInputPostCommand(Int32(GXXRCommandSetAdditive), on ? 1 : 0) }
    static func cancelPlacement() { GXXRInputPostCommand(Int32(GXXRCommandCancelPlacement), 0) }
    static func cancelAll() { GXXRInputPostCommand(Int32(GXXRCommandCancelAll), 0) }
    static func recenterBoard() { GXXRInputPostCommand(Int32(GXXRCommandRecenterBoard), 0) }
    static func resetWorkspace() { GXXRInputPostCommand(Int32(GXXRCommandResetWorkspace), 0) }
    static func enterGroundView() { GXXRInputPostCommand(Int32(GXXRCommandEnterGroundView), 0) }
    static func exitGroundView() { GXXRInputPostCommand(Int32(GXXRCommandExitGroundView), 0) }
    /// Rotate the building ghost by `degrees` (signed): button / keyboard alternative to the hand twist.
    static func rotatePlacement(degrees: Int) { GXXRInputPostCommand(Int32(GXXRCommandRotatePlacementStep), Int32(degrees)) }
    /// Escape/Back into the engine (closes menus and dialogs).
    static func engineBack() { GXXRInputPostCommand(Int32(GXXRCommandEngineBack), 0) }

    static func setModifiers(_ mods: EventModifiers) { GXXRInputSetModifiers(modifierBits(mods)) }

    static func modifierBits(_ mods: EventModifiers) -> UInt32 {
        var bits: UInt32 = 0
        if mods.contains(.shift) { bits |= UInt32(GXXRModShift) }
        if mods.contains(.control) { bits |= UInt32(GXXRModControl) }
        if mods.contains(.option) { bits |= UInt32(GXXRModOption) }
        if mods.contains(.command) { bits |= UInt32(GXXRModCommand) }
        return bits
    }
}
