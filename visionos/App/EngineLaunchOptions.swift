import Foundation

/// Launch arguments for the engine host (in addition to LaunchOptions.swift / GameDataLaunchOptions.swift).
///   -autoStartEngine          start the real engine as soon as game data is ready (no tap on "Start Game")
///   -fakeEngine               run the GLES3 test scene as an engine client on the ENGINE thread (no game data needed);
///                             alias -angleTestScene. Extra: -fakeEngineBoot <s>, -fakeEngineStall <s>, -fakeEngineFps <n>
///   -allowNoData              enable "Enter Tabletop" without game data (built-in test scene / fake engine)
///   -engineFpsCap <n>         engine render cap (default 45; 0 = uncapped); -engineLogicHz <n> (default 30)
extension LaunchOptions {
    static var autoStartEngine: Bool { arguments.contains("-autoStartEngine") }
    static var fakeEngine: Bool { arguments.contains("-fakeEngine") || arguments.contains("-angleTestScene") }

    static var engineFpsCap: Int? {
        guard let i = arguments.firstIndex(of: "-engineFpsCap"), i + 1 < arguments.count else { return nil }
        return Int(arguments[i + 1])
    }
    static var engineLogicHz: Int? {
        guard let i = arguments.firstIndex(of: "-engineLogicHz"), i + 1 < arguments.count else { return nil }
        return Int(arguments[i + 1])
    }
}
