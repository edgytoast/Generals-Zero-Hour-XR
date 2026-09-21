// GXGameDataValidator.h - portable, dependency-free detector for a user-supplied
// Command & Conquer Generals + Zero Hour install.
//
// This is a faithful C++ port of the Quest/Android validator
// (android/app/src/main/java/com/generalsx/zerohour/GameDataValidator.java) with a
// structured report on top. It has NO Apple dependencies (standard C++17 only) so the
// same code runs in the visionOS app and in the host test
// (scripts/qa/vision-gamedata-test.cpp).
//
// Rules ported from the Android validator:
//   * Zero Hour marker file: INIZH.big.  Base Generals marker file: Terrain.big.
//   * File names are matched case-insensitively.
//   * Discovery is bounded: search depth 2 below the selected folder, at most 128
//     folders visited, dot-folders skipped, symlink loops broken by canonical path.
//     Selecting a Steam/common parent works; several installs need an explicit choice.
//   * Base Generals is found as: merged into the Zero Hour folder, nested below it
//     (Steam "ZH_Generals"), an unambiguous sibling of it, or an explicit folder.
//   * 10 Zero Hour archives + 10 base Generals archives must exist (lists below).
//   * Zero Hour needs a language string table (Data/<Lang>/generals.csf or .str, loose
//     or inside a top-level Zero Hour archive) and Data/INI/Default/Weather.ini (loose
//     or inside any top-level archive of either tree).  A base-only string table never
//     satisfies the Zero Hour requirement.
//   * Every top-level *.big is opened and its BIGF table is bounds-checked against the
//     real file length (detects truncated transfers).  PatchZH.big-style archives that
//     contain zero-size placeholder entries are tolerated.
//
// The BIGF parsing matches Core/GameEngineDevice/Source/StdDevice/Common/StdBIGFileSystem.cpp
// (openArchiveFile): magic "BIGF", 4 unused size bytes, big-endian file count at offset 8,
// entries from offset 0x10 as {u32 BE offset, u32 BE size, NUL-terminated path with '\\'
// or '/' separators}.
#ifndef GX_GAMEDATA_VALIDATOR_H
#define GX_GAMEDATA_VALIDATOR_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gxgd {

// Archive lists (identical to GameDataValidator.BASE / ZH).
extern const char* const kBaseArchives[10];
extern const char* const kZeroHourArchives[10];

// Bounded discovery limits (identical to the Android validator).
constexpr int kMaxSearchDepth = 2;
constexpr int kMaxFoldersVisited = 128;
// Sanity limits for the BIGF reader (identical to the Android validator).
constexpr uint64_t kMaxArchiveEntries = 500000;
constexpr size_t kMaxArchivePathLength = 511;

enum class Scope { ZeroHour, Generals, Shared };

struct MissingItem {
    Scope scope = Scope::ZeroHour;
    std::string name;    // "INIZH.big", "Data/INI/Default/Weather.ini", ...
    std::string reason;  // human readable, one sentence
    // Android-compatible display: "MapsZH.big" or "Generals: Maps.big".
    std::string display() const;
};

struct DamagedItem {
    std::string path;    // absolute path of the archive
    std::string reason;  // "BIGF header", "BIGF table", "BIGF name", "BIGF payload bounds", ...
};

struct ArchiveSummary {
    std::string path;
    std::string name;
    Scope scope = Scope::ZeroHour;
    uint64_t sizeBytes = 0;
    uint32_t entryCount = 0;
    bool damaged = false;
};

enum class BaseLayout {
    None,      // no base Generals folder resolved
    Merged,    // Zero Hour and base Generals share one folder
    Nested,    // base Generals is below the Zero Hour folder (Steam: ZH_Generals)
    Sibling,   // an unambiguous neighbour of the Zero Hour folder (CD/ISO installs)
    Explicit   // the caller passed a separate base folder
};

enum class Verdict {
    NoSelection,   // nothing chosen
    Unreadable,    // folder cannot be listed (or a non-folder was chosen)
    InstallerOnly, // .iso/.cab/.msi/setup.exe seen but no installed game data
    NoZeroHour,    // no INIZH.big within the search bounds
    Ambiguous,     // several Zero Hour installs, the caller must choose one
    Incomplete,    // a Zero Hour folder was found but required data is missing or damaged
    Ready          // playable ("minimal" data set present)
};

struct Report {
    // Inputs.
    std::string selected;
    std::string explicitBase;

    // Resolved roots (absolute, no trailing slash). Empty when not found.
    std::string zhRoot;
    std::string baseRoot;
    BaseLayout baseLayout = BaseLayout::None;
    std::vector<std::string> zhChoices;  // every INIZH.big folder found (ambiguity list)

    // Findings.
    std::vector<MissingItem> missing;
    std::vector<DamagedItem> damaged;
    std::vector<std::string> warnings;   // never block; shown to the user
    bool installerDetected = false;
    bool unreadable = false;
    bool baseFolderMissing = false;      // no base Generals folder could be resolved
    bool weatherFound = false;
    bool languageFound = false;
    std::vector<std::string> languages;  // e.g. "English" (from Data/<lang>/generals.csf|str)

    // "Complete" (beyond minimal) indicators. Heuristics: they never block play.
    bool videosFound = false;            // any *.bik loose under Data/ or inside an archive
    bool voiceArchivesFound = false;     // Audio<Lang>ZH.big / Speech<Lang>ZH.big present
    std::vector<std::string> optionalMissing;  // sentences describing what "complete" lacks

    // Size estimate = sum of the top-level *.big files of both trees (bytes). Loose
    // files (movies, Data/) are counted exactly later, when an import plan is built.
    std::vector<ArchiveSummary> archives;
    uint64_t zhArchiveBytes = 0;
    uint64_t baseArchiveBytes = 0;
    uint64_t totalBytesEstimate() const { return zhArchiveBytes + (baseSeparate() ? baseArchiveBytes : 0); }
    bool baseSeparate() const { return !baseRoot.empty() && baseRoot != zhRoot; }

    // Android ready(): game && base && missing.empty() && damaged.empty() && !unreadable
    //                  && weather && language.
    bool ready() const;
    bool complete() const { return ready() && optionalMissing.empty(); }
    Verdict verdict() const;

    // Human-readable multi-line description (used by the CLI/tests and the launcher).
    std::string summary() const;
    // Line-oriented machine-readable dump ("key=value"), used by tests and diagnostics.
    std::string dump() const;
};

// Validate a user selection. `selected` is a Zero Hour folder or a parent that contains
// one (bounded search). `explicitBase` optionally names a separate base Generals folder.
// Never modifies anything on disk.
Report Validate(const std::string& selected, const std::string& explicitBase = std::string());

// --- Lower-level pieces (exposed for the importer and the tests) -------------------------

// Case-insensitive lookup of a regular file directly inside `dir`. Returns the absolute
// path or "" when absent.
std::string NamedFile(const std::string& dir, const std::string& name);

// Bounded discovery of folders that directly contain `marker` (case-insensitive), sorted
// by absolute path. Mirrors GameDataValidator.candidates().
std::vector<std::string> FindCandidates(const std::string& root, const std::string& marker);

// One entry of a BIGF table.
struct BigEntry {
    uint32_t offset = 0;
    uint32_t size = 0;
    std::string path;  // exactly as stored (may use backslashes)
};

struct BigArchiveInfo {
    bool ok = false;
    std::string error;       // set when !ok: "BIGF header", "BIGF table", "BIGF name", ...
    uint64_t fileSize = 0;
    uint32_t entryCount = 0;
    uint64_t tableEnd = 0;   // header field at offset 12 (big endian)
};

// Read a .big header and walk its table, checking every entry against the real file
// length (no payload is read). `onEntry` (optional) is called for every well-formed entry.
BigArchiveInfo ReadBigArchive(const std::string& path,
                              const std::function<void(const BigEntry&)>& onEntry = nullptr);

// Convenience for the importer: verify a copied archive (header + table bounds) and that
// its length equals `expectedSize` (pass UINT64_MAX to skip the size check).
bool VerifyBigArchive(const std::string& path, uint64_t expectedSize, std::string* error = nullptr);

// Lower-case ASCII copy.
std::string LowerAscii(const std::string& s);
// True when `name` ends (case-insensitively) with `suffix`.
bool EndsWithNoCase(const std::string& name, const std::string& suffix);

}  // namespace gxgd

#endif  // GX_GAMEDATA_VALIDATOR_H
