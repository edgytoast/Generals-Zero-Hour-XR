// GeneralsX @test visionOS port - the handful of SDL3 symbols the d3d8gles translation units
// reference. In XR mode the backend touches none of the window/GL-context functions (they belong to
// the desktop/Android windowed path, which is never entered when D3D8GLES_XRConfig is set); it
// only needs SDL_GetTicks for its two-second perf log line. Stubs let the device test link
// without building SDL3. Signatures match SDL3 3.4 (checked against SDL_timer.h / SDL_video.h).
#include <cstdint>
#include <chrono>

extern "C" {
uint64_t SDL_GetTicks(void)
{
	using namespace std::chrono;
	static const steady_clock::time_point t0 = steady_clock::now();
	return (uint64_t)duration_cast<milliseconds>(steady_clock::now() - t0).count();
}
const char *SDL_GetError(void) { return "SDL stub"; }
void *SDL_GL_CreateContext(void *) { return nullptr; }
bool SDL_GL_MakeCurrent(void *, void *) { return false; }
bool SDL_GL_SetSwapInterval(int) { return false; }
bool SDL_GL_GetSwapInterval(int *) { return false; }
bool SDL_GL_SwapWindow(void *) { return false; }
bool SDL_GetWindowSizeInPixels(void *, int *, int *) { return false; }
}
