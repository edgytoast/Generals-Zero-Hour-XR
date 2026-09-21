// GXXRFakeEngine.h - the GLES3 test scene as an engine client (launch argument -fakeEngine).
//
// It runs on the engine thread through exactly the path the real engine uses (head mailbox -> ring slot -> GL frame ->
// publish -> compositor), so it proves the decoupled compositor without game data:
//   -fakeEngineBoot <s>    boots for s seconds first (progress text, loading indicator in the compositor), default 3
//   -fakeEngineStall <s>   sleeps s seconds inside a frame every 10 s (simulated map load); the compositor must keep
//                          presenting the last frame at display rate
//   -fakeEngineFps <n>     frame cap of the fake engine (default 45)
#import <Foundation/Foundation.h>

#ifdef __cplusplus
extern "C" {
#endif
/// Starts the fake engine thread. Returns false when an engine was already started.
bool GXXRFakeEngineStart(void);
#ifdef __cplusplus
}
#endif
