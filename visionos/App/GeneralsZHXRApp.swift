import CompositorServices
import SwiftUI

@main
struct GeneralsZHXRApp: App {
    @State private var model = AppModel()
    @State private var immersionStyle: ImmersionStyle = .mixed

    var body: some Scene {
        WindowGroup(id: "launcher") {
            LauncherView()
                .environment(model)
        }
        .defaultSize(width: 760, height: 1000)

        // Stereo tabletop rendered with Metal through Compositor Services.
        // .mixed keeps passthrough visible; the layer's alpha channel (premultiplied)
        // decides what the player sees through the scene.
        ImmersiveSpace(id: AppModel.spaceID) {
            CompositorLayer(configuration: TabletopLayerConfiguration()) { layerRenderer in
                model.layerRendererReady(layerRenderer)
            }
        }
        .immersionStyle(selection: $immersionStyle, in: .mixed)
    }
}
