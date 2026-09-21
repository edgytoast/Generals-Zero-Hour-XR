import Foundation

/// Launch arguments for the game-data flow (in addition to the ones in LaunchOptions.swift).
///   -allowNoData            enable Enter Tabletop without game data (built-in test scene)
///   -importFrom <path>      run the import flow on a folder path without the file picker
///                           (simulator/CI; the simulator app can read host paths directly)
///   -importBaseFrom <path>  explicit separate base Generals folder for -importFrom
///   -importInPlace          with -importFrom: use the folder in place instead of copying
///   -resetGameData          delete the imported copy and every saved data preference first
///                           (removes only the app's own imported files, never user sources)
extension LaunchOptions {
    static var allowNoData: Bool { arguments.contains("-allowNoData") }
    static var importInPlace: Bool { arguments.contains("-importInPlace") }
    static var resetGameData: Bool { arguments.contains("-resetGameData") }

    static var importFromPath: String? { value(after: "-importFrom") }
    static var importBaseFromPath: String? { value(after: "-importBaseFrom") }

    private static func value(after flag: String) -> String? {
        guard let i = arguments.firstIndex(of: flag), i + 1 < arguments.count else { return nil }
        return arguments[i + 1]
    }
}
