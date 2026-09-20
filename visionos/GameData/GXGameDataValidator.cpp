// GXGameDataValidator.cpp - see GXGameDataValidator.h. Portable C++17, no Apple APIs.
#include "GXGameDataValidator.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace gxgd {

const char* const kBaseArchives[10] = {"INI.big",    "Terrain.big", "Textures.big", "W3D.big",   "Window.big",
                                       "Shaders.big", "Audio.big",   "Speech.big",   "Maps.big",  "Music.big"};
const char* const kZeroHourArchives[10] = {"INIZH.big",   "TerrainZH.big", "TexturesZH.big", "W3DZH.big",
                                           "WindowZH.big", "MapsZH.big",    "AudioZH.big",    "SpeechZH.big",
                                           "MusicZH.big",  "ShadersZH.big"};

// ---------------------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------------------

std::string LowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool EndsWithNoCase(const std::string& name, const std::string& suffix) {
    if (name.size() < suffix.size()) return false;
    return LowerAscii(name.substr(name.size() - suffix.size())) == LowerAscii(suffix);
}

std::string MissingItem::display() const {
    return scope == Scope::Generals ? "Generals: " + name : name;
}

namespace {

// Absolute, lexically normalised path without a trailing separator.
std::string AbsNorm(const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    if (ec) a = p;
    a = a.lexically_normal();
    std::string s = a.string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

struct DirListing {
    bool ok = false;
    std::vector<fs::path> files;  // regular files (symlinks followed, like java.io.File.isFile)
    std::vector<fs::path> dirs;   // directories (symlinks followed)
};

DirListing ListDir(const std::string& dir) {
    DirListing out;
    std::error_code ec;
    fs::directory_iterator it(fs::path(dir), fs::directory_options::skip_permission_denied, ec);
    if (ec) return out;
    out.ok = true;
    for (fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code e2;
        const fs::directory_entry& de = *it;
        if (de.is_directory(e2)) out.dirs.push_back(de.path());
        else if (de.is_regular_file(e2)) out.files.push_back(de.path());
    }
    return out;
}

bool IsHiddenName(const fs::path& p) {
    const std::string n = p.filename().string();
    return !n.empty() && n[0] == '.';
}

bool NameEqualsNoCase(const fs::path& p, const std::string& name) {
    return LowerAscii(p.filename().string()) == LowerAscii(name);
}

uint64_t FileSizeOr0(const fs::path& p) {
    std::error_code ec;
    auto s = fs::file_size(p, ec);
    return ec ? 0 : static_cast<uint64_t>(s);
}

// Java: loose(root, "a/b/c") - walk components case-insensitively; final must be a
// non-empty regular file.
bool Loose(const std::string& root, const std::string& path) {
    if (root.empty()) return false;
    fs::path current(root);
    std::stringstream ss(path);
    std::string component;
    while (std::getline(ss, component, '/')) {
        DirListing l = ListDir(current.string());
        if (!l.ok) return false;
        bool found = false;
        for (const auto& d : l.dirs) {
            if (NameEqualsNoCase(d, component)) { current = d; found = true; break; }
        }
        if (!found) {
            for (const auto& f : l.files) {
                if (NameEqualsNoCase(f, component)) { current = f; found = true; break; }
            }
        }
        if (!found) return false;
    }
    std::error_code ec;
    return fs::is_regular_file(current, ec) && FileSizeOr0(current) > 0;
}

// Java: looseLanguage(root). Also records the language folder names found.
bool LooseLanguage(const std::string& root, std::vector<std::string>* languages) {
    if (root.empty()) return false;
    DirListing top = ListDir(root);
    if (!top.ok) return false;
    bool found = false;
    for (const auto& d : top.dirs) {
        if (!NameEqualsNoCase(d, "data")) continue;
        DirListing langs = ListDir(d.string());
        for (const auto& lang : langs.dirs) {
            if (Loose(lang.string(), "generals.csf") || Loose(lang.string(), "generals.str")) {
                found = true;
                if (languages) languages->push_back(lang.filename().string());
            }
        }
    }
    return found;
}

// Bounded search for any *.bik below <root>/Data (depth <= 3, <= 512 folders).
bool LooseVideos(const std::string& root) {
    if (root.empty()) return false;
    DirListing top = ListDir(root);
    if (!top.ok) return false;
    int visited = 0;
    std::vector<std::pair<fs::path, int>> stack;
    for (const auto& d : top.dirs) {
        if (NameEqualsNoCase(d, "data")) stack.emplace_back(d, 0);
    }
    while (!stack.empty()) {
        auto [dir, depth] = stack.back();
        stack.pop_back();
        if (++visited > 512) return false;
        DirListing l = ListDir(dir.string());
        for (const auto& f : l.files) {
            if (EndsWithNoCase(f.filename().string(), ".bik") && FileSizeOr0(f) > 0) return true;
        }
        if (depth < 3) {
            for (const auto& d : l.dirs) {
                if (!IsHiddenName(d)) stack.emplace_back(d, depth + 1);
            }
        }
    }
    return false;
}

void Discover(const fs::path& dir, const std::string& marker, int depth, std::vector<std::string>& out,
              std::set<std::string>& seen, int& visited) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || visited++ >= kMaxFoldersVisited) return;
    fs::path canon = fs::canonical(dir, ec);
    if (ec) return;
    if (!seen.insert(canon.string()).second) return;
    if (!NamedFile(dir.string(), marker).empty()) {
        out.push_back(AbsNorm(dir));
        return;
    }
    if (depth >= kMaxSearchDepth) return;
    DirListing l = ListDir(dir.string());
    std::sort(l.dirs.begin(), l.dirs.end(),
              [](const fs::path& a, const fs::path& b) { return a.filename().string() < b.filename().string(); });
    for (const auto& child : l.dirs) {
        if (!IsHiddenName(child)) Discover(child, marker, depth + 1, out, seen, visited);
    }
}

// --- BIGF reading ---------------------------------------------------------------------

class BufferedReader {
public:
    explicit BufferedReader(FILE* f) : f_(f) {}
    // Returns false at end of file.
    bool ReadByte(uint8_t& b) {
        if (pos_ == len_) {
            len_ = fread(buf_, 1, sizeof(buf_), f_);
            pos_ = 0;
            if (len_ == 0) return false;
        }
        b = buf_[pos_++];
        return true;
    }
    bool ReadU32BE(uint32_t& v) {
        uint8_t b[4];
        for (int i = 0; i < 4; i++) {
            if (!ReadByte(b[i])) return false;
        }
        v = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
        return true;
    }

private:
    FILE* f_;
    uint8_t buf_[64 * 1024];
    size_t pos_ = 0, len_ = 0;
};

bool IsWeatherEntry(const std::string& normalized) { return normalized == "data/ini/default/weather.ini"; }

// data/<lang>/generals.(csf|str), <lang> non-empty and without '/'.
bool IsLanguageEntry(const std::string& normalized, std::string* lang) {
    if (normalized.compare(0, 5, "data/") != 0) return false;
    size_t slash = normalized.find('/', 5);
    if (slash == std::string::npos || slash == 5) return false;
    const std::string leaf = normalized.substr(slash + 1);
    if (leaf != "generals.csf" && leaf != "generals.str") return false;
    if (lang) *lang = normalized.substr(5, slash - 5);
    return true;
}

std::string Capitalize(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

struct ArchiveFindings {
    bool weather = false;
    bool language = false;
    bool video = false;
    bool zeroSize = false;
    bool over2GiB = false;
    std::vector<std::string> languages;
};

}  // namespace

std::string NamedFile(const std::string& dir, const std::string& name) {
    if (dir.empty()) return std::string();
    DirListing l = ListDir(dir);
    if (!l.ok) return std::string();
    for (const auto& f : l.files) {
        if (NameEqualsNoCase(f, name)) return AbsNorm(f);
    }
    return std::string();
}

std::vector<std::string> FindCandidates(const std::string& root, const std::string& marker) {
    std::vector<std::string> out;
    if (root.empty()) return out;
    std::set<std::string> seen;
    int visited = 0;
    Discover(fs::path(root), marker, 0, out, seen, visited);
    std::sort(out.begin(), out.end());
    return out;
}

BigArchiveInfo ReadBigArchive(const std::string& path, const std::function<void(const BigEntry&)>& onEntry) {
    BigArchiveInfo info;
    std::error_code ec;
    const uint64_t length = static_cast<uint64_t>(fs::file_size(fs::path(path), ec));
    if (ec) {
        info.error = "cannot read file size";
        return info;
    }
    info.fileSize = length;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        info.error = "cannot open file";
        return info;
    }
    struct Closer { FILE* f; ~Closer() { fclose(f); } } closer{f};
    BufferedReader in(f);

    uint8_t magic[4];
    if (length < 16) { info.error = "BIGF header"; return info; }
    for (int i = 0; i < 4; i++) {
        if (!in.ReadByte(magic[i])) { info.error = "BIGF header"; return info; }
    }
    if (memcmp(magic, "BIGF", 4) != 0) { info.error = "BIGF header"; return info; }
    uint32_t unusedSize = 0, count = 0, tableEnd = 0;
    // Retail total-size field is not used by the native loader.
    if (!in.ReadU32BE(unusedSize) || !in.ReadU32BE(count) || !in.ReadU32BE(tableEnd)) {
        info.error = "BIGF header";
        return info;
    }
    if (uint64_t(count) > kMaxArchiveEntries || tableEnd < 16 || uint64_t(tableEnd) > length) {
        info.error = "BIGF table";
        return info;
    }
    info.entryCount = count;
    info.tableEnd = tableEnd;

    uint64_t position = 16;
    for (uint64_t i = 0; i < count; i++) {
        BigEntry e;
        uint32_t offset = 0, size = 0;
        if (!in.ReadU32BE(offset) || !in.ReadU32BE(size)) { info.error = "BIGF table (truncated)"; return info; }
        position += 8;
        std::string name;
        for (;;) {
            uint8_t b;
            if (!in.ReadByte(b)) { info.error = "BIGF name (truncated)"; return info; }
            if (b == 0) break;
            position++;
            if (name.size() >= kMaxArchivePathLength || position >= tableEnd) { info.error = "BIGF name"; return info; }
            name.push_back(static_cast<char>(b));
        }
        position++;
        // Retail PatchZH.big contains a zero-size Data\* placeholder at offset 0. Empty
        // entries have no payload to overlap the table.
        if (position > tableEnd || (size > 0 && offset < tableEnd) || uint64_t(offset) > length ||
            uint64_t(size) > length - offset) {
            info.error = "BIGF payload bounds";
            return info;
        }
        e.offset = offset;
        e.size = size;
        e.path = std::move(name);
        if (onEntry) onEntry(e);
    }
    info.ok = true;
    return info;
}

bool VerifyBigArchive(const std::string& path, uint64_t expectedSize, std::string* error) {
    BigArchiveInfo info = ReadBigArchive(path);
    if (!info.ok) {
        if (error) *error = info.error;
        return false;
    }
    if (expectedSize != UINT64_MAX && info.fileSize != expectedSize) {
        if (error) *error = "size mismatch (" + std::to_string(info.fileSize) + " bytes, expected " +
                            std::to_string(expectedSize) + ")";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------------------

bool Report::ready() const {
    return !zhRoot.empty() && !baseRoot.empty() && missing.empty() && damaged.empty() && !unreadable &&
           weatherFound && languageFound;
}

Verdict Report::verdict() const {
    if (selected.empty()) return Verdict::NoSelection;
    if (zhRoot.empty()) {
        if (installerDetected) return Verdict::InstallerOnly;
        if (unreadable) return Verdict::Unreadable;
        if (zhChoices.size() > 1) return Verdict::Ambiguous;
        return Verdict::NoZeroHour;
    }
    if (unreadable) return Verdict::Unreadable;
    return ready() ? Verdict::Ready : Verdict::Incomplete;
}

namespace {

const char* LayoutName(BaseLayout l) {
    switch (l) {
        case BaseLayout::None: return "none";
        case BaseLayout::Merged: return "merged";
        case BaseLayout::Nested: return "nested";
        case BaseLayout::Sibling: return "sibling";
        case BaseLayout::Explicit: return "explicit";
    }
    return "none";
}

const char* VerdictName(Verdict v) {
    switch (v) {
        case Verdict::NoSelection: return "no-selection";
        case Verdict::Unreadable: return "unreadable";
        case Verdict::InstallerOnly: return "installer-only";
        case Verdict::NoZeroHour: return "no-zero-hour";
        case Verdict::Ambiguous: return "ambiguous";
        case Verdict::Incomplete: return "incomplete";
        case Verdict::Ready: return "ready";
    }
    return "?";
}

std::string FormatBytes(uint64_t b) {
    char buf[64];
    if (b >= (1ULL << 30)) snprintf(buf, sizeof(buf), "%.2f GB", double(b) / double(1ULL << 30));
    else if (b >= (1ULL << 20)) snprintf(buf, sizeof(buf), "%.1f MB", double(b) / double(1ULL << 20));
    else snprintf(buf, sizeof(buf), "%llu bytes", (unsigned long long)b);
    return buf;
}

}  // namespace

std::string Report::dump() const {
    std::ostringstream o;
    o << "verdict=" << VerdictName(verdict()) << "\n";
    o << "ready=" << (ready() ? 1 : 0) << "\n";
    o << "complete=" << (complete() ? 1 : 0) << "\n";
    o << "zhRoot=" << zhRoot << "\n";
    o << "baseRoot=" << baseRoot << "\n";
    o << "baseLayout=" << LayoutName(baseLayout) << "\n";
    o << "choices=" << zhChoices.size() << "\n";
    o << "installer=" << (installerDetected ? 1 : 0) << "\n";
    o << "unreadable=" << (unreadable ? 1 : 0) << "\n";
    o << "weather=" << (weatherFound ? 1 : 0) << "\n";
    o << "language=" << (languageFound ? 1 : 0) << "\n";
    o << "videos=" << (videosFound ? 1 : 0) << "\n";
    o << "voiceArchives=" << (voiceArchivesFound ? 1 : 0) << "\n";
    o << "archiveBytes=" << totalBytesEstimate() << "\n";
    for (const auto& m : missing) o << "missing=" << m.display() << " | " << m.reason << "\n";
    for (const auto& d : damaged) o << "damaged=" << d.path << " | " << d.reason << "\n";
    for (const auto& w : warnings) o << "warning=" << w << "\n";
    for (const auto& l : languages) o << "lang=" << l << "\n";
    return o.str();
}

std::string Report::summary() const {
    std::ostringstream o;
    switch (verdict()) {
        case Verdict::NoSelection: o << "No folder chosen."; break;
        case Verdict::Unreadable: o << "The chosen folder could not be read."; break;
        case Verdict::InstallerOnly:
            o << "This looks like an installer or disc image (.iso, .cab, .msi, setup.exe), not installed game data. "
                 "Install or extract the game on a computer first.";
            break;
        case Verdict::NoZeroHour:
            o << "No Zero Hour data found (looked for INIZH.big up to " << kMaxSearchDepth << " folders deep).";
            break;
        case Verdict::Ambiguous:
            o << "Several Zero Hour installs were found. Choose one folder:";
            for (const auto& c : zhChoices) o << "\n  " << c;
            break;
        case Verdict::Incomplete:
        case Verdict::Ready: {
            o << "Zero Hour: " << zhRoot << "\nGenerals:  " << (baseRoot.empty() ? "(not found)" : baseRoot);
            if (baseRoot.empty()) {
                o << "\nThe base Generals folder (the one containing Terrain.big) was not found.";
            }
            if (baseLayout != BaseLayout::None) o << "\nBase layout: " << LayoutName(baseLayout);
            for (const auto& m : missing) {
                if (baseFolderMissing && m.scope == Scope::Generals) continue;
                o << "\nMissing: " << m.display() << " - " << m.reason;
            }
            for (const auto& d : damaged) o << "\nDamaged: " << d.path << " - " << d.reason;
            if (!weatherFound) o << "\nMissing: Data/INI/Default/Weather.ini";
            if (!languageFound) o << "\nMissing: Zero Hour language text (Data/<Language>/generals.csf or .str)";
            for (const auto& w : warnings) o << "\nNote: " << w;
            for (const auto& s : optionalMissing) o << "\nOptional: " << s;
            o << "\nArchive size: " << FormatBytes(totalBytesEstimate());
            if (ready()) o << (complete() ? "\nReady (complete data set)." : "\nReady (minimal data set).");
            break;
        }
    }
    return o.str();
}

// ---------------------------------------------------------------------------------------
// Validate
// ---------------------------------------------------------------------------------------

namespace {

struct Inspector {
    Report& r;
    std::set<std::string> seen;
    ArchiveFindings zhFindings;    // aggregated over the Zero Hour tree
    ArchiveFindings baseFindings;  // aggregated over the base tree

    // Java inspect(root, r, seen). Records damaged/unreadable in the report.
    void InspectRoot(const std::string& root, Scope scope, ArchiveFindings& agg, uint64_t& bytes) {
        DirListing l = ListDir(root);
        if (!l.ok) {
            r.unreadable = true;
            return;
        }
        std::vector<fs::path> archives;
        for (const auto& f : l.files) {
            if (EndsWithNoCase(f.filename().string(), ".big")) archives.push_back(f);
        }
        std::sort(archives.begin(), archives.end(),
                  [](const fs::path& a, const fs::path& b) { return a.filename().string() < b.filename().string(); });
        for (const auto& file : archives) {
            std::error_code ec;
            fs::path canon = fs::canonical(file, ec);
            if (ec) {
                r.damaged.push_back({AbsNorm(file), "cannot resolve path"});
                continue;
            }
            if (!seen.insert(canon.string()).second) continue;
            InspectArchive(file, scope, agg, bytes);
        }
    }

    void InspectArchive(const fs::path& file, Scope scope, ArchiveFindings& agg, uint64_t& bytes) {
        ArchiveSummary summary;
        summary.path = AbsNorm(file);
        summary.name = file.filename().string();
        summary.scope = scope;
        summary.sizeBytes = FileSizeOr0(file);
        ArchiveFindings local;
        BigArchiveInfo info = ReadBigArchive(summary.path, [&](const BigEntry& e) {
            std::string n = e.path;
            std::replace(n.begin(), n.end(), '\\', '/');
            n = LowerAscii(n);
            if (e.size == 0) {
                local.zeroSize = true;
                return;
            }
            if (e.size >= 0x80000000u || e.offset >= 0x80000000u) local.over2GiB = true;
            if (IsWeatherEntry(n)) local.weather = true;
            std::string lang;
            if (IsLanguageEntry(n, &lang)) {
                local.language = true;
                local.languages.push_back(Capitalize(lang));
            }
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".bik") == 0) local.video = true;
        });
        summary.entryCount = info.entryCount;
        bytes += summary.sizeBytes;
        if (!info.ok) {
            summary.damaged = true;
            r.damaged.push_back({summary.path, info.error});
            r.archives.push_back(summary);
            return;
        }
        r.archives.push_back(summary);
        agg.weather |= local.weather;
        agg.language |= local.language;
        agg.video |= local.video;
        for (auto& l : local.languages) agg.languages.push_back(l);
        const std::string lname = LowerAscii(summary.name);
        if (local.over2GiB) {
            r.warnings.push_back(summary.name +
                                 ": contains an entry beyond 2 GiB; the engine reads BIGF offsets as signed 32-bit values.");
        }
        if (local.zeroSize && lname.compare(0, 5, "patch") == 0) {
            r.warnings.push_back(summary.name +
                                 ": contains empty placeholder entries (normal for retail patch archives; tolerated).");
        }
    }
};

}  // namespace

Report Validate(const std::string& selected, const std::string& explicitBase) {
    Report r;
    r.selected = selected;
    r.explicitBase = explicitBase;
    if (selected.empty()) return r;

    std::error_code ec;
    fs::path sel(selected);
    if (!fs::is_directory(sel, ec) || !ListDir(selected).ok) {
        r.unreadable = true;
        r.installerDetected = EndsWithNoCase(sel.filename().string(), ".iso");
        return r;
    }

    r.zhChoices = FindCandidates(selected, "INIZH.big");
    if (r.zhChoices.size() != 1) {
        DirListing l = ListDir(selected);
        for (const auto& f : l.files) {
            const std::string n = LowerAscii(f.filename().string());
            if (EndsWithNoCase(n, ".iso") || EndsWithNoCase(n, ".cab") || EndsWithNoCase(n, ".msi") || n == "setup.exe")
                r.installerDetected = true;
        }
        return r;
    }
    r.zhRoot = r.zhChoices[0];

    for (const char* n : kZeroHourArchives) {
        if (NamedFile(r.zhRoot, n).empty()) {
            r.missing.push_back({Scope::ZeroHour, n, "Zero Hour archive not found next to INIZH.big"});
        }
    }

    // Resolve the base Generals folder.
    if (!explicitBase.empty()) {
        auto bases = FindCandidates(explicitBase, "Terrain.big");
        if (bases.size() == 1) {
            r.baseRoot = bases[0];
            r.baseLayout = BaseLayout::Explicit;
        }
    } else if (!NamedFile(r.zhRoot, "Terrain.big").empty()) {
        r.baseRoot = r.zhRoot;
        r.baseLayout = BaseLayout::Merged;
    } else {
        // Store the resolved base path explicitly: never promise a directory the engine's
        // native mount search would not visit.
        auto bases = FindCandidates(r.zhRoot, "Terrain.big");
        if (bases.size() == 1) {
            r.baseRoot = bases[0];
            r.baseLayout = BaseLayout::Nested;
        }
        if (r.baseRoot.empty()) {
            fs::path parent = fs::path(r.zhRoot).parent_path();
            if (!parent.empty()) {
                DirListing siblings = ListDir(parent.string());
                std::string only;
                bool ambiguous = false;
                for (const auto& s : siblings.dirs) {
                    if (!NamedFile(s.string(), "Terrain.big").empty()) {
                        if (!only.empty()) { ambiguous = true; break; }
                        only = AbsNorm(s);
                    }
                }
                if (!ambiguous && !only.empty()) {
                    r.baseRoot = only;
                    r.baseLayout = BaseLayout::Sibling;
                }
            }
        }
    }
    r.baseFolderMissing = r.baseRoot.empty();
    for (const char* n : kBaseArchives) {
        if (r.baseRoot.empty()) {
            r.missing.push_back({Scope::Generals, n, "base Generals folder not found (no folder with Terrain.big)"});
        } else if (NamedFile(r.baseRoot, n).empty()) {
            r.missing.push_back({Scope::Generals, n, "base Generals archive not found next to Terrain.big"});
        }
    }

    Inspector inspector{r, {}, {}, {}};
    inspector.InspectRoot(r.zhRoot, Scope::ZeroHour, inspector.zhFindings, r.zhArchiveBytes);
    std::vector<std::string> looseLangs;
    const bool expansionLanguage = inspector.zhFindings.language || LooseLanguage(r.zhRoot, &looseLangs);
    if (!r.baseRoot.empty() && r.baseRoot != r.zhRoot) {
        inspector.InspectRoot(r.baseRoot, Scope::Generals, inspector.baseFindings, r.baseArchiveBytes);
    }
    r.weatherFound = inspector.zhFindings.weather || inspector.baseFindings.weather ||
                     Loose(r.zhRoot, "Data/INI/Default/Weather.ini") || Loose(r.baseRoot, "Data/INI/Default/Weather.ini");
    // A base-only string table cannot supply Zero Hour's additional UI.
    r.languageFound = expansionLanguage;

    std::set<std::string> langSet;
    for (const auto& l : inspector.zhFindings.languages) langSet.insert(l);
    for (const auto& l : looseLangs) langSet.insert(Capitalize(l));
    r.languages.assign(langSet.begin(), langSet.end());

    // Complete-set indicators (heuristics, never blocking).
    r.videosFound = inspector.zhFindings.video || inspector.baseFindings.video || LooseVideos(r.zhRoot) ||
                    (!r.baseRoot.empty() && r.baseRoot != r.zhRoot && LooseVideos(r.baseRoot));
    for (const auto& a : r.archives) {
        if (a.scope != Scope::ZeroHour || a.damaged) continue;
        const std::string n = LowerAscii(a.name);
        if (n.size() > 6 && n.compare(n.size() - 6, 6, "zh.big") == 0 &&
            (n.compare(0, 5, "audio") == 0 || n.compare(0, 6, "speech") == 0) && n != "audiozh.big" &&
            n != "speechzh.big") {
            r.voiceArchivesFound = true;
        }
    }
    if (!r.videosFound) {
        r.optionalMissing.push_back(
            "No intro or campaign videos (*.bik) were found under Data/. The game plays without them, but movies will be skipped.");
    }
    if (!r.voiceArchivesFound) {
        r.optionalMissing.push_back(
            "No language voice archives (for example AudioEnglishZH.big, SpeechEnglishZH.big) were found. Spoken audio may be missing.");
    }
    return r;
}

}  // namespace gxgd
