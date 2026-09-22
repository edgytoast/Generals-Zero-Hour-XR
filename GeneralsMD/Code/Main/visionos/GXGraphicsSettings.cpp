// GXGraphicsSettings.cpp - storage, validation and text form of the graphics / performance settings (GXEngineHost.h).
//
// Plain C++ with no engine and no Apple dependency, so scripts/qa/vision-presentation-test.sh compiles it on the host.
// Threading: Set/Get may be called from any thread (one mutex). The engine thread calls
// GXGraphicsSettings_ConsumeForEngine once per frame; it copies the requested settings into the "applied" slot and
// reports whether they changed since its previous call.
#include "GXEngineHost.h"
#include "GXGraphicsSettings.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {
std::mutex g_lock;
GXGraphicsSettings g_requested;
GXGraphicsSettings g_applied;
bool g_initialised = false;
uint32_t g_generation = 0;        // increments when a Set changed the requested settings
uint32_t g_appliedGeneration = 0;

void ensureInit()
{
	if (g_initialised) return;
	g_initialised = true;
	g_requested = gxGraphicsDefaults();
	g_applied = g_requested;
}

// Copies `count` bytes of the settings that lie inside [0, size) of the caller's struct.
void copyPrefix(GXGraphicsSettings &dst, const GXGraphicsSettings &src, uint32_t size)
{
	const uint32_t bytes = size < sizeof(GXGraphicsSettings) ? size : (uint32_t)sizeof(GXGraphicsSettings);
	const uint32_t keep = dst.size;
	memcpy(&dst, &src, bytes);
	dst.size = keep;
}
} // namespace

extern "C" {

void GXEngineHost_GetGraphicsDefaults(GXGraphicsSettings *out)
{
	if (out == nullptr || out->size < (uint32_t)offsetof(GXGraphicsSettings, renderScale) + sizeof(float)) return;
	copyPrefix(*out, gxGraphicsDefaults(), out->size);
}

void GXEngineHost_SanitizeGraphics(GXGraphicsSettings *settings)
{
	if (settings == nullptr) return;
	gxGraphicsSanitize(*settings);
}

bool GXEngineHost_SetGraphics(const GXGraphicsSettings *settings)
{
	if (settings == nullptr || settings->size < (uint32_t)(offsetof(GXGraphicsSettings, renderScale) + sizeof(float))) return false;
	std::lock_guard<std::mutex> lock(g_lock);
	ensureInit();
	GXGraphicsSettings next = g_requested;
	// Only the fields the caller's struct really has.
	const uint32_t size = settings->size < sizeof(GXGraphicsSettings) ? settings->size : (uint32_t)sizeof(GXGraphicsSettings);
	GXGraphicsSettings in = {};
	memcpy(&in, settings, size); // never read past the caller's (older, smaller) struct
	const auto has = [&](size_t offset, size_t bytes) { return offset + bytes <= size; };
	if (has(offsetof(GXGraphicsSettings, renderScale), sizeof(float))) next.renderScale = in.renderScale;
	if (has(offsetof(GXGraphicsSettings, renderFpsCap), sizeof(int32_t))) next.renderFpsCap = in.renderFpsCap;
	if (has(offsetof(GXGraphicsSettings, shadowMode), sizeof(int32_t))) next.shadowMode = in.shadowMode;
	if (has(offsetof(GXGraphicsSettings, eyeTier), sizeof(int32_t))) next.eyeTier = in.eyeTier;
	if (has(offsetof(GXGraphicsSettings, uiResolution), sizeof(int32_t))) next.uiResolution = in.uiResolution;
	if (has(offsetof(GXGraphicsSettings, flags), sizeof(uint32_t))) next.flags = in.flags;
	gxGraphicsSanitize(next);
	if (!gxGraphicsEqual(next, g_requested)) {
		g_requested = next;
		++g_generation;
	}
	return true;
}

void GXEngineHost_GetGraphics(GXGraphicsSettings *out)
{
	if (out == nullptr || out->size < (uint32_t)(offsetof(GXGraphicsSettings, renderScale) + sizeof(float))) return;
	std::lock_guard<std::mutex> lock(g_lock);
	ensureInit();
	copyPrefix(*out, g_requested, out->size);
}

void GXEngineHost_GetAppliedGraphics(GXGraphicsSettings *out)
{
	if (out == nullptr || out->size < (uint32_t)(offsetof(GXGraphicsSettings, renderScale) + sizeof(float))) return;
	std::lock_guard<std::mutex> lock(g_lock);
	ensureInit();
	copyPrefix(*out, g_applied, out->size);
}

uint32_t GXEngineHost_GetGraphicsGeneration(void)
{
	std::lock_guard<std::mutex> lock(g_lock);
	ensureInit();
	return g_generation;
}

size_t GXEngineHost_FormatGraphics(const GXGraphicsSettings *settings, char *out, size_t capacity)
{
	if (settings == nullptr || out == nullptr || capacity == 0) return 0;
	GXGraphicsSettings s = gxGraphicsDefaults();
	copyPrefix(s, *settings, settings->size);
	gxGraphicsSanitize(s);
	const int n = snprintf(out, capacity, "scale=%.3f fps=%d shadows=%d tier=%d ui=%d flags=%u", (double)s.renderScale, (int)s.renderFpsCap,
		(int)s.shadowMode, (int)s.eyeTier, (int)s.uiResolution, (unsigned)s.flags);
	return n < 0 ? 0 : (size_t)n;
}

bool GXEngineHost_ParseGraphics(const char *line, GXGraphicsSettings *out)
{
	if (line == nullptr || out == nullptr || out->size < (uint32_t)(offsetof(GXGraphicsSettings, renderScale) + sizeof(float))) return false;
	GXGraphicsSettings s = gxGraphicsDefaults();
	{
		std::lock_guard<std::mutex> lock(g_lock);
		ensureInit();
		s = g_requested;
	}
	bool any = false;
	const char *p = line;
	while (*p != '\0') {
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
		if (*p == '\0') break;
		char key[16] = {};
		size_t k = 0;
		while (*p != '\0' && *p != '=' && *p != ' ' && k + 1 < sizeof(key)) key[k++] = *p++;
		if (*p != '=') { // a token without '=' is not ours
			while (*p != '\0' && *p != ' ') ++p;
			continue;
		}
		++p;
		char *end = nullptr;
		const double value = strtod(p, &end);
		if (end == p || !std::isfinite(value)) return false;
		p = end;
		if (!strcmp(key, "scale")) { s.renderScale = (float)value; any = true; }
		else if (!strcmp(key, "fps")) { s.renderFpsCap = (int32_t)value; any = true; }
		else if (!strcmp(key, "shadows")) { s.shadowMode = (int32_t)value; any = true; }
		else if (!strcmp(key, "tier")) { s.eyeTier = (int32_t)value; any = true; }
		else if (!strcmp(key, "ui")) { s.uiResolution = (int32_t)value; any = true; }
		else if (!strcmp(key, "flags")) { s.flags = (uint32_t)value; any = true; }
	}
	if (!any) return false;
	gxGraphicsSanitize(s);
	copyPrefix(*out, s, out->size);
	return true;
}

} // extern "C"

// ---- engine-thread side --------------------------------------------------------------------------------------------

bool GXGraphicsSettings_ConsumeForEngine(GXGraphicsSettings *applied, uint32_t *generation)
{
	std::lock_guard<std::mutex> lock(g_lock);
	ensureInit();
	const bool changed = g_appliedGeneration != g_generation;
	if (changed) {
		g_applied = g_requested;
		g_appliedGeneration = g_generation;
	}
	if (applied != nullptr) *applied = g_applied;
	if (generation != nullptr) *generation = g_appliedGeneration;
	return changed;
}

uint32_t GXGraphicsSettings_AppliedGeneration()
{
	std::lock_guard<std::mutex> lock(g_lock);
	ensureInit();
	return g_appliedGeneration;
}

GXGraphicsSettings GXGraphicsSettings_Requested()
{
	std::lock_guard<std::mutex> lock(g_lock);
	ensureInit();
	return g_requested;
}

void GXGraphicsSettings_ResetForTest()
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_initialised = false;
	g_generation = 0;
	g_appliedGeneration = 0;
	ensureInit();
}
