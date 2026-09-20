import Foundation
import SwiftUI
import os

private let dataLog = Logger(subsystem: "com.generalsx.zerohour.xr.vision", category: "gamedata")

/// Preference keys (all in UserDefaults; nothing secret is stored).
private enum Keys {
    static let activeSource = "GXGDActiveSource"
    static let inPlace = "GXGDInPlaceSource"
    static let importSource = "GXGDImportSource"
    static let importInPlaceFlag = "GXGDImportInPlace"
}

/// Game-data flow: detect -> validate -> import (or use in place) -> publish.
///
/// Every heavy call runs off the main actor; state changes happen through `setGameData(_:)`,
/// which also publishes/retracts the C snapshot read by the engine bridge
/// (GXXRGameData_IsReady / GXXRGameData_GetPaths).
extension AppModel {

    // MARK: - Derived state

    var isGameDataReady: Bool {
        if case .ready = gameData { return true }
        return false
    }

    var isGameDataBusy: Bool {
        switch gameData {
        case .validating, .importing: return true
        default: return false
        }
    }

    /// Enter Tabletop is allowed with real data, or for the built-in test scene when a test
    /// launch argument asks for it.
    var canEnterTabletop: Bool {
        isGameDataReady || LaunchOptions.allowNoData || LaunchOptions.autoImmersive
    }

    /// One line for logs and the test harness.
    var gameDataStatusLine: String {
        switch gameData {
        case .notConfigured: return "notConfigured"
        case .validating: return "validating"
        case .importing(let p): return "importing \(Int(p.fraction * 100))% (\(p.filesDone)/\(p.filesTotal) files)"
        case .ready(let p): return "ready source=\(p.source.rawValue) zh=\(p.zhRoot) base=\(p.baseRoot) complete=\(p.isComplete)"
        case .invalid(let r): return "invalid verdict=\(r.verdict.rawValue) missing=\(r.missing.count) damaged=\(r.damaged.count)"
        }
    }

    // MARK: - State plumbing

    func setGameData(_ status: GameDataStatus) {
        gameData = status
        if case .ready(let p) = status {
            GXGameDataService.publishReadyZeroHour(p.zhRoot, base: p.baseRoot)
        } else {
            GXGameDataService.retractReady()
        }
        if case .importing = status { return }  // progress ticks are too chatty to log
        dataLog.info("game data: \(self.gameDataStatusLine, privacy: .public)")
        print("[GameData] \(gameDataStatusLine)")
    }

    private func notice(_ text: String?, error: Bool = false) {
        gameDataNotice = text
        gameDataNoticeIsError = error
        if let text { print("[GameData] notice(\(error ? "error" : "info")): \(text)") }
    }

    private func releaseScopes(keeping keep: [URL] = []) {
        let keepPaths = Set(keep.map(\.path))
        var remaining: [URL] = []
        for url in heldScopes {
            if keepPaths.contains(url.path) { remaining.append(url) } else { url.stopAccessingSecurityScopedResource() }
        }
        heldScopes = remaining
    }

    private func acquire(_ url: URL) {
        if url.startAccessingSecurityScopedResource() { heldScopes.append(url) }
    }

    // MARK: - Startup

    /// Called once from the launcher. Recovers an interrupted import, then finds usable data.
    func startGameData() async {
        guard !gameDataStartupDone else { return }
        gameDataStartupDone = true
        if LaunchOptions.resetGameData { await resetForTesting() }
        importInPlace = UserDefaults.standard.bool(forKey: Keys.importInPlaceFlag) || LaunchOptions.importInPlace
        await refreshInterruptedImport()

        if let path = LaunchOptions.importFromPath {
            let url = URL(fileURLWithPath: path)
            let base = LaunchOptions.importBaseFromPath.map { URL(fileURLWithPath: $0) }
            print("[GameData] -importFrom \(path) inPlace=\(LaunchOptions.importInPlace)")
            beginSelection(url, baseURL: base, inPlace: LaunchOptions.importInPlace)
            return
        }
        await evaluateAvailableData()
    }

    /// Re-check when the app returns to the foreground: the player may have added files with the
    /// Files app in the meantime.
    func gameDataBecameActive() async {
        guard gameDataStartupDone else { return }
        switch gameData {
        case .notConfigured, .invalid: await evaluateAvailableData()
        default: break
        }
    }

    private func refreshInterruptedImport() async {
        let rec = await Task.detached { GXGameDataService.recover() }.value
        switch rec.action {
        case .partialCopyKept:
            let saved = savedImportSource()
            interruptedImport = InterruptedImport(
                filesStaged: rec.filesStaged, filesTotal: rec.filesTotal,
                bytesStaged: rec.bytesStaged, bytesTotal: rec.bytesTotal,
                sourceName: (rec.sourceZh as NSString).lastPathComponent,
                canResumeAutomatically: saved != nil)
            print("[GameData] interrupted import: \(rec.filesStaged)/\(rec.filesTotal) files")
        case .rolledForward:
            interruptedImport = nil
            notice("An import that was interrupted has been completed.")
        default:
            interruptedImport = nil
        }
    }

    // MARK: - Finding usable data

    /// Looks, in order, at the source used last time, the imported copy, an in-place folder and
    /// the Documents/GameData drop folder; publishes the first one that validates.
    func evaluateAvailableData() async {
        setGameData(.validating)
        validationSubject = "Checking saved game data"

        var order: [GameDataSource] = [.imported, .inPlace, .sharedDocuments]
        if let raw = UserDefaults.standard.string(forKey: Keys.activeSource), let preferred = GameDataSource(rawValue: raw) {
            order.removeAll { $0 == preferred }
            order.insert(preferred, at: 0)
        }

        var firstFailure: GXGDReport?
        for source in order {
            switch await candidateReport(for: source) {
            case .none: continue
            case .some(let candidate):
                if candidate.report.isReady {
                    publishReady(from: candidate.report, source: source, installed: candidate.installed)
                    return
                }
                if firstFailure == nil { firstFailure = candidate.report }
            }
        }
        if let firstFailure { setGameData(.invalid(firstFailure)) } else { setGameData(.notConfigured) }
    }

    private struct Candidate {
        var report: GXGDReport
        var installed: GXGDInstalled?
    }

    private func candidateReport(for source: GameDataSource) async -> Candidate? {
        switch source {
        case .imported:
            return await Task.detached { () -> Candidate? in
                guard let inst = GXGameDataService.installed() else { return nil }
                let report = GXGameDataService.validateSelected(inst.zhRoot, explicitBase: inst.merged ? nil : inst.baseRoot)
                return Candidate(report: report, installed: inst)
            }.value
        case .inPlace:
            guard let record = savedInPlace() else { return nil }
            var urls: [URL] = []
            for data in [record.bookmark].compactMap({ $0 }) {
                if let url = resolveBookmark(data) { urls.append(url) }
            }
            var baseURL: URL?
            if let data = record.baseBookmark { baseURL = resolveBookmark(data) }
            let primary = urls.first ?? URL(fileURLWithPath: record.path)
            let base = baseURL ?? record.basePath.map { URL(fileURLWithPath: $0) }
            // Access must stay open for the whole process: the engine reads these paths later.
            acquire(primary)
            if let base { acquire(base) }
            let selected = primary.path, explicit = base?.path
            return await Task.detached {
                Candidate(report: GXGameDataService.validateSelected(selected, explicitBase: explicit), installed: nil)
            }.value
        case .sharedDocuments:
            let path = GXGameDataService.sharedDocumentsPath()
            guard GXGameDataService.folderHasVisibleContent(path) else { return nil }
            return await Task.detached {
                Candidate(report: GXGameDataService.validateSelected(path, explicitBase: nil), installed: nil)
            }.value
        }
    }

    private func publishReady(from report: GXGDReport, source: GameDataSource, installed: GXGDInstalled?) {
        let paths = GameDataPaths(
            zhRoot: report.zhRoot,
            baseRoot: report.baseRoot.isEmpty ? report.zhRoot : report.baseRoot,
            userDataRoot: GXGameDataService.userDataRootPath(),
            source: source,
            isComplete: report.isComplete,
            languages: report.languages,
            optionalNotes: report.optionalMissing,
            bytes: installed?.bytes ?? report.totalBytesEstimate)
        if source == .sharedDocuments { GXGameDataService.excludeFromBackup(atPath: GXGameDataService.sharedDocumentsPath()) }
        UserDefaults.standard.set(source.rawValue, forKey: Keys.activeSource)
        setGameData(.ready(paths))
    }

    // MARK: - Picking a folder

    func presentFolderPicker(_ purpose: FilePickerPurpose = .gameFolder) {
        filePickerPurpose = purpose
        filePickerPresented = true
    }

    func handlePickerResult(_ result: Result<[URL], Error>) {
        switch result {
        case .failure(let error):
            notice("The folder could not be opened: \(error.localizedDescription)", error: true)
        case .success(let urls):
            guard let url = urls.first else { return }
            if filePickerPurpose == .baseFolder, let first = firstPick {
                beginSelection(first, baseURL: url, inPlace: importInPlace)
            } else {
                beginSelection(url, baseURL: nil, inPlace: importInPlace)
            }
        }
    }

    /// Validate `url` (and optionally an explicit base folder), then import or use in place.
    func beginSelection(_ url: URL, baseURL: URL?, inPlace: Bool) {
        guard !isGameDataBusy else { return }
        notice(nil)
        awaitingBaseFolder = false
        releaseScopes()
        acquire(url)
        if let baseURL { acquire(baseURL) }
        validationSubject = url.lastPathComponent
        setGameData(.validating)

        let selected = url.path, explicit = baseURL?.path
        importTask = Task { [weak self] in
            let report = await Task.detached { GXGameDataService.validateSelected(selected, explicitBase: explicit) }.value
            guard let self else { return }
            print("[GameData] validation of \(selected):\n\(report.dump)")
            if report.isReady {
                if inPlace { await self.useInPlace(report: report, url: url, baseURL: baseURL) }
                else { self.startImport(report: report, url: url, baseURL: baseURL) }
            } else {
                // Keep the Zero Hour selection open so a separate Generals folder can be added.
                let offerBase = !report.zhRoot.isEmpty && report.baseFolderMissing && baseURL == nil
                self.firstPick = offerBase ? url : nil
                self.awaitingBaseFolder = offerBase
                self.releaseScopes(keeping: offerBase ? [url] : [])
                self.setGameData(.invalid(report))
            }
        }
    }

    // MARK: - Import (default)

    private func startImport(report: GXGDReport, url: URL, baseURL: URL?) {
        saveImportSource(url: url, baseURL: baseURL)
        let token = GXGDCancelToken()
        cancelToken = token
        var initial = ImportProgress()
        initial.bytesTotal = report.totalBytesEstimate
        setGameData(.importing(initial))
        print("[GameData] import start: zh=\(report.zhRoot) base=\(report.baseRoot)")

        importTask = Task { [weak self] in
            let result = await Task.detached { () -> GXGDInstallResult in
                GXGameDataService.installReport(report, token: token) { progress in
                    Task { @MainActor [weak self] in self?.applyProgress(ImportProgress(progress), cancelling: token.isCancelled) }
                }
            }.value
            await self?.importFinished(result)
        }
    }

    private func applyProgress(_ progress: ImportProgress, cancelling: Bool) {
        guard case .importing = gameData else { return }
        var p = progress
        p.cancelRequested = cancelling
        gameData = .importing(p)  // no publish/log: progress is not a state change
    }

    private func importFinished(_ result: GXGDInstallResult) async {
        releaseScopes()
        cancelToken = nil
        firstPick = nil
        print("[GameData] import finished: outcome=\(result.outcome.rawValue) copied=\(result.filesCopied) reused=\(result.filesReused) message=\(result.message)")
        switch result.outcome {
        case .installed:
            clearImportSource()
            UserDefaults.standard.set(GameDataSource.imported.rawValue, forKey: Keys.activeSource)
            UserDefaults.standard.removeObject(forKey: Keys.inPlace)
            interruptedImport = nil
            await evaluateAvailableData()
            if isGameDataReady {
                notice("Game data imported (\(result.filesCopied) files copied\(result.filesReused > 0 ? ", \(result.filesReused) reused from the earlier attempt" : "")).")
            }
        case .cancelled:
            await refreshInterruptedImport()
            await evaluateAvailableData()
            notice("Import cancelled. The files copied so far were kept; choose the same folder again or press Resume to continue.")
        case .insufficientSpace:
            await refreshInterruptedImport()
            await evaluateAvailableData()
            let need = ByteCountFormatter.string(fromByteCount: Int64(result.bytesNeeded), countStyle: .file)
            let have = ByteCountFormatter.string(fromByteCount: Int64(result.bytesAvailable), countStyle: .file)
            let detail = result.bytesNeeded > 0 ? " About \(need) is needed and \(have) is free." : ""
            notice("Not enough free storage on this device.\(detail) Free up space (or remove the imported copy of an older import), then choose the folder again.", error: true)
        default:
            await refreshInterruptedImport()
            await evaluateAvailableData()
            notice(result.message.isEmpty ? "The import failed." : result.message, error: true)
        }
    }

    func cancelImport() {
        guard case .importing(var p) = gameData else { return }
        cancelToken?.cancel()
        p.cancelRequested = true
        gameData = .importing(p)
    }

    // MARK: - Resume, discard, remove

    func resumeInterruptedImport() {
        guard !isGameDataBusy, let saved = savedImportSource() else {
            notice("The original folder is no longer known. Choose the same folder again to resume.", error: true)
            return
        }
        var url: URL?
        if let data = saved.bookmark { url = resolveBookmark(data) }
        if url == nil { url = URL(fileURLWithPath: saved.path) }
        var base: URL?
        if let data = saved.baseBookmark { base = resolveBookmark(data) }
        if base == nil, let p = saved.basePath { base = URL(fileURLWithPath: p) }
        guard let url else { return }
        beginSelection(url, baseURL: base, inPlace: false)
    }

    func discardInterruptedImport() {
        Task {
            await Task.detached { GXGameDataService.discardPartialImport() }.value
            clearImportSource()
            interruptedImport = nil
            notice("The partial import was deleted.")
        }
    }

    /// Delete the app's own imported copy (never touches the folders the player picked).
    func removeImportedData() {
        Task {
            let error: String? = await Task.detached { () -> String? in
                do { try GXGameDataService.removeInstalled(); return nil } catch { return error.localizedDescription }
            }.value
            if let error { notice("Could not remove the imported data: \(error)", error: true); return }
            if UserDefaults.standard.string(forKey: Keys.activeSource) == GameDataSource.imported.rawValue {
                UserDefaults.standard.removeObject(forKey: Keys.activeSource)
            }
            notice("Imported game data removed.")
            await evaluateAvailableData()
        }
    }

    func recheck() {
        guard !isGameDataBusy else { return }
        Task { await evaluateAvailableData() }
    }

    func setImportInPlace(_ value: Bool) {
        importInPlace = value
        UserDefaults.standard.set(value, forKey: Keys.importInPlaceFlag)
    }

    // MARK: - Use in place (advanced)

    private func useInPlace(report: GXGDReport, url: URL, baseURL: URL?) async {
        var record = SavedFolderSource(bookmark: nil, path: url.path, baseBookmark: nil, basePath: baseURL?.path)
        record.bookmark = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil)
        if let baseURL { record.baseBookmark = try? baseURL.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) }
        if let data = try? JSONEncoder().encode(record) { UserDefaults.standard.set(data, forKey: Keys.inPlace) }
        // The scoped access acquired for the pick stays open: the engine reads these paths for the
        // rest of the process.
        firstPick = nil
        publishReady(from: report, source: .inPlace, installed: nil)
        notice("Using the folder in place. It must stay available (for example a Files provider that is downloaded and online).")
    }

    private func savedInPlace() -> SavedFolderSource? {
        guard let data = UserDefaults.standard.data(forKey: Keys.inPlace) else { return nil }
        return try? JSONDecoder().decode(SavedFolderSource.self, from: data)
    }

    // MARK: - Bookmarks and saved sources

    private func resolveBookmark(_ data: Data) -> URL? {
        var stale = false
        guard let url = try? URL(resolvingBookmarkData: data, options: [], relativeTo: nil, bookmarkDataIsStale: &stale) else { return nil }
        return url
    }

    private func saveImportSource(url: URL, baseURL: URL?) {
        var record = SavedFolderSource(bookmark: nil, path: url.path, baseBookmark: nil, basePath: baseURL?.path)
        record.bookmark = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil)
        if let baseURL { record.baseBookmark = try? baseURL.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) }
        if let data = try? JSONEncoder().encode(record) { UserDefaults.standard.set(data, forKey: Keys.importSource) }
    }

    private func savedImportSource() -> SavedFolderSource? {
        guard let data = UserDefaults.standard.data(forKey: Keys.importSource) else { return nil }
        return try? JSONDecoder().decode(SavedFolderSource.self, from: data)
    }

    private func clearImportSource() {
        UserDefaults.standard.removeObject(forKey: Keys.importSource)
    }

    // MARK: - Test support

    private func resetForTesting() async {
        print("[GameData] -resetGameData: removing the imported copy and saved preferences")
        await Task.detached {
            GXGameDataService.discardPartialImport()
            try? GXGameDataService.removeInstalled()
        }.value
        for key in [Keys.activeSource, Keys.inPlace, Keys.importSource, Keys.importInPlaceFlag] {
            UserDefaults.standard.removeObject(forKey: key)
        }
    }
}
