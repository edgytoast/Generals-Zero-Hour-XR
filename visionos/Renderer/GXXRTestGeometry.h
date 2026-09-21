// GXXRTestGeometry.h - the temporary test tabletop mesh, shared by the direct-Metal renderer
// (GXXRMetalRenderer) and the GLES 3.0 / ANGLE test scene (GXXRGLTestScene) so both draw
// exactly the same board, units and floor grid.
//
// Board space: origin at the board center on the play surface, +X right, +Z toward the player,
// +Y up. Colors are authored in sRGB; the shaders convert.
#ifndef GXXR_TEST_GEOMETRY_H
#define GXXR_TEST_GEOMETRY_H

#include <simd/simd.h>

#include <cmath>
#include <cstddef>
#include <vector>

#include "ShaderTypes.h"

namespace gxxr {

struct Range {
    size_t first = 0;
    size_t count = 0;
};

// Sizes of the test tabletop, meters. Board space: origin at board center on the
// play surface, +X right, +Z toward the player, +Y up.
constexpr float kBoardHalfX = 0.5f;
constexpr float kBoardHalfZ = 0.3f;

class MeshBuilder {
public:
    std::vector<GXXRVertex> v;

    void tri(simd_float3 a, simd_float3 b, simd_float3 c, simd_float3 n, simd_float4 col) {
        push(a, n, col);
        push(b, n, col);
        push(c, n, col);
    }
    void quad(simd_float3 a, simd_float3 b, simd_float3 c, simd_float3 d, simd_float3 n, simd_float4 col) {
        tri(a, b, c, n, col);
        tri(a, c, d, n, col);
    }
    // Axis-aligned box.
    void box(simd_float3 c, simd_float3 h, simd_float4 col) {
        const float x0 = c.x - h.x, x1 = c.x + h.x, y0 = c.y - h.y, y1 = c.y + h.y, z0 = c.z - h.z, z1 = c.z + h.z;
        quad({x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}, {0, 1, 0}, col);   // top
        quad({x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, {0, -1, 0}, col);  // bottom
        quad({x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {0, 0, 1}, col);   // +Z
        quad({x1, y0, z0}, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {0, 0, -1}, col);  // -Z
        quad({x1, y0, z1}, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {1, 0, 0}, col);   // +X
        quad({x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {-1, 0, 0}, col);  // -X
    }
    // Square-base pyramid; c is the center of the base.
    void pyramid(simd_float3 c, float halfBase, float height, simd_float4 col) {
        const simd_float3 p00 = {c.x - halfBase, c.y, c.z - halfBase};
        const simd_float3 p10 = {c.x + halfBase, c.y, c.z - halfBase};
        const simd_float3 p11 = {c.x + halfBase, c.y, c.z + halfBase};
        const simd_float3 p01 = {c.x - halfBase, c.y, c.z + halfBase};
        const simd_float3 apex = {c.x, c.y + height, c.z};
        quad(p00, p10, p11, p01, {0, -1, 0}, col);
        side(p01, p11, apex, col);  // +Z
        side(p11, p10, apex, col);  // +X
        side(p10, p00, apex, col);  // -Z
        side(p00, p01, apex, col);  // -X
    }
    // Flat horizontal quad from (x0,z0) to (x1,z1) at height y, facing +Y.
    void floorQuad(float x0, float z0, float x1, float z1, float y, simd_float4 col) {
        quad({x0, y, z1}, {x1, y, z1}, {x1, y, z0}, {x0, y, z0}, {0, 1, 0}, col);
    }
    Range mark(size_t first) const { return Range{first, v.size() - first}; }

private:
    void push(simd_float3 p, simd_float3 n, simd_float4 c) {
        GXXRVertex vert = {{p.x, p.y, p.z}, {n.x, n.y, n.z}, {c.x, c.y, c.z, c.w}};
        v.push_back(vert);
    }
    void side(simd_float3 a, simd_float3 b, simd_float3 apex, simd_float4 col) {
        simd_float3 n = simd_normalize(simd_cross(b - a, apex - a));
        tri(a, b, apex, n, col);
    }
};

struct TestGeometry {
    std::vector<GXXRVertex> vertices;
    Range board;      // static, board-local
    Range orbiter;    // board-local, animated per frame (drawn at the origin)
    Range floorGrid;  // world space, floor-relative
    Range shadow;     // floor-relative, board-local xz
};

inline TestGeometry BuildTestGeometry() {
    TestGeometry g;
    MeshBuilder m;
    auto rgb = [](float r, float g, float b, float a = 1.0f) { return simd_make_float4(r, g, b, a); };

    // ---- Static board (board-local) ----
    size_t start = m.v.size();
    m.box({0, -0.0205f, 0}, {kBoardHalfX + 0.02f, 0.02f, kBoardHalfZ + 0.02f}, rgb(0.13f, 0.14f, 0.16f));  // slab
    const int nx = 10, nz = 6;
    const float tx = 2 * kBoardHalfX / nx, tz = 2 * kBoardHalfZ / nz;
    for (int iz = 0; iz < nz; ++iz) {
        for (int ix = 0; ix < nx; ++ix) {
            const bool dark = ((ix + iz) & 1) != 0;
            simd_float4 c = dark ? rgb(0.30f, 0.38f, 0.22f) : rgb(0.42f, 0.50f, 0.30f);
            const float x0 = -kBoardHalfX + ix * tx, z0 = -kBoardHalfZ + iz * tz;
            m.floorQuad(x0, z0, x0 + tx, z0 + tz, 0.0f, c);
        }
    }
    // Lake and road (flush details so the board reads as terrain).
    m.floorQuad(-0.02f, -0.30f, 0.14f, -0.06f, 0.0008f, rgb(0.16f, 0.32f, 0.55f));
    m.floorQuad(-0.50f, 0.16f, 0.50f, 0.20f, 0.0008f, rgb(0.55f, 0.52f, 0.45f));
    // Corner posts: orientation reference (mirroring or swapped eyes is obvious).
    const float py = 0.03f;
    m.box({-kBoardHalfX, py, -kBoardHalfZ}, {0.012f, py, 0.012f}, rgb(0.90f, 0.15f, 0.12f));  // back-left  red
    m.box({kBoardHalfX, py, -kBoardHalfZ}, {0.012f, py, 0.012f}, rgb(0.15f, 0.80f, 0.25f));   // back-right green
    m.box({kBoardHalfX, py, kBoardHalfZ}, {0.012f, py, 0.012f}, rgb(0.20f, 0.35f, 0.95f));    // front-right blue
    m.box({-kBoardHalfX, py, kBoardHalfZ}, {0.012f, py, 0.012f}, rgb(0.95f, 0.85f, 0.15f));   // front-left yellow
    // Blue team: cubes with turrets.
    const simd_float2 blue[] = {{-0.32f, -0.12f}, {-0.26f, 0.00f}, {-0.32f, 0.10f}};
    for (auto p : blue) {
        m.box({p.x, 0.015f, p.y}, {0.03f, 0.015f, 0.045f}, rgb(0.20f, 0.40f, 0.95f));
        m.box({p.x, 0.035f, p.y}, {0.018f, 0.008f, 0.02f}, rgb(0.12f, 0.25f, 0.70f));
        m.box({p.x + 0.04f, 0.036f, p.y}, {0.025f, 0.004f, 0.004f}, rgb(0.85f, 0.85f, 0.90f));
    }
    // Red team: pyramids.
    const simd_float2 red[] = {{0.30f, -0.10f}, {0.24f, 0.03f}, {0.32f, 0.11f}};
    for (auto p : red) m.pyramid({p.x, 0.0f, p.y}, 0.03f, 0.07f, rgb(0.92f, 0.22f, 0.18f));
    // Objective: tall yellow pyramid at center; green crate; orange crate.
    m.pyramid({0.0f, 0.0f, 0.02f}, 0.035f, 0.13f, rgb(0.95f, 0.78f, 0.12f));
    m.box({0.20f, 0.02f, -0.20f}, {0.02f, 0.02f, 0.02f}, rgb(0.20f, 0.75f, 0.35f));
    m.box({-0.12f, 0.012f, -0.20f}, {0.03f, 0.012f, 0.015f}, rgb(0.95f, 0.50f, 0.10f));
    g.board = m.mark(start);

    // ---- Orbiter (drawn at origin, animated) ----
    start = m.v.size();
    m.box({0, 0, 0}, {0.015f, 0.015f, 0.015f}, rgb(0.98f, 0.98f, 1.0f));
    m.pyramid({0, 0.015f, 0}, 0.012f, 0.02f, rgb(0.98f, 0.30f, 0.80f));
    g.orbiter = m.mark(start);

    // ---- World floor grid (translucent, y ~ 0) ----
    start = m.v.size();
    const float extent = 6.0f;
    for (int i = -12; i <= 12; ++i) {
        const float p = i * 0.5f;
        const bool major = (i % 4) == 0;
        const float w = major ? 0.008f : 0.004f;
        const float a = major ? 0.55f : 0.30f;
        simd_float4 c = rgb(0.62f, 0.78f, 0.95f, a);
        m.floorQuad(p - w, -extent, p + w, extent, 0.001f, c);  // line along Z
        m.floorQuad(-extent, p - w, extent, p + w, 0.001f, c);  // line along X
    }
    // Origin cross (where the player's floor origin is).
    m.floorQuad(-0.25f, -0.012f, 0.25f, 0.012f, 0.0015f, rgb(0.25f, 0.85f, 0.95f, 0.85f));
    m.floorQuad(-0.012f, -0.25f, 0.012f, 0.25f, 0.0015f, rgb(0.25f, 0.85f, 0.95f, 0.85f));
    g.floorGrid = m.mark(start);

    // ---- Contact shadow under the board: nested translucent quads (soft-ish edge) ----
    start = m.v.size();
    for (int i = 0; i < 5; ++i) {
        const float grow = 0.05f * (float)(4 - i);
        m.floorQuad(-(kBoardHalfX + 0.02f + grow), -(kBoardHalfZ + 0.02f + grow),
                    (kBoardHalfX + 0.02f + grow), (kBoardHalfZ + 0.02f + grow), 0.002f + 0.0001f * i,
                    rgb(0.0f, 0.0f, 0.0f, 0.07f));
    }
    g.shadow = m.mark(start);

    g.vertices = std::move(m.v);
    return g;
}

inline simd_float4x4 Translation(simd_float3 t) {
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[3] = simd_make_float4(t.x, t.y, t.z, 1.0f);
    return m;
}

inline simd_float4x4 RotationY(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[0] = simd_make_float4(c, 0, -s, 0);
    m.columns[2] = simd_make_float4(s, 0, c, 0);
    return m;
}

/// Board-local transform of the animated orbiter at time t (seconds).
inline simd_float4x4 OrbiterLocalTransform(float t) {
    return simd_mul(Translation(simd_make_float3(0.15f * std::cos(t * 0.9f), 0.05f + 0.01f * std::sin(t * 2.3f), 0.15f * std::sin(t * 0.9f))),
                    RotationY(t * 1.7f));
}

}  // namespace gxxr

#endif  // GXXR_TEST_GEOMETRY_H
