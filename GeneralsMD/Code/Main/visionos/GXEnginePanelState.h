/*
 * GXEnginePanelState.h - plain-C surface (Swift-importable) of the native SwiftUI panels of the visionOS port:
 * the thread-safe engine snapshot, the control model of the Quest 'Commands' / 'Menu' consoles, and the actions.
 *
 * What it is: the Quest host draws two consoles (XrPanelLayout.h control tables, painted by XrMenuPainting.h,
 * driven by XrCommandUI.h applyCommandAction / XrMenuUI.h applyMenuAction). The visionOS app shows the same
 * controls as native SwiftUI windows. This header is the seam between the two worlds:
 *
 *   engine thread  --(GXEngineHost frame hook, ~15 Hz)-->  GXPanelSnapshot  --(copy under a lock)-->  SwiftUI
 *   SwiftUI button --(GXPanelAction_Perform)--> GXEngineHost_Post --> engine thread: the Quest switch tables
 *
 * Nothing here decides a game rule: every action ends in the same XrGameBoot_* call the Quest host makes
 * (through package E's VisionEngineBridge for the calls it exports, and a small companion interface for the rest,
 * see VisionCommandActions.h). The model itself (VisionPanelModel.cpp / VisionCommandActions.cpp) is pure C++
 * and host-tested by scripts/qa/vision-ui-*-test.sh. docs/visionos-ui.md has the full description.
 *
 * Threading: every function here is thread safe. Languages are ints: 0 = German, 1 = English (XrLanguage order).
 * Strings are UTF-8, NUL terminated, always truncated to their buffer.
 */
#ifndef GX_ENGINE_PANEL_STATE_H
#define GX_ENGINE_PANEL_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pages -------------------------------------------------------------------------------------------------- */

/* Which control table a view is built from. 0..4 are the Quest 'Menu' pages (XrMenuUI.h menu.page), 10 is the
 * Quest 'Commands' console (its 'tactics' and 'help' variants follow GXPanelSession). */
enum {
    GX_PANEL_MENU_WINDOWS = 0,   /* workspace: target, size, distance, height, map coverage (host-side actions) */
    GX_PANEL_MENU_UNITS = 1,     /* orders, selection, instant, navigation */
    GX_PANEL_MENU_GROUPS = 2,    /* previous/next group, save/select/add/center, selection */
    GX_PANEL_MENU_VIEW = 3,      /* presentation, graphics, controls & language, Ground View */
    GX_PANEL_MENU_HELP = 4,      /* controller guide (Quest) */
    GX_PANEL_COMMANDS = 10       /* the Commands console */
};

/* XrPanelRole (XrPanelLayout.h), same numbers. */
enum {
    GX_ROLE_BUTTON = 0, GX_ROLE_TARGETED = 1, GX_ROLE_IMMEDIATE = 2, GX_ROLE_CLOSE = 3, GX_ROLE_SECTION = 4,
    GX_ROLE_CONTEXT = 5, GX_ROLE_GROUPNUM = 6, GX_ROLE_TAB = 7, GX_ROLE_INFO = 8, GX_ROLE_BODY = 9,
    GX_ROLE_CARD = 10, GX_ROLE_OP = 11, GX_ROLE_DANGER = 12
};

/* XrPanelState bits (XrPanelLayout.h), same numbers. HOVER is never set by the model (SwiftUI has its own hover). */
enum {
    GX_STATE_HOVER = 1, GX_STATE_SELECTED = 2, GX_STATE_ARMED = 4, GX_STATE_PENDING = 8,
    GX_STATE_DISABLED = 16, GX_STATE_ON = 32, GX_STATE_ACTIVE = 64
};

/* ---- the UI-side state of the consoles (Quest: XrCommandState + XrMenuState + XrLayout toggles) ------------------ */

typedef struct GXPanelPrefs {
    bool healthBars;        /* XrLayout.healthBars   (default on)  */
    bool unitRings;         /* XrLayout.unitRings    (default on)  */
    bool boardFrame;        /* XrLayout.boardFrame   (default on)  */
    bool leftHanded;        /* XrLayout.leftHanded   (Quest only; kept for the table, no visionOS effect) */
    int32_t resolutionTier; /* XrLayout.resolutionTier 0..2 (Balanced, High, Ultra+) */
    bool volumeShadows;     /* XrPerformance.volumeShadows */
    bool measurement;       /* XrPerformance.enabled */
    bool atlasStereo;       /* XrPerformance.atlasStereo */
    bool multiviewStereo;   /* XrPerformance.multiviewStereo (no multiview on visionOS: kept false there) */
    bool elideWorldCopy;    /* XrPerformance.elideWorldCopy */
    int32_t language;       /* 0 German, 1 English: the UI language, also what the engine strings are produced in */
} GXPanelPrefs;

typedef struct GXPanelSession {
    int32_t menuPage;       /* XrMenuState.page (0..4) */
    int32_t menuHelpPage;   /* XrMenuState.helpPage */
    int32_t groupOperation; /* XrCommandState.groupOperation: 0 none, 1 save, 2 add, 3 center (pending, waiting for a number) */
    bool help;              /* XrCommandState.help */
    int32_t helpPage;       /* XrCommandState.helpPage 0..3 */
    bool tactics;           /* XrCommandState.tactics (the foldout) */
    bool bookmarkSave;      /* XrCommandState.bookmarkSave ('Save view' armed, waiting for A-D) */
    GXPanelPrefs prefs;
} GXPanelSession;

/* ---- the engine snapshot ------------------------------------------------------------------------------------ */

enum { GX_ENGINE_KIND_NONE = 0, GX_ENGINE_KIND_REAL = 1, GX_ENGINE_KIND_SCRIPTED = 2 };

typedef struct GXPanelSnapshot {
    uint32_t version;        /* increments whenever anything below changes: cheap "did it change" test for the UI */
    int32_t engineKind;      /* GX_ENGINE_KIND_*: NONE = no engine state (fake engine / not booted) */
    /* Read-only engine state (VisionEngineBridge / XrGameBoot_*). */
    bool interactiveGame;    /* XrGameBoot_IsInteractiveGame */
    bool canAdjustWorld;     /* XrGameBoot_CanAdjustWorld: the console may act */
    bool canStereoWorld;     /* XrGameBoot_CanStereoWorld */
    bool canObserveGround;   /* XrGameBoot_CanObserveGround */
    bool expandedUI;         /* XrGameBoot_ExpandedUI: a native dialog is open */
    bool splitVisible;       /* presentation: tabletop world + panels visible (host fed; default: interactiveGame && canStereoWorld) */
    bool stereoVisible;      /* presentation: stereo world is showing (host fed; default: canStereoWorld) */
    int32_t mode;            /* XrGameBoot_TacticalState: XrOrderMode (0 context ... 10 guard-hold) */
    int32_t group;           /* current group slot 0..9 */
    bool queue;              /* waypoint queue active */
    int32_t groupSize[10];   /* XrGameBoot_GroupSize */
    bool formationActive;    /* XrGameBoot_FormationActive */
    bool bookmarkKnown[4];   /* XrGameBoot_BookmarkKnown (map views A-D) */
    int32_t matchResult;     /* XrGameBoot_MatchResult: 0 none, 1 victory, 2 defeat, 3 match over */
    int32_t defaultCameraPreset; /* XrGameBoot_DefaultCameraPreset */
    int32_t groundViewMode;  /* GXEnginePanelState_SetGroundView (0 off, 1 armed, 2 active) */
    /* XrGameBoot_TacticalReason for the actions the console can disable: [0] = 8 (guard), [1] = 40 (formation),
     * [2] = 41 (force move), [3] = 42 (guard without pursuit). Empty = allowed. */
    char reason[4][160];
    char status[320];        /* XrGameBoot_TacticalStatus */
    char hint[320];          /* XrGameBoot_TacticalHint */
    char languageStatus[160];/* XrGameBoot_LanguageStatus */
    /* Last pinched / hand-pointed target (XrGameBoot_WorldHoverInfo): there is no continuous gaze, so this is what the
     * engine reported the last time a spatial pointer was active, NOT what the user looks at now. */
    char worldHover[512];
    uint32_t worldHoverSeq;  /* increments when worldHover changes to a new non-empty text */
    /* Engine panel hover (XrGameBoot_HoverInfo at the probe position set by GXEnginePanelState_SetHoverProbe). */
    char panelHover[512];
    GXPanelSession session;
} GXPanelSnapshot;

/* ---- the control model -------------------------------------------------------------------------------------- */

typedef struct GXPanelControl {
    int32_t id;          /* Quest control id (page local): what GXPanelAction_Perform takes */
    int32_t section;     /* index into the view's sections; -1 = none (page level control) */
    int32_t role;        /* GX_ROLE_* */
    int32_t state;       /* GX_STATE_* bits */
    int32_t badge;       /* group count / bookmark known (1) or unknown (0); -1 = none */
    char label[64];
    char value[32];      /* chip value ("ON", "OFF", "1.2x", ...), empty when none */
    char explain[200];   /* what the control does; for a disabled control: why it is disabled */
} GXPanelControl;

typedef struct GXPanelSection {
    int32_t id;          /* the Quest header id (-10 ...) */
    char title[96];
} GXPanelSection;

enum { GX_PANEL_MAX_SECTIONS = 8, GX_PANEL_MAX_CONTROLS = 72 };

typedef struct GXPanelView {
    int32_t page;
    char title[96];
    char status[320];    /* the context card, first line (mode, selection, group) */
    char hint[320];      /* the context card, second line (what to do next / notices) */
    int32_t sectionCount;
    GXPanelSection sections[GX_PANEL_MAX_SECTIONS];
} GXPanelView;

/* Builds the controls of `page` for the snapshot in `language`. Fills `view` and up to `maxControls` entries of
 * `controls` in table order (section headers become entries of view->sections, not controls). Returns the control
 * count. Pure: same input, same output; no engine access. */
int GXPanelModel_Build(int page, const GXPanelSnapshot* snapshot, int language, GXPanelView* view, GXPanelControl* controls,
                       int maxControls);

/* The German -> `language` translation of a Quest XR string (XrStrings.h table). Unknown strings come back unchanged.
 * The returned pointer is valid for the life of the process. Lets Swift share the C++ table where a key exists. */
const char* GXPanelModel_Translate(const char* german, int language);

/* ---- actions ------------------------------------------------------------------------------------------------ */

/* Host-side effects an action asks for (the engine calls have already been made). Not engine state: the SwiftUI /
 * presentation layer applies them. */
enum {
    GX_HOST_NONE = 0,
    GX_HOST_STEREO_WORLD = 1,        /* the Quest set x.stereoWorld = true: show the tabletop world */
    GX_HOST_CLOSE_MENU = 2,          /* the Quest closed the menu after this action */
    GX_HOST_HIDE_COMMANDS = 3,       /* Commands close button (33) */
    GX_HOST_ARM_GROUND_VIEW = 4,     /* Menu View / Ground view (16): armGroundView */
    GX_HOST_RECENTER_WORKSPACE = 5,  /* Windows / 'Align everything in front of me' (18) */
    GX_HOST_WORKSPACE_EDIT = 6,      /* Windows page 0..17: value = the Quest action id */
    GX_HOST_TOGGLE_PREF = 7,         /* the pref changed in GXPanelSession.prefs (value = control id) */
    GX_HOST_PHOTO_ARRANGEMENT = 8,   /* Menu View / Photo arrangement (9) */
    GX_HOST_FINISH_ARRANGEMENT = 9   /* Close / Finish (17, 33 in help) */
};

typedef struct GXPanelActionResult {
    int32_t page;          /* the page the action was performed on (-1 for GX_EXTRA_*) */
    int32_t controlId;     /* the control id (or the GX_EXTRA_* number) */
    bool handled;          /* the id was a live control of that page (false: ignored, like a dead gap on the Quest panel) */
    bool engineCalled;     /* at least one engine call was made */
    bool stereoWorld;      /* GX_HOST_STEREO_WORLD */
    bool closeMenu;        /* GX_HOST_CLOSE_MENU */
    int32_t host;          /* GX_HOST_* effect other than the two flags above, GX_HOST_NONE if none */
    int32_t hostValue;
} GXPanelActionResult;

/* Extra actions that exist on the Quest as controller gestures rather than panel buttons (XrInteraction.h). */
enum {
    GX_EXTRA_CAMERA_PRESET = 1,      /* value 0..4: XrGameBoot_CameraPreset (0 classic, 1 TABLE 78, 2 OBLIQUE 65, 3 TOP 85, 4 FAVORITE) */
    GX_EXTRA_SAVE_CAMERA_DEFAULT = 2,/* XrGameBoot_SaveCameraDefault */
    GX_EXTRA_VIEW_BASE = 3,          /* XrGameBoot_ViewBase (home base) */
    GX_EXTRA_CANCEL_TARGET = 4,      /* XrGameBoot_CancelTarget */
    GX_EXTRA_SET_LANGUAGE = 5,       /* value 0/1: XrGameBoot_SetLanguage */
    GX_EXTRA_TACTICAL_ACTION = 6     /* value = XrGameBoot_TacticalAction id (direct) */
};

/* Performs a panel control on the ENGINE thread (posted with GXEngineHost_Post; with a scripted engine, synchronously).
 * `page` is a GX_PANEL_*; for GX_PANEL_COMMANDS the console variant follows the session. Returns false when the
 * action could not even be queued (no running engine). The outcome shows up in the next snapshot. */
bool GXPanelAction_Perform(int page, int controlId);
bool GXPanelAction_PerformExtra(int extra, int value);

/* The host-side effects (GX_HOST_*, stereoWorld, closeMenu, ...) of performed actions, oldest first. The action itself has
 * already run on the engine thread; the app (SwiftUI) applies what only it can: Ground View, recenter, hiding the console.
 * Returns false when nothing is pending. Only results that carry an effect or a refused camera call are queued. */
bool GXPanelAction_PopResult(GXPanelActionResult* out);

/* ---- the snapshot store ------------------------------------------------------------------------------------- */

/* Registers the frame hook with GXEngineHost (idempotent). Call once at launch. */
void GXEnginePanelState_Install(void);

/* Copy of the latest snapshot (thread safe). */
void GXEnginePanelState_Get(GXPanelSnapshot* out);

/* The UI language (0 German, 1 English). Applied to the engine's own XR strings (g_xrLanguage) on the engine thread;
 * `persistGameText` additionally calls XrGameBoot_SetLanguage (stages the game text language for the next start,
 * writes game_language.cfg) exactly like Menu / View / Language does on the Quest. */
void GXEnginePanelState_SetLanguage(int language, bool persistGameText);

/* Preferences owned by the UI (persisted by the app, restored at launch through this call). */
void GXEnginePanelState_SetPrefs(const GXPanelPrefs* prefs);
void GXEnginePanelState_GetPrefs(GXPanelPrefs* out);

/* Presentation flags only the presentation layer (package C2) knows. Until it calls this the snapshot derives them
 * from the engine (interactiveGame && canStereoWorld, canStereoWorld). */
void GXEnginePanelState_SetPresentation(bool splitVisible, bool stereoVisible);

/* Ground View state as the interaction layer knows it (0 off, 1 armed, 2 active). Optional: the presentation layer (package
 * C2) reports it so the panels can show the toggle correctly; without it snapshot.groundViewMode stays 0. */
void GXEnginePanelState_SetGroundView(int mode);

/* Pixel position (composed engine frame, GameWidth x GameHeight) whose engine tooltip should be refreshed into
 * snapshot.panelHover (throttled to ~5 Hz). x < 0 clears it. For the interaction layer's panel focus. */
void GXEnginePanelState_SetHoverProbe(float x, float y);

/* Scripted engine for tests and screenshots (-uiFakeSnapshot): the snapshot comes from a small in-process model of
 * the tactical state instead of the engine, and GXPanelAction_Perform runs against it synchronously.
 * scenario: 0 = off, 1 = a skirmish in progress with armed / on / disabled / group-count states, 2 = no selection
 * (formation and guard disabled with reasons), 3 = a dialog is open (nothing enabled). */
void GXEnginePanelState_SetScripted(int scenario);
bool GXEnginePanelState_IsScripted(void);

#ifdef __cplusplus
}
#endif

#endif /* GX_ENGINE_PANEL_STATE_H */
