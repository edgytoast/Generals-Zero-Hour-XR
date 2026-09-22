import Foundation
import Observation

/// Master and category volumes and the "Spatial battlefield audio" switch (visionos/Audio/GXAudioListener.h, defined inside the
/// engine, thread safe, valid before the engine has booted: the values are held and applied when the audio manager exists).
/// Persisted in UserDefaults and re-applied at launch.
@MainActor
@Observable
final class AudioSettingsStore {
    private static let key = "ui.audio.v1"

    struct Values: Equatable, Codable {
        var master = 1.0
        var music = 1.0
        var speech = 1.0
        var interface2D = 1.0
        var battlefield3D = 1.0
        var spatial = true
    }

    var values: Values {
        didSet {
            guard values != oldValue else { return }
            if let data = try? JSONEncoder().encode(values) { UserDefaults.standard.set(data, forKey: Self.key) }
            apply(values)
        }
    }

    init() {
        if let data = UserDefaults.standard.data(forKey: Self.key), let v = try? JSONDecoder().decode(Values.self, from: data) {
            values = v
        } else {
            values = Values()
        }
        apply(values)
    }

    private func apply(_ v: Values) {
        GXAudio_SetMasterVolume(Float(v.master))
        GXAudio_SetCategoryVolume(Int32(GXAUDIO_CATEGORY_MUSIC), Float(v.music))
        GXAudio_SetCategoryVolume(Int32(GXAUDIO_CATEGORY_SPEECH), Float(v.speech))
        GXAudio_SetCategoryVolume(Int32(GXAUDIO_CATEGORY_SFX_2D), Float(v.interface2D))
        GXAudio_SetCategoryVolume(Int32(GXAUDIO_CATEGORY_SFX_3D), Float(v.battlefield3D))
        GXAudio_SetSpatialBattlefieldAudio(v.spatial)
    }
}
