import Foundation

extension GXGDReport: @unchecked Sendable {}
extension GXGDProgress: @unchecked Sendable {}
extension GXGDInstallResult: @unchecked Sendable {}
extension GXGDRecovery: @unchecked Sendable {}
extension GXGDInstalled: @unchecked Sendable {}
extension GXGDCancelToken: @unchecked Sendable {}

/// Where the data that is in use comes from.
enum GameDataSource: String, Equatable, Sendable {
    /// Copied into Library/Application Support/GeneralsX/GameData (the default).
    case imported
    /// A folder the user picked, kept alive by a persisted security-scoped bookmark (advanced).
    case inPlace
    /// Files the user placed in the app's Documents/GameData folder (Files app / file sharing).
    case sharedDocuments

    var title: String {
        switch self {
        case .imported: return "Copied into the app"
        case .inPlace: return "Used in place from the folder you chose"
        case .sharedDocuments: return "Files in the app's Documents/GameData folder"
        }
    }
}

/// Everything the engine needs, plus display details.
struct GameDataPaths: Equatable, Sendable {
    var zhRoot: String
    var baseRoot: String
    var userDataRoot: String
    var source: GameDataSource
    var isComplete: Bool
    var languages: [String]
    var bytes: UInt64
    var baseMerged: Bool { zhRoot == baseRoot }
}

struct ImportProgress: Equatable, Sendable {
    enum Phase: Int, Sendable { case planning, checkingSpace, copying, verifying, committing, cleaning, done }
    var phase: Phase = .planning
    var bytesDone: UInt64 = 0
    var bytesTotal: UInt64 = 0
    var filesDone: UInt32 = 0
    var filesTotal: UInt32 = 0
    var bytesReused: UInt64 = 0
    var bytesPerSecond: Double = 0
    var currentFile: String = ""
    var cancelRequested = false

    var fraction: Double { bytesTotal > 0 ? min(1, Double(bytesDone) / Double(bytesTotal)) : 0 }

    init() {}
    init(_ p: GXGDProgress) {
        phase = Phase(rawValue: p.phase.rawValue) ?? .copying
        bytesDone = p.bytesDone
        bytesTotal = p.bytesTotal
        filesDone = p.filesDone
        filesTotal = p.filesTotal
        bytesReused = p.bytesReused
        bytesPerSecond = p.bytesPerSecond
        currentFile = p.currentFile
    }
}

/// The single state the launcher and the engine bridge follow.
enum GameDataStatus {
    /// Nothing usable has been chosen yet.
    case notConfigured
    /// Reading archive tables (a few seconds at most).
    case validating
    /// Copying into app storage; the associated value drives the progress bar.
    case importing(ImportProgress)
    /// Validated and published to the engine bridge (GXXRGameData_IsReady() is true).
    case ready(GameDataPaths)
    /// A folder was inspected and cannot be used; the report says why.
    case invalid(GXGDReport)
}

/// An import that was interrupted (app quit, cancelled, device restarted) and can be resumed.
struct InterruptedImport: Equatable, Sendable {
    var filesStaged: UInt32
    var filesTotal: UInt32
    var bytesStaged: UInt64
    var bytesTotal: UInt64
    var sourceName: String
    var canResumeAutomatically: Bool
}

enum FilePickerPurpose { case gameFolder, baseFolder }

/// Persisted description of where an import (or in-place use) reads from, so an interrupted
/// import can resume without asking again.
struct SavedFolderSource: Codable {
    var bookmark: Data?
    var path: String
    var baseBookmark: Data?
    var basePath: String?
}
