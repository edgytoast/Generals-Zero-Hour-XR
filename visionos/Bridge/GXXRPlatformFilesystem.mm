// GXXRPlatformFilesystem.mm - implements PlatformFilesystem.h for visionOS.
#import <Foundation/Foundation.h>
#include <cstring>
#include <mutex>

#include "PlatformFilesystem.h"

namespace {

NSString* const kBookmarkDefaultsKey = @"GXXRGameDataBookmark";

bool copyOut(NSString* s, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (!s) return false;
    const char* u = s.fileSystemRepresentation;
    if (!u || strlen(u) + 1 > cap) return false;
    strcpy(out, u);
    return true;
}

NSString* ensureDir(NSSearchPathDirectory dir, NSString* leaf) {
    NSURL* base = [[NSFileManager defaultManager] URLForDirectory:dir inDomain:NSUserDomainMask appropriateForURL:nil create:YES error:nil];
    if (!base) return nil;
    NSURL* u = leaf ? [base URLByAppendingPathComponent:leaf isDirectory:YES] : base;
    [[NSFileManager defaultManager] createDirectoryAtURL:u withIntermediateDirectories:YES attributes:nil error:nil];
    return u.path;
}

// Resolve the bookmarked folder if one was stored. `stale` and errors fall back to Documents.
NSURL* resolveBookmark() {
    NSData* data = [[NSUserDefaults standardUserDefaults] dataForKey:kBookmarkDefaultsKey];
    if (!data) return nil;
    BOOL stale = NO;
    NSError* err = nil;
    NSURL* url = [NSURL URLByResolvingBookmarkData:data options:0 relativeToURL:nil bookmarkDataIsStale:&stale error:&err];
    return url;
}

}  // namespace

struct PlatformFSAccess {
    NSURL* url;
};

extern "C" {

bool PlatformFS_GetAppDataDir(char* out_path, size_t capacity) {
    return copyOut(ensureDir(NSApplicationSupportDirectory, @"GeneralsZHXR"), out_path, capacity);
}

bool PlatformFS_GetCacheDir(char* out_path, size_t capacity) {
    return copyOut(ensureDir(NSCachesDirectory, @"GeneralsZHXR"), out_path, capacity);
}

bool PlatformFS_GetGameDataDir(char* out_path, size_t capacity) {
    if (NSURL* b = resolveBookmark()) return copyOut(b.path, out_path, capacity);
    return copyOut(ensureDir(NSDocumentDirectory, @"GameData"), out_path, capacity);
}

bool PlatformFS_GameDataLooksPresent(void) {
    char path[1024];
    if (!PlatformFS_GetGameDataDir(path, sizeof(path))) return false;
    NSArray* items = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:[NSString stringWithUTF8String:path] error:nil];
    for (NSString* n in items) {
        if (![n hasPrefix:@"."]) return true;
    }
    return false;
}

bool PlatformFS_SetGameDataBookmark(const void* bookmark, size_t length) {
    if (!bookmark || length == 0) return false;
    [[NSUserDefaults standardUserDefaults] setObject:[NSData dataWithBytes:bookmark length:length] forKey:kBookmarkDefaultsKey];
    return true;
}

void PlatformFS_ClearGameDataBookmark(void) {
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:kBookmarkDefaultsKey];
}

PlatformFSAccessHandle PlatformFS_BeginGameDataAccess(void) {
    NSURL* url = resolveBookmark();
    if (!url) return nullptr;  // Documents/GameData needs no scoped access
    if (![url startAccessingSecurityScopedResource]) return nullptr;
    PlatformFSAccess* a = new PlatformFSAccess{url};
    return a;
}

void PlatformFS_EndGameDataAccess(PlatformFSAccessHandle handle) {
    if (!handle) return;
    [handle->url stopAccessingSecurityScopedResource];
    delete handle;
}

}  // extern "C"
