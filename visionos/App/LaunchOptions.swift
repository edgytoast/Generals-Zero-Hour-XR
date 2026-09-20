import Foundation

/// Launch-argument switches used for automated testing and renderer bring-up.
///   -autoImmersive          open the tabletop immersive space as soon as the app starts
///   -externalEyeTextures    exercise the engine hand-off path (offscreen per-eye textures
///                           submitted through XRPresentation_SubmitEyeTexture)
///   -layout <name>          force layered | shared | dedicated texture layout
enum LaunchOptions {
    static let arguments = CommandLine.arguments

    static var autoImmersive: Bool { arguments.contains("-autoImmersive") }
    static var externalEyeTextures: Bool { arguments.contains("-externalEyeTextures") }

    static var forcedLayout: String? {
        guard let i = arguments.firstIndex(of: "-layout"), i + 1 < arguments.count else { return nil }
        return arguments[i + 1].lowercased()
    }
}
