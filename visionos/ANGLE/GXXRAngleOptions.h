// GXXRAngleOptions.h - launch-argument switches for the ANGLE ring and the fake engine (test / bring-up only).
//   -angleEyeScale <f>        ring eye targets = drawable viewport x f (default 1.0)
//   -angleAtlas               both eyes side by side in one STEREO_LEFT target
//   -angleSync glfinish       force the CPU glFinish fallback instead of MTLSharedEvent fences
//   -angleTargetFormat rgba|bgra
//   -angleNoUIPanel           fake engine: skip the 1280x720 UI panel test
//   -fakeEngine               run the GLES3 test scene as an engine client on the engine thread (alias: -angleTestScene)
//   -fakeEngineStall <s>      the fake engine sleeps s seconds every 10 s (simulated map load)
//   -fakeEngineBoot <s>       the fake engine "boots" for s seconds first (default 3)
//   -fakeEngineFps <n>        fake engine frame cap (default 45)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>

struct GXXRAngleOptions {
    bool fakeEngine = false;
    bool atlas = false;
    bool forceGLFinish = false;
    bool noUIPanel = false;
    float eyeScale = 1.0f;
    MTLPixelFormat format = MTLPixelFormatRGBA8Unorm;
    double fakeStallSeconds = 0;
    double fakeBootSeconds = 3;
    int fakeFps = 45;
};

inline const GXXRAngleOptions& GXXRAngleLaunchOptions() {
    static GXXRAngleOptions o = [] {
        GXXRAngleOptions r;
        NSArray<NSString*>* a = NSProcessInfo.processInfo.arguments;
        for (NSUInteger i = 0; i < a.count; ++i) {
            NSString* arg = a[i];
            NSString* next = i + 1 < a.count ? a[i + 1] : @"";
            if ([arg isEqualToString:@"-angleTestScene"] || [arg isEqualToString:@"-fakeEngine"]) r.fakeEngine = true;
            else if ([arg isEqualToString:@"-angleAtlas"]) r.atlas = true;
            else if ([arg isEqualToString:@"-angleNoUIPanel"]) r.noUIPanel = true;
            else if ([arg isEqualToString:@"-angleSync"] && [next isEqualToString:@"glfinish"]) r.forceGLFinish = true;
            else if ([arg isEqualToString:@"-angleEyeScale"]) r.eyeScale = std::max(0.1f, std::min(2.0f, next.floatValue));
            else if ([arg isEqualToString:@"-angleTargetFormat"]) r.format = [next isEqualToString:@"bgra"] ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
            else if ([arg isEqualToString:@"-fakeEngineStall"]) r.fakeStallSeconds = std::max(0.0, next.doubleValue);
            else if ([arg isEqualToString:@"-fakeEngineBoot"]) r.fakeBootSeconds = std::max(0.0, next.doubleValue);
            else if ([arg isEqualToString:@"-fakeEngineFps"]) r.fakeFps = std::max(5, std::min(240, next.intValue));
        }
        return r;
    }();
    return o;
}
