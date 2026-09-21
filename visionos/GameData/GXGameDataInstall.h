// GXGameDataInstall.h - resumable, transactional import of a validated Generals + Zero Hour
// install into the app's private storage. Portable C++17 (no Apple dependencies): the Apple
// specifics (coordinated reads of security-scoped sources, free-space query, "exclude from
// backup") are injected through Hooks by GXGameDataService.mm, and the host test drives the
// same code with fault injection (scripts/qa/vision-gamedata-test.cpp).
//
// On-disk layout below <gameDataRoot> (= Library/Application Support/GeneralsX/GameData):
//
//   ZH/                    installed Zero Hour tree (what CNC_GENERALS_ZH_PATH points at)
//   Generals/              installed base Generals tree (CNC_GENERALS_PATH); absent when the
//                          source had base and Zero Hour merged in one folder
//   install.manifest       the authority for "an install exists" (key=value text)
//   .staging/{ZH,Generals} import in progress; a file is present here only after its copy was
//                          verified (size + BIGF table for *.big); partial copies are *.gxpart
//   .import-journal        present while an import is in progress (state=copying|committing)
//   .backup/               previous install parked during the commit swap
//
// Transaction:
//   1. copy every file to .staging/<role>/<rel>.gxpart, verify, rename to <rel>   (resumable)
//   2. validate the staged tree end to end with the same validator as the source
//   3. journal state=committing (atomic write)
//   4. previous ZH/ and Generals/ -> .backup/, then .staging/<role> -> <role>
//   5. write install.manifest (atomic), remove journal, .staging and .backup
// Recover() after a crash: copying -> keep the partial copy so the import can resume (or
// discard it); committing -> roll forward (the staged tree was already verified), so the
// user ends up with exactly one complete install: the new one.
#ifndef GX_GAMEDATA_INSTALL_H
#define GX_GAMEDATA_INSTALL_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "GXGameDataValidator.h"

namespace gxgd {

struct PlanFile {
    std::string rel;  // relative path below the role's source root, '/' separated
    uint64_t size = 0;
};

struct PlanRole {
    std::string name;        // "ZH" or "Generals"
    std::string sourceRoot;  // absolute
    std::vector<PlanFile> files;
    uint64_t bytes = 0;
};

struct InstallPlan {
    std::vector<PlanRole> roles;
    uint64_t totalBytes = 0;
    uint64_t totalFiles = 0;
    uint32_t skippedEntries = 0;  // junk / executables / symlinks not copied
    bool baseMerged = false;      // no Generals role: base data lives in the ZH tree
};

// Exclusion rules of the importer (documented in docs/GAME_DATA_SETUP.md).
bool ShouldSkipEntry(const std::string& name, bool isDirectory);

// Walk both source roots and list every file to copy. Returns false (and sets *error) when
// the source cannot be listed. `cancelled` (optional) aborts a long walk.
bool BuildInstallPlan(const Report& report, InstallPlan& plan, std::string* error,
                      const std::function<bool()>& cancelled = nullptr);

struct Progress {
    enum class Phase { Planning, CheckingSpace, Copying, Verifying, Committing, Cleaning, Done };
    Phase phase = Phase::Planning;
    uint64_t bytesDone = 0;    // includes bytes reused from an earlier partial import
    uint64_t bytesTotal = 0;
    uint32_t filesDone = 0;
    uint32_t filesTotal = 0;
    uint64_t bytesReused = 0;  // reused from a resumed import
    double bytesPerSecond = 0; // smoothed
    std::string currentFile;   // "ZH/Data/INI/foo.big"
};

struct Hooks {
    // Polled between chunks. Returning true stops the import and keeps the partial copy.
    std::function<bool()> cancelled;
    std::function<void(const Progress&)> progress;
    // Free bytes on the volume that holds <path>; return false when unknown. Default:
    // std::filesystem::space().
    std::function<bool(const std::string& path, uint64_t* freeBytes)> freeSpace;
    // Mark a directory as excluded from device backup. Default: no-op.
    std::function<void(const std::string& path)> excludeFromBackup;
    // Run `body` with a readable path for the source file (used for NSFileCoordinator
    // coordinated reads). Default: body(sourcePath). Returns body's result.
    std::function<bool(const std::string& sourcePath, const std::function<bool(const std::string&)>& body)> readSource;
    // TEST ONLY: simulate the process dying at a named step. Return true to make Install()
    // return Aborted immediately, without cleanup. Steps: "copy-file", "before-commit-journal",
    // "commit-backup", "commit-move", "before-manifest", "before-cleanup".
    std::function<bool(const char* step, int index)> crashAt;
};

enum class InstallOutcome {
    Installed,
    Cancelled,          // partial copy kept; run Install() again with the same source to resume
    InsufficientSpace,
    InvalidSource,      // report not ready or plan empty
    SourceChanged,      // a source file changed size while being copied
    VerificationFailed, // a copied archive or the staged tree failed validation
    IoError,
    Aborted             // test-only simulated crash
};

struct InstallResult {
    InstallOutcome outcome = InstallOutcome::IoError;
    std::string message;
    uint64_t bytesCopied = 0;
    uint64_t bytesReused = 0;
    uint32_t filesCopied = 0;
    uint32_t filesReused = 0;
    uint64_t bytesNeeded = 0;     // InsufficientSpace: bytes still required (with headroom)
    uint64_t bytesAvailable = 0;  // InsufficientSpace: bytes free
    bool resumable = false;
    bool ok() const { return outcome == InstallOutcome::Installed; }
};

// Import a validated source (report.ready()) into <gameDataRoot>. Recovers any interrupted
// earlier import first. Safe to call again after Cancelled / IoError to resume.
InstallResult Install(const std::string& gameDataRoot, const Report& report, const Hooks& hooks = Hooks());

// --- Recovery -------------------------------------------------------------------------

enum class RecoveryAction {
    None,              // nothing pending
    RolledForward,     // an interrupted commit was completed; the new install is in place
    CleanedUp,         // leftovers of a finished commit were removed
    PartialCopyKept,   // an interrupted copy can be resumed (see RecoveryInfo)
    DiscardedUnusable  // an unreadable journal / empty staging was removed
};

struct RecoveryInfo {
    RecoveryAction action = RecoveryAction::None;
    std::string message;
    // PartialCopyKept:
    std::string sourceZh;      // where the interrupted import was reading from
    std::string sourceBase;    // "" when merged
    uint64_t bytesStaged = 0;
    uint64_t bytesTotal = 0;
    uint32_t filesStaged = 0;
    uint32_t filesTotal = 0;
};

// Idempotent. Call once at launch, before reading the install state.
RecoveryInfo Recover(const std::string& gameDataRoot);

// Delete a partial (resumable) import.
void DiscardPartialImport(const std::string& gameDataRoot);

// --- Installed state --------------------------------------------------------------------

struct InstalledManifest {
    std::string installId;
    std::string zhRoot;    // absolute
    std::string baseRoot;  // absolute; equals zhRoot when merged
    bool baseMerged = false;
    bool complete = false;
    uint64_t bytes = 0;
    uint32_t files = 0;
    std::string sourceZh;  // display only
    std::string languages; // comma separated, display only
    int64_t installedAtEpoch = 0;
};

// True when install.manifest exists and both trees it names exist.
bool ReadInstalled(const std::string& gameDataRoot, InstalledManifest& out);

// Remove the installed trees, manifest, journal, staging and backup (user-initiated).
bool RemoveInstalled(const std::string& gameDataRoot, std::string* error = nullptr);

}  // namespace gxgd

#endif  // GX_GAMEDATA_INSTALL_H
