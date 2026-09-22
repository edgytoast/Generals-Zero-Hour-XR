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
/* Reason recorded by the last failed boot / frame (empty when the engine simply quit). */
const char *GXEngineHostEngine_LastError(void);
bool GXEngineHostEngine_IsInteractiveGame(void);
/* Package C2: the graphics settings the engine thread applies from now on (called by the loop when GXGraphicsSettings_ConsumeForEngine reports a change). */
void GXEngineHostEngine_ApplyGraphics(const GXGraphicsSettings *applied);
/* Types text into the focused text entry (see XrGameBoot_TextInput); `replace` erases the existing content first. False when no entry is focused. */
bool GXEngineHostEngine_TextInput(const char *utf8, bool replace, bool enter);
/* Installed on XrGameBoot_SetLoadingPresenter (engine thread, called from INSIDE a blocked XrGameBoot_Frame): publishes one nested
 * frame of the engine's loading screen without stepping the simulation. */
void GXEngineHostEngine_PresentLoading();

#endif
