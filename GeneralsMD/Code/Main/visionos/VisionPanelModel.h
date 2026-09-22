// visionOS spatial UI: the control model of the Quest 'Commands' and 'Menu' consoles.
//
// Builds, for one console page, the list of controls the native SwiftUI windows show: id, section, label (in the selected
// language), role, state bits (armed / pending / disabled / on / active) and count badges. It is driven by exactly the
// tables the Quest paints from:
//   * geometry / ids / roles : XrPanelLayout.h  xrCommandLayout(), xrMenuLayout()   (the single Quest control table)
//   * labels / states / chips: XrMenuPainting.h updateMenuTextures()                (ported label by label)
//   * hover explanations     : XrMenuPainting.h commandExplanation()
//   * strings                : XrStrings.h      kXrTranslations                     (German source, English translation)
// The pixel rectangles of the Quest table are ignored: SwiftUI lays the same controls out natively.
//
// Pure C++ (no Apple types, no engine symbols, no globals): input is a GXPanelSnapshot value, output plain structs.
// The C entry points GXPanelModel_Build / GXPanelModel_Translate are declared in GXEnginePanelState.h.
#pragma once

#include "GXEnginePanelState.h"

// German source string -> `language` (0 German returns the source). Unknown strings come back unchanged. Thread safe:
// reads the constant Quest table, never the mutable g_xrLanguage.
const char *visionTr(const char *german, int language);

// Same as GXPanelModel_Build; kept as a C++ name for tests.
int visionPanelBuild(int page, const GXPanelSnapshot &snapshot, int language, GXPanelView &view, GXPanelControl *controls,
	int maxControls);

// A default snapshot with sane values (no engine: everything false / empty, group 0, prefs at the Quest defaults).
void visionPanelSnapshotDefaults(GXPanelSnapshot &out);
// Quest defaults of the UI preferences (XrLayout / XrPerformance).
void visionPanelPrefsDefaults(GXPanelPrefs &out, int language);
