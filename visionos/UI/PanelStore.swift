import Foundation
import Observation
import SwiftUI
import os

private let uiLog = Logger(subsystem: "com.generalsx.zerohour.xr.vision", category: "ui")

/// Launch arguments of the spatial UI (in addition to LaunchOptions.swift / EngineLaunchOptions.swift).
///   -openWindow commands|settings|help[,...]   open those windows at launch (also without an immersive space)
///   -settingsPage workspace|presentation|graphics|audio|controls|data|diagnostics
///   -uiFakeSnapshot [1|2|3]                    scripted panel state instead of the engine: 1 skirmish (armed / on / pending /
///                                              group counts), 2 nothing selected (disabled with reasons), 3 dialog open
///   -uiLanguage de|en                          UI language for this run (not stored)
///   -uiAppearance light|dark                   colour scheme for the windows of this run
///   -uiScript c31,c24,m3.11,x1.2               replay actions after launch: cN = Commands control N, mP.N = Menu page P
///                                              control N, xE.V = extra action E with value V
///   -uiTactics                                 open the tactics foldout of the Commands window at launch
enum UILaunchOptions {
    private static var args: [String] { CommandLine.arguments }

    private static func value(after flag: String) -> String? {
        guard let i = args.firstIndex(of: flag), i + 1 < args.count else { return nil }
        return args[i + 1]
    }

    static var openWindows: [String] {
        (value(after: "-openWindow") ?? "").split(separator: ",").map { String($0).lowercased() }
    }
    static var settingsPage: String? { value(after: "-settingsPage")?.lowercased() }
    static var fakeSnapshot: Int? {
        guard let i = args.firstIndex(of: "-uiFakeSnapshot") else { return nil }
        if i + 1 < args.count, let n = Int(args[i + 1]) { return n }
        return 1
    }
    static var language: UILang? {
        switch value(after: "-uiLanguage")?.lowercased() {
        case "de", "german": return .german
        case "en", "english": return .english
        default: return nil
        }
    }
    static var appearance: ColorScheme? {
        switch value(after: "-uiAppearance")?.lowercased() {
        case "light": return .light
        case "dark": return .dark
        default: return nil
        }
    }
    static var script: [String] { (value(after: "-uiScript") ?? "").split(separator: ",").map(String.init) }
    static var openTactics: Bool { args.contains("-uiTactics") }
}

// MARK: - View models built from the C++ panel model

struct PanelControlItem: Identifiable, Equatable {
    let id: Int
    let section: Int
    let role: Int
    let state: Int
    let badge: Int
    let label: String
    let value: String
    let explain: String

    var isDisabled: Bool { state & Int(GX_STATE_DISABLED) != 0 }
    var isOn: Bool { state & Int(GX_STATE_ON) != 0 }
    var isArmed: Bool { state & Int(GX_STATE_ARMED) != 0 }
    var isPending: Bool { state & Int(GX_STATE_PENDING) != 0 }
    var isActive: Bool { state & Int(GX_STATE_ACTIVE) != 0 }
    /// Anything the eye should read as "switched on".
    var isHighlighted: Bool { isOn || isArmed || isPending }
    var isTargeted: Bool { role == Int(GX_ROLE_TARGETED) }
    var isDanger: Bool { role == Int(GX_ROLE_DANGER) }
}

struct PanelSectionItem: Identifiable, Equatable {
    let id: Int
    let title: String
    var controls: [PanelControlItem]
}

struct PanelPageModel: Equatable {
    var title = ""
    var status = ""
    var hint = ""
    var sections: [PanelSectionItem] = []
    /// Controls that belong to no section (tabs, close).
    var loose: [PanelControlItem] = []

    func control(_ id: Int) -> PanelControlItem? {
        for s in sections { if let c = s.controls.first(where: { $0.id == id }) { return c } }
        return loose.first { $0.id == id }
    }
    var allControls: [PanelControlItem] { sections.flatMap(\.controls) + loose }
}

func cString<T>(_ tuple: T) -> String {
    withUnsafeBytes(of: tuple) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
}

// MARK: - Store

/// The one object behind the Commands, Settings and hover windows: the engine snapshot (polled from the engine thread's
/// hook, see GXEnginePanelState.h), the language, the persisted preferences and the actions. Everything the panels show comes
/// from `GXPanelModel_Build`, i.e. from the Quest control tables; everything they do goes through `GXPanelAction_Perform`, i.e.
/// through the ported Quest switch tables on the engine thread.
@MainActor
@Observable
final class PanelStore {
    static let languageKey = "ui.language"

    private(set) var snapshot = GXPanelSnapshot()
    private(set) var commands = PanelPageModel()
    private(set) var language: UILang
    /// Ground View as the panels last requested it (the interaction layer does not report it back yet).
    var groundViewRequested = false
    /// A camera / action that the engine refused (menu, cutscene, dialog) - shown once next to the button.
    var refusedNotice = ""
    var lastEffect = ""
    /// Called for host-side effects the app must apply (hide the console, ...).
    @ObservationIgnored var effectHandler: ((GXPanelActionResult) -> Void)?

    @ObservationIgnored private var lastVersion: UInt32 = .max
    @ObservationIgnored private var lastLanguage: UILang?
    @ObservationIgnored private var lastCommandsKey = ""
    @ObservationIgnored private var pollTask: Task<Void, Never>?
    @ObservationIgnored private var scriptRan = false

    init() {
        if let forced = UILaunchOptions.language {
            language = forced
        } else if let stored = UserDefaults.standard.object(forKey: Self.languageKey) as? Int32, let l = UILang(rawValue: stored) {
            language = l
        } else {
            language = .system
        }
        GXEnginePanelState_Install()
        var prefs = Self.loadPrefs(language: language)
        prefs.language = language.rawValue
        GXEnginePanelState_SetPrefs(&prefs)
        GXEnginePanelState_SetLanguage(language.rawValue, false)
        if let scenario = UILaunchOptions.fakeSnapshot { GXEnginePanelState_SetScripted(Int32(scenario)) }
        if UILaunchOptions.openTactics { perform(page: Int(GX_PANEL_COMMANDS), id: 37) }
        refresh()
        startPolling()
    }

    // MARK: strings

    func t(_ key: String) -> String { L10n.t(key, language) }
    func xr(_ german: String) -> String { L10n.xr(german, language) }

    // MARK: polling

    /// The ~10 Hz refresh loop: owned by the store (started in `init`), so it survives any single window closing.
    private func startPolling() {
        guard pollTask == nil else { return }
        pollTask = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                self.refresh()
                try? await Task.sleep(for: .milliseconds(100))
            }
        }
    }

    func refresh() {
        var s = GXPanelSnapshot()
        GXEnginePanelState_Get(&s)
        drainEffects()
        guard s.version != lastVersion || language != lastLanguage else { return }
        lastVersion = s.version
        lastLanguage = language
        snapshot = s
        rebuildCommands()
        persistPrefsIfChanged(s.session.prefs)
        runScriptOnce()
    }

    private func rebuildCommands() {
        let built = page(Int(GX_PANEL_COMMANDS))
        if built != commands { commands = built }
    }

    /// The model of any Quest page in the current language.
    func page(_ page: Int) -> PanelPageModel {
        var view = GXPanelView()
        var controls = [GXPanelControl](repeating: GXPanelControl(), count: Int(GX_PANEL_MAX_CONTROLS))
        var snap = snapshot
        let n = Int(GXPanelModel_Build(Int32(page), &snap, language.rawValue, &view, &controls, Int32(controls.count)))
        return Self.page(from: view, controls: Array(controls.prefix(n)))
    }

    private static func page(from view: GXPanelView, controls: [GXPanelControl]) -> PanelPageModel {
        var model = PanelPageModel()
        model.title = cString(view.title)
        model.status = cString(view.status)
        model.hint = cString(view.hint)
        var sections: [PanelSectionItem] = []
        withUnsafeBytes(of: view.sections) { raw in
            let base = raw.bindMemory(to: GXPanelSection.self)
            for i in 0..<Int(view.sectionCount) {
                sections.append(PanelSectionItem(id: Int(base[i].id), title: cString(base[i].title), controls: []))
            }
        }
        for c in controls {
            let item = PanelControlItem(id: Int(c.id), section: Int(c.section), role: Int(c.role), state: Int(c.state), badge: Int(c.badge),
                                        label: cString(c.label), value: cString(c.value), explain: cString(c.explain))
            if item.section >= 0 && item.section < sections.count { sections[item.section].controls.append(item) } else { model.loose.append(item) }
        }
        model.sections = sections
        return model
    }

    // MARK: actions

    /// A control of a Quest page (GX_PANEL_*). Runs on the engine thread; the result shows up in the next snapshot.
    func perform(page: Int, id: Int) {
        if !GXPanelAction_Perform(Int32(page), Int32(id)) {
            uiLog.info("action \(page, privacy: .public)/\(id, privacy: .public) not queued: no running engine")
        }
        refresh()
    }

    func performExtra(_ extra: Int, value: Int = 0) {
        if !GXPanelAction_PerformExtra(Int32(extra), Int32(value)) {
            uiLog.info("extra action \(extra, privacy: .public) not queued: no running engine")
        }
        refresh()
    }

    private func drainEffects() {
        var r = GXPanelActionResult()
        while GXPanelAction_PopResult(&r) {
            if r.page < 0, r.hostValue == 0, r.handled {
                refusedNotice = t("The camera cannot be changed right now (menu, cutscene or dialog).")
            }
            lastEffect = "host \(r.host) value \(r.hostValue) stereo \(r.stereoWorld) close \(r.closeMenu)"
            handle(r)
            effectHandler?(r)
        }
    }

    /// The effects only this process can apply: Ground View and the workspace go through the interaction layer's command queue.
    private func handle(_ r: GXPanelActionResult) {
        switch Int(r.host) {
        case Int(GX_HOST_ARM_GROUND_VIEW):
            groundViewRequested = true
            InteractionControls.enterGroundView()
        case Int(GX_HOST_RECENTER_WORKSPACE):
            InteractionControls.recenterBoard()
        case Int(GX_HOST_PHOTO_ARRANGEMENT):
            InteractionControls.resetWorkspace()
        default: break
        }
    }

    func setLanguage(_ new: UILang) {
        guard new != language else { return }
        language = new
        UserDefaults.standard.set(new.rawValue, forKey: Self.languageKey)
        GXEnginePanelState_SetLanguage(new.rawValue, true)
        lastVersion = .max
        refresh()
    }

    func toggleGroundView() {
        if groundViewRequested || snapshot.groundViewMode != 0 {
            groundViewRequested = false
            InteractionControls.exitGroundView()
        } else {
            perform(page: Int(GX_PANEL_MENU_VIEW), id: 16)
        }
    }

    // MARK: preferences (the Quest XrLayout / XrPerformance toggles)

    private static let prefsKey = "ui.prefs.v1"

    private static func loadPrefs(language: UILang) -> GXPanelPrefs {
        var p = GXPanelPrefs()
        p.healthBars = true; p.unitRings = true; p.boardFrame = true; p.elideWorldCopy = true
        p.language = language.rawValue
        guard let d = UserDefaults.standard.dictionary(forKey: prefsKey) else { return p }
        func b(_ k: String, _ def: Bool) -> Bool { (d[k] as? Bool) ?? def }
        p.healthBars = b("healthBars", true); p.unitRings = b("unitRings", true); p.boardFrame = b("boardFrame", true)
        p.leftHanded = b("leftHanded", false); p.volumeShadows = b("volumeShadows", false); p.measurement = false
        p.atlasStereo = b("atlasStereo", false); p.multiviewStereo = false; p.elideWorldCopy = b("elideWorldCopy", true)
        p.resolutionTier = Int32(min(2, max(0, (d["resolutionTier"] as? Int) ?? 0)))
        return p
    }

    private func persistPrefsIfChanged(_ p: GXPanelPrefs) {
        let d: [String: Any] = ["healthBars": p.healthBars, "unitRings": p.unitRings, "boardFrame": p.boardFrame, "leftHanded": p.leftHanded,
                                "volumeShadows": p.volumeShadows, "atlasStereo": p.atlasStereo, "elideWorldCopy": p.elideWorldCopy,
                                "resolutionTier": Int(p.resolutionTier)]
        if let old = UserDefaults.standard.dictionary(forKey: Self.prefsKey), NSDictionary(dictionary: old).isEqual(to: d) { return }
        UserDefaults.standard.set(d, forKey: Self.prefsKey)
    }

    // MARK: scripted replay (-uiScript)

    private func runScriptOnce() {
        guard !scriptRan, !UILaunchOptions.script.isEmpty else { return }
        scriptRan = true
        let steps = UILaunchOptions.script
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(2))
            for step in steps {
                Self.run(step, on: self)
                try? await Task.sleep(for: .milliseconds(400))
            }
        }
    }

    private static func run(_ step: String, on store: PanelStore) {
        guard let head = step.first else { return }
        let rest = String(step.dropFirst())
        switch head {
        case "c": if let id = Int(rest) { store.perform(page: Int(GX_PANEL_COMMANDS), id: id) }
        case "m":
            let parts = rest.split(separator: ".").compactMap { Int($0) }
            if parts.count == 2 { store.perform(page: parts[0], id: parts[1]) }
        case "x":
            let parts = rest.split(separator: ".").compactMap { Int($0) }
            if parts.count == 2 { store.performExtra(parts[0], value: parts[1]) }
        default: uiLog.error("unknown -uiScript step \(step, privacy: .public)")
        }
    }
}
