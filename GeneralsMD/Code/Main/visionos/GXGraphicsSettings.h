// GXGraphicsSettings.h - pure helpers behind the GXGraphicsSettings C API of GXEngineHost.h (defaults, clamping) and the
// engine-thread side of the hand-off. No engine or Apple dependencies (host tested).
#pragma once

#include <cmath>
#include <cstddef>

#include "GXEngineHost.h"

namespace gxgfx {
constexpr float kMinRenderScale = 0.5f, kMaxRenderScale = 1.5f;
constexpr int kMinFpsCap = 30, kMaxFpsCap = 120, kDefaultFpsCap = 45;
} // namespace gxgfx

inline GXGraphicsSettings gxGraphicsDefaults()
{
	GXGraphicsSettings s = {};
	s.size = (uint32_t)sizeof(GXGraphicsSettings);
	s.renderScale = 1.0f;
	s.renderFpsCap = gxgfx::kDefaultFpsCap;
	s.shadowMode = GX_SHADOWS_DECALS;
	s.eyeTier = GX_EYE_BALANCED;
	s.uiResolution = GX_UI_720P;
	s.flags = GX_GFX_FLAGS_DEFAULT;
	return s;
}

// Clamps every field to its documented range. `size` is left alone.
inline void gxGraphicsSanitize(GXGraphicsSettings &s)
{
	if (!std::isfinite(s.renderScale)) s.renderScale = 1.0f;
	if (s.renderScale < gxgfx::kMinRenderScale) s.renderScale = gxgfx::kMinRenderScale;
	if (s.renderScale > gxgfx::kMaxRenderScale) s.renderScale = gxgfx::kMaxRenderScale;
	if (s.renderFpsCap == 0) s.renderFpsCap = gxgfx::kDefaultFpsCap;
	else if (s.renderFpsCap < 0) s.renderFpsCap = -1;
	else if (s.renderFpsCap < gxgfx::kMinFpsCap) s.renderFpsCap = gxgfx::kMinFpsCap;
	else if (s.renderFpsCap > gxgfx::kMaxFpsCap) s.renderFpsCap = gxgfx::kMaxFpsCap;
	if (s.shadowMode < GX_SHADOWS_OFF || s.shadowMode > GX_SHADOWS_VOLUMES) s.shadowMode = GX_SHADOWS_DECALS;
	if (s.eyeTier < GX_EYE_BALANCED || s.eyeTier > GX_EYE_ULTRA) s.eyeTier = GX_EYE_BALANCED;
	if (s.uiResolution < GX_UI_720P || s.uiResolution > GX_UI_1080P) s.uiResolution = GX_UI_720P;
	s.flags &= GX_GFX_FLAGS_DEFAULT;
}

inline bool gxGraphicsEqual(const GXGraphicsSettings &a, const GXGraphicsSettings &b)
{
	return a.renderScale == b.renderScale && a.renderFpsCap == b.renderFpsCap && a.shadowMode == b.shadowMode &&
		a.eyeTier == b.eyeTier && a.uiResolution == b.uiResolution && a.flags == b.flags;
}

// UI backbuffer size of a GXUIResolution value.
inline void gxUIResolutionSize(int32_t uiResolution, int &width, int &height)
{
	if (uiResolution == GX_UI_1080P) { width = 1920; height = 1080; }
	else { width = 1280; height = 720; }
}

// Engine thread: copies the requested settings into the applied slot when a Set happened since the last call.
// Returns true when the applied settings changed. Both out pointers are optional.
bool GXGraphicsSettings_ConsumeForEngine(GXGraphicsSettings *applied, uint32_t *generation);
GXGraphicsSettings GXGraphicsSettings_Requested();
uint32_t GXGraphicsSettings_AppliedGeneration();
void GXGraphicsSettings_ResetForTest();
