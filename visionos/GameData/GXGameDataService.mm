// GXGameDataService.mm - Apple glue around the portable validator/installer.
#import "GXGameDataService.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "GXGameDataInstall.h"
#include "GXGameDataValidator.h"

// ---------------------------------------------------------------------------------------
// C snapshot for the engine bridge
// ---------------------------------------------------------------------------------------

namespace {
std::mutex g_pathsMutex;
std::string g_zhRoot, g_baseRoot, g_userRoot;
bool g_ready = false;

bool copyOut(const std::string& s, char* out, size_t cap) {
    if (!out || cap == 0) return true;  // caller skipped this path
    if (s.size() + 1 > cap) {
        out[0] = 0;
        return false;
    }
    memcpy(out, s.c_str(), s.size() + 1);
    return true;
}
}  // namespace

extern "C" {

bool GXXRGameData_IsReady(void) {
    std::lock_guard<std::mutex> lock(g_pathsMutex);
    return g_ready;
}

bool GXXRGameData_GetPaths(char* zhRoot, size_t zhCap, char* baseRoot, size_t baseCap, char* userDataRoot, size_t userCap) {
    std::lock_guard<std::mutex> lock(g_pathsMutex);
    if (!g_ready) {
        if (zhRoot && zhCap) zhRoot[0] = 0;
        if (baseRoot && baseCap) baseRoot[0] = 0;
        if (userDataRoot && userCap) userDataRoot[0] = 0;
        return false;
    }
    bool ok = copyOut(g_zhRoot, zhRoot, zhCap);
    ok = copyOut(g_baseRoot, baseRoot, baseCap) && ok;
    ok = copyOut(g_userRoot, userDataRoot, userCap) && ok;
    return ok;
}

void GXXRGameData_SetPaths(const char* zhRoot, const char* baseRoot, const char* userDataRoot) {
    std::lock_guard<std::mutex> lock(g_pathsMutex);
    g_zhRoot = zhRoot ? zhRoot : "";
    g_baseRoot = (baseRoot && baseRoot[0]) ? baseRoot : g_zhRoot;
    g_userRoot = userDataRoot ? userDataRoot : "";
    g_ready = !g_zhRoot.empty();
}

void GXXRGameData_Clear(void) {
    std::lock_guard<std::mutex> lock(g_pathsMutex);
    g_zhRoot.clear();
    g_baseRoot.clear();
    g_userRoot.clear();
    g_ready = false;
}

}  // extern "C"

// ---------------------------------------------------------------------------------------
// Objective-C value classes
// ---------------------------------------------------------------------------------------

static NSString* NS(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

static NSArray<NSString*>* NSStrings(const std::vector<std::string>& v) {
    NSMutableArray* a = [NSMutableArray arrayWithCapacity:v.size()];
    for (const auto& s : v) [a addObject:NS(s)];
    return a;
}

@interface GXGDMissingItem ()
@property (nonatomic, readwrite, copy) NSString* display;
@property (nonatomic, readwrite, copy) NSString* name;
@property (nonatomic, readwrite, copy) NSString* reason;
@property (nonatomic, readwrite) BOOL isBaseGenerals;
@end
@implementation GXGDMissingItem
@end

@interface GXGDReport () {
@public
    std::shared_ptr<gxgd::Report> _cpp;
}
@property (nonatomic, readwrite, copy) NSString* selected;
@property (nonatomic, readwrite, copy) NSString* zhRoot;
@property (nonatomic, readwrite, copy) NSString* baseRoot;
@property (nonatomic, readwrite, copy) NSString* baseLayout;
@property (nonatomic, readwrite) GXGDVerdict verdict;
@property (nonatomic, readwrite) BOOL isReady;
@property (nonatomic, readwrite) BOOL isComplete;
@property (nonatomic, readwrite) BOOL baseFolderMissing;
@property (nonatomic, readwrite) BOOL installerDetected;
@property (nonatomic, readwrite) BOOL weatherFound;
@property (nonatomic, readwrite) BOOL languageFound;
@property (nonatomic, readwrite, copy) NSArray<GXGDMissingItem*>* missing;
@property (nonatomic, readwrite, copy) NSArray<NSString*>* damaged;
@property (nonatomic, readwrite, copy) NSArray<NSString*>* warnings;
@property (nonatomic, readwrite, copy) NSArray<NSString*>* optionalMissing;
@property (nonatomic, readwrite, copy) NSArray<NSString*>* languages;
@property (nonatomic, readwrite, copy) NSArray<NSString*>* zhChoices;
@property (nonatomic, readwrite) uint64_t totalBytesEstimate;
@property (nonatomic, readwrite, copy) NSString* summary;
@property (nonatomic, readwrite, copy) NSString* dump;
@end

@implementation GXGDReport
- (instancetype)initWithCpp:(const gxgd::Report&)r {
    if ((self = [super init])) {
        _cpp = std::make_shared<gxgd::Report>(r);
        _selected = NS(r.selected);
        _zhRoot = NS(r.zhRoot);
        _baseRoot = NS(r.baseRoot);
        static const char* const layouts[] = {"none", "merged", "nested", "sibling", "explicit"};
        _baseLayout = NS(layouts[static_cast<int>(r.baseLayout)]);
        switch (r.verdict()) {
            case gxgd::Verdict::NoSelection: _verdict = GXGDVerdictNoSelection; break;
            case gxgd::Verdict::Unreadable: _verdict = GXGDVerdictUnreadable; break;
            case gxgd::Verdict::InstallerOnly: _verdict = GXGDVerdictInstallerOnly; break;
            case gxgd::Verdict::NoZeroHour: _verdict = GXGDVerdictNoZeroHour; break;
            case gxgd::Verdict::Ambiguous: _verdict = GXGDVerdictAmbiguous; break;
            case gxgd::Verdict::Incomplete: _verdict = GXGDVerdictIncomplete; break;
            case gxgd::Verdict::Ready: _verdict = GXGDVerdictReady; break;
        }
        _isReady = r.ready();
        _isComplete = r.complete();
        _baseFolderMissing = r.baseFolderMissing;
        _installerDetected = r.installerDetected;
        _weatherFound = r.weatherFound;
        _languageFound = r.languageFound;
        NSMutableArray* missing = [NSMutableArray array];
        for (const auto& m : r.missing) {
            GXGDMissingItem* item = [GXGDMissingItem new];
            item.display = NS(m.display());
            item.name = NS(m.name);
            item.reason = NS(m.reason);
            item.isBaseGenerals = (m.scope == gxgd::Scope::Generals);
            [missing addObject:item];
        }
        _missing = missing;
        NSMutableArray* damaged = [NSMutableArray array];
        for (const auto& d : r.damaged) {
            NSString* leaf = NS(d.path).lastPathComponent;
            [damaged addObject:[NSString stringWithFormat:@"%@: %@", leaf, NS(d.reason)]];
        }
        _damaged = damaged;
        _warnings = NSStrings(r.warnings);
        _optionalMissing = NSStrings(r.optionalMissing);
        _languages = NSStrings(r.languages);
        _zhChoices = NSStrings(r.zhChoices);
        _totalBytesEstimate = r.totalBytesEstimate();
        _summary = NS(r.summary());
        _dump = NS(r.dump());
    }
    return self;
}
@end

@interface GXGDProgress ()
@property (nonatomic, readwrite) GXGDPhase phase;
@property (nonatomic, readwrite) uint64_t bytesDone;
@property (nonatomic, readwrite) uint64_t bytesTotal;
@property (nonatomic, readwrite) uint32_t filesDone;
@property (nonatomic, readwrite) uint32_t filesTotal;
@property (nonatomic, readwrite) uint64_t bytesReused;
@property (nonatomic, readwrite) double bytesPerSecond;
@property (nonatomic, readwrite, copy) NSString* currentFile;
@end
@implementation GXGDProgress
@end

@interface GXGDInstallResult ()
@property (nonatomic, readwrite) GXGDInstallOutcome outcome;
@property (nonatomic, readwrite, copy) NSString* message;
@property (nonatomic, readwrite) BOOL resumable;
@property (nonatomic, readwrite) uint64_t bytesNeeded;
@property (nonatomic, readwrite) uint64_t bytesAvailable;
@property (nonatomic, readwrite) uint32_t filesCopied;
@property (nonatomic, readwrite) uint32_t filesReused;
@end
@implementation GXGDInstallResult
@end

@interface GXGDRecovery ()
@property (nonatomic, readwrite) GXGDRecoveryAction action;
@property (nonatomic, readwrite, copy) NSString* message;
@property (nonatomic, readwrite, copy) NSString* sourceZh;
@property (nonatomic, readwrite, copy) NSString* sourceBase;
@property (nonatomic, readwrite) uint64_t bytesStaged;
@property (nonatomic, readwrite) uint64_t bytesTotal;
@property (nonatomic, readwrite) uint32_t filesStaged;
@property (nonatomic, readwrite) uint32_t filesTotal;
@end
@implementation GXGDRecovery
@end

@interface GXGDInstalled ()
@property (nonatomic, readwrite, copy) NSString* zhRoot;
@property (nonatomic, readwrite, copy) NSString* baseRoot;
@property (nonatomic, readwrite) BOOL merged;
@property (nonatomic, readwrite) BOOL complete;
@property (nonatomic, readwrite) uint64_t bytes;
@property (nonatomic, readwrite) uint32_t files;
@property (nonatomic, readwrite, copy) NSString* languages;
@property (nonatomic, readwrite, strong) NSDate* installedAt;
@end
@implementation GXGDInstalled
@end

@implementation GXGDCancelToken {
    std::atomic<bool> _flag;
}
- (void)cancel {
    _flag.store(true);
}
- (BOOL)isCancelled {
    return _flag.load();
}
@end

// ---------------------------------------------------------------------------------------
// Service
// ---------------------------------------------------------------------------------------

@implementation GXGameDataService

+ (NSURL*)directory:(NSSearchPathDirectory)dir leaf:(NSString*)leaf {
    NSFileManager* fm = [NSFileManager defaultManager];
    NSURL* base = [fm URLForDirectory:dir inDomain:NSUserDomainMask appropriateForURL:nil create:YES error:nil];
    NSURL* url = base ? [base URLByAppendingPathComponent:leaf isDirectory:YES] : [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:leaf] isDirectory:YES];
    [fm createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:nil];
    return url;
}

+ (NSString*)gameDataRootPath {
    NSURL* url = [self directory:NSApplicationSupportDirectory leaf:@"GeneralsX/GameData"];
    [self excludeFromBackupAtPath:url.path];
    return url.path;
}

+ (NSString*)sharedDocumentsPath {
    NSURL* url = [self directory:NSDocumentDirectory leaf:@"GameData"];
    return url.path;
}

+ (NSString*)userDataRootPath {
    return [self directory:NSApplicationSupportDirectory leaf:@"GeneralsX/GeneralsZH"].path;
}

+ (BOOL)folderHasVisibleContent:(NSString*)path {
    NSArray* items = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:path error:nil];
    for (NSString* n in items) {
        if (![n hasPrefix:@"."]) return YES;
    }
    return NO;
}

+ (void)excludeFromBackupAtPath:(NSString*)path {
    NSURL* url = [NSURL fileURLWithPath:path];
    NSError* err = nil;
    [url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&err];
}

+ (GXGDReport*)validateSelected:(NSString*)selected explicitBase:(NSString*)base {
    gxgd::Report r = gxgd::Validate(selected.UTF8String ?: "", base.length ? base.UTF8String : "");
    return [[GXGDReport alloc] initWithCpp:r];
}

+ (GXGDRecovery*)recover {
    gxgd::RecoveryInfo info = gxgd::Recover(self.gameDataRootPath.UTF8String);
    GXGDRecovery* out = [GXGDRecovery new];
    out.action = (GXGDRecoveryAction)static_cast<int>(info.action);
    out.message = NS(info.message);
    out.sourceZh = NS(info.sourceZh);
    out.sourceBase = NS(info.sourceBase);
    out.bytesStaged = info.bytesStaged;
    out.bytesTotal = info.bytesTotal;
    out.filesStaged = info.filesStaged;
    out.filesTotal = info.filesTotal;
    return out;
}

+ (void)discardPartialImport {
    gxgd::DiscardPartialImport(self.gameDataRootPath.UTF8String);
}

+ (GXGDInstalled*)installed {
    gxgd::InstalledManifest m;
    if (!gxgd::ReadInstalled(self.gameDataRootPath.UTF8String, m)) return nil;
    GXGDInstalled* out = [GXGDInstalled new];
    out.zhRoot = NS(m.zhRoot);
    out.baseRoot = NS(m.baseRoot);
    out.merged = m.baseMerged;
    out.complete = m.complete;
    out.bytes = m.bytes;
    out.files = m.files;
    out.languages = NS(m.languages);
    out.installedAt = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)m.installedAtEpoch];
    return out;
}

+ (BOOL)removeInstalled:(NSError**)error {
    std::string err;
    if (gxgd::RemoveInstalled(self.gameDataRootPath.UTF8String, &err)) return YES;
    if (error) *error = [NSError errorWithDomain:@"GXGameData" code:1 userInfo:@{NSLocalizedDescriptionKey: NS(err)}];
    return NO;
}

+ (GXGDInstallResult*)installReport:(GXGDReport*)report token:(GXGDCancelToken*)token progress:(void (^)(GXGDProgress*))progressBlock {
    NSString* homePath = NSHomeDirectory();
    NSString* root = self.gameDataRootPath;

    gxgd::Hooks hooks;
    hooks.cancelled = [token] { return token.isCancelled == YES; };
    if (progressBlock) {
        hooks.progress = [progressBlock](const gxgd::Progress& p) {
            GXGDProgress* out = [GXGDProgress new];
            out.phase = (GXGDPhase)static_cast<int>(p.phase);
            out.bytesDone = p.bytesDone;
            out.bytesTotal = p.bytesTotal;
            out.filesDone = p.filesDone;
            out.filesTotal = p.filesTotal;
            out.bytesReused = p.bytesReused;
            out.bytesPerSecond = p.bytesPerSecond;
            out.currentFile = NS(p.currentFile);
            progressBlock(out);
        };
    }
    hooks.freeSpace = [](const std::string& path, uint64_t* out) {
        @autoreleasepool {
            NSURL* url = [NSURL fileURLWithPath:NS(path)];
            NSError* err = nil;
            NSDictionary* v = [url resourceValuesForKeys:@[NSURLVolumeAvailableCapacityForImportantUsageKey, NSURLVolumeAvailableCapacityKey] error:&err];
            NSNumber* n = v[NSURLVolumeAvailableCapacityForImportantUsageKey];
            if (n == nil || n.longLongValue <= 0) n = v[NSURLVolumeAvailableCapacityKey];
            if (n == nil || n.longLongValue <= 0) return false;
            *out = (uint64_t)n.longLongValue;
            return true;
        }
    };
    hooks.excludeFromBackup = [](const std::string& path) {
        @autoreleasepool {
            [GXGameDataService excludeFromBackupAtPath:NS(path)];
        }
    };
    // Coordinated reads for sources outside the app container (Files providers, iCloud Drive, USB).
    // Coordination makes a not-yet-downloaded iCloud file materialise before it is read.
    hooks.readSource = [homePath](const std::string& src, const std::function<bool(const std::string&)>& body) {
        @autoreleasepool {
            NSString* path = NS(src);
            if ([path hasPrefix:homePath]) return body(src);
            NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
            NSError* coordError = nil;
            __block bool result = false;
            __block bool ran = false;
            [coordinator coordinateReadingItemAtURL:[NSURL fileURLWithPath:path]
                                            options:0
                                              error:&coordError
                                         byAccessor:^(NSURL* readable) {
                                             ran = true;
                                             result = body(std::string(readable.fileSystemRepresentation));
                                         }];
            if (!ran) return body(src);  // coordination unavailable: read directly, verification still applies
            return result;
        }
    };

    gxgd::InstallResult r = gxgd::Install(root.UTF8String, *report->_cpp, hooks);
    GXGDInstallResult* out = [GXGDInstallResult new];
    switch (r.outcome) {
        case gxgd::InstallOutcome::Installed: out.outcome = GXGDInstallOutcomeInstalled; break;
        case gxgd::InstallOutcome::Cancelled: out.outcome = GXGDInstallOutcomeCancelled; break;
        case gxgd::InstallOutcome::InsufficientSpace: out.outcome = GXGDInstallOutcomeInsufficientSpace; break;
        case gxgd::InstallOutcome::InvalidSource: out.outcome = GXGDInstallOutcomeInvalidSource; break;
        case gxgd::InstallOutcome::SourceChanged: out.outcome = GXGDInstallOutcomeSourceChanged; break;
        case gxgd::InstallOutcome::VerificationFailed: out.outcome = GXGDInstallOutcomeVerificationFailed; break;
        default: out.outcome = GXGDInstallOutcomeIOError; break;
    }
    out.message = NS(r.message);
    out.resumable = r.resumable;
    out.bytesNeeded = r.bytesNeeded;
    out.bytesAvailable = r.bytesAvailable;
    out.filesCopied = r.filesCopied;
    out.filesReused = r.filesReused;
    return out;
}

+ (void)publishReadyZeroHour:(NSString*)zhRoot base:(NSString*)baseRoot {
    GXXRGameData_SetPaths(zhRoot.fileSystemRepresentation, baseRoot.fileSystemRepresentation, self.userDataRootPath.fileSystemRepresentation);
}

+ (void)retractReady {
    GXXRGameData_Clear();
}

@end
