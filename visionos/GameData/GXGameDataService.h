// GXGameDataService.h - Objective-C surface of the game-data subsystem for the Swift shell.
//
// A thin wrapper over the portable C++ validator/installer (GXGameDataValidator.h,
// GXGameDataInstall.h) plus the Apple-specific pieces: locations in the app container,
// coordinated reads of security-scoped sources (NSFileCoordinator), the free-space query,
// "exclude from backup", and the C snapshot read by the engine bridge (GXXRGameData.h).
//
// Every method here is synchronous and may take seconds (validation reads archive tables,
// import copies gigabytes): call them off the main thread.
#import <Foundation/Foundation.h>

#import "GXXRGameData.h"

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, GXGDVerdict) {
    GXGDVerdictNoSelection = 0,
    GXGDVerdictUnreadable,
    GXGDVerdictInstallerOnly,
    GXGDVerdictNoZeroHour,
    GXGDVerdictAmbiguous,
    GXGDVerdictIncomplete,
    GXGDVerdictReady,
};

typedef NS_ENUM(NSInteger, GXGDInstallOutcome) {
    GXGDInstallOutcomeInstalled = 0,
    GXGDInstallOutcomeCancelled,
    GXGDInstallOutcomeInsufficientSpace,
    GXGDInstallOutcomeInvalidSource,
    GXGDInstallOutcomeSourceChanged,
    GXGDInstallOutcomeVerificationFailed,
    GXGDInstallOutcomeIOError,
};

typedef NS_ENUM(NSInteger, GXGDPhase) {
    GXGDPhasePlanning = 0,
    GXGDPhaseCheckingSpace,
    GXGDPhaseCopying,
    GXGDPhaseVerifying,
    GXGDPhaseCommitting,
    GXGDPhaseCleaning,
    GXGDPhaseDone,
};

typedef NS_ENUM(NSInteger, GXGDRecoveryAction) {
    GXGDRecoveryActionNone = 0,
    GXGDRecoveryActionRolledForward,
    GXGDRecoveryActionCleanedUp,
    GXGDRecoveryActionPartialCopyKept,
    GXGDRecoveryActionDiscardedUnusable,
};

/// One missing required file, with the reason in plain words.
@interface GXGDMissingItem : NSObject
@property (nonatomic, readonly, copy) NSString *display;   // "MapsZH.big" or "Generals: Maps.big"
@property (nonatomic, readonly, copy) NSString *name;
@property (nonatomic, readonly, copy) NSString *reason;
@property (nonatomic, readonly) BOOL isBaseGenerals;
@end

/// Immutable snapshot of a validation run.
@interface GXGDReport : NSObject
@property (nonatomic, readonly, copy) NSString *selected;
@property (nonatomic, readonly, copy) NSString *zhRoot;      // "" when not found
@property (nonatomic, readonly, copy) NSString *baseRoot;    // "" when not found
@property (nonatomic, readonly, copy) NSString *baseLayout;  // none|merged|nested|sibling|explicit
@property (nonatomic, readonly) GXGDVerdict verdict;
@property (nonatomic, readonly) BOOL isReady;                // minimal data set present, playable
@property (nonatomic, readonly) BOOL isComplete;             // ready + movies + voice archives
@property (nonatomic, readonly) BOOL baseFolderMissing;
@property (nonatomic, readonly) BOOL installerDetected;
@property (nonatomic, readonly) BOOL weatherFound;
@property (nonatomic, readonly) BOOL languageFound;
@property (nonatomic, readonly, copy) NSArray<GXGDMissingItem *> *missing;
@property (nonatomic, readonly, copy) NSArray<NSString *> *damaged;         // "Name.big: reason"
@property (nonatomic, readonly, copy) NSArray<NSString *> *warnings;
@property (nonatomic, readonly, copy) NSArray<NSString *> *optionalMissing;
@property (nonatomic, readonly, copy) NSArray<NSString *> *languages;
@property (nonatomic, readonly, copy) NSArray<NSString *> *zhChoices;       // ambiguity list
@property (nonatomic, readonly) uint64_t totalBytesEstimate;
@property (nonatomic, readonly, copy) NSString *summary;                    // multi-line, human readable
@property (nonatomic, readonly, copy) NSString *dump;                       // key=value lines
@end

@interface GXGDProgress : NSObject
@property (nonatomic, readonly) GXGDPhase phase;
@property (nonatomic, readonly) uint64_t bytesDone;
@property (nonatomic, readonly) uint64_t bytesTotal;
@property (nonatomic, readonly) uint32_t filesDone;
@property (nonatomic, readonly) uint32_t filesTotal;
@property (nonatomic, readonly) uint64_t bytesReused;
@property (nonatomic, readonly) double bytesPerSecond;
@property (nonatomic, readonly, copy) NSString *currentFile;
@end

@interface GXGDInstallResult : NSObject
@property (nonatomic, readonly) GXGDInstallOutcome outcome;
@property (nonatomic, readonly, copy) NSString *message;
@property (nonatomic, readonly) BOOL resumable;
@property (nonatomic, readonly) uint64_t bytesNeeded;
@property (nonatomic, readonly) uint64_t bytesAvailable;
@property (nonatomic, readonly) uint32_t filesCopied;
@property (nonatomic, readonly) uint32_t filesReused;
@end

@interface GXGDRecovery : NSObject
@property (nonatomic, readonly) GXGDRecoveryAction action;
@property (nonatomic, readonly, copy) NSString *message;
@property (nonatomic, readonly, copy) NSString *sourceZh;
@property (nonatomic, readonly, copy) NSString *sourceBase;
@property (nonatomic, readonly) uint64_t bytesStaged;
@property (nonatomic, readonly) uint64_t bytesTotal;
@property (nonatomic, readonly) uint32_t filesStaged;
@property (nonatomic, readonly) uint32_t filesTotal;
@end

@interface GXGDInstalled : NSObject
@property (nonatomic, readonly, copy) NSString *zhRoot;
@property (nonatomic, readonly, copy) NSString *baseRoot;
@property (nonatomic, readonly) BOOL merged;
@property (nonatomic, readonly) BOOL complete;
@property (nonatomic, readonly) uint64_t bytes;
@property (nonatomic, readonly) uint32_t files;
@property (nonatomic, readonly, copy) NSString *languages;
@property (nonatomic, readonly, strong) NSDate *installedAt;
@end

/// Thread-safe cancellation flag polled by the copy loop between chunks.
@interface GXGDCancelToken : NSObject
- (void)cancel;
@property (nonatomic, readonly, getter=isCancelled) BOOL cancelled;
@end

@interface GXGameDataService : NSObject

/// <Application Support>/GeneralsX/GameData (created, excluded from backup): imported trees.
+ (NSString *)gameDataRootPath;
/// <Documents>/GameData (created; visible in the Files app): drop-in folder, used in place.
+ (NSString *)sharedDocumentsPath;
/// <Application Support>/GeneralsX/GeneralsZH: saves, Options.ini, maps, logs (created).
+ (NSString *)userDataRootPath;

/// True when the folder holds any non-hidden entry.
+ (BOOL)folderHasVisibleContent:(NSString *)path;

/// Validate a folder chosen by the user (Zero Hour folder or a parent that contains it).
+ (GXGDReport *)validateSelected:(NSString *)selected explicitBase:(nullable NSString *)base;

/// Finish or clean up an interrupted import. Call once at launch.
+ (GXGDRecovery *)recover;
+ (void)discardPartialImport;

/// The imported install (manifest + both trees), or nil.
+ (nullable GXGDInstalled *)installed;
+ (BOOL)removeInstalled:(NSError *_Nullable *_Nullable)error;

/// Copy a validated source into app storage. Blocking; report progress and honour `token`.
+ (GXGDInstallResult *)installReport:(GXGDReport *)report
                               token:(GXGDCancelToken *)token
                            progress:(void (^_Nullable)(GXGDProgress *progress))progress;

/// Mark a folder as excluded from device backup (best effort).
+ (void)excludeFromBackupAtPath:(NSString *)path;

/// Publish / retract the snapshot read through GXXRGameData_* (the engine bridge).
+ (void)publishReadyZeroHour:(NSString *)zhRoot base:(NSString *)baseRoot;
+ (void)retractReady;

@end

NS_ASSUME_NONNULL_END
