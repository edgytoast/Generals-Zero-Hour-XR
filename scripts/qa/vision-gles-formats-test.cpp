// GeneralsX @test visionOS port - see vision-gles-formats-test.sh. The production conversion code is
// #included from the extracted include; this file supplies the few names it uses and the expectations.
#include <d3d8.h>
#include <GLES3/gl3.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define WARN_ONCE(flagvar, ...) do { static bool flagvar = false; if (!flagvar) { flagvar = true; } } while (0)
#include "production.inc"

static int checks = 0, failures = 0;
static void check(bool ok, const char *what)
{
	++checks;
	if (!ok) { ++failures; fprintf(stderr, "FAIL: %s\n", what); }
}
struct RGBA { int r, g, b, a; };
static RGBA px(const std::vector<uint8_t> &v, size_t i) { return { v[i * 4], v[i * 4 + 1], v[i * 4 + 2], v[i * 4 + 3] }; }
static bool same(RGBA a, RGBA b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

// ---- independent reference decoders (from the S3TC spec) -----------------------------------
static RGBA e565(uint16_t c) { return { ((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31, 255 }; }
static void refColorBlock(const uint8_t *b, bool punchThrough, RGBA out[16])
{
	const uint16_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
	RGBA p0 = e565(c0), p1 = e565(c1), pal[4] = { p0, p1, {}, {} };
	if (!punchThrough || c0 > c1) {
		pal[2] = { (2 * p0.r + p1.r) / 3, (2 * p0.g + p1.g) / 3, (2 * p0.b + p1.b) / 3, 255 };
		pal[3] = { (p0.r + 2 * p1.r) / 3, (p0.g + 2 * p1.g) / 3, (p0.b + 2 * p1.b) / 3, 255 };
	} else {
		pal[2] = { (p0.r + p1.r) / 2, (p0.g + p1.g) / 2, (p0.b + p1.b) / 2, 255 };
		pal[3] = { 0, 0, 0, 0 };
	}
	const uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);
	for (int i = 0; i < 16; ++i) out[i] = pal[(idx >> (2 * i)) & 3];
}
static void refAlphaBlock(const uint8_t *b, int out[16])
{
	int a[8] = { b[0], b[1] };
	if (a[0] > a[1]) for (int i = 1; i < 7; ++i) a[1 + i] = ((7 - i) * a[0] + i * a[1]) / 7;
	else { for (int i = 1; i < 5; ++i) a[1 + i] = ((5 - i) * a[0] + i * a[1]) / 5; a[6] = 0; a[7] = 255; }
	uint64_t bits = 0; for (int i = 0; i < 6; ++i) bits |= (uint64_t)b[2 + i] << (8 * i);
	for (int i = 0; i < 16; ++i) out[i] = a[(bits >> (3 * i)) & 7];
}

static uint32_t lcg = 12345;
static uint8_t rnd() { lcg = lcg * 1664525u + 1013904223u; return (uint8_t)(lcg >> 24); }

static void testDXT(D3DFORMAT fmt, const char *name, int w, int h)
{
	const int bw = (w + 3) / 4, bh = (h + 3) / 4, bytes = fmt == D3DFMT_DXT1 ? 8 : 16;
	std::vector<uint8_t> data((size_t)bw * bh * bytes);
	for (auto &b : data) b = rnd(); // random blocks hit every palette/alpha-mode combination
	UploadDesc up;
	const bool ok = prepareLevelUpload(fmt, w, h, data.data(), data.size(), /*hasS3TC=*/false, &up);
	check(ok && !up.compressed && up.internalFormat == GL_RGBA && up.format == GL_RGBA && up.type == GL_UNSIGNED_BYTE, name);
	if (!ok) return;
	int bad = 0;
	std::vector<uint8_t> out(up.pixels, up.pixels + (size_t)w * h * 4);
	for (int by = 0; by < bh; ++by) for (int bx = 0; bx < bw; ++bx) {
		const uint8_t *blk = &data[((size_t)by * bw + bx) * bytes];
		const uint8_t *cb = fmt == D3DFMT_DXT1 ? blk : blk + 8;
		RGBA col[16]; refColorBlock(cb, fmt == D3DFMT_DXT1, col);
		int alpha[16];
		if (fmt == D3DFMT_DXT3 || fmt == D3DFMT_DXT2) for (int i = 0; i < 16; ++i) alpha[i] = ((blk[i / 2] >> ((i & 1) * 4)) & 15) * 17;
		else if (fmt == D3DFMT_DXT5 || fmt == D3DFMT_DXT4) refAlphaBlock(blk, alpha);
		else for (int i = 0; i < 16; ++i) alpha[i] = col[i].a;
		for (int i = 0; i < 16; ++i) {
			const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
			if (x >= w || y >= h) continue;
			RGBA want = col[i]; want.a = alpha[i];
			if (!same(px(out, (size_t)y * w + x), want)) ++bad;
		}
	}
	char msg[128]; snprintf(msg, sizeof msg, "%s %dx%d software decode equals the reference for every texel (%d mismatches)", name, w, h, bad);
	check(bad == 0, msg);
	// truncated data must be refused, not read out of bounds
	UploadDesc up2;
	check(!prepareLevelUpload(fmt, w, h, data.data(), data.size() - 1, false, &up2), "truncated DXT data is rejected");
	// with S3TC available the level is passed through compressed
	UploadDesc up3;
	check(prepareLevelUpload(fmt, w, h, data.data(), data.size(), true, &up3) && up3.compressed && up3.compressedSize == data.size(), "S3TC present: level stays compressed");
}

int main()
{
	testDXT(D3DFMT_DXT1, "DXT1", 8, 8);
	testDXT(D3DFMT_DXT1, "DXT1", 13, 6);  // non-multiple-of-4 edges
	testDXT(D3DFMT_DXT1, "DXT1", 1, 1);
	testDXT(D3DFMT_DXT3, "DXT3", 16, 8);
	testDXT(D3DFMT_DXT3, "DXT3", 5, 9);
	testDXT(D3DFMT_DXT5, "DXT5", 32, 16);
	testDXT(D3DFMT_DXT5, "DXT5", 7, 3);
	testDXT(D3DFMT_DXT2, "DXT2", 8, 4);
	testDXT(D3DFMT_DXT4, "DXT4", 8, 4);
	// DXT1 punch-through: c0 <= c1, index 3 -> transparent black
	{
		uint8_t blk[8] = { 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }; // c0=0 <= c1=0xFFFF, all index 3
		UploadDesc up; std::vector<uint8_t> v(blk, blk + 8);
		check(prepareLevelUpload(D3DFMT_DXT1, 4, 4, v.data(), 8, false, &up) && up.pixels[3] == 0 && up.pixels[0] == 0, "DXT1 c0<=c1 index 3 decodes to transparent black");
	}

	// ---- uncompressed formats -------------------------------------------------------------
	{
		const uint8_t bgra[4] = { 10, 20, 30, 40 }; // B G R A
		UploadDesc u; check(prepareLevelUpload(D3DFMT_A8R8G8B8, 1, 1, bgra, 4, false, &u) && u.internalFormat == GL_RGBA && u.type == GL_UNSIGNED_BYTE
			&& u.pixels[0] == 30 && u.pixels[1] == 20 && u.pixels[2] == 10 && u.pixels[3] == 40, "A8R8G8B8: BGRA -> RGBA, alpha kept");
		UploadDesc x; check(prepareLevelUpload(D3DFMT_X8R8G8B8, 1, 1, bgra, 4, false, &x) && x.pixels[0] == 30 && x.pixels[3] == 255, "X8R8G8B8: alpha forced opaque");
	}
	{
		const uint16_t v = (31 << 11) | (0 << 5) | 31; // R5G6B5 magenta
		UploadDesc u; check(prepareLevelUpload(D3DFMT_R5G6B5, 1, 1, (const uint8_t *)&v, 2, false, &u) && u.internalFormat == GL_RGB565 && u.format == GL_RGB
			&& u.type == GL_UNSIGNED_SHORT_5_6_5 && memcmp(u.pixels, &v, 2) == 0, "R5G6B5 uploaded as GL_RGB565 unchanged");
	}
	{
		const uint16_t v = (0xA << 12) | (0xB << 8) | (0xC << 4) | 0xD; // A R G B
		UploadDesc u; check(prepareLevelUpload(D3DFMT_A4R4G4B4, 1, 1, (const uint8_t *)&v, 2, false, &u) && u.internalFormat == GL_RGBA4 && u.type == GL_UNSIGNED_SHORT_4_4_4_4
			&& *(const uint16_t *)u.pixels == (uint16_t)((0xB << 12) | (0xC << 8) | (0xD << 4) | 0xA), "A4R4G4B4 -> RGBA4444 component rotate");
	}
	{
		const uint16_t v = (1 << 15) | (31 << 10) | (0 << 5) | 16; // A1 R31 G0 B16
		UploadDesc u; check(prepareLevelUpload(D3DFMT_A1R5G5B5, 1, 1, (const uint8_t *)&v, 2, false, &u) && u.internalFormat == GL_RGB5_A1 && u.type == GL_UNSIGNED_SHORT_5_5_5_1
			&& *(const uint16_t *)u.pixels == (uint16_t)((31 << 11) | (0 << 6) | (16 << 1) | 1), "A1R5G5B5 -> RGB5_A1");
		UploadDesc o; check(prepareLevelUpload(D3DFMT_X1R5G5B5, 1, 1, (const uint8_t *)&v, 2, false, &o) && (*(const uint16_t *)o.pixels & 1) == 1, "X1R5G5B5: alpha bit forced");
		const uint16_t z = 0; UploadDesc c; check(prepareLevelUpload(D3DFMT_A1R5G5B5, 1, 1, (const uint8_t *)&z, 2, false, &c) && (*(const uint16_t *)c.pixels & 1) == 0, "A1R5G5B5: cleared alpha stays transparent");
	}
	{
		const uint8_t l = 77; UploadDesc u;
		check(prepareLevelUpload(D3DFMT_L8, 1, 1, &l, 1, false, &u) && u.internalFormat == GL_LUMINANCE && u.format == GL_LUMINANCE && u.type == GL_UNSIGNED_BYTE && u.pixels[0] == 77, "L8 -> GL_LUMINANCE");
		UploadDesc a; check(prepareLevelUpload(D3DFMT_A8, 1, 1, &l, 1, false, &a) && a.internalFormat == GL_ALPHA && a.format == GL_ALPHA && a.pixels[0] == 77, "A8 -> GL_ALPHA");
		const uint8_t la[2] = { 77, 200 }; UploadDesc b;
		check(prepareLevelUpload(D3DFMT_A8L8, 1, 1, la, 2, false, &b) && b.internalFormat == GL_LUMINANCE_ALPHA && b.format == GL_LUMINANCE_ALPHA && b.pixels[0] == 77 && b.pixels[1] == 200, "A8L8 -> GL_LUMINANCE_ALPHA");
	}
	{
		UploadDesc u; const uint8_t z[4] = {};
		check(!prepareLevelUpload(D3DFMT_R8G8B8, 1, 1, z, 4, false, &u), "an unimplemented format is reported (magenta upload), never silently mis-decoded");
	}
	printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
	return failures ? 1 : 0;
}
