// GeneralsX @test visionOS port - the cases of the d3d8gles-on-ANGLE-Metal device test.
//
// Everything here goes through the real backend: Direct3DCreate8_GLES -> IDirect3D8 ->
// IDirect3DDevice8 (fixed-function D3D8 API, exactly what the engine calls) plus the d3d8gles_*
// XR C API. Render targets are host-supplied Private-storage MTLTextures wrapped by the Objective-C++
// half (vision-gles-device-test.mm); pixels are verified from the Metal side.
//
// Frame protocol exercised (docs/visionos-gles-backend.md):
//   makeCurrent; d3d8gles_SetXRHostTargets(slot targets); d3d8gles_BeginXRFrame; engine frame
//   (Clear, BeginXRStereo, world draws, EndXRStereo, BeginXRUI, UI draws, Present);
//   glFlush + shared-event signal; d3d8gles_InvalidateCachedState(); composite (here: Metal blit).
#include <d3d8.h>
#include "d3d8gles.h"
#include "vision-gles-harness.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdarg>

// ------------------------------------------------------------------------------------------
// tiny test framework
// ------------------------------------------------------------------------------------------
static Harness *H = nullptr;
static int *g_checks = nullptr;
static int g_failures = 0;

static void check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(bool ok, const char *fmt, ...)
{
	++*g_checks;
	va_list ap;
	va_start(ap, fmt);
	char buf[512];
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (!ok) { ++g_failures; printf("  FAIL: %s\n", buf); }
	else printf("  ok:   %s\n", buf);
}

// ------------------------------------------------------------------------------------------
// column-major GL math (same convention as the XR host: OpenGL clip space, z in [-1,1])
// ------------------------------------------------------------------------------------------
typedef float Mat[16];
static void matIdentity(Mat m) { memset(m, 0, sizeof(Mat)); m[0] = m[5] = m[10] = m[15] = 1.f; }
static void matMul(Mat out, const Mat a, const Mat b) // out = a * b
{
	Mat r;
	for (int c = 0; c < 4; ++c)
		for (int rr = 0; rr < 4; ++rr) {
			float s = 0;
			for (int k = 0; k < 4; ++k) s += a[k * 4 + rr] * b[c * 4 + k];
			r[c * 4 + rr] = s;
		}
	memcpy(out, r, sizeof(Mat));
}
static void matPerspective(Mat m, float fovY, float aspect, float n, float f)
{
	memset(m, 0, sizeof(Mat));
	const float t = 1.f / tanf(fovY * 0.5f);
	m[0] = t / aspect; m[5] = t;
	m[10] = (f + n) / (n - f); m[11] = -1.f; m[14] = 2.f * f * n / (n - f);
}
static void matTranslate(Mat m, float x, float y, float z) { matIdentity(m); m[12] = x; m[13] = y; m[14] = z; }
static void projectToPixel(const Mat clip, float x, float y, float z, int rectX, int rectY, int rectW, int rectH, int *px, int *py)
{
	const float cx = clip[0] * x + clip[4] * y + clip[8] * z + clip[12];
	const float cy = clip[1] * x + clip[5] * y + clip[9] * z + clip[13];
	const float cw = clip[3] * x + clip[7] * y + clip[11] * z + clip[15];
	*px = rectX + (int)floorf((cx / cw * 0.5f + 0.5f) * rectW);
	*py = rectY + (int)floorf((cy / cw * 0.5f + 0.5f) * rectH); // GL window y (0 = bottom of the GL image)
}

struct RGBA { int r, g, b, a; };
static bool nearInt(int a, int b, int tol) { return abs(a - b) <= tol; }
static bool sameColor(RGBA a, RGBA b, int tol) { return nearInt(a.r, b.r, tol) && nearInt(a.g, b.g, tol) && nearInt(a.b, b.b, tol) && nearInt(a.a, b.a, tol); }

// ------------------------------------------------------------------------------------------
// device bring-up
// ------------------------------------------------------------------------------------------
static const int W = 256, Hh = 256; // engine backbuffer == GAME/WORLD/UI slot size
static IDirect3D8 *g_d3d = nullptr;
static IDirect3DDevice8 *g_dev = nullptr;
static D3D8GLES_XRConfig g_cfg; // must outlive the device

static bool createDevice()
{
	g_cfg = D3D8GLES_XRConfig{};
	g_cfg.getProcAddress = H->getProcAddress;
	g_cfg.flags = 0;
	d3d8gles_SetXRConfig(&g_cfg);
	g_d3d = Direct3DCreate8_GLES(D3D_SDK_VERSION);
	if (!g_d3d) return false;
	D3DPRESENT_PARAMETERS pp = {};
	pp.BackBufferWidth = W; pp.BackBufferHeight = Hh; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.Windowed = TRUE;
	pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	return SUCCEEDED(g_d3d->CreateDevice(0, D3DDEVTYPE_HAL, nullptr, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_dev));
}

// The fixed-function state a normal engine draw would have set.
static void baseState()
{
	g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
	g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	g_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
	g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	g_dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
	g_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	g_dev->SetRenderState(D3DRS_AMBIENT, 0);
	g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
	g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
	g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	g_dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	g_dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
	g_dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
	g_dev->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
	g_dev->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
	g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

static void setViewport()
{
	D3DVIEWPORT8 vp = { 0, 0, (DWORD)W, (DWORD)Hh, 0.f, 1.f };
	g_dev->SetViewport(&vp);
}

// ------------------------------------------------------------------------------------------
// textures
// ------------------------------------------------------------------------------------------
static IDirect3DTexture8 *makeTexture(int w, int h, D3DFORMAT fmt, const std::vector<uint8_t> &bytes)
{
	IDirect3DTexture8 *t = nullptr;
	if (FAILED(g_dev->CreateTexture(w, h, 1, 0, fmt, D3DPOOL_MANAGED, &t)) || !t) return nullptr;
	D3DLOCKED_RECT lr = {};
	if (FAILED(t->LockRect(0, &lr, nullptr, 0))) return nullptr;
	// Callers hand over bytes in the level's own packed layout (pitch == packed row size).
	memcpy(lr.pBits, bytes.data(), bytes.size());
	t->UnlockRect(0);
	return t;
}

// 4x4 A8R8G8B8 texture: texel (i,j) = (R=40+60i, G=40+60j, B=128, A=255)
static IDirect3DTexture8 *makeCheckTexture()
{
	std::vector<uint8_t> b(4 * 4 * 4);
	for (int j = 0; j < 4; ++j)
		for (int i = 0; i < 4; ++i) {
			uint8_t *p = &b[(j * 4 + i) * 4];
			p[0] = 128; p[1] = 40 + 60 * j; p[2] = 40 + 60 * i; p[3] = 255; // B G R A in memory
		}
	return makeTexture(4, 4, D3DFMT_A8R8G8B8, b);
}
static RGBA checkTexel(int i, int j) { return { 40 + 60 * i, 40 + 60 * j, 128, 255 }; }

// ------------------------------------------------------------------------------------------
// world quad in the XY plane, normal +Z, uv (i,j) grid like checkTexel
// ------------------------------------------------------------------------------------------
struct WorldVertex { float x, y, z, nx, ny, nz, u, v; };
static const DWORD kWorldFVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;

static void drawWorldQuad(float half)
{
	auto V = [&](float x, float y) { return WorldVertex{ x, y, 0.f, 0.f, 0.f, 1.f, (x + half) / (2 * half), (half - y) / (2 * half) }; };
	WorldVertex q[6] = { V(-half, half), V(half, half), V(-half, -half), V(half, half), V(half, -half), V(-half, -half) };
	g_dev->SetVertexShader(kWorldFVF);
	g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(WorldVertex));
}

static void setLight(float dirX, float dirY, float dirZ)
{
	D3DLIGHT8 l = {};
	l.Type = D3DLIGHT_DIRECTIONAL;
	l.Diffuse = { 1.f, 1.f, 1.f, 1.f };
	l.Direction = { dirX, dirY, dirZ };
	g_dev->SetLight(0, &l);
	g_dev->LightEnable(0, TRUE);
	D3DMATERIAL8 m = {};
	m.Diffuse = { 1.f, 1.f, 1.f, 1.f };
	g_dev->SetMaterial(&m);
	g_dev->SetRenderState(D3DRS_LIGHTING, TRUE);
}

// A perspective 3D draw needs a D3D-style projection (m44 == 0) for the backend to classify it as
// a world draw; the stereo path then replaces gl_Position with uXrEyeClip * world position.
static void setWorldTransforms()
{
	D3DMATRIX id = {}; id._11 = id._22 = id._33 = id._44 = 1.f;
	D3DMATRIX proj = {}; proj._11 = 1.7f; proj._22 = 1.7f; proj._33 = 1.01f; proj._34 = 1.f; proj._43 = -1.01f; // _44 = 0
	g_dev->SetTransform(D3DTS_WORLD, &id);
	g_dev->SetTransform(D3DTS_VIEW, &id);
	g_dev->SetTransform(D3DTS_PROJECTION, &proj);
}

// ------------------------------------------------------------------------------------------
// XR frame helpers
// ------------------------------------------------------------------------------------------
struct EyeSetup {
	Mat clip[2];
	Mat board;
	Mat camera;
	float aspect = 0.75f;
};

static EyeSetup makeEyes(float parallax = 1.0f, float boardScale = 0.01f, float aspect = 0.75f)
{
	EyeSetup e;
	e.aspect = aspect;
	Mat p; matPerspective(p, 60.f * 3.14159265f / 180.f, 1.f, 1.f, 200.f);
	for (int eye = 0; eye < 2; ++eye) {
		Mat v; matTranslate(v, eye == 0 ? parallax : -parallax, 0.f, -40.f); // camera at (∓parallax,0,40) looking -Z
		matMul(e.clip[eye], p, v);
	}
	matIdentity(e.board); e.board[0] = e.board[5] = e.board[10] = boardScale; // game units -> board units
	matIdentity(e.camera);
	return e;
}

struct FrameTargets {
	HostTex eye[2];
	HostTex game, world, ui;
	bool atlas = false;
	int eyeRect[2][4] = {};
};

static D3D8GLES_XRTargets toTargets(const FrameTargets &f)
{
	D3D8GLES_XRTargets t = {};
	t.slot[D3D8GLES_XRT_STEREO_LEFT] = { f.eye[0].gl, f.eye[0].width, f.eye[0].height };
	t.slot[D3D8GLES_XRT_STEREO_RIGHT] = { f.atlas ? 0u : f.eye[1].gl, f.eye[1].width, f.eye[1].height };
	t.slot[D3D8GLES_XRT_GAME] = { f.game.gl, f.game.width, f.game.height };
	t.slot[D3D8GLES_XRT_WORLD] = { f.world.gl, f.world.width, f.world.height };
	t.slot[D3D8GLES_XRT_UI] = { f.ui.gl, f.ui.width, f.ui.height };
	t.atlas = f.atlas ? 1 : 0;
	for (int e = 0; e < 2; ++e) for (int i = 0; i < 4; ++i) t.eyeRect[e][i] = f.eyeRect[e][i];
	return t;
}

static bool makeSlot(FrameTargets *f, int eyeW, int eyeH, bool atlas)
{
	f->atlas = atlas;
	if (atlas) {
		if (!H->makeTexture(eyeW * 2, eyeH, &f->eye[0])) return false;
		f->eye[1] = HostTex{};
		f->eyeRect[0][0] = 0;     f->eyeRect[0][1] = 0; f->eyeRect[0][2] = eyeW; f->eyeRect[0][3] = eyeH;
		f->eyeRect[1][0] = eyeW;  f->eyeRect[1][1] = 0; f->eyeRect[1][2] = eyeW; f->eyeRect[1][3] = eyeH;
	} else {
		if (!H->makeTexture(eyeW, eyeH, &f->eye[0]) || !H->makeTexture(eyeW, eyeH, &f->eye[1])) return false;
	}
	if (!H->makeTexture(W, Hh, &f->game) || !H->makeTexture(W, Hh, &f->world) || !H->makeTexture(W, Hh, &f->ui)) return false;
	return true;
}

static void fillSlot(const FrameTargets &f, uint8_t v)
{
	H->fillTexture(f.eye[0], v);
	if (!f.atlas) H->fillTexture(f.eye[1], v);
	H->fillTexture(f.game, v); H->fillTexture(f.world, v); H->fillTexture(f.ui, v);
}

struct FrameOptions {
	bool split = true, elide = false, drawUI = true;
	int worldDraws = 1;
	float quadHalf = 10.f;
	float parallax = 1.f;
	float boardScale = 0.01f, aspect = 0.75f; // game units -> board units; aspect == -1 = observer (no board clip)
	bool stereoAtlasArg = false; // the atlas flag the "engine" passes to BeginXRStereo
};

struct FrameResult {
	bool stereoBegun = false;
	unsigned eyeTex[2] = { 0, 0 };
	bool atlasReported = false, multiviewReported = false;
	unsigned gameTex = 0, worldTex = 0, uiTex = 0;
	bool splitReady = false;
	unsigned skipped = 0;
	unsigned bindCalls = 0, drawCalls = 0;
};

// One engine frame as the host drives it. `targets` may be null (backend-allocated textures).
static FrameResult runFrame(const FrameTargets *f, const FrameOptions &o, IDirect3DTexture8 *tex)
{
	FrameResult r;
	if (f) { D3D8GLES_XRTargets t = toTargets(*f); d3d8gles_SetXRHostTargets(&t); }
	else d3d8gles_SetXRHostTargets(nullptr);
	H->resetCounters();
	d3d8gles_BeginXRFrame(o.split, o.elide);
	g_dev->BeginScene();
	baseState();
	setViewport();
	g_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF102030, 1.f, 0);

	// --- the 3D world, drawn once through the ordinary path and replayed per eye by the backend
	const EyeSetup eyes = makeEyes(o.parallax, o.boardScale, o.aspect);
	r.stereoBegun = d3d8gles_BeginXRStereo(W, Hh, eyes.clip[0], eyes.clip[1], eyes.board, eyes.aspect, eyes.camera, o.stereoAtlasArg, false);
	setWorldTransforms();
	setLight(0.f, 0.f, -1.f);
	g_dev->SetTexture(0, tex);
	const int prevCat = d3d8gles_SetDrawCategory(D3D8GLES_DRAWCAT_MODELS);
	for (int i = 0; i < o.worldDraws; ++i) drawWorldQuad(o.quadHalf);
	d3d8gles_SetDrawCategory(prevCat);
	g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	g_dev->SetTexture(0, nullptr);
	d3d8gles_EndXRStereo();

	// --- the UI layer: a solid red rectangle at D3D pixels (20,20)-(100,60), screen-space vertices
	if (o.drawUI) {
		d3d8gles_BeginXRUI(o.elide);
		struct UIV { float x, y, z, rhw; DWORD c; };
		const DWORD red = 0xFFFF0000;
		UIV q[4] = { { 20, 20, 0.5f, 1, red }, { 100, 20, 0.5f, 1, red }, { 20, 60, 0.5f, 1, red }, { 100, 60, 0.5f, 1, red } };
		g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
		g_dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
		const int prev2d = d3d8gles_SetDrawCategory(D3D8GLES_DRAWCAT_2D);
		g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(UIV));
		d3d8gles_SetDrawCategory(prev2d);
	}
	g_dev->EndScene();
	g_dev->Present(nullptr, nullptr, nullptr, nullptr);
	r.bindCalls = H->bindFramebufferCalls();
	r.drawCalls = H->drawCalls();
	H->finishFrame();
	d3d8gles_InvalidateCachedState(); // host protocol: after any host GL use

	r.eyeTex[0] = d3d8gles_XRStereoTexture(0);
	r.eyeTex[1] = d3d8gles_XRStereoTexture(1);
	r.atlasReported = d3d8gles_XRStereoAtlas();
	r.multiviewReported = d3d8gles_XRStereoMultiview();
	r.gameTex = d3d8gles_GetGameTexture();
	r.worldTex = d3d8gles_GetXRWorldTexture();
	r.uiTex = d3d8gles_GetXRUITexture();
	r.splitReady = d3d8gles_XRSplitReady();
	r.skipped = d3d8gles_XROrdinarySkipped();
	return r;
}

// ------------------------------------------------------------------------------------------
// pixel verification
// ------------------------------------------------------------------------------------------
static bool g_convKnown = false;
static bool g_metalRowIsGLY = true; // texel row 0 == GL window y 0 ?   (measured by the first case)

static RGBA at(const std::vector<uint8_t> &px, int w, int x, int row)
{
	const uint8_t *p = &px[((size_t)row * w + x) * 4];
	return { p[0], p[1], p[2], p[3] };
}

// Reads a rect of a host texture (Metal blit) and returns the pixel at GL window coordinates
// (x, glY) inside it, using the measured orientation.
struct EyeReader {
	std::vector<uint8_t> px; int w = 0, h = 0, x0 = 0, y0 = 0;
	bool load(const HostTex &t, int rx, int ry, int rw, int rh) { w = rw; h = rh; x0 = rx; y0 = ry; return H->readTexture(t, rx, ry, rw, rh, &px); }
	// gx, gy: GL window coordinates inside the whole texture; rect offsets already included
	RGBA glPixel(int gx, int gy, int texH) const
	{
		const int row = g_metalRowIsGLY ? gy : (texH - 1 - gy);
		return at(px, w, gx - x0, row - y0);
	}
};

// Reads the whole texture once and evaluates pixels in GL window coordinates.
struct FullTex {
	std::vector<uint8_t> px; int w = 0, h = 0;
	bool load(const HostTex &t) { w = t.width; h = t.height; return H->readTexture(t, 0, 0, t.width, t.height, &px); }
	RGBA gl(int gx, int gy) const { return at(px, w, gx, g_metalRowIsGLY ? gy : (h - 1 - gy)); }
	RGBA rawRow(int x, int row) const { return at(px, w, x, row); }
};

// Checks the 16 texel centres of the world quad in one eye. Returns the number of matches under
// the current orientation convention.
static int matchQuad(const FullTex &t, const Mat clip, int rx, int ry, int rw, int rh, float half, RGBA (*expect)(int, int), int tol)
{
	int matches = 0;
	for (int j = 0; j < 4; ++j)
		for (int i = 0; i < 4; ++i) {
			const float cell = 2 * half / 4;
			const float wx = -half + cell * i + cell / 2, wy = half - cell * j - cell / 2;
			int px, py;
			projectToPixel(clip, wx, wy, 0.f, rx, ry, rw, rh, &px, &py);
			if (sameColor(t.gl(px, py), expect(i, j), tol)) ++matches;
		}
	return matches;
}
static RGBA expectCheck(int i, int j) { return checkTexel(i, j); }
static RGBA expectCheckHalf(int i, int j) { RGBA c = checkTexel(i, j); return { c.r / 2, c.g / 2, c.b / 2, 255 }; }

// ------------------------------------------------------------------------------------------
// cases
// ------------------------------------------------------------------------------------------
static void caseSeparateEyes()
{
	printf("\n[case] stereo, separate eye textures (host Private MTLTextures, no multiview)\n");
	FrameTargets f;
	check(makeSlot(&f, 256, 256, false), "host textures created (eyes 256x256 x2, game/world/ui 256x256), all Private storage");
	fillSlot(f, 0xAA);
	IDirect3DTexture8 *tex = makeCheckTexture();
	check(tex != nullptr, "4x4 A8R8G8B8 texture created through the D3D8 API");
	const FrameOptions o;
	FrameResult r = runFrame(&f, o, tex);
	check(r.stereoBegun, "d3d8gles_BeginXRStereo accepted the host targets");
	check(r.eyeTex[0] == f.eye[0].gl && r.eyeTex[1] == f.eye[1].gl && r.eyeTex[0] != 0,
	      "d3d8gles_XRStereoTexture(0/1) return the host GL names (%u,%u)", r.eyeTex[0], r.eyeTex[1]);
	check(!r.atlasReported && !r.multiviewReported, "reported as separate-eye, no multiview");
	check(r.gameTex == f.game.gl, "d3d8gles_GetGameTexture returns the host GAME name");

	FullTex L, R;
	check(L.load(f.eye[0]) && R.load(f.eye[1]), "Metal blit readback of both eye textures");
	// Determine the row convention: exactly one hypothesis must reproduce all 16 texels in both eyes.
	g_metalRowIsGLY = true;
	const int h0 = matchQuad(L, makeEyes().clip[0], 0, 0, 256, 256, 10.f, expectCheck, 3) + matchQuad(R, makeEyes().clip[1], 0, 0, 256, 256, 10.f, expectCheck, 3);
	g_metalRowIsGLY = false;
	const int h1 = matchQuad(L, makeEyes().clip[0], 0, 0, 256, 256, 10.f, expectCheck, 3) + matchQuad(R, makeEyes().clip[1], 0, 0, 256, 256, 10.f, expectCheck, 3);
	g_metalRowIsGLY = h0 >= h1;
	g_convKnown = (h0 == 32) != (h1 == 32);
	printf("  orientation: matches with row0==GL y0: %d/32, row0==GL top: %d/32 -> texel row 0 is GL window y %s\n", h0, h1, g_metalRowIsGLY ? "= 0 (bottom of the GL image)" : "= H-1 (top of the GL image)");
	check(g_convKnown, "exactly one texel-row convention reproduces the lit, textured quad in BOTH eyes (per-eye clip matrices, texture swizzle, lighting=1.0, coverage)");
	check(g_metalRowIsGLY, "ANGLE-Metal target rows: MTLTexture row 0 == GL window y 0 (as derived from source); hosts must flip V when sampling");

	// alpha coverage: everything outside the quad is fully transparent, the quad is opaque
	int outsideOK = 0, outsideN = 0;
	for (int e = 0; e < 2; ++e) {
		const FullTex &t = e ? R : L;
		const int pts[4][2] = { { 3, 3 }, { 252, 3 }, { 3, 252 }, { 252, 252 } };
		for (auto &p : pts) { ++outsideN; if (t.gl(p[0], p[1]).a == 0 && t.gl(p[0], p[1]).r == 0) ++outsideOK; }
	}
	check(outsideOK == outsideN, "pixels outside the quad stay (0,0,0,0) so passthrough shows (%d/%d)", outsideOK, outsideN);
	// parallax: the quad's centre is drawn at different columns in the two eyes
	int lx, ly, rx, ry;
	projectToPixel(makeEyes().clip[0], 0, 0, 0, 0, 0, 256, 256, &lx, &ly);
	projectToPixel(makeEyes().clip[1], 0, 0, 0, 0, 0, 256, 256, &rx, &ry);
	check(lx != rx, "eye matrices differ (quad centre column left=%d right=%d)", lx, rx);
	// the game texture must NOT be the eye image; UI texture untouched by the world draws
	check(r.uiTex == f.ui.gl, "d3d8gles_GetXRUITexture returns the host UI name");

	// second frame, different light: N.L = 0.5 must halve the colour (fixed-function lighting)
	fillSlot(f, 0xAA);
	// (re-run with a dimmer light by drawing through a custom frame)
	{
		D3D8GLES_XRTargets t = toTargets(f); d3d8gles_SetXRHostTargets(&t);
		d3d8gles_BeginXRFrame(true, false);
		g_dev->BeginScene(); baseState(); setViewport();
		g_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF102030, 1.f, 0);
		const EyeSetup eyes = makeEyes();
		d3d8gles_BeginXRStereo(W, Hh, eyes.clip[0], eyes.clip[1], eyes.board, eyes.aspect, eyes.camera, false, false);
		setWorldTransforms();
		setLight(0.f, -0.8660254f, -0.5f); // N=(0,0,1): N.(-L) = 0.5
		g_dev->SetTexture(0, tex);
		const int prev = d3d8gles_SetDrawCategory(D3D8GLES_DRAWCAT_MODELS);
		drawWorldQuad(10.f);
		d3d8gles_SetDrawCategory(prev);
		d3d8gles_EndXRStereo();
		g_dev->EndScene(); g_dev->Present(nullptr, nullptr, nullptr, nullptr);
		H->finishFrame(); d3d8gles_InvalidateCachedState();
	}
	check(L.load(f.eye[0]), "readback after the half-lit frame");
	const int hl = matchQuad(L, makeEyes().clip[0], 0, 0, 256, 256, 10.f, expectCheckHalf, 4);
	check(hl == 16, "directional light with N.L=0.5 halves the texel colours (%d/16 texel centres)", hl);
	tex->Release();
}

static void caseAtlas()
{
	printf("\n[case] stereo, one atlas texture (2W x H), eyeRect viewports + scissor\n");
	FrameTargets f;
	check(makeSlot(&f, 256, 256, true), "host atlas texture 512x256 created (Private)");
	fillSlot(f, 0xAA);
	IDirect3DTexture8 *tex = makeCheckTexture();
	FrameOptions o; o.stereoAtlasArg = false; // the host's `atlas` decides, not the engine argument
	FrameResult r = runFrame(&f, o, tex);
	check(r.stereoBegun, "d3d8gles_BeginXRStereo accepted the atlas target");
	check(r.atlasReported && !r.multiviewReported, "reported as atlas");
	check(r.eyeTex[0] == f.eye[0].gl && r.eyeTex[1] == f.eye[0].gl && r.eyeTex[0] != 0, "both eyes report the atlas host name (%u)", r.eyeTex[0]);
	FullTex A;
	check(A.load(f.eye[0]), "Metal blit readback of the atlas");
	const EyeSetup e = makeEyes();
	const int m0 = matchQuad(A, e.clip[0], 0, 0, 256, 256, 10.f, expectCheck, 3);
	const int m1 = matchQuad(A, e.clip[1], 256, 0, 256, 256, 10.f, expectCheck, 3);
	check(m0 == 16, "left eye in atlas rect {0,0,256,256}: %d/16 texel centres", m0);
	check(m1 == 16, "right eye in atlas rect {256,0,256,256}: %d/16 texel centres", m1);
	// scissor: nothing of the left eye leaks into the right half
	int leak = 0;
	// the left eye's quad spans x in [~62,~194]; sample the right half column just outside the right eye's quad
	int qx, qy; projectToPixel(e.clip[1], -10.f, 0.f, 0.f, 256, 0, 256, 256, &qx, &qy);
	for (int x = 256; x < qx - 4; ++x) if (A.gl(x, 128).a != 0) ++leak;
	check(leak == 0, "no left-eye pixels leak into the right eye rect (%d stray pixels left of the right eye's quad)", leak);
	tex->Release();
}

// The tabletop board clip (XRStereoShader.h GX_XR_STEREO_FRAGMENT_BODY): fragments whose board-space
// position leaves |x| <= 0.5, |y| <= aspect/2 are discarded; aspect == -1 (observer) switches it off.
static void caseBoardClip()
{
	printf("\n[case] board clipping in the stereo fragment shader (tabletop board bounds, observer opt-out)\n");
	FrameTargets f;
	check(makeSlot(&f, 256, 256, true), "atlas ring slot created");
	IDirect3DTexture8 *tex = makeCheckTexture();
	// board scale 0.03: |x| <= 0.5 <=> |wx| <= 16.67 game units, |y| <= 0.375 <=> |wy| <= 12.5; the quad
	// (half 20) is wider than both, and still inside the camera's view (half extent 23 at 40 units)
	struct Probe { float wx, wy; bool covered; const char *what; };
	const Probe probes[] = {
		{ 0.f, 11.f, true, "inside, near the +Y edge" }, { 0.f, 14.f, false, "outside +Y edge" },
		{ 0.f, -11.f, true, "inside, near the -Y edge" }, { 0.f, -14.f, false, "outside -Y edge" },
		{ 15.f, 0.f, true, "inside, near the +X edge" }, { 18.f, 0.f, false, "outside +X edge" },
		{ -15.f, 0.f, true, "inside, near the -X edge" }, { -18.f, 0.f, false, "outside -X edge" },
	};
	for (int pass = 0; pass < 2; ++pass) {
		const bool observer = pass == 1;
		fillSlot(f, 0xAA);
		FrameOptions o; o.quadHalf = 20.f; o.boardScale = 0.03f; o.aspect = observer ? -1.0f : 0.75f; o.drawUI = false;
		FrameResult r = runFrame(&f, o, tex);
		check(r.stereoBegun, "%s: d3d8gles_BeginXRStereo accepted the atlas target", observer ? "observer (aspect == -1)" : "tabletop");
		FullTex A;
		check(A.load(f.eye[0]), "%s: Metal blit readback of the atlas", observer ? "observer" : "tabletop");
		const EyeSetup e = makeEyes(o.parallax, o.boardScale, o.aspect);
		int ok = 0, n = 0;
		for (int eye = 0; eye < 2; ++eye)
			for (const Probe &p : probes) {
				int px, py;
				projectToPixel(e.clip[eye], p.wx, p.wy, 0.f, eye ? 256 : 0, 0, 256, 256, &px, &py);
				const bool covered = A.gl(px, py).a != 0;
				const bool want = observer ? true : p.covered;
				++n;
				if (covered == want) ++ok; else printf("  eye %d (%s): pixel (%d,%d) covered=%d, expected %d\n", eye, p.what, px, py, covered, want);
			}
		if (observer) check(ok == n, "observer: aspect -1 (observer) disables the board clip, every probe covered (%d/%d)", ok, n);
		else check(ok == n, "tabletop: fragments outside the board bounds discarded, inside kept (%d/%d probes, both eyes)", ok, n);
	}
	tex->Release();
}

static void caseRingRotation()
{
	printf("\n[case] ring rotation: host names change every frame\n");
	FrameTargets a, b;
	check(makeSlot(&a, 256, 256, false) && makeSlot(&b, 256, 256, false), "two ring slots (A, B) created");
	fillSlot(a, 0xAA); fillSlot(b, 0xAA);
	IDirect3DTexture8 *tex = makeCheckTexture();
	FrameOptions o1; o1.parallax = 1.f;
	FrameResult r1 = runFrame(&a, o1, tex);
	FullTex a0, a1;
	check(a0.load(a.eye[0]) && a1.load(a.eye[1]), "slot A read back after frame 1");
	FrameOptions o2; o2.parallax = 2.5f; // different image so a stale texture would be detected
	FrameResult r2 = runFrame(&b, o2, tex);
	check(r1.eyeTex[0] == a.eye[0].gl && r2.eyeTex[0] == b.eye[0].gl && r2.eyeTex[1] == b.eye[1].gl,
	      "frame 2 reports slot B names (%u,%u), frame 1 reported slot A (%u)", r2.eyeTex[0], r2.eyeTex[1], r1.eyeTex[0]);
	FullTex b0, b1, a0b, a1b;
	check(b0.load(b.eye[0]) && b1.load(b.eye[1]) && a0b.load(a.eye[0]) && a1b.load(a.eye[1]), "both slots read back after frame 2");
	const EyeSetup e2 = makeEyes(2.5f);
	check(matchQuad(b0, e2.clip[0], 0, 0, 256, 256, 10.f, expectCheck, 3) == 16 && matchQuad(b1, e2.clip[1], 0, 0, 256, 256, 10.f, expectCheck, 3) == 16,
	      "slot B holds frame 2 (parallax 2.5) in both eyes");
	check(a0b.px == a0.px && a1b.px == a1.px, "slot A is byte-identical after frame 2 (never touched by the next frame)");
	// UI layer (MRT colour attachment 1) must follow the ring exactly like the eyes: regression 22/09/2026, the real app's HUD
	// texture stayed empty (the control bar reached the GAME target, attachment 0, but never the UI slot the compositor reads).
	{
		const RGBA red = { 255, 0, 0, 255 };
		FullTex uiB, uiA;
		check(uiB.load(b.ui) && sameColor(uiB.gl(60, Hh - 1 - 40), red, 2),
		      "slot B's UI texture holds frame 2's UI rectangle (MRT attachment 1 re-pointed at the rotated slot)");
		check(r2.uiTex == b.ui.gl, "frame 2 reports slot B's UI name (%u vs %u)", r2.uiTex, b.ui.gl);
		check(uiA.load(a.ui) && sameColor(uiA.gl(60, Hh - 1 - 40), red, 2), "slot A's UI texture still holds frame 1's UI rectangle");
	}
	// and back to A again
	fillSlot(a, 0xAA);
	FrameResult r3 = runFrame(&a, o1, tex);
	FullTex a0c;
	check(r3.eyeTex[0] == a.eye[0].gl && a0c.load(a.eye[0]) && a0c.px == a0.px, "frame 3 back in slot A reproduces frame 1 exactly (names re-attached)");
	tex->Release();
}

static void caseBackendTargets()
{
	printf("\n[case] no host targets: backend allocates its own textures (Android behaviour)\n");
	IDirect3DTexture8 *tex = makeCheckTexture();
	FrameOptions o;
	FrameResult r = runFrame(nullptr, o, tex);
	check(r.stereoBegun && r.eyeTex[0] != 0 && r.eyeTex[1] != 0 && r.eyeTex[0] != r.eyeTex[1],
	      "backend-owned eye textures reported (%u,%u), separate mode", r.eyeTex[0], r.eyeTex[1]);
	std::vector<uint8_t> px;
	check(H->readGLTexture(r.eyeTex[0], 256, 256, &px), "backend-owned texture read through a scratch FBO");
	d3d8gles_InvalidateCachedState();
	// GL order: row 0 == GL y 0
	const EyeSetup e = makeEyes();
	int good = 0;
	for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) {
		const float cell = 20.f / 4;
		int px_, py_; projectToPixel(e.clip[0], -10.f + cell * i + cell / 2, 10.f - cell * j - cell / 2, 0.f, 0, 0, 256, 256, &px_, &py_);
		if (sameColor(at(px, 256, px_, py_), checkTexel(i, j), 3)) ++good;
	}
	check(good == 16, "backend-allocated eye render is correct too (%d/16 texel centres)", good);
	check(r.gameTex != 0, "backend-owned GAME texture reported (%u)", r.gameTex);
	tex->Release();
}

static void caseLayersAndElision()
{
	printf("\n[case] world/UI split on host slots, world elision without multiview, lazy FBO restore\n");
	FrameTargets f;
	check(makeSlot(&f, 256, 256, true), "atlas ring slot created");
	fillSlot(f, 0xAA);
	IDirect3DTexture8 *tex = makeCheckTexture();
	FrameOptions o; o.elide = true; o.worldDraws = 24;
	// Frame 1 certifies coverage and allocates the UI/world layers; from frame 2 on the ordinary
	// central-camera copy of every world draw is omitted (elision is not tied to multiview).
	FrameResult r1 = runFrame(&f, o, tex);
	fillSlot(f, 0xAA);
	FrameResult r2 = runFrame(&f, o, tex);
	printf("  frame 2: draws submitted to GL=%u framebuffer binds=%u ordinary-skipped=%u\n", r2.drawCalls, r2.bindCalls, r2.skipped);
	check(r1.stereoBegun && r2.stereoBegun && r2.eyeTex[0] == f.eye[0].gl, "stereo active on host atlas both frames");
	check(r2.skipped > 0, "ordinary world draws were elided (%u omitted) with a non-multiview host target", r2.skipped);
	check(r2.gameTex == 0, "GAME texture withheld while the ordinary world is incomplete (split path only)");
	check(r2.splitReady && r2.uiTex == f.ui.gl, "split ready, UI layer is the host UI slot");
	check(r2.bindCalls < 24, "framebuffer binds per frame (%u) do not scale with the %d world draws: eye FBO stays bound, ordinary FBO restored once", r2.bindCalls, o.worldDraws);
	// UI texture: red rectangle at D3D (20,20)-(100,60); everything else transparent
	FullTex ui;
	check(ui.load(f.ui), "Metal blit readback of the UI slot");
	// D3D top row py = 0 -> GL y = H-1 (D3D +Y is the top and GL NDC +Y keeps it)
	int inTop = 0, inBottom = 0;
	const RGBA red = { 255, 0, 0, 255 };
	if (sameColor(ui.gl(60, Hh - 1 - 40), red, 2)) ++inTop;
	if (sameColor(ui.gl(60, 40), red, 2)) ++inBottom;
	check(inTop == 1 && inBottom == 0, "UI rectangle lands at D3D pixel (60,40): visual top-left of the GL-native image (top=%d bottom=%d)", inTop, inBottom);
	check(ui.gl(200, Hh - 1 - 200).a == 0 && ui.gl(10, Hh - 1 - 10).a == 0, "UI layer is transparent outside the rectangle (alpha 0)");
	// eyes are still right in the elided frame
	FullTex A;
	check(A.load(f.eye[0]), "atlas read back");
	const EyeSetup e = makeEyes();
	check(matchQuad(A, e.clip[0], 0, 0, 256, 256, 10.f, expectCheck, 3) == 16 && matchQuad(A, e.clip[1], 256, 0, 256, 256, 10.f, expectCheck, 3) == 16,
	      "both eyes correct after the elided frame (24 replayed world draws)");
	// non-elided frame afterwards: ordinary world present again, GAME texture is the host slot
	FrameOptions plain; plain.elide = false; plain.worldDraws = 1;
	fillSlot(f, 0xAA);
	FrameResult r3 = runFrame(&f, plain, tex);
	check(r3.gameTex == f.game.gl && r3.worldTex == f.world.gl, "elision off again: GAME and WORLD host slots are published (%u,%u)", r3.gameTex, r3.worldTex);
	FullTex g;
	check(g.load(f.game), "GAME slot read back");
	const int gy = Hh - 1 - 40;
	check(sameColor(g.gl(60, gy), red, 2), "composed GAME frame contains the UI rectangle too");
	tex->Release();
}

// ------------------------------------------------------------------------------------------
// software DXT decode + format audit, drawn 1:1 into the host GAME texture
// ------------------------------------------------------------------------------------------
static uint16_t rgb565(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }
static void unpack565(uint16_t c, int *r, int *g, int *b)
{
	*r = ((c >> 11) & 31) * 255 / 31; *g = ((c >> 5) & 63) * 255 / 63; *b = (c & 31) * 255 / 31;
}
// Independent reference decoders (written from the S3TC spec, not copied from the backend).
static void refBC1(const uint8_t *b, bool dxt1, RGBA out[16])
{
	const uint16_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
	int r0, g0, b0, r1, g1, b1;
	unpack565(c0, &r0, &g0, &b0); unpack565(c1, &r1, &g1, &b1);
	RGBA pal[4] = { { r0, g0, b0, 255 }, { r1, g1, b1, 255 }, {}, {} };
	if (c0 > c1 || !dxt1) {
		pal[2] = { (2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255 };
		pal[3] = { (r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255 };
	} else {
		pal[2] = { (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255 };
		pal[3] = { 0, 0, 0, 0 };
	}
	const uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);
	for (int i = 0; i < 16; ++i) out[i] = pal[(idx >> (2 * i)) & 3];
}
static void refAlpha5(const uint8_t *b, int out[16])
{
	int a[8]; a[0] = b[0]; a[1] = b[1];
	if (a[0] > a[1]) for (int i = 1; i < 7; ++i) a[1 + i] = ((7 - i) * a[0] + i * a[1]) / 7;
	else { for (int i = 1; i < 5; ++i) a[1 + i] = ((5 - i) * a[0] + i * a[1]) / 5; a[6] = 0; a[7] = 255; }
	uint64_t bits = 0; for (int i = 0; i < 6; ++i) bits |= (uint64_t)b[2 + i] << (8 * i);
	for (int i = 0; i < 16; ++i) out[i] = a[(bits >> (3 * i)) & 7];
}

struct Decoded { std::vector<RGBA> texels; int w, h; };

// Builds an 8x8 DXT texture (2x2 blocks) with deterministic, varied content and returns the
// D3D8 texture plus the expected decoded texels (row-major).
static IDirect3DTexture8 *makeDXT(D3DFORMAT fmt, Decoded *expected)
{
	const int blockBytes = fmt == D3DFMT_DXT1 ? 8 : 16;
	std::vector<uint8_t> data(4 * blockBytes);
	expected->w = expected->h = 8; expected->texels.assign(64, RGBA{});
	for (int blk = 0; blk < 4; ++blk) {
		uint8_t *b = &data[blk * blockBytes];
		uint8_t *cb = b + (fmt == D3DFMT_DXT1 ? 0 : 8);
		// colour endpoints differ per block; block 3 of DXT1 uses c0 <= c1 (punch-through alpha)
		uint16_t c0 = rgb565(255, 40 * blk, 0), c1 = rgb565(0, 128, 255 - 60 * blk);
		if (fmt == D3DFMT_DXT1 && blk == 3) std::swap(c0, c1);
		cb[0] = c0 & 255; cb[1] = c0 >> 8; cb[2] = c1 & 255; cb[3] = c1 >> 8;
		uint32_t idx = 0; for (int i = 0; i < 16; ++i) idx |= (uint32_t)(((i * 3 + blk) & 3)) << (2 * i);
		cb[4] = idx & 255; cb[5] = (idx >> 8) & 255; cb[6] = (idx >> 16) & 255; cb[7] = (idx >> 24) & 255;
		RGBA col[16]; refBC1(cb, fmt == D3DFMT_DXT1, col);
		int alpha[16];
		if (fmt == D3DFMT_DXT3) {
			for (int i = 0; i < 16; ++i) { const int n = (i * 5 + blk * 3) & 15; if (i & 1) b[i / 2] |= n << 4; else b[i / 2] = n; alpha[i] = n * 17; }
		} else if (fmt == D3DFMT_DXT5) {
			b[0] = blk == 2 ? 40 : 250; b[1] = blk == 2 ? 200 : 10; // blk 2 exercises the 6-value + {0,255} mode
			uint64_t bits = 0; for (int i = 0; i < 16; ++i) bits |= (uint64_t)((i + blk) & 7) << (3 * i);
			for (int i = 0; i < 6; ++i) b[2 + i] = (bits >> (8 * i)) & 255;
			refAlpha5(b, alpha);
		} else for (int i = 0; i < 16; ++i) alpha[i] = col[i].a;
		const int bx = (blk & 1) * 4, by = (blk >> 1) * 4;
		for (int i = 0; i < 16; ++i) {
			RGBA t = col[i]; if (fmt != D3DFMT_DXT1) t.a = alpha[i];
			expected->texels[(by + i / 4) * 8 + bx + (i & 3)] = t;
		}
	}
	return makeTexture(8, 8, fmt, data);
}

struct UIV2 { float x, y, z, rhw; DWORD c; float u, v; };
// Draws `tex` 1:1 scaled by `scale` at pixel (ox,oy) with point sampling and *raw texture* colour/alpha.
static void drawTexturedRect(IDirect3DTexture8 *tex, int ox, int oy, int texW, int texH, int scale, float uMax = 1.f, float vMax = 1.f)
{
	const float x0 = (float)ox, y0 = (float)oy, x1 = (float)(ox + texW * scale), y1 = (float)(oy + texH * scale);
	UIV2 q[4] = { { x0, y0, 0.5f, 1, 0xFFFFFFFF, 0, 0 }, { x1, y0, 0.5f, 1, 0xFFFFFFFF, uMax, 0 },
	              { x0, y1, 0.5f, 1, 0xFFFFFFFF, 0, vMax }, { x1, y1, 0.5f, 1, 0xFFFFFFFF, uMax, vMax } };
	g_dev->SetTexture(0, tex);
	g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	g_dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(UIV2));
}

// Starts a plain 2D frame that renders into the host GAME slot only.
static void beginGameFrame(const FrameTargets &f)
{
	D3D8GLES_XRTargets t = toTargets(f);
	d3d8gles_SetXRHostTargets(&t);
	d3d8gles_BeginXRFrame(false, false);
	g_dev->BeginScene();
	baseState();
	setViewport();
	g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	g_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF000000, 1.f, 0);
}
static void endGameFrame()
{
	g_dev->EndScene();
	g_dev->Present(nullptr, nullptr, nullptr, nullptr);
	H->finishFrame();
	d3d8gles_InvalidateCachedState();
}
// pixel of the GAME texture at D3D coordinates (x, y from the top)
static RGBA gamePixel(const FullTex &g, int x, int y) { return g.gl(x, Hh - 1 - y); }

static void caseDXT()
{
	printf("\n[case] DXT1/DXT3/DXT5 through the D3D8 texture path with no S3TC on ANGLE-Metal\n");
	const char *ext = H->glString(0x1F03);
	const bool hasS3TC = ext && (strstr(ext, "GL_EXT_texture_compression_s3tc") || strstr(ext, "GL_EXT_texture_compression_dxt1") || strstr(ext, "GL_ANGLE_texture_compression_dxt"));
	printf("  GL_EXTENSIONS advertises S3TC/DXT: %s\n", hasS3TC ? "yes" : "no");
	if (H->visionOS) check(!hasS3TC, "ANGLE-Metal on visionOS exposes no S3TC/DXT extension, so the backend's software decode is the only path");
	else printf("  (not visionOS: the software decode is forced with D3D8GLES_DISABLE_S3TC, set before device creation)\n");
	FrameTargets f;
	check(makeSlot(&f, 256, 256, true), "host slots created");
	fillSlot(f, 0xAA);
	const D3DFORMAT fmts[3] = { D3DFMT_DXT1, D3DFMT_DXT3, D3DFMT_DXT5 };
	const char *names[3] = { "DXT1", "DXT3", "DXT5" };
	Decoded exp[3]; IDirect3DTexture8 *tex[3];
	for (int i = 0; i < 3; ++i) { tex[i] = makeDXT(fmts[i], &exp[i]); check(tex[i] != nullptr, "%s 8x8 texture created and filled block-wise through LockRect", names[i]); }
	beginGameFrame(f);
	for (int i = 0; i < 3; ++i) drawTexturedRect(tex[i], 8 + i * 72, 8, 8, 8, 8); // texel = 8x8 pixels
	endGameFrame();
	FullTex g;
	check(g.load(f.game), "GAME slot read back through Metal");
	for (int i = 0; i < 3; ++i) {
		int good = 0, total = 0, worst = 0;
		for (int ty = 0; ty < 8; ++ty) for (int tx = 0; tx < 8; ++tx) {
			const RGBA want = exp[i].texels[ty * 8 + tx];
			const RGBA got = gamePixel(g, 8 + i * 72 + tx * 8 + 4, 8 + ty * 8 + 4);
			++total;
			// premultiplication does not apply (blend off); allow 1 for float rounding in the sampler
			const bool ok = sameColor(got, want, 1);
			if (ok) ++good; else worst = std::max({ worst, abs(got.r - want.r), abs(got.g - want.g), abs(got.b - want.b), abs(got.a - want.a) });
			if (!ok && total <= 3) printf("    %s texel(%d,%d): got (%d,%d,%d,%d) expected (%d,%d,%d,%d)\n", names[i], tx, ty, got.r, got.g, got.b, got.a, want.r, want.g, want.b, want.a);
		}
		check(good == total, "%s software decode matches the reference RGBA for %d/%d texels (worst channel error %d)", names[i], good, total, worst);
	}
	for (auto t : tex) t->Release();
}

struct FormatCase {
	D3DFORMAT fmt; const char *name; int bytes;
	void (*fill)(uint8_t *dst, int tx, int ty);
	RGBA (*expect)(int tx, int ty);
	int tol;
};
static uint8_t q5(int v) { return (uint8_t)(v & 31); }
static void fillA8R8G8B8(uint8_t *d, int x, int y) { d[0] = 200 - x * 40; d[1] = 30 + y * 50; d[2] = 60 + x * 50; d[3] = 255 - y * 60; }
static RGBA expA8R8G8B8(int x, int y) { return { 60 + x * 50, 30 + y * 50, 200 - x * 40, 255 - y * 60 }; }
static void fillX8R8G8B8(uint8_t *d, int x, int y) { d[0] = 200 - x * 40; d[1] = 30 + y * 50; d[2] = 60 + x * 50; d[3] = 7; }
static RGBA expX8R8G8B8(int x, int y) { return { 60 + x * 50, 30 + y * 50, 200 - x * 40, 255 }; }
static void fillR5G6B5(uint8_t *d, int x, int y) { const uint16_t v = (uint16_t)((q5(x * 9 + 2) << 11) | (((y * 17 + 5) & 63) << 5) | q5(31 - x * 7)); memcpy(d, &v, 2); }
static RGBA expR5G6B5(int x, int y) { return { q5(x * 9 + 2) * 255 / 31, ((y * 17 + 5) & 63) * 255 / 63, q5(31 - x * 7) * 255 / 31, 255 }; }
static void fillA1R5G5B5(uint8_t *d, int x, int y) { const uint16_t v = (uint16_t)((((x + y) & 1) << 15) | (q5(x * 9 + 2) << 10) | (q5(y * 9 + 4) << 5) | q5(31 - x * 7)); memcpy(d, &v, 2); }
static RGBA expA1R5G5B5(int x, int y) { return { q5(x * 9 + 2) * 255 / 31, q5(y * 9 + 4) * 255 / 31, q5(31 - x * 7) * 255 / 31, ((x + y) & 1) ? 255 : 0 }; }
static void fillA4R4G4B4(uint8_t *d, int x, int y) { const uint16_t v = (uint16_t)(((x * 5 + y) & 15) << 12 | ((x * 4 + 1) & 15) << 8 | ((y * 4 + 2) & 15) << 4 | ((15 - x * 3) & 15)); memcpy(d, &v, 2); }
static RGBA expA4R4G4B4(int x, int y) { return { ((x * 4 + 1) & 15) * 17, ((y * 4 + 2) & 15) * 17, ((15 - x * 3) & 15) * 17, ((x * 5 + y) & 15) * 17 }; }
static void fillA8(uint8_t *d, int x, int y) { d[0] = 30 + x * 50 + y * 8; }
static RGBA expA8(int x, int y) { return { 0, 0, 0, 30 + x * 50 + y * 8 }; }
static void fillL8(uint8_t *d, int x, int y) { d[0] = 30 + x * 50 + y * 8; }
static RGBA expL8(int x, int y) { const int l = 30 + x * 50 + y * 8; return { l, l, l, 255 }; }
static void fillA8L8(uint8_t *d, int x, int y) { d[0] = 30 + x * 50 + y * 8; d[1] = 255 - x * 40 - y * 20; }
static RGBA expA8L8(int x, int y) { const int l = 30 + x * 50 + y * 8; return { l, l, l, 255 - x * 40 - y * 20 }; }

static void caseFormats()
{
	printf("\n[case] every texture format the engine requests, sampled through the ANGLE-Metal path\n");
	FrameTargets f;
	check(makeSlot(&f, 256, 256, true), "host slots created");
	fillSlot(f, 0xAA);
	const FormatCase cases[] = {
		{ D3DFMT_A8R8G8B8, "A8R8G8B8", 4, fillA8R8G8B8, expA8R8G8B8, 1 },
		{ D3DFMT_X8R8G8B8, "X8R8G8B8 (alpha forced opaque)", 4, fillX8R8G8B8, expX8R8G8B8, 1 },
		{ D3DFMT_R5G6B5, "R5G6B5", 2, fillR5G6B5, expR5G6B5, 2 },
		{ D3DFMT_A1R5G5B5, "A1R5G5B5", 2, fillA1R5G5B5, expA1R5G5B5, 2 },
		{ D3DFMT_A4R4G4B4, "A4R4G4B4", 2, fillA4R4G4B4, expA4R4G4B4, 2 },
		{ D3DFMT_A8, "A8 (RGB=0, A=value)", 1, fillA8, expA8, 1 },
		{ D3DFMT_L8, "L8 (RGB=L, A=1)", 1, fillL8, expL8, 1 },
		{ D3DFMT_A8L8, "A8L8 (RGB=L, A=A)", 2, fillA8L8, expA8L8, 1 },
	};
	const int nCases = (int)(sizeof(cases) / sizeof(cases[0]));
	std::vector<IDirect3DTexture8 *> tex;
	for (int c = 0; c < nCases; ++c) {
		std::vector<uint8_t> bytes(4 * 4 * cases[c].bytes);
		for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) cases[c].fill(&bytes[(y * 4 + x) * cases[c].bytes], x, y);
		tex.push_back(makeTexture(4, 4, cases[c].fmt, bytes));
		check(tex.back() != nullptr, "%s texture created", cases[c].name);
	}
	beginGameFrame(f);
	for (int c = 0; c < nCases; ++c) drawTexturedRect(tex[c], 6 + (c % 4) * 60, 6 + (c / 4) * 60, 4, 4, 12);
	endGameFrame();
	FullTex g;
	check(g.load(f.game), "GAME slot read back through Metal");
	for (int c = 0; c < nCases; ++c) {
		int good = 0, worst = 0;
		for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
			const RGBA want = cases[c].expect(x, y);
			const RGBA got = gamePixel(g, 6 + (c % 4) * 60 + x * 12 + 6, 6 + (c / 4) * 60 + y * 12 + 6);
			if (sameColor(got, want, cases[c].tol)) ++good;
			else {
				worst = std::max({ worst, abs(got.r - want.r), abs(got.g - want.g), abs(got.b - want.b), abs(got.a - want.a) });
				if (worst == std::max({ abs(got.r - want.r), abs(got.g - want.g), abs(got.b - want.b), abs(got.a - want.a) }))
					printf("    %s texel(%d,%d): got (%d,%d,%d,%d) expected (%d,%d,%d,%d)\n", cases[c].name, x, y, got.r, got.g, got.b, got.a, want.r, want.g, want.b, want.a);
			}
		}
		check(good == 16, "%s decodes correctly: %d/16 texels within +-%d (worst %d)", cases[c].name, good, cases[c].tol, worst);
	}
	for (auto t : tex) t->Release();
}

static void caseVaryingPrecision()
{
	printf("\n[case] mediump varying concern: texture coordinates far beyond fp16's exact range\n");
	const double mediumErr = H->probeVaryingPrecision(false);
	const double highErr = H->probeVaryingPrecision(true);
	d3d8gles_InvalidateCachedState(); // the raw probe used the GL context behind the backend's back
	printf("  raw probe (u runs 0..64 over 512 px): mediump worst error %.4f of a repeat, highp %.4f\n", mediumErr, highErr);
	check(highErr >= 0 && highErr < 0.01, "highp varying interpolates exactly (worst %.4f of a repeat)", highErr);
	printf("  => ANGLE-Metal %s mediump varyings as half precision\n", mediumErr > 0.01 ? "TREATS" : "does not degrade");

	// Through the real backend: a 64-texel ramp repeated 64 times across 256 pixels.
	FrameTargets f;
	check(makeSlot(&f, 256, 256, true), "host slots created");
	fillSlot(f, 0xAA);
	std::vector<uint8_t> ramp(64 * 2 * 4);
	for (int y = 0; y < 2; ++y) for (int x = 0; x < 64; ++x) { uint8_t *p = &ramp[(y * 64 + x) * 4]; p[0] = 0; p[1] = 0; p[2] = (uint8_t)(x * 4); p[3] = 255; } // R = x*4
	IDirect3DTexture8 *tex = makeTexture(64, 2, D3DFMT_A8R8G8B8, ramp);
	beginGameFrame(f);
	drawTexturedRect(tex, 0, 100, 256, 8, 1, 64.f, 1.f); // u 0..64 across 256 px (64 repeats), scale 1 -> rect is 256x8
	endGameFrame();
	FullTex g;
	check(g.load(f.game), "GAME slot read back");
	// The backend places D3D screen-space vertices with its own sub-pixel convention (the engine
	// only applies the -0.5 UV bias on Vulkan, see render2d.cpp), so find the constant pixel offset
	// that explains the picture and judge the NOISE around it: fp16 interpolation at u~64 has a step
	// of 0.0625 u = 4 texels, so it cannot fit any constant offset; an fp32 path fits exactly.
	int worst = 1 << 30; double bestOffset = 0;
	for (int k = 0; k <= 32; ++k) {
		const double off = k * 0.0625;
		int w = 0;
		for (int x = 0; x < 256; ++x) {
			const double u = (x + off) / 256.0 * 64.0;
			const int want = (int)floor((u - floor(u)) * 64.0) * 4;
			int d = abs(gamePixel(g, x, 103).r - want); d = std::min(d, 256 - d);
			w = std::max(w, d);
		}
		if (w < worst) { worst = w; bestOffset = off; }
	}
	printf("  first 12 texels sampled (R/4):");
	for (int x = 0; x < 12; ++x) printf(" %d", gamePixel(g, x, 103).r / 4);
	printf("\n  last 6 texels sampled (R/4):");
	for (int x = 250; x < 256; ++x) printf(" %d", gamePixel(g, x, 103).r / 4);
	printf("\n  best constant pixel offset %.4f\n", bestOffset);
	check(worst <= 4, "backend-generated shaders sample the 64x-repeated ramp exactly (worst error %d/255 = %.2f texel around a constant %.4f pixel offset)", worst, worst / 4.0, bestOffset);
	tex->Release();
}

// Indicative cost of the no-multiview stereo path (simulator GPU, CPU wall time including the
// shared-event wait for the GPU): many small world draws, three layouts. The checks assert the
// structural facts (fewer GL draws with elision, fewer FBO binds in atlas mode); the milliseconds
// are printed for docs/visionos-gles-backend.md and are only indicative on a simulator.
static void casePerf()
{
	printf("\n[case] cost of the stereo path without multiview (200 world draws per frame, 1024x1024 per eye)\n");
	IDirect3DTexture8 *tex = makeCheckTexture();
	struct Mode { const char *name; bool atlas, elide; double ms = 0; unsigned draws = 0, binds = 0; };
	Mode modes[3] = { { "separate eyes, ordinary world draw kept", false, false }, { "atlas, ordinary world draw kept", true, false }, { "atlas, ordinary world draw elided", true, true } };
	for (Mode &m : modes) {
		FrameTargets f;
		if (!makeSlot(&f, 1024, 1024, m.atlas)) { check(false, "%s: slot created", m.name); continue; }
		FrameOptions o; o.elide = m.elide; o.worldDraws = 200;
		for (int i = 0; i < 4; ++i) runFrame(&f, o, tex); // warm up: shaders, layers, elision certification
		const int frames = 15, batches = 5; // best batch mean: the machine is shared, the minimum is the least disturbed
		FrameResult last;
		m.ms = 1e30;
		for (int b = 0; b < batches; ++b) {
			const auto t0 = std::chrono::steady_clock::now();
			for (int i = 0; i < frames; ++i) last = runFrame(&f, o, tex);
			const auto t1 = std::chrono::steady_clock::now();
			m.ms = std::min(m.ms, std::chrono::duration<double, std::milli>(t1 - t0).count() / frames);
		}
		m.draws = last.drawCalls; m.binds = last.bindCalls;
		printf("  %-42s %7.2f ms/frame   GL draws/frame %4u   glBindFramebuffer/frame %4u\n", m.name, m.ms, m.draws, m.binds);
	}
	check(modes[1].binds < modes[0].binds, "atlas mode binds fewer framebuffers per frame than separate eyes (%u < %u)", modes[1].binds, modes[0].binds);
	check(modes[2].draws < modes[1].draws, "elision submits fewer GL draws per frame (%u < %u)", modes[2].draws, modes[1].draws);
	tex->Release();
}

static void caseApiSurface()
{
	printf("\n[case] API surface\n");
	check(!d3d8gles_ShouldUseVulkanBackend() && !d3d8gles_ShouldUseANGLE(), "d3d8gles_ShouldUseVulkanBackend / ShouldUseANGLE stubs answer false off Android");
	const D3D8GLES_XRConfig *cfg = d3d8gles_GetXRConfig();
	check(cfg == &g_cfg && cfg->getProcAddress == H->getProcAddress, "d3d8gles_GetXRConfig returns the host config with its resolver");
	d3d8gles_ConfigureXRMultiview(H->getProcAddress); // must be ignored when a host resolver is set
	check(!d3d8gles_XRStereoMultiview(), "d3d8gles_ConfigureXRMultiview is ignored with a host resolver (no OVR_multiview on ANGLE-Metal)");
}

int RunDeviceCases(Harness *h, int *checks)
{
	H = h; g_checks = checks; g_failures = 0;
	printf("GL_RENDERER: %s\n", H->glString(0x1F01));
	{
		const char *ext = H->glString(0x1F03);
		if (ext && (strstr(ext, "GL_EXT_texture_compression_s3tc") || strstr(ext, "GL_EXT_texture_compression_dxt1"))) setenv("D3D8GLES_DISABLE_S3TC", "1", 1);
	}
	if (!createDevice()) { printf("FATAL: could not create the d3d8gles device\n"); return 1; }
	printf("d3d8gles device created on the host resolver\n");
	caseApiSurface();
	caseSeparateEyes();
	caseAtlas();
	caseBoardClip();
	caseRingRotation();
	caseBackendTargets();
	caseLayersAndElision();
	caseDXT();
	caseFormats();
	caseVaryingPrecision();
	casePerf();
	return g_failures;
}
