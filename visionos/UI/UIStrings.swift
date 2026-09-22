import Foundation

/// UI language of the native windows. The same value is handed to the C++ panel model (labels come back translated with the
/// Quest XrStrings table) and to the engine (`GXEnginePanelState_SetLanguage`), so the windows, the status texts of the
/// engine and the game text all follow one switch. 0 = German, 1 = English (XrLanguage order).
enum UILang: Int32, CaseIterable, Identifiable {
    case german = 0
    case english = 1

    var id: Int32 { rawValue }

    static var system: UILang {
        Locale.current.language.languageCode?.identifier == "de" ? .german : .english
    }

    /// The name of the language in that language (a language picker shows each in its own tongue).
    var nativeName: String { self == .german ? "Deutsch" : "English" }
}

/// One table for every string the SwiftUI windows show that is not produced by the C++ model. English is the key (it reads
/// well at the call site and doubles as the fallback), German is the translation. Strings that come from the Quest table
/// (control labels, status texts) are translated by `GXPanelModel_Translate`, so both worlds share `kXrTranslations`.
/// `scripts/qa/vision-ui-strings-check.sh` fails when a `t("...")` call site has no entry here.
enum L10n {
    static func t(_ key: String, _ lang: UILang) -> String {
        lang == .german ? (german[key] ?? key) : key
    }

    /// A Quest XR string (German source) in `lang`, straight from the C++ table.
    static func xr(_ german: String, _ lang: UILang) -> String {
        guard let out = GXPanelModel_Translate(german, lang.rawValue) else { return german }
        return String(cString: out)
    }

    /// English key -> German.
    static let german: [String: String] = [
        // Windows and scenes
        "Commands": "Befehle",
        "Settings": "Einstellungen",
        "Help": "Hilfe",
        "Menu": "Menü",
        "Pause": "Pause",
        "Recenter": "Neu ausrichten",
        "Ground View": "Bodenansicht",
        "Leave Tabletop": "Tabletop verlassen",
        "Enter Tabletop": "Tabletop betreten",
        "Open Commands": "Befehle öffnen",
        "Open Launcher": "Startfenster öffnen",
        "Open Help": "Hilfe öffnen",
        "Open Settings": "Einstellungen öffnen",
        "Done": "Fertig",
        "Close": "Schließen",
        "On": "An",
        "Off": "Aus",
        "Yes": "Ja",
        "No": "Nein",
        "Cancel": "Abbrechen",
        "Language": "Sprache",
        // Commands window
        "No match is running": "Es läuft keine Partie",
        "The console lights up once a match is running. Start a skirmish or a mission from the game menu.": "Die Konsole wird aktiv, sobald eine Partie läuft. Starte ein Gefecht oder eine Mission im Spielmenü.",
        "Scripted demo state (no engine)": "Skriptierter Demozustand (keine Engine)",
        "Why some controls are disabled": "Warum manche Schaltflächen gesperrt sind",
        "Tap a number to recall a group. To create one: select units, tap Replace group or New / Extend, then a number.": "Zahl antippen ruft die Gruppe auf. Zum Anlegen: Einheiten wählen, Gruppe ersetzen oder Neu / Erweitern antippen, dann eine Zahl.",
        "members": "Mitglieder",
        "empty": "leer",
        "current group": "aktuelle Gruppe",
        "armed": "aktiv",
        "pending": "wartet auf Zahl",
        "unavailable": "nicht verfügbar",
        "saved": "gespeichert",
        "not saved": "nicht gespeichert",
        "Commands help": "Befehle: Hilfe",
        "Selection and orders": "Auswahl und Befehle",
        "Select your units with a pinch on the table. Pick an order here, then pinch the target on the table. Stop and Scatter act at once.": "Wähle Einheiten mit einem Pinch auf dem Tisch. Wähle hier einen Befehl und pinche dann das Ziel auf dem Tisch. Stopp und Auseinanderlaufen wirken sofort.",
        "Groups": "Gruppen",
        "Replace group: select units, tap it, then a number. New / Extend adds the selected units to that number. A number alone recalls the group. Center view only moves the camera.": "Gruppe ersetzen: Einheiten wählen, antippen, dann eine Zahl. Neu / Erweitern fügt die gewählten Einheiten zur Zahl hinzu. Eine Zahl allein ruft die Gruppe auf. Zentrieren bewegt nur die Kamera.",
        "Waypoints": "Wegpunkte",
        "Turn Waypoints on, pinch the destinations in order, turn it off again. Cancel or Stop ends the mode.": "Wegpunkte einschalten, Ziele der Reihe nach pinchen, wieder ausschalten. Abbrechen oder Stopp beendet den Modus.",
        "Tactics and map views": "Taktik und Kartenplätze",
        "Formation keeps at least two selected units together. Save view stores the camera in A to D for this match, A to D alone recalls it.": "Formation hält mindestens zwei gewählte Einheiten zusammen. Ansicht merken speichert die Kamera in A bis D für diese Partie, A bis D allein ruft sie auf.",
        "Back to commands": "Zurück zu den Befehlen",
        // Settings pages
        "Workspace": "Arbeitsplatz",
        "Presentation": "Darstellung",
        "Graphics": "Grafik",
        "Audio": "Audio",
        "Controls": "Steuerung",
        "Controls & language": "Steuerung & Sprache",
        "Data": "Daten",
        "Diagnostics": "Diagnose",
        "Board": "Spielbrett",
        "Bring the board and the panels back in front of you.": "Bringt Spielbrett und Fenster wieder vor dich.",
        "Recenter board": "Brett neu ausrichten",
        "Reset workspace": "Arbeitsplatz zurücksetzen",
        "Recenter puts the board 0.9 m ahead of you and keeps its size. Reset also restores the default size and map zoom.": "Neu ausrichten setzt das Brett 0,9 m vor dich und behält die Größe. Zurücksetzen stellt auch Standardgröße und Kartenzoom wieder her.",
        "Move the board with a pinch-drag on the grab bar at its near edge. Use both hands to move, turn and scale it at once. Pan the map with a pinch-drag on the rim.": "Verschiebe das Brett mit Pinch und Ziehen an der Griffleiste am vorderen Rand. Mit beiden Händen bewegst, drehst und skalierst du es zugleich. Die Karte verschiebst du mit Pinch und Ziehen am Rand.",
        "Board size, distance and height presets": "Voreinstellungen für Größe, Abstand und Höhe",
        "Not available yet: the interaction layer only exposes Recenter and Reset. Resize with the two-hand pinch on the grab bar.": "Noch nicht verfügbar: Die Interaktionsschicht bietet nur Neu ausrichten und Zurücksetzen. Größe per Zwei-Hand-Pinch an der Griffleiste ändern.",
        "Screen": "Bildschirm",
        "Matches always play on the tabletop. Menus, briefings and videos use the upright screen above the far edge of the board.": "Partien laufen immer auf dem Tisch. Menüs, Briefings und Videos nutzen den aufrechten Bildschirm über dem hinteren Rand des Bretts.",
        "Selection indicators": "Auswahlanzeigen",
        "Health bars, unit rings and the board frame are drawn by the engine; these switches are stored for the presentation layer.": "Lebensbalken, Einheitenringe und Brettrahmen zeichnet die Engine; diese Schalter werden für die Darstellungsschicht gespeichert.",
        "Camera": "Kamera",
        "Camera presets": "Kamera-Voreinstellungen",
        "Classic": "Klassisch",
        "Table 78°": "Tisch 78°",
        "Oblique 65°": "Schräg 65°",
        "Top 85°": "Oben 85°",
        "Favorite": "Favorit",
        "Save current view as favorite": "Aktuelle Ansicht als Favorit speichern",
        "Home base view": "Ansicht der Basis",
        "The camera cannot be changed right now (menu, cutscene or dialog).": "Die Kamera lässt sich gerade nicht ändern (Menü, Zwischensequenz oder Dialog).",
        "Ground View is available in offline matches while the world can be adjusted.": "Die Bodenansicht ist in Offline-Partien verfügbar, solange die Welt verändert werden darf.",
        "Enter Ground View": "Bodenansicht betreten",
        "Leave Ground View": "Bodenansicht verlassen",
        "Then pinch visible open ground to stand there. Leave returns to the unchanged table.": "Dann sichtbaren freien Boden pinchen, um dort zu stehen. Verlassen bringt dich zum unveränderten Tisch zurück.",
        "Render scale": "Renderauflösung",
        "Render frame rate cap": "Bildratenbegrenzung",
        "Shadows": "Schatten",
        "Eye size": "Augenauflösung",
        "Uncapped": "Unbegrenzt",
        "Off (fastest)": "Aus (am schnellsten)",
        "Decals": "Decals",
        "Volumes (original)": "Volumen (Original)",
        "Balanced": "Ausgewogen",
        "High": "Hoch",
        "Ultra": "Ultra",
        "UI resolution": "UI-Auflösung",
        "720p": "720p",
        "1080p": "1080p",
        "Read once at engine start. Takes effect the next time the tabletop is entered.": "Wird einmal beim Engine-Start gelesen. Gilt ab dem nächsten Betreten des Tabletops.",
        "Comfort fade": "Komfort-Abblendung",
        "A brief dark fade during Ground View enter, exit and teleport.": "Eine kurze Abdunkelung beim Betreten, Verlassen und Teleportieren in der Bodenansicht.",
        "Focus marker": "Fokusmarkierung",
        "A pointer dot on panels and highlights on the grab bar while pinching.": "Ein Zeigerpunkt auf Panels und Hervorhebungen am Griffbalken beim Zwicken.",
        "Applied on the engine thread. Changes to the render scale, eye size and UI resolution take effect when the tabletop is entered again.": "Wird auf dem Engine-Thread angewendet. Änderungen an Renderauflösung, Augenauflösung und UI-Auflösung gelten nach erneutem Betreten des Tabletops.",
        "Master volume": "Gesamtlautstärke",
        "Music": "Musik",
        "Speech": "Sprache",
        "Interface sounds": "Oberflächenklänge",
        "Battlefield effects": "Schlachtfeldeffekte",
        "Spatial battlefield audio": "Räumliches Schlachtfeld-Audio",
        "On: sounds come from where the units are on the table and follow your head. Off: the original camera-relative mix.": "An: Klänge kommen von dort, wo die Einheiten auf dem Tisch stehen, und folgen deinem Kopf. Aus: die originale kamerarelative Mischung.",
        "Additive selection": "Auswahl erweitern",
        "Every pinch adds to or removes from the selection (Shift in the simulator).": "Jeder Pinch fügt zur Auswahl hinzu oder entfernt daraus (Shift im Simulator).",
        "Cancel targeting": "Zielwahl abbrechen",
        "Cancel building": "Bauen abbrechen",
        "Rotate building": "Gebäude drehen",
        "Rotate left 15°": "15° nach links",
        "Rotate right 15°": "15° nach rechts",
        "Rotates the building preview while a placement is pending.": "Dreht die Bauvorschau, solange eine Platzierung offen ist.",
        "Game text language": "Spieltext-Sprache",
        "Switches this app at once. The game's own text is staged and needs a restart of the app.": "Schaltet diese App sofort um. Der Spieltext wird vorbereitet und braucht einen Neustart der App.",
        "Game data": "Spieldaten",
        "Ready": "Bereit",
        "Not ready": "Nicht bereit",
        "Choose game folder…": "Spieleordner wählen…",
        "Remove imported data": "Importierte Daten entfernen",
        "Removes only the copy inside this app. Your original files stay untouched.": "Entfernt nur die Kopie in dieser App. Deine Originaldateien bleiben unberührt.",
        "Keep it": "Behalten",
        "Remove the imported game data?": "Importierte Spieldaten entfernen?",
        "Documentation": "Dokumentation",
        "Open the documentation": "Dokumentation öffnen",
        "The folder picker lives in the launcher window.": "Die Ordnerauswahl befindet sich im Launcher-Fenster.",
        "Open the launcher": "Launcher öffnen",
        "Engine": "Engine",
        "Phase": "Phase",
        "Engine frames per second": "Engine-Bilder pro Sekunde",
        "Simulation rate": "Simulationsrate",
        "Frames produced": "Erzeugte Bilder",
        "Frames skipped": "Übersprungene Bilder",
        "Last frame": "Letztes Bild",
        "Longest frame": "Längstes Bild",
        "Render targets": "Renderziele",
        "slots in use": "Slots belegt",
        "Synchronisation": "Synchronisation",
        "Renderer": "Renderer",
        "Engine log": "Engine-Log",
        "Share the log": "Log teilen",
        "The log lists file paths of your game data and technical details of your device. Read it before you share it.": "Das Log enthält Dateipfade deiner Spieldaten und technische Angaben zu deinem Gerät. Lies es, bevor du es teilst.",
        "No log yet": "Noch kein Log",
        "Refresh": "Aktualisieren",
        "n/a": "n. v.",
        "Idle": "Leerlauf",
        "Booting": "Startet",
        "Running": "Läuft",
        "Paused": "Pausiert",
        "Failed": "Fehlgeschlagen",
        "Stopping": "Beendet",
        // Help window
        "Controls guide": "Steuerungsanleitung",
        "How to play": "So spielst du",
        "Look at what you want, then pinch. There is no cursor: the system knows where you look only at the moment you pinch.": "Schau auf das Ziel und pinche. Es gibt keinen Zeiger: Das System kennt deinen Blick nur im Moment des Pinchens.",
        "Look and pinch (tap)": "Ansehen und pinchen (Tippen)",
        "Look and pinch, then drag": "Ansehen, pinchen und ziehen",
        "Both hands": "Beide Hände",
        "Placement": "Platzierung",
        "Panels": "Fenster",
        "Simulator": "Simulator",
        "Select a unit. With units selected: the engine's contextual command (move, attack, capture, enter, guard, gather).": "Wählt eine Einheit. Mit gewählten Einheiten: der Kontextbefehl der Engine (Bewegen, Angreifen, Erobern, Einsteigen, Bewachen, Sammeln).",
        "On empty terrain: selection box. On the rim: pan the map. On the grab bar: move the board.": "Auf freiem Gelände: Auswahlrahmen. Am Rand: Karte verschieben. An der Griffleiste: Brett bewegen.",
        "Pinch with both hands and hold: rotate, zoom and pan the map. On the grab bar: move, turn and scale the board.": "Mit beiden Händen pinchen und halten: Karte drehen, zoomen und verschieben. An der Griffleiste: Brett bewegen, drehen und skalieren.",
        "Additive selection: use the switch in Settings, or tap with the other hand while the first hand pinches. Tap a selected unit to deselect it.": "Auswahl erweitern: Schalter in den Einstellungen, oder mit der anderen Hand tippen, während die erste pinchet. Eine gewählte Einheit antippen entfernt sie.",
        "With a building chosen: pinch the ground to place the preview, drag to move it, twist your wrist to rotate it, release to build. Cancel: tap with the other hand, or use Cancel building.": "Mit gewähltem Gebäude: Boden pinchen, um die Vorschau zu setzen, ziehen zum Verschieben, Handgelenk drehen zum Drehen, loslassen zum Bauen. Abbrechen: mit der anderen Hand tippen oder Bauen abbrechen.",
        "Ground View (offline matches): arm it from the toolbar or Settings, then pinch visible open ground to stand there. Hold a still pinch for 1.2 s, or use Leave Ground View, to return to the table.": "Bodenansicht (Offline-Partien): in der Leiste oder den Einstellungen einschalten, dann sichtbaren freien Boden pinchen. Zum Zurückkehren 1,2 s still pinchen halten oder Bodenansicht verlassen wählen.",
        "The engine's own panels (build menu, production queue, powers, radar, unit info) are textured panels near the board: look and pinch them like buttons.": "Die Engine-eigenen Fenster (Baumenü, Produktionsliste, Kräfte, Radar, Einheiteninfo) sind Texturfenster am Brett: ansehen und pinchen wie Schaltflächen.",
        "The Commands window holds orders, groups, waypoints, formations and map views. Settings holds workspace, graphics, audio and language.": "Das Befehlefenster enthält Befehle, Gruppen, Wegpunkte, Formationen und Kartenplätze. Die Einstellungen enthalten Arbeitsplatz, Grafik, Audio und Sprache.",
        "Mouse click = pinch at the pointer. Shift = additive selection. Option = emulate the second hand. Escape = back in the engine.": "Mausklick = Pinch am Zeiger. Shift = Auswahl erweitern. Option = zweite Hand nachahmen. Escape = zurück in der Engine.",
        "What only a headset shows": "Was nur ein Headset zeigt",
        "Hand tracking, real gaze targets and the depth impression only exist on a device. The simulator shows the windows and the layout.": "Handtracking, echte Blickziele und der Tiefeneindruck gibt es nur auf einem Gerät. Der Simulator zeigt Fenster und Layout.",
        // Hover / target info
        "Last target": "Letztes Ziel",
        "Shows the last unit or building you pinched or pointed at. The system does not share where you look between pinches.": "Zeigt die zuletzt gepinchte oder angezeigte Einheit oder das Gebäude. Zwischen zwei Pinches gibt das System nicht weiter, wohin du schaust.",
        "Panel tooltip": "Fenster-Hinweis",
        // Ornament
        "Toolbar": "Leiste",
        "Return to the launcher": "Zurück zum Launcher",
        "Send Back to the engine (opens the game menu in a match)": "Zurück an die Engine (öffnet in einer Partie das Spielmenü)",
        "Windows": "Fenster",
    ]
}
