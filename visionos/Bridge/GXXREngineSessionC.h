// GXXREngineSessionC.h - the plain-C part of GXXREngineSession.h, importable from Swift.
#ifndef GXXR_ENGINE_SESSION_C_H
#define GXXR_ENGINE_SESSION_C_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/// Starts the synthetic GLES3 test client on the engine thread (launch argument -fakeEngine): head mailbox -> ring
/// slot -> GL frame -> publish -> compositor, exactly like the real engine, without game data. Idempotent.
bool GXXRBridgeStartFakeEngine(void);
#ifdef __cplusplus
}
#endif
#endif
