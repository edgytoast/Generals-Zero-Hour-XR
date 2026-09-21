// GXEngineHostEngine.h - INTERNAL: the real-engine half of the engine host, kept in a pure C++ translation unit
// (GXEngineHostEngine.cpp) because the engine headers (Lib/BaseType.h, ...) must not be mixed with Foundation in the
// same translation unit. GXEngineHost.mm (Objective-C++, thread / status / log) calls these on the engine thread.
#ifndef GX_ENGINE_HOST_ENGINE_H
#define GX_ENGINE_HOST_ENGINE_H

#include <stddef.h>

#include "GXEngineHost.h"
#include "GXEngineHostServices.h"

/* All functions: engine thread only, except InstallLogSink. */
bool GXEngineHostEngine_Boot(const GXEngineHostConfig *config, const GXHostGLInfo *gl, char *error, size_t errorCapacity);
void GXEngineHostEngine_Describe(const XRFrameInfo *head, GXHostFrameRequest *request);
bool GXEngineHostEngine_Frame(GXHostFrame *frame, GXHostFrameOutput *output);
void GXEngineHostEngine_SetPaused(bool paused);
void GXEngineHostEngine_Shutdown(void);
/* Effective simulation rate over the window since the last successful sample (false until minSeconds elapsed). */
bool GXEngineHostEngine_SampleLogicRate(double minSeconds, double *logicHz, double *engineFps, bool *inGame);
void GXEngineHostEngine_InstallLogSink(const char *path);
bool GXEngineHostEngine_IsInteractiveGame(void);

#endif
