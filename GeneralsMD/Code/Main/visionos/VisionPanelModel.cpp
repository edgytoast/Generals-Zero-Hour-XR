// visionOS spatial UI: the control model of the Quest consoles. See VisionPanelModel.h.
//
// Every label / state below is a line of XrMenuPainting.h updateMenuTextures() (the Quest reference is in the comment of
// each block, file lines of the commit this port started from). The only deliberate differences are listed in
// docs/visionos-ui.md section 3.3:
//   1. controls that the Quest would silently ignore (the engine rejects them: no controllable selection, no adjustable
//      world) are marked DISABLED with the reason as explanation, instead of accepting the press;
//   2. the Guard order (id 4) is disabled with TacticalReason(8) like the formation buttons (the engine ignores it then);
//   3. window-placement values (sizes in metres, map zoom) are host state that the Quest reads from XrHello; the chips
//      of the 'Windows' page are therefore left empty here (the SwiftUI workspace page shows its own).
#include "VisionPanelModel.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "XrCommands.h" // xrCommandAction
#include "XrPanelLayout.h"
#include "XrStrings.h"

namespace {

// Strings the SwiftUI windows show that the Quest table does not have (native UI additions). German is the key, like
// the Quest table, so one lookup serves both.
const XrTranslation kExtraTranslations[] = {
	{"Im Dialog oder bei gesperrter Kamera nicht verfügbar", "Unavailable in a dialog or while camera is locked"},
	{"Keine Auswahl", "No selection"},
	{"Beispieleinheit", "Sample unit"},
};

const char *lookup(const char *german, int language)
{
	if (language != 1) return german;
	for (const auto &entry : kXrTranslations)
		if (!std::strcmp(german, entry.de)) return entry.en;
	for (const auto &entry : kExtraTranslations)
		if (!std::strcmp(german, entry.de)) return entry.en;
	return german;
}

void put(char *dst, size_t capacity, const std::string &text)
{
	if (capacity == 0) return;
	std::strncpy(dst, text.c_str(), capacity - 1);
	dst[capacity - 1] = '\0';
	const size_t n = std::strlen(dst);
	if (n == 0) return;
	size_t lead = n - 1;
	while (lead > 0 && (static_cast<unsigned char>(dst[lead]) & 0xC0) == 0x80) --lead;
	const unsigned char b = static_cast<unsigned char>(dst[lead]);
	const size_t need = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
	if (lead + need > n) dst[lead] = '\0';
}

struct Entry {
	std::string label, value, explain;
	int badge = -1;
	int state = 0;
	bool labelled = false;
};

class Builder {
public:
	explicit Builder(int language) : lang_(language == 1 ? 1 : 0) {}

	std::string tr(const char *german) const { return lookup(german, lang_); }
	int lang() const { return lang_; }

	void bind(XrPanelControl *table, int count)
	{
		table_ = table;
		count_ = count;
		entries_.assign(size_t(count), Entry());
	}
	int indexOf(int id) const
	{
		for (int i = 0; i < count_; ++i)
			if (table_[i].id == id) return i;
		return -1;
	}
	void label(int id, const std::string &text, const std::string &value = std::string())
	{
		const int i = indexOf(id);
		if (i < 0) return;
		entries_[size_t(i)].label = text;
		entries_[size_t(i)].value = value;
		entries_[size_t(i)].labelled = true;
	}
	void mark(int id, int bits)
	{
		const int i = indexOf(id);
		if (i >= 0) entries_[size_t(i)].state |= bits;
	}
	void badge(int id, int value)
	{
		const int i = indexOf(id);
		if (i >= 0) entries_[size_t(i)].badge = value;
	}
	void explain(int id, const std::string &text)
	{
		const int i = indexOf(id);
		if (i >= 0) entries_[size_t(i)].explain = text;
	}
	bool disabled(int id) const
	{
		const int i = indexOf(id);
		return i >= 0 && (entries_[size_t(i)].state & GX_STATE_DISABLED) != 0;
	}

	int emit(GXPanelView &view, GXPanelControl *controls, int maxControls)
	{
		view.sectionCount = 0;
		int out = 0, section = -1;
		for (int i = 0; i < count_; ++i) {
			const XrPanelControl &c = table_[i];
			const Entry &e = entries_[size_t(i)];
			if (c.role == kXrRoleSection) {
				if (view.sectionCount < GX_PANEL_MAX_SECTIONS && e.labelled) {
					section = view.sectionCount++;
					view.sections[section].id = c.id;
					put(view.sections[section].title, sizeof(view.sections[section].title), e.label);
				}
				continue;
			}
			if (c.role == kXrRoleContext || c.role == kXrRoleBody) continue; // the card / help text: view.status / view.hint
			if (!e.labelled || out >= maxControls) continue;
			GXPanelControl &g = controls[out++];
			std::memset(&g, 0, sizeof(g));
			g.id = c.id;
			g.section = c.role == kXrRoleTab || c.role == kXrRoleClose ? -1 : section;
			g.role = c.role;
			g.state = e.state;
			g.badge = e.badge;
			put(g.label, sizeof(g.label), e.label);
			put(g.value, sizeof(g.value), e.value);
			put(g.explain, sizeof(g.explain), e.explain);
		}
		return out;
	}

private:
	int lang_;
	XrPanelControl *table_ = nullptr;
	int count_ = 0;
	std::vector<Entry> entries_;
};

void toggleLabel(Builder &b, int id, const char *name, bool on)
{
	b.label(id, b.tr(name), b.tr(on ? "AN" : "AUS"));
	if (on) b.mark(id, GX_STATE_ON);
}

const char *kNotAdjustable = "Im Dialog oder bei gesperrter Kamera nicht verfügbar";

// XrMenuPainting.h:118-165 (the 'Commands' console).
void buildCommands(Builder &b, const GXPanelSnapshot &s, GXPanelView &view)
{
	const GXPanelSession &ss = s.session;
	const bool help = ss.help, tactics = ss.tactics && !help;
	XrPanelControl table[64];
	const int count = xrCommandLayout(help, ss.tactics, table, 64);
	b.bind(table, count);
	b.label(33, "✕");
	if (help) { // :124-127
		put(view.title, sizeof(view.title), b.tr("Befehle") + " · " + std::to_string(ss.helpPage + 1) + "/4");
		b.label(34, b.tr("Zurück zu Befehlen"));
		b.label(36, b.tr("Weiter"));
	} else {
		put(view.title, sizeof(view.title), b.tr("Befehle")); // :129
		const char *operations[] = {"Zahl wählt Gruppe; Aktion → Zahl führt sie aus", "Gruppe ersetzen: jetzt Zahl wählen",
			"Neu / Erweitern: jetzt Zahl wählen", "Zentrieren: Zahl wählen"};
		const int op = ss.groupOperation >= 0 && ss.groupOperation < 4 ? ss.groupOperation : 0;
		const std::string hint = ss.bookmarkSave ? b.tr("Ansicht merken: jetzt A–D wählen") : std::string(s.hint); // :131
		put(view.status, sizeof(view.status), s.status);                                                           // :132
		put(view.hint, sizeof(view.hint), (ss.groupOperation || hint.empty()) ? b.tr(operations[op]) : hint);
		b.label(-10, b.tr("Auftrag · danach Ziel wählen"));
		b.label(1, b.tr("Bewegen"));
		b.label(2, b.tr("Angriffsmarsch"));
		b.label(3, b.tr("Zwangsangriff"));
		b.label(4, b.tr("Position bewachen"));
		b.label(8, b.tr("Wegpunkte"), b.tr(s.queue ? "AN" : "AUS"));
		b.label(0, b.tr("Auswahl / Befehle"));
		b.label(-11, b.tr("Sofort & Auswahl"));
		b.label(5, b.tr("STOPP"));
		b.label(6, b.tr("Auseinanderlaufen"));
		b.label(15, b.tr("Abbrechen / Abwahl"));
		b.label(7, b.tr("Freier Bauarbeiter"));
		b.label(13, b.tr("Nächste Einheit"));
		b.label(14, b.tr("Nächster Bauarbeiter"));
		b.label(11, b.tr("Held auswählen"));
		b.label(12, b.tr("Alle Flugzeuge"));
		b.label(9, b.tr("Gleicher Typ: Karte"));
		b.label(10, b.tr("Alle Einheiten"));
		b.label(35, b.tr("Communicator"));
		b.label(-12, b.tr("Gruppen · Aktion wählen → Zahl"));
		for (int i = 0; i < 10; ++i) {
			b.label(20 + i, std::to_string(i + 1));
			b.badge(20 + i, s.groupSize[i]);
		}
		b.label(30, b.tr("Gruppe ersetzen"));
		b.label(31, b.tr("Neu / Erweitern"));
		b.label(32, b.tr("Zentrieren"));
		b.label(37, b.tr(ss.tactics ? "Taktik −" : "Taktik +"));
		b.label(34, b.tr("Hilfe"));
		if (tactics) {
			b.label(-13, b.tr("Taktik · erweitert"));
			b.label(40, b.tr(s.formationActive ? "Formation lösen" : "Formation bilden"));
			b.label(41, b.tr("Zwangsbewegung"));
			b.label(42, b.tr("Ohne Verfolgung"));
			b.label(43, b.tr("Ansicht merken"), b.tr(ss.bookmarkSave ? "AN" : "AUS"));
			b.label(-14, b.tr("KARTENPLÄTZE · merken → A–D"));
			for (int i = 0; i < 4; ++i) {
				b.label(44 + i, std::string(1, char('A' + i)), s.bookmarkKnown[i] ? "●" : "○");
				b.badge(44 + i, s.bookmarkKnown[i] ? 1 : 0);
			}
		}
		// :166-178 persistent states.
		for (int id = 1; id <= 4; ++id)
			if (xrCommandAction(id) == s.mode) b.mark(id, GX_STATE_ARMED);
		if (s.mode == 9) b.mark(41, GX_STATE_ARMED);
		if (s.mode == 10) b.mark(42, GX_STATE_ARMED);
		if (s.queue) b.mark(8, GX_STATE_ON);
		const char *hints[] = {"Formation: aktuelle Anordnung zusammenhalten. Erneut klicken löst sie auf.",
			"Zwangsbewegung: danach Boden anklicken. Ersetzt den Auftrag; keine Wegpunktfolge.",
			"Ohne Verfolgung: danach Boden anklicken. Bewacht dort ohne Gegner zu verfolgen."};
		for (int id = 40; id <= 42; ++id) {
			if (s.reason[id - 39][0] != '\0') { // difference 1: the disabled reason is the explanation
				b.mark(id, GX_STATE_DISABLED);
				b.explain(id, s.reason[id - 39]);
			} else b.explain(id, b.tr(hints[id - 40]));
		}
		if (s.reason[0][0] != '\0') { // difference 2: Guard is ignored by the engine without a controllable selection
			b.mark(4, GX_STATE_DISABLED);
			b.explain(4, s.reason[0]);
		} else b.explain(4, b.tr("Bewachen: danach Boden oder anderes verbündetes Objekt anklicken. Bewegliche Ziele werden begleitet."));
		if (s.formationActive) b.mark(40, GX_STATE_ON);
		if (ss.bookmarkSave) b.mark(43, GX_STATE_ON);
		for (int i = 0; i < 10; ++i)
			if (s.group == i) b.mark(20 + i, GX_STATE_ON);
		if (ss.groupOperation) b.mark(29 + ss.groupOperation, GX_STATE_PENDING);
		// :cmdExplanation, hit 43..47 / 20..29 / 37
		const std::string bookmarkText = b.tr(ss.bookmarkSave ?
			"Ansicht merken: A–D wählen. Überschreibt diesen Kartenplatz, nur für diese Partie." :
			"A–D ruft die Kamera auf. Erst Ansicht merken → A–D zum Speichern. Tisch bleibt stehen.");
		for (int id = 43; id <= 47; ++id) b.explain(id, bookmarkText);
		for (int id = 20; id < 30; ++id) b.explain(id, b.tr("Zahl ruft die Gruppe auf. Zum Anlegen: Einheiten markieren → Speichern → Zahl."));
		b.explain(37, b.tr("Taktik aufklappen: Formation, Zwangsbewegung, Bewachen ohne Verfolgung und Kartenplätze."));
		// Quest gating made visible: applyCommandAction returns before the engine call while the world cannot be adjusted.
		if (!s.canAdjustWorld) {
			const int gated[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
				35, 40, 41, 42, 43, 44, 45, 46, 47};
			for (int id : gated) {
				b.mark(id, GX_STATE_DISABLED);
				b.explain(id, b.tr(kNotAdjustable));
			}
		}
	}
}

const char *kMenuTabs[] = {"Fenster", "Einheiten", "Gruppen", "Ansicht"};

// XrMenuPainting.h:226-330 (the 'Menu' console).
void buildMenu(Builder &b, int page, const GXPanelSnapshot &s, GXPanelView &view)
{
	const GXPanelSession &ss = s.session;
	XrPanelControl table[80];
	const int count = xrMenuLayout(page, table, 80);
	b.bind(table, count);
	if (page == GX_PANEL_MENU_HELP) { // :246-251
		put(view.title, sizeof(view.title), b.tr("Controller-Anleitung") + " · " + std::to_string(ss.menuHelpPage + 1) + "/5");
		b.label(33, "✕");
		b.label(34, b.tr("Zurück zu Fenstern"));
		b.label(36, b.tr("Weiter"));
		return;
	}
	for (int i = 0; i < 4; ++i) b.label(20 + i, b.tr(kMenuTabs[i])); // :226-228
	b.mark(20 + page, GX_STATE_ACTIVE);
	b.label(24, "?");
	put(view.title, sizeof(view.title), b.tr(kMenuTabs[page < 0 || page > 3 ? 0 : page]));
	const bool worldAdjustable = s.canAdjustWorld;
	if (page == GX_PANEL_MENU_WINDOWS) { // :253-274 (chips are host state, see the file comment)
		b.label(0, b.tr(s.splitVisible ? "Tisch" : "Bildschirm"));
		b.label(1, b.tr("Baufenster"));
		b.label(-10, b.tr("Ziel"));
		b.label(-11, b.tr("Größe & Abstand"));
		b.label(2, b.tr("Kleiner"));
		b.label(3, b.tr("Größer"));
		b.label(4, b.tr("Näher"));
		b.label(5, b.tr("Weiter weg"));
		b.label(-12, b.tr("Lage"));
		b.label(6, b.tr("Höher"));
		b.label(7, b.tr("Tiefer"));
		b.label(8, b.tr("Flacher"));
		b.label(9, b.tr("Steiler"));
		b.label(10, b.tr("Links drehen"));
		b.label(11, b.tr("Rechts drehen"));
		b.label(-13, b.tr("Karte"));
		b.label(12, b.tr("Mehr Karte"));
		b.label(13, b.tr("Weniger Karte"));
		b.label(-14, b.tr("Aktionen"));
		b.label(14, b.tr("Greifen / Anordnen"));
		b.label(15, b.tr("Position zurücksetzen"));
		b.label(16, b.tr("Spielplatz einrichten"));
		b.label(17, b.tr("Schließen"));
		b.label(18, b.tr("Alles vor mir ausrichten"));
		if (!worldAdjustable) {
			b.mark(12, GX_STATE_DISABLED);
			b.mark(13, GX_STATE_DISABLED);
		}
	} else if (page == GX_PANEL_MENU_UNITS) { // :275-291
		put(view.status, sizeof(view.status), s.status);
		b.label(-10, b.tr("Auftrag · danach Ziel wählen"));
		b.label(5, b.tr("Bewegen"));
		b.label(6, b.tr("Angriffsmarsch"));
		b.label(7, b.tr("Zwangsangriff"));
		b.label(8, b.tr("Position bewachen"));
		b.label(9, b.tr("Wegpunkte an / aus"));
		b.label(0, b.tr("Kontextbefehl"));
		b.label(-11, b.tr("Auswahl"));
		b.label(1, b.tr("Einheit wählen"));
		b.label(2, b.tr("Auswahl +/-"));
		b.label(3, b.tr("Bereich: zwei Ecken"));
		b.label(4, b.tr("Bereich hinzufügen"));
		b.label(16, b.tr("Alle Einheiten"));
		b.label(12, b.tr("Freier Bauarbeiter"));
		b.label(-12, b.tr("Sofort"));
		b.label(10, b.tr("STOPP"));
		b.label(11, b.tr("Auseinanderlaufen"));
		b.label(13, b.tr("Abbrechen / Abwahl"));
		char line[128];
		std::snprintf(line, sizeof(line), b.tr("Direkter im Spiel: Befehle-Konsole (%s)").c_str(), ss.prefs.leftHanded ? "X" : "A");
		b.label(-20, line);
		b.label(-13, b.tr("Navigation"));
		b.label(14, b.tr("Gruppen verwalten"));
		b.label(15, b.tr("Zurück zum Spiel"));
		b.label(17, b.tr("Schließen"));
		for (int id = 0; id <= 9; ++id) // applyMenuAction :78-81 gate
			if (!s.splitVisible || !worldAdjustable) b.mark(id, GX_STATE_DISABLED);
		if (!worldAdjustable) {
			for (int id : {10, 11, 12, 13, 16}) b.mark(id, GX_STATE_DISABLED);
		}
		if (s.queue) b.mark(9, GX_STATE_ON);
		// Addition: the armed order (XrOrderMode 1..8 equals the control id of this page) is shown as armed.
		if (s.mode >= 1 && s.mode <= 8) b.mark(s.mode, GX_STATE_ARMED);
	} else if (page == GX_PANEL_MENU_GROUPS) { // :292-305
		put(view.status, sizeof(view.status), s.status);
		b.label(-10, b.tr("Gruppen"));
		b.label(0, b.tr("Gruppe vorher"));
		b.label(1, b.tr("Gruppe weiter"));
		b.label(2, b.tr("Auswahl speichern"));
		b.label(3, b.tr("Gruppe auswählen"));
		b.label(4, b.tr("Gruppe zur Auswahl"));
		b.label(5, b.tr("Zur Gruppe schauen"));
		b.label(-11, b.tr("Auswahl"));
		b.label(6, b.tr("Nächste Einheit"));
		b.label(7, b.tr("Nächster Bauarbeiter"));
		b.label(8, b.tr("Held auswählen"));
		b.label(9, b.tr("Alle Flugzeuge"));
		b.label(10, b.tr("Gleicher Typ: Karte"));
		b.label(11, b.tr("Alle Einheiten"));
		b.label(12, b.tr("Freier Bauarbeiter"));
		b.label(13, b.tr("STOPP"));
		b.label(16, b.tr("Abbrechen / Abwahl"));
		b.label(-13, b.tr("Navigation"));
		b.label(14, b.tr("Einheitenbefehle"));
		b.label(15, b.tr("Fenster einstellen"));
		b.label(17, b.tr("Schließen"));
		if (!worldAdjustable) {
			for (int id : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16}) b.mark(id, GX_STATE_DISABLED);
		}
	} else { // GX_PANEL_MENU_VIEW :306-329
		std::string hint = b.tr(ss.prefs.measurement ? "Messung: Tabletop öffnen, kurz ruhig halten" : "Schatten A/B nur diese Sitzung; B ohne Volumen");
		put(view.status, sizeof(view.status), s.languageStatus);
		put(view.hint, sizeof(view.hint), hint);
		b.label(-10, b.tr("Darstellung"));
		b.label(-20, b.tr("Spiel: Tisch; Bodenansicht optional"));
		b.label(-21, b.tr("Videos: Bildschirm"));
		toggleLabel(b, 4, "Lebenspunkte", ss.prefs.healthBars);
		toggleLabel(b, 5, "Einheitenringe", ss.prefs.unitRings);
		toggleLabel(b, 6, "Brettkörper", ss.prefs.boardFrame);
		b.label(-11, b.tr("Grafik"));
		const int tier = ss.prefs.resolutionTier;
		b.label(10, b.tr("Auflösung"), b.tr(tier == 2 ? "Ultra+" : tier == 1 ? "Hoch" : "Ausgewogen"));
		if (tier) b.mark(10, GX_STATE_ON);
		b.label(12, b.tr("Schatten"), b.tr(ss.prefs.volumeShadows ? "Original" : "Leicht"));
		if (ss.prefs.volumeShadows) b.mark(12, GX_STATE_ON);
		b.label(14, b.tr("Stereo"), b.tr(ss.prefs.multiviewStereo ? "Multiview" : ss.prefs.atlasStereo ? "Kompakt" : "Referenz"));
		b.label(15, b.tr("Zusatzwelt"), b.tr(ss.prefs.elideWorldCopy ? "Auto" : "Immer"));
		toggleLabel(b, 13, "Messung", ss.prefs.measurement);
		b.label(-12, b.tr("Steuerung & Sprache"));
		toggleLabel(b, 8, "Linkshändig", ss.prefs.leftHanded);
		b.label(11, b.tr("Sprache"), b.tr(ss.prefs.language == 0 ? "Deutsch" : "English"));
		b.label(9, b.tr("Foto-Anordnung"));
		b.label(7, b.tr("Schließen"));
		b.label(16, b.tr("Bodenansicht · Ort wählen"));
		b.explain(16, b.tr("Nur Offline-Gefecht: Bodenansicht wählen, dann sichtbaren freien Boden anklicken. B/Y kehrt zurück."));
		if (s.languageStatus[0] != '\0') b.explain(11, s.languageStatus);
		if (!s.stereoVisible || !s.canObserveGround) b.mark(16, GX_STATE_DISABLED);
	}
}

} // namespace

const char *visionTr(const char *german, int language) { return lookup(german, language); }

int visionPanelBuild(int page, const GXPanelSnapshot &snapshot, int language, GXPanelView &view, GXPanelControl *controls,
	int maxControls)
{
	std::memset(&view, 0, sizeof(view));
	view.page = page;
	Builder b(language);
	if (page == GX_PANEL_COMMANDS) buildCommands(b, snapshot, view);
	else if (page >= GX_PANEL_MENU_WINDOWS && page <= GX_PANEL_MENU_HELP) buildMenu(b, page, snapshot, view);
	else return 0;
	return b.emit(view, controls, maxControls);
}

void visionPanelPrefsDefaults(GXPanelPrefs &p, int language)
{
	std::memset(&p, 0, sizeof(p));
	p.healthBars = p.unitRings = p.boardFrame = true; // XrLayout defaults
	p.elideWorldCopy = true;                          // XrPerformance default
	p.language = language == 1 ? 1 : 0;
}

void visionPanelSnapshotDefaults(GXPanelSnapshot &s)
{
	std::memset(&s, 0, sizeof(s));
	visionPanelPrefsDefaults(s.session.prefs, 0);
}

extern "C" {
int GXPanelModel_Build(int page, const GXPanelSnapshot *snapshot, int language, GXPanelView *view, GXPanelControl *controls,
	int maxControls)
{
	if (!snapshot || !view || !controls || maxControls <= 0) return 0;
	return visionPanelBuild(page, *snapshot, language, *view, controls, maxControls);
}
const char *GXPanelModel_Translate(const char *german, int language) { return german ? lookup(german, language) : ""; }
}
