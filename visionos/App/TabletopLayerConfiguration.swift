import CompositorServices
import SwiftUI
import Metal
import os

private let configLog = Logger(subsystem: "com.generalsx.zerohour.xr.vision", category: "config")

/// Compositor layer configuration for the stereo tabletop.
///
/// Deliberate choices (see visionos/README-shell in the recon report):
///  - colorFormat: bgra8Unorm_srgb. Shaders work in linear space, hardware encodes sRGB on write.
///    Output alpha is PREMULTIPLIED; mixed immersion blends with passthrough using it.
///  - depthFormat: depth32Float, reverse-Z (1 = near, 0 = far) as Compositor Services requires.
///  - isFoveationEnabled: false. With foveation off the drawable has no rasterization rate map, so
///    a per-eye texture from the engine can be composited with a plain 1:1 copy that is
///    geometrically correct. Foveation can be added later behind a flag.
///  - layout: layered (one 2D-array texture, slice per eye) when supported, because it is the
///    system's native layout and cheapest for the compositor. The renderer handles shared and
///    dedicated as well: it draws one pass per view into whatever texture/slice/viewport the view maps to.
struct TabletopLayerConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities, configuration: inout LayerRenderer.Configuration) {
        let depthFormats = capabilities.supportedDepthFormats
        configuration.depthFormat = depthFormats.contains(.depth32Float) ? .depth32Float : (depthFormats.first ?? .depth32Float)

        let colorFormats = capabilities.supportedColorFormats(options: [])
        configuration.colorFormat = colorFormats.contains(.bgra8Unorm_srgb) ? .bgra8Unorm_srgb : (colorFormats.first ?? .bgra8Unorm_srgb)

        configuration.isFoveationEnabled = false

        let layouts = capabilities.supportedLayouts(options: [])
        var chosen: LayerRenderer.Layout = layouts.contains(.layered) ? .layered : (layouts.contains(.shared) ? .shared : .dedicated)
        switch LaunchOptions.forcedLayout {
        case "layered" where layouts.contains(.layered): chosen = .layered
        case "shared" where layouts.contains(.shared): chosen = .shared
        case "dedicated" where layouts.contains(.dedicated): chosen = .dedicated
        default: break
        }
        configuration.layout = chosen

        configLog.info("supported layouts=\(String(describing: layouts), privacy: .public) depth=\(String(describing: depthFormats), privacy: .public) color=\(String(describing: colorFormats), privacy: .public) minNear=\(capabilities.supportedMinimumNearPlaneDistance) chosen=\(String(describing: chosen), privacy: .public)")
    }
}
