// GXGameDataInstall.cpp - see GXGameDataInstall.h. Portable C++17, no Apple APIs.
#include "GXGameDataInstall.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <system_error>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace gxgd {

namespace {

constexpr const char* kManifestName = "install.manifest";
constexpr const char* kJournalName = ".import-journal";
constexpr const char* kStagingName = ".staging";
constexpr const char* kBackupName = ".backup";
constexpr const char* kPartSuffix = ".gxpart";
const char* const kRoles[2] = {"ZH", "Generals"};
constexpr size_t kChunk = 2u * 1024u * 1024u;

// ---- small file helpers ---------------------------------------------------------------

bool WriteAtomic(const fs::path& path, const std::string& text) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        FILE* f = fopen(tmp.string().c_str(), "wb");
        if (!f) return false;
        bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = (fflush(f) == 0) && ok;
#if !defined(_WIN32)
        if (ok) fsync(fileno(f));
#endif
        ok = (fclose(f) == 0) && ok;
        if (!ok) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

std::map<std::string, std::string> ReadKV(const fs::path& path) {
    std::map<std::string, std::string> kv;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

uint64_t ToU64(const std::map<std::string, std::string>& kv, const char* key) {
    auto it = kv.find(key);
    if (it == kv.end()) return 0;
    return strtoull(it->second.c_str(), nullptr, 10);
}

std::string Get(const std::map<std::string, std::string>& kv, const char* key) {
    auto it = kv.find(key);
    return it == kv.end() ? std::string() : it->second;
}

bool Exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

void RemoveAll(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

bool Rename(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    return !ec;
}

std::string NewInstallId() {
    std::random_device rd;
    std::ostringstream o;
    o << std::hex;
    for (int i = 0; i < 4; i++) o << rd();
    return o.str();
}

std::string RelJoin(const std::string& a, const std::string& b) { return a.empty() ? b : a + "/" + b; }

// Count regular files (excluding *.gxpart) and bytes below `dir`.
void TreeStats(const fs::path& dir, uint64_t& bytes, uint32_t& files) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        if (EndsWithNoCase(it->path().filename().string(), kPartSuffix)) continue;
        bytes += static_cast<uint64_t>(fs::file_size(it->path(), e2));
        files++;
    }
}

std::string RolesCsv(const InstallPlan& plan) {
    std::string s;
    for (const auto& r : plan.roles) {
        if (!s.empty()) s += ",";
        s += r.name;
    }
    return s;
}

bool CsvContains(const std::string& csv, const std::string& item) {
    std::stringstream ss(csv);
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (part == item) return true;
    }
    return false;
}

// ---- plan ----------------------------------------------------------------------------

bool IsUnder(const std::string& child, const std::string& parent) {
    if (child == parent || parent.empty()) return false;
    return child.size() > parent.size() && child.compare(0, parent.size(), parent) == 0 && child[parent.size()] == '/';
}

bool WalkRole(const fs::path& dir, const std::string& rel, const std::string& excludeRoot, PlanRole& role,
              InstallPlan& plan, std::string* error, const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) {
        if (error) *error = "cancelled";
        return false;
    }
    std::error_code ec;
    std::vector<fs::directory_entry> entries;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec;
         it.increment(ec)) {
        entries.push_back(*it);
    }
    if (ec) {
        if (error) *error = "cannot list " + dir.string() + ": " + ec.message();
        return false;
    }
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename().string() < b.path().filename().string();
    });
    for (const auto& de : entries) {
        const std::string name = de.path().filename().string();
        std::error_code e2;
        if (de.is_symlink(e2)) {
            plan.skippedEntries++;
            continue;
        }
        const bool isDir = de.is_directory(e2);
        if (ShouldSkipEntry(name, isDir)) {
            plan.skippedEntries++;
            continue;
        }
        if (isDir) {
            // Do not copy the other role's tree twice (Steam nests base Generals in ZH_Generals).
            std::string abs = de.path().lexically_normal().string();
            if (!excludeRoot.empty() && abs == excludeRoot) continue;
            if (!WalkRole(de.path(), RelJoin(rel, name), excludeRoot, role, plan, error, cancelled)) return false;
        } else if (de.is_regular_file(e2)) {
            PlanFile f;
            f.rel = RelJoin(rel, name);
            f.size = static_cast<uint64_t>(fs::file_size(de.path(), e2));
            if (e2) {
                if (error) *error = "cannot read size of " + de.path().string();
                return false;
            }
            role.bytes += f.size;
            role.files.push_back(std::move(f));
        }
    }
    return true;
}

// ---- copy ----------------------------------------------------------------------------

class Tracker {
public:
    Tracker(const Hooks& h, uint64_t totalBytes, uint32_t totalFiles)
        : hooks_(h), start_(std::chrono::steady_clock::now()), last_(start_) {
        p_.bytesTotal = totalBytes;
        p_.filesTotal = totalFiles;
    }
    void Phase(Progress::Phase phase, bool force = true) {
        p_.phase = phase;
        Emit(force);
    }
    void AddReused(uint64_t bytes, const std::string& file) {
        p_.bytesDone += bytes;
        p_.bytesReused += bytes;
        p_.filesDone++;
        p_.currentFile = file;
        Emit(false);
    }
    void SetFile(const std::string& file) {
        p_.currentFile = file;
        Emit(false);
    }
    void AddBytes(uint64_t bytes) {
        p_.bytesDone += bytes;
        copiedThisRun_ += bytes;
        Emit(false);
    }
    void FileDone() {
        p_.filesDone++;
        Emit(false);
    }
    const Progress& Current() const { return p_; }

private:
    void Emit(bool force) {
        if (!hooks_.progress) return;
        auto now = std::chrono::steady_clock::now();
        if (!force && now - last_ < std::chrono::milliseconds(100)) return;
        double dt = std::chrono::duration<double>(now - start_).count();
        if (dt > 0.25) {
            double inst = double(copiedThisRun_) / dt;
            p_.bytesPerSecond = p_.bytesPerSecond == 0 ? inst : (0.7 * p_.bytesPerSecond + 0.3 * inst);
        }
        last_ = now;
        hooks_.progress(p_);
    }
    const Hooks& hooks_;
    Progress p_;
    uint64_t copiedThisRun_ = 0;
    std::chrono::steady_clock::time_point start_, last_;
};

enum class CopyStatus { Ok, Cancelled, SourceChanged, NoSpace, Io, Verify };

struct CopyContext {
    const Hooks& hooks;
    Tracker& tracker;
    std::string error;
};

CopyStatus CopyOne(CopyContext& ctx, const std::string& src, const fs::path& dstFinal, uint64_t expectedSize,
                   bool requireBigVerify, std::vector<std::string>& warnings) {
    std::error_code ec;
    fs::create_directories(dstFinal.parent_path(), ec);
    if (ec) {
        ctx.error = "cannot create " + dstFinal.parent_path().string() + ": " + ec.message();
        return CopyStatus::Io;
    }
    fs::path part = dstFinal;
    part += kPartSuffix;
    fs::remove(part, ec);

    CopyStatus status = CopyStatus::Io;
    auto body = [&](const std::string& readable) -> bool {
        FILE* in = fopen(readable.c_str(), "rb");
        if (!in) {
            ctx.error = "cannot open source " + src + ": " + strerror(errno);
            status = CopyStatus::Io;
            return false;
        }
        FILE* out = fopen(part.string().c_str(), "wb");
        if (!out) {
            ctx.error = "cannot create " + part.string() + ": " + strerror(errno);
            status = errno == ENOSPC ? CopyStatus::NoSpace : CopyStatus::Io;
            fclose(in);
            return false;
        }
        std::vector<char> buf(kChunk);
        uint64_t total = 0;
        bool ok = true;
        for (;;) {
            if (ctx.hooks.cancelled && ctx.hooks.cancelled()) {
                status = CopyStatus::Cancelled;
                ok = false;
                break;
            }
            size_t n = fread(buf.data(), 1, buf.size(), in);
            if (n == 0) {
                if (ferror(in)) {
                    ctx.error = "read error on " + src + ": " + strerror(errno);
                    status = CopyStatus::Io;
                    ok = false;
                }
                break;
            }
            if (fwrite(buf.data(), 1, n, out) != n) {
                ctx.error = std::string("write error: ") + strerror(errno);
                status = errno == ENOSPC ? CopyStatus::NoSpace : CopyStatus::Io;
                ok = false;
                break;
            }
            total += n;
            ctx.tracker.AddBytes(n);
        }
        fclose(in);
        if (ok) {
            ok = (fflush(out) == 0);
#if !defined(_WIN32)
            if (ok) ok = (fsync(fileno(out)) == 0);
#endif
            if (!ok) {
                ctx.error = std::string("flush error: ") + strerror(errno);
                status = errno == ENOSPC ? CopyStatus::NoSpace : CopyStatus::Io;
            }
        }
        if (fclose(out) != 0 && ok) {
            ctx.error = std::string("close error: ") + strerror(errno);
            status = errno == ENOSPC ? CopyStatus::NoSpace : CopyStatus::Io;
            ok = false;
        }
        if (ok && total != expectedSize) {
            ctx.error = src + " changed while it was being copied (read " + std::to_string(total) + " bytes, expected " +
                        std::to_string(expectedSize) + ")";
            status = CopyStatus::SourceChanged;
            ok = false;
        }
        if (ok) status = CopyStatus::Ok;
        return ok;
    };
    bool ran = ctx.hooks.readSource ? ctx.hooks.readSource(src, body) : body(src);
    if (!ran && status == CopyStatus::Io && ctx.error.empty()) ctx.error = "cannot read source " + src;
    if (status != CopyStatus::Ok) {
        // Keep nothing half-written under the final name; the partial is disposable.
        fs::remove(part, ec);
        if (status == CopyStatus::Cancelled || status == CopyStatus::NoSpace) { /* still resumable */ }
        return status;
    }

    // Verify by size and, for archives, by BIGF header + table bounds.
    uint64_t written = static_cast<uint64_t>(fs::file_size(part, ec));
    if (ec || written != expectedSize) {
        ctx.error = "size mismatch after copying " + dstFinal.filename().string();
        fs::remove(part, ec);
        return CopyStatus::Verify;
    }
    if (EndsWithNoCase(dstFinal.filename().string(), ".big")) {
        std::string why;
        if (!VerifyBigArchive(part.string(), expectedSize, &why)) {
            if (requireBigVerify) {
                ctx.error = dstFinal.filename().string() + ": " + why;
                fs::remove(part, ec);
                return CopyStatus::Verify;
            }
            warnings.push_back(dstFinal.filename().string() + " (not a top-level archive) failed the BIGF check: " + why);
        }
    }
    fs::rename(part, dstFinal, ec);
    if (ec) {
        ctx.error = "cannot finalise " + dstFinal.string() + ": " + ec.message();
        fs::remove(part, ec);
        return CopyStatus::Io;
    }
    return CopyStatus::Ok;
}

// Is <dst> already a complete, verified copy of a file of `size` bytes?
bool AlreadyStaged(const fs::path& dst, uint64_t size) {
    std::error_code ec;
    if (!fs::is_regular_file(dst, ec)) return false;
    if (static_cast<uint64_t>(fs::file_size(dst, ec)) != size || ec) return false;
    // Files only get their final name after size (and, for archives, BIGF table) verification
    // in CopyOne(), so a same-size file under its final name is a verified copy.
    return true;
}

bool IsTopLevel(const std::string& rel) { return rel.find('/') == std::string::npos; }

std::string LayoutString(const Report& r) { return r.baseLayout == BaseLayout::Merged ? "merged" : "separate"; }

// ---- commit / recovery -----------------------------------------------------------------

struct CommitOutcome {
    bool ok = false;
    bool aborted = false;  // test crash
    std::string error;
};

bool WriteManifest(const fs::path& root, const std::string& installId, const std::string& base, bool merged,
                   const Report& staged, uint64_t bytes, uint32_t files, const std::string& sourceZh) {
    std::ostringstream o;
    o << "gxgd-install=1\n";
    o << "install_id=" << installId << "\n";
    o << "zh=ZH\n";
    o << "base=" << base << "\n";
    o << "merged=" << (merged ? 1 : 0) << "\n";
    o << "complete=" << (staged.complete() ? 1 : 0) << "\n";
    o << "bytes=" << bytes << "\n";
    o << "files=" << files << "\n";
    o << "source_zh=" << sourceZh << "\n";
    std::string langs;
    for (const auto& l : staged.languages) {
        if (!langs.empty()) langs += ",";
        langs += l;
    }
    o << "languages=" << langs << "\n";
    o << "installed_at=" << static_cast<long long>(std::time(nullptr)) << "\n";
    return WriteAtomic(root / kManifestName, o.str());
}

// Move previous role trees to .backup/ and staged trees into place. Idempotent.
bool CommitSwap(const fs::path& root, const std::string& journalRoles, const Hooks& hooks, bool& aborted,
                std::string& error) {
    std::error_code ec;
    fs::create_directories(root / kBackupName, ec);
    for (int i = 0; i < 2; i++) {
        const std::string role = kRoles[i];
        const fs::path fin = root / role, stage = root / kStagingName / role, back = root / kBackupName / role;
        const bool inNew = CsvContains(journalRoles, role);
        const bool stageExists = Exists(stage);
        if (hooks.crashAt && hooks.crashAt("commit-backup", i)) { aborted = true; return false; }
        if (Exists(fin) && (stageExists || !inNew)) {
            if (Exists(back)) RemoveAll(back);
            if (!Rename(fin, back)) {
                error = "cannot move the previous " + role + " install aside";
                return false;
            }
        }
        if (hooks.crashAt && hooks.crashAt("commit-move", i)) { aborted = true; return false; }
        if (stageExists) {
            if (!Rename(stage, fin)) {
                error = "cannot move the imported " + role + " data into place";
                return false;
            }
            if (hooks.excludeFromBackup) hooks.excludeFromBackup(fin.string());
        }
    }
    return true;
}

void Cleanup(const fs::path& root) {
    RemoveAll(root / kBackupName);
    RemoveAll(root / kStagingName);
    std::error_code ec;
    fs::remove(root / kJournalName, ec);
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Public: skip rules and plan
// ---------------------------------------------------------------------------------------

bool ShouldSkipEntry(const std::string& name, bool isDirectory) {
    if (name.empty() || name[0] == '.') return true;  // .DS_Store, .git, hidden
    const std::string l = LowerAscii(name);
    if (isDirectory) return l == "__macosx" || l == "_commonredist";
    if (l == "thumbs.db" || l == "desktop.ini") return true;
    if (EndsWithNoCase(l, kPartSuffix)) return true;
    static const char* const kSkipExt[] = {".exe", ".dll", ".msi", ".cab", ".iso", ".bat", ".cmd", ".lnk", ".url", ".scr", ".com"};
    for (const char* ext : kSkipExt) {
        if (EndsWithNoCase(l, ext)) return true;
    }
    return false;
}

bool BuildInstallPlan(const Report& report, InstallPlan& plan, std::string* error, const std::function<bool()>& cancelled) {
    plan = InstallPlan();
    if (!report.ready()) {
        if (error) *error = "the source has not passed validation";
        return false;
    }
    const std::string zh = report.zhRoot;
    const std::string base = report.baseRoot;
    plan.baseMerged = (base == zh);

    PlanRole zhRole;
    zhRole.name = "ZH";
    zhRole.sourceRoot = zh;
    // Skip the base tree when it sits inside the Zero Hour tree (Steam: ZH_Generals).
    std::string skipBase = IsUnder(base, zh) ? fs::path(base).lexically_normal().string() : std::string();
    if (!WalkRole(fs::path(zh), "", skipBase, zhRole, plan, error, cancelled)) return false;
    plan.roles.push_back(std::move(zhRole));

    if (!plan.baseMerged) {
        PlanRole baseRole;
        baseRole.name = "Generals";
        baseRole.sourceRoot = base;
        std::string skipZh = IsUnder(zh, base) ? fs::path(zh).lexically_normal().string() : std::string();
        if (!WalkRole(fs::path(base), "", skipZh, baseRole, plan, error, cancelled)) return false;
        plan.roles.push_back(std::move(baseRole));
    }
    for (const auto& r : plan.roles) {
        plan.totalBytes += r.bytes;
        plan.totalFiles += r.files.size();
    }
    if (plan.totalFiles == 0) {
        if (error) *error = "nothing to copy";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// Public: Install
// ---------------------------------------------------------------------------------------

InstallResult Install(const std::string& gameDataRoot, const Report& report, const Hooks& hooks) {
    InstallResult res;
    const fs::path root(gameDataRoot);
    std::error_code ec;

    auto fail = [&](InstallOutcome o, const std::string& msg) {
        res.outcome = o;
        res.message = msg;
        return res;
    };

    if (!report.ready()) return fail(InstallOutcome::InvalidSource, "The chosen folder has not passed validation.");

    fs::create_directories(root, ec);
    if (ec) return fail(InstallOutcome::IoError, "Cannot create the game data folder: " + ec.message());
    if (hooks.excludeFromBackup) hooks.excludeFromBackup(root.string());

    // Finish or clean up whatever an earlier run left behind.
    RecoveryInfo rec = Recover(gameDataRoot);

    {
        Tracker planning(hooks, 0, 0);
        planning.Phase(Progress::Phase::Planning);
    }
    InstallPlan plan;
    std::string planError;
    if (!BuildInstallPlan(report, plan, &planError, hooks.cancelled)) {
        if (planError == "cancelled") return fail(InstallOutcome::Cancelled, "Cancelled.");
        return fail(InstallOutcome::InvalidSource, planError);
    }
    Tracker run(hooks, plan.totalBytes, static_cast<uint32_t>(plan.totalFiles));

    const std::string sourceZh = report.zhRoot;
    const std::string sourceBase = plan.baseMerged ? std::string() : report.baseRoot;
    const fs::path staging = root / kStagingName;

    // Resume only when the interrupted import read from the same source folders.
    std::string installId;
    if (rec.action == RecoveryAction::PartialCopyKept) {
        auto kv = ReadKV(root / kJournalName);
        if (Get(kv, "source_zh") == sourceZh && Get(kv, "source_base") == sourceBase && !Get(kv, "install_id").empty()) {
            installId = Get(kv, "install_id");
        } else {
            DiscardPartialImport(gameDataRoot);
        }
    } else if (Exists(staging)) {
        RemoveAll(staging);
    }
    const bool resuming = !installId.empty();
    if (!resuming) installId = NewInstallId();

    // Bytes already staged (reusable), needed for an honest free-space check.
    run.Phase(Progress::Phase::CheckingSpace);
    uint64_t alreadyStaged = 0;
    if (resuming) {
        for (const auto& role : plan.roles) {
            for (const auto& f : role.files) {
                fs::path dst = staging / role.name / f.rel;
                if (AlreadyStaged(dst, f.size)) alreadyStaged += f.size;
            }
        }
    }
    const uint64_t remaining = plan.totalBytes - alreadyStaged;
    const uint64_t headroom = std::max<uint64_t>(256ull << 20, plan.totalBytes / 50);  // 256 MiB or 2 %
    res.bytesNeeded = remaining + headroom;
    uint64_t freeBytes = 0;
    bool freeKnown = false;
    if (hooks.freeSpace) {
        freeKnown = hooks.freeSpace(gameDataRoot, &freeBytes);
    } else {
        std::error_code sec;
        auto space = fs::space(root, sec);
        if (!sec) {
            freeBytes = static_cast<uint64_t>(space.available);
            freeKnown = true;
        }
    }
    if (freeKnown && freeBytes < res.bytesNeeded) {
        res.bytesAvailable = freeBytes;
        res.resumable = resuming;
        return fail(InstallOutcome::InsufficientSpace,
                    "Not enough free storage: about " + std::to_string(res.bytesNeeded >> 20) + " MB needed, " +
                        std::to_string(freeBytes >> 20) + " MB free.");
    }

    // Journal: state=copying.
    {
        std::ostringstream j;
        j << "gxgd-journal=1\nstate=copying\ninstall_id=" << installId << "\nsource_zh=" << sourceZh
          << "\nsource_base=" << sourceBase << "\nlayout=" << LayoutString(report) << "\nroles=" << RolesCsv(plan)
          << "\nfiles_total=" << plan.totalFiles << "\nbytes_total=" << plan.totalBytes
          << "\nstarted_at=" << static_cast<long long>(std::time(nullptr)) << "\n";
        if (!WriteAtomic(root / kJournalName, j.str())) return fail(InstallOutcome::IoError, "Cannot write the import journal.");
    }
    fs::create_directories(staging, ec);
    if (hooks.excludeFromBackup) hooks.excludeFromBackup(staging.string());

    // ---- copy ----
    run.Phase(Progress::Phase::Copying);
    CopyContext ctx{hooks, run, {}};
    std::vector<std::string> warnings;
    int fileIndex = 0;
    for (const auto& role : plan.roles) {
        for (const auto& f : role.files) {
            const fs::path dst = staging / role.name / f.rel;
            const std::string shown = role.name + "/" + f.rel;
            if (hooks.cancelled && hooks.cancelled()) {
                res.resumable = true;
                res.outcome = InstallOutcome::Cancelled;
                res.message = "Import cancelled. The copied files were kept so it can be resumed.";
                return res;
            }
            if (resuming && AlreadyStaged(dst, f.size)) {
                run.AddReused(f.size, shown);
                res.filesReused++;
                res.bytesReused += f.size;
                fileIndex++;
                continue;
            }
            run.SetFile(shown);
            const std::string src = (fs::path(role.sourceRoot) / f.rel).string();
            CopyStatus st = CopyOne(ctx, src, dst, f.size, IsTopLevel(f.rel), warnings);
            if (st != CopyStatus::Ok) {
                res.resumable = true;
                switch (st) {
                    case CopyStatus::Cancelled:
                        res.outcome = InstallOutcome::Cancelled;
                        res.message = "Import cancelled. The copied files were kept so it can be resumed.";
                        break;
                    case CopyStatus::NoSpace:
                        res.outcome = InstallOutcome::InsufficientSpace;
                        res.message = "The device ran out of storage while copying " + shown + ".";
                        break;
                    case CopyStatus::SourceChanged:
                        res.outcome = InstallOutcome::SourceChanged;
                        res.message = ctx.error;
                        break;
                    case CopyStatus::Verify:
                        res.outcome = InstallOutcome::VerificationFailed;
                        res.message = "A copied file failed verification: " + ctx.error;
                        break;
                    default:
                        res.outcome = InstallOutcome::IoError;
                        res.message = ctx.error.empty() ? "Copy failed." : ctx.error;
                        break;
                }
                return res;
            }
            res.filesCopied++;
            res.bytesCopied += f.size;
            run.FileDone();
            if (hooks.crashAt && hooks.crashAt("copy-file", fileIndex)) return fail(InstallOutcome::Aborted, "crash at copy-file");
            fileIndex++;
        }
    }

    // ---- verify the staged tree end to end ----
    run.Phase(Progress::Phase::Verifying);
    const std::string stagedZh = (staging / "ZH").string();
    const std::string stagedBase = plan.baseMerged ? std::string() : (staging / "Generals").string();
    Report stagedReport = Validate(stagedZh, stagedBase);
    if (!stagedReport.ready()) {
        res.resumable = false;
        res.outcome = InstallOutcome::VerificationFailed;
        res.message = "The copied data failed validation:\n" + stagedReport.summary();
        RemoveAll(staging);
        std::error_code rec2;
        fs::remove(root / kJournalName, rec2);
        return res;
    }

    // ---- commit ----
    run.Phase(Progress::Phase::Committing);
    if (hooks.crashAt && hooks.crashAt("before-commit-journal", 0)) return fail(InstallOutcome::Aborted, "crash");
    {
        std::ostringstream j;
        j << "gxgd-journal=1\nstate=committing\ninstall_id=" << installId << "\nsource_zh=" << sourceZh
          << "\nsource_base=" << sourceBase << "\nlayout=" << LayoutString(report) << "\nroles=" << RolesCsv(plan)
          << "\nfiles_total=" << plan.totalFiles << "\nbytes_total=" << plan.totalBytes
          << "\nstarted_at=" << static_cast<long long>(std::time(nullptr)) << "\n";
        if (!WriteAtomic(root / kJournalName, j.str())) return fail(InstallOutcome::IoError, "Cannot write the import journal.");
    }
    RemoveAll(root / kBackupName);  // stale leftovers; recovery already ran
    bool aborted = false;
    std::string swapError;
    if (!CommitSwap(root, RolesCsv(plan), hooks, aborted, swapError)) {
        if (aborted) return fail(InstallOutcome::Aborted, "crash during commit");
        // Roll the swap back so the previous install (if any) is intact and usable.
        for (const char* role : kRoles) {
            if (!Exists(root / role) && Exists(root / kBackupName / role)) Rename(root / kBackupName / role, root / role);
        }
        Cleanup(root);
        return fail(InstallOutcome::IoError, swapError);
    }
    if (hooks.crashAt && hooks.crashAt("before-manifest", 0)) return fail(InstallOutcome::Aborted, "crash");
    if (!WriteManifest(root, installId, plan.baseMerged ? "ZH" : "Generals", plan.baseMerged, stagedReport,
                       plan.totalBytes, static_cast<uint32_t>(plan.totalFiles), sourceZh)) {
        return fail(InstallOutcome::IoError, "Cannot write the install manifest.");
    }
    if (hooks.crashAt && hooks.crashAt("before-cleanup", 0)) return fail(InstallOutcome::Aborted, "crash");
    run.Phase(Progress::Phase::Cleaning);
    Cleanup(root);
    run.Phase(Progress::Phase::Done);
    res.outcome = InstallOutcome::Installed;
    res.message = warnings.empty() ? "Imported." : warnings.front();
    return res;
}

// ---------------------------------------------------------------------------------------
// Public: recovery and state
// ---------------------------------------------------------------------------------------

RecoveryInfo Recover(const std::string& gameDataRoot) {
    RecoveryInfo info;
    const fs::path root(gameDataRoot);
    if (!Exists(root)) return info;
    const fs::path journal = root / kJournalName;

    if (!Exists(journal)) {
        // No transaction in flight: anything left in .staging is disposable; a backup is only
        // useful when the install it protects went missing.
        bool touched = false;
        if (Exists(root / kStagingName)) { RemoveAll(root / kStagingName); touched = true; }
        if (Exists(root / kBackupName)) {
            for (const char* role : kRoles) {
                if (!Exists(root / role) && Exists(root / kBackupName / role) && !Exists(root / kManifestName)) {
                    Rename(root / kBackupName / role, root / role);
                }
            }
            RemoveAll(root / kBackupName);
            touched = true;
        }
        if (touched) {
            info.action = RecoveryAction::CleanedUp;
            info.message = "Removed leftovers of an earlier import.";
        }
        return info;
    }

    auto kv = ReadKV(journal);
    const std::string state = Get(kv, "state");
    const std::string installId = Get(kv, "install_id");
    const std::string roles = Get(kv, "roles");

    if (state == "committing" && !installId.empty() && !roles.empty()) {
        auto mk = ReadKV(root / kManifestName);
        if (Get(mk, "install_id") == installId) {
            Cleanup(root);  // commit had finished; only the cleanup was interrupted
            info.action = RecoveryAction::CleanedUp;
            info.message = "Finished cleaning up a completed import.";
            return info;
        }
        // Roll forward: the staged tree was verified before the journal flipped to committing.
        bool ready = true;
        for (const char* role : kRoles) {
            if (CsvContains(roles, role) && !Exists(root / role) && !Exists(root / kStagingName / role)) ready = false;
        }
        if (!ready) {
            // Unexpected: restore whatever was parked so the previous install survives.
            for (const char* role : kRoles) {
                if (!Exists(root / role) && Exists(root / kBackupName / role)) Rename(root / kBackupName / role, root / role);
            }
            Cleanup(root);
            info.action = RecoveryAction::DiscardedUnusable;
            info.message = "An interrupted import could not be completed and was rolled back.";
            return info;
        }
        Hooks none;
        bool aborted = false;
        std::string err;
        if (!CommitSwap(root, roles, none, aborted, err)) {
            info.action = RecoveryAction::DiscardedUnusable;
            info.message = "Could not complete an interrupted import: " + err;
            return info;
        }
        // Rebuild the manifest from the trees now in place.
        const bool merged = !CsvContains(roles, "Generals");
        Report staged = Validate((root / "ZH").string(), merged ? std::string() : (root / "Generals").string());
        uint64_t bytes = 0;
        uint32_t files = 0;
        TreeStats(root / "ZH", bytes, files);
        if (!merged) TreeStats(root / "Generals", bytes, files);
        WriteManifest(root, installId, merged ? "ZH" : "Generals", merged, staged, bytes, files, Get(kv, "source_zh"));
        Cleanup(root);
        info.action = RecoveryAction::RolledForward;
        info.message = "Completed an interrupted import.";
        return info;
    }

    if (state == "copying" && !installId.empty() && Exists(root / kStagingName)) {
        info.action = RecoveryAction::PartialCopyKept;
        info.sourceZh = Get(kv, "source_zh");
        info.sourceBase = Get(kv, "source_base");
        info.bytesTotal = ToU64(kv, "bytes_total");
        info.filesTotal = static_cast<uint32_t>(ToU64(kv, "files_total"));
        TreeStats(root / kStagingName, info.bytesStaged, info.filesStaged);
        // Stale partial files are worthless.
        std::error_code ec;
        std::vector<fs::path> parts;
        for (fs::recursive_directory_iterator it(root / kStagingName, ec), end; it != end && !ec; it.increment(ec)) {
            std::error_code e2;
            if (it->is_regular_file(e2) && EndsWithNoCase(it->path().filename().string(), kPartSuffix)) parts.push_back(it->path());
        }
        for (const auto& p : parts) fs::remove(p, ec);
        info.message = "An earlier import was interrupted and can be resumed.";
        return info;
    }

    // Unreadable journal, or copying without a staging tree.
    RemoveAll(root / kStagingName);
    std::error_code ec;
    fs::remove(journal, ec);
    info.action = RecoveryAction::DiscardedUnusable;
    info.message = "Removed an unusable import record.";
    return info;
}

void DiscardPartialImport(const std::string& gameDataRoot) {
    const fs::path root(gameDataRoot);
    RemoveAll(root / kStagingName);
    std::error_code ec;
    fs::remove(root / kJournalName, ec);
}

bool ReadInstalled(const std::string& gameDataRoot, InstalledManifest& out) {
    const fs::path root(gameDataRoot);
    if (!Exists(root / kManifestName)) return false;
    auto kv = ReadKV(root / kManifestName);
    if (Get(kv, "gxgd-install") != "1") return false;
    const std::string zh = Get(kv, "zh"), base = Get(kv, "base");
    if (zh.empty() || base.empty()) return false;
    std::error_code ec;
    if (!fs::is_directory(root / zh, ec) || !fs::is_directory(root / base, ec)) return false;
    out = InstalledManifest();
    out.installId = Get(kv, "install_id");
    out.zhRoot = (root / zh).lexically_normal().string();
    out.baseRoot = (root / base).lexically_normal().string();
    out.baseMerged = Get(kv, "merged") == "1";
    out.complete = Get(kv, "complete") == "1";
    out.bytes = ToU64(kv, "bytes");
    out.files = static_cast<uint32_t>(ToU64(kv, "files"));
    out.sourceZh = Get(kv, "source_zh");
    out.languages = Get(kv, "languages");
    out.installedAtEpoch = static_cast<int64_t>(ToU64(kv, "installed_at"));
    return true;
}

bool RemoveInstalled(const std::string& gameDataRoot, std::string* error) {
    const fs::path root(gameDataRoot);
    std::error_code ec;
    for (const char* role : kRoles) fs::remove_all(root / role, ec);
    fs::remove(root / kManifestName, ec);
    fs::remove(root / kJournalName, ec);
    fs::remove_all(root / kStagingName, ec);
    fs::remove_all(root / kBackupName, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

}  // namespace gxgd
