// GXXRGLTestScene.mm - see header. Objective-C++ with ARC.
#import "GXXRGLTestScene.h"

#import <QuartzCore/QuartzCore.h>

#include <GLES3/gl3.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "GXXRTestGeometry.h"

#define GXXR_LOG(fmt, ...) fprintf(stderr, "[GXXR/gltest] " fmt "\n", ##__VA_ARGS__)

namespace {

constexpr int kUIWidth = 1280;
constexpr int kUIHeight = 720;

// ---- GLSL ES 3.00 -----------------------------------------------------------------------------

const char* kSceneVS = R"GLSL(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aColor;
uniform mat4 uClipFromWorld;
uniform mat4 uWorldFromModel;
out vec3 vWorldPos;
out vec3 vWorldNormal;
out vec4 vColor;
void main() {
    vec4 world = uWorldFromModel * vec4(aPos, 1.0);
    gl_Position = uClipFromWorld * world;
    vWorldPos = world.xyz;
    vWorldNormal = mat3(uWorldFromModel) * aNormal;
    vColor = aColor;
}
)GLSL";

const char* kCommonFS = R"GLSL(#version 300 es
precision highp float;
in vec3 vWorldPos;
in vec3 vWorldNormal;
in vec4 vColor;
uniform vec3 uEyePos;
uniform vec3 uLightDir;
uniform vec4 uParams;
out vec4 fragColor;
vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
vec3 linearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
)GLSL";

// Opaque, lit geometry (same model as scene_fragment in TestScene.metal), written gamma-encoded.
const char* kOpaqueFSBody = R"GLSL(
void main() {
    vec3 n = normalize(vWorldNormal);
    vec3 l = normalize(uLightDir);
    vec3 v = normalize(uEyePos - vWorldPos);
    float ndl = clamp(dot(n, l), 0.0, 1.0);
    float hemi = 0.5 + 0.5 * n.y;
    vec3 albedo = srgbToLinear(vColor.rgb);
    vec3 ambient = albedo * mix(vec3(0.10, 0.10, 0.12), vec3(0.22, 0.24, 0.28), hemi);
    vec3 diffuse = albedo * ndl * vec3(1.0, 0.96, 0.90) * 0.95;
    vec3 h = normalize(l + v);
    float spec = pow(clamp(dot(n, h), 0.0, 1.0), 48.0) * 0.18 * ndl;
    float rim = pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 3.0) * 0.06;
    vec3 color = ambient + diffuse + spec + rim;
    fragColor = vec4(linearToSrgb(color), 1.0);
}
)GLSL";

// Translucent ground reference: premultiplied by coverage, fading with distance from the origin.
const char* kGroundFSBody = R"GLSL(
void main() {
    float dist = length(vWorldPos.xz);
    float fade = 1.0 - smoothstep(uParams.y, uParams.z, dist);
    float a = vColor.a * fade;
    fragColor = vec4(vColor.rgb * a, a);
}
)GLSL";

const char* kUIVS = R"GLSL(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
uniform vec2 uSize;
out vec4 vColor;
void main() {
    gl_Position = vec4(aPos.x / uSize.x * 2.0 - 1.0, 1.0 - aPos.y / uSize.y * 2.0, 0.0, 1.0);
    vColor = vec4(aColor.rgb * aColor.a, aColor.a);
}
)GLSL";

const char* kUIFS = R"GLSL(#version 300 es
precision mediump float;
in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)GLSL";

// ---- helpers ----------------------------------------------------------------------------------

simd_float4x4 M(const float* f) {
    simd_float4x4 m;
    memcpy(&m, f, sizeof(float) * 16);
    return m;
}

/// Metal clip space (z in [0,w], reverse-Z) -> GL clip space (z in [-w,w]): z' = 2z - w.
simd_float4x4 GLClipFromMetalClip() {
    simd_float4x4 c = matrix_identity_float4x4;
    c.columns[2] = simd_make_float4(0, 0, 2, 0);
    c.columns[3] = simd_make_float4(0, 0, -1, 1);
    return c;
}

GLuint CompileShader(GLenum type, const std::string& source, const char* label) {
    GLuint s = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        GXXR_LOG("shader %s failed to compile: %s", label, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint LinkProgram(const std::string& vs, const std::string& fs, const char* label) {
    GLuint v = CompileShader(GL_VERTEX_SHADER, vs, label);
    GLuint f = CompileShader(GL_FRAGMENT_SHADER, fs, label);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        GXXR_LOG("program %s failed to link: %s", label, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

struct SceneProgram {
    GLuint id = 0;
    GLint clipFromWorld = -1, worldFromModel = -1, eyePos = -1, lightDir = -1, params = -1;
    void resolve() {
        clipFromWorld = glGetUniformLocation(id, "uClipFromWorld");
        worldFromModel = glGetUniformLocation(id, "uWorldFromModel");
        eyePos = glGetUniformLocation(id, "uEyePos");
        lightDir = glGetUniformLocation(id, "uLightDir");
        params = glGetUniformLocation(id, "uParams");
    }
};

// 3x5 digit font, 15 bits per glyph, top row first, MSB = left.
const uint16_t kDigits[10] = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF};

}  // namespace

@implementation GXXRGLTestScene {
    GXXRANGLEContext* _ctx;
    CFTimeInterval _t0;

    gxxr::TestGeometry _geometry;
    GLuint _meshVBO, _meshVAO;
    SceneProgram _opaque, _ground;
    GLuint _uiProgram, _uiVBO, _uiVAO;
    GLint _uiSize;
    GLuint _fbo;
    GLuint _depthRB[2];
    int _depthW[2], _depthH[2];
    std::vector<float> _uiVerts;
    uint64_t _glErrorCount;
    uint64_t _frames;
    BOOL _ok;
}

- (nullable instancetype)initWithContext:(GXXRANGLEContext*)context {
    self = [super init];
    if (!self) return nil;
    _ctx = context;
    _t0 = CACurrentMediaTime();
    _depthRB[0] = _depthRB[1] = 0;
    _depthW[0] = _depthW[1] = _depthH[0] = _depthH[1] = 0;

    _geometry = gxxr::BuildTestGeometry();
    glGenVertexArrays(1, &_meshVAO);
    glBindVertexArray(_meshVAO);
    glGenBuffers(1, &_meshVBO);
    glBindBuffer(GL_ARRAY_BUFFER, _meshVBO);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(_geometry.vertices.size() * sizeof(GXXRVertex)), _geometry.vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GXXRVertex), (const void*)offsetof(GXXRVertex, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GXXRVertex), (const void*)offsetof(GXXRVertex, normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(GXXRVertex), (const void*)offsetof(GXXRVertex, color));

    _opaque.id = LinkProgram(kSceneVS, std::string(kCommonFS) + kOpaqueFSBody, "opaque");
    _ground.id = LinkProgram(kSceneVS, std::string(kCommonFS) + kGroundFSBody, "ground");
    _uiProgram = LinkProgram(kUIVS, kUIFS, "ui");
    if (!_opaque.id || !_ground.id || !_uiProgram) return nil;
    _opaque.resolve();
    _ground.resolve();
    _uiSize = glGetUniformLocation(_uiProgram, "uSize");

    glGenVertexArrays(1, &_uiVAO);
    glBindVertexArray(_uiVAO);
    glGenBuffers(1, &_uiVBO);
    glBindBuffer(GL_ARRAY_BUFFER, _uiVBO);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (const void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (const void*)(2 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenFramebuffers(1, &_fbo);
    _ok = YES;
    GXXR_LOG("GLES3 test scene ready: %zu vertices, programs linked (GLSL ES 3.00 via ANGLE-Metal)", _geometry.vertices.size());
    return self;
}

- (void)teardown {
    if (!_ctx) return;
    if (_fbo) glDeleteFramebuffers(1, &_fbo);
    for (int i = 0; i < 2; ++i) if (_depthRB[i]) glDeleteRenderbuffers(1, &_depthRB[i]);
    if (_meshVBO) glDeleteBuffers(1, &_meshVBO);
    if (_meshVAO) glDeleteVertexArrays(1, &_meshVAO);
    if (_uiVBO) glDeleteBuffers(1, &_uiVBO);
    if (_uiVAO) glDeleteVertexArrays(1, &_uiVAO);
    if (_opaque.id) glDeleteProgram(_opaque.id);
    if (_ground.id) glDeleteProgram(_ground.id);
    if (_uiProgram) glDeleteProgram(_uiProgram);
    _fbo = _meshVBO = _meshVAO = _uiVBO = _uiVAO = _uiProgram = 0;
    _opaque.id = _ground.id = 0;
    _depthRB[0] = _depthRB[1] = 0;
    _ctx = nil;
    GXXR_LOG("GL test scene torn down (%llu frames, %llu GL errors)", (unsigned long long)_frames, (unsigned long long)_glErrorCount);
}

#pragma mark - Stereo scene

- (BOOL)prepareFramebufferForColor:(unsigned)colorTex width:(int)w height:(int)h depthIndex:(int)di {
    glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    BOOL check = NO;
    if (di >= 0) {
        if (_depthW[di] != w || _depthH[di] != h) {
            if (!_depthRB[di]) glGenRenderbuffers(1, &_depthRB[di]);
            glBindRenderbuffer(GL_RENDERBUFFER, _depthRB[di]);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
            _depthW[di] = w;
            _depthH[di] = h;
            check = YES;
        }
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, _depthRB[di]);
    } else {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
    }
    if (check || _frames == 0) {
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            GXXR_LOG("FBO incomplete (0x%x) for %dx%d color %u depth %d", st, w, h, colorTex, di);
            return NO;
        }
    }
    return YES;
}

- (void)drawSceneWithClipFromWorld:(simd_float4x4)clipFromWorld eyePosition:(simd_float3)eyePos
                    worldFromBoard:(simd_float4x4)worldFromBoard hasBoard:(BOOL)hasBoard time:(float)t {
    const simd_float3 light = simd_normalize(simd_make_float3(0.35f, 0.85f, 0.40f));
    const simd_float4x4 glClip = simd_mul(GLClipFromMetalClip(), clipFromWorld);
    auto useProgram = [&](const SceneProgram& p) {
        glUseProgram(p.id);
        glUniformMatrix4fv(p.clipFromWorld, 1, GL_FALSE, (const float*)&glClip);
        glUniform3f(p.eyePos, eyePos.x, eyePos.y, eyePos.z);
        glUniform3f(p.lightDir, light.x, light.y, light.z);
        glUniform4f(p.params, t, 3.0f, 6.0f, 0.0f);
    };
    auto draw = [&](const SceneProgram& p, gxxr::Range r, simd_float4x4 worldFromModel) {
        glUniformMatrix4fv(p.worldFromModel, 1, GL_FALSE, (const float*)&worldFromModel);
        glDrawArrays(GL_TRIANGLES, (GLint)r.first, (GLsizei)r.count);
    };

    glBindVertexArray(_meshVAO);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);  // reverse-Z
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    useProgram(_opaque);
    if (hasBoard) {
        draw(_opaque, _geometry.board, worldFromBoard);
        draw(_opaque, _geometry.orbiter, simd_mul(worldFromBoard, gxxr::OrbiterLocalTransform(t)));
    }

    // Translucent ground reference: premultiplied blend, depth-tested, no depth write.
    useProgram(_ground);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    const float floorY = hasBoard ? worldFromBoard.columns[3].y - 0.8f : 0.0f;
    draw(_ground, _geometry.floorGrid, gxxr::Translation(simd_make_float3(0.0f, floorY, 0.0f)));
    if (hasBoard) {
        simd_float4x4 onFloor = worldFromBoard;
        onFloor.columns[3].y = floorY;
        draw(_ground, _geometry.shadow, onFloor);
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

#pragma mark - UI panel

- (void)rectX:(float)x y:(float)y w:(float)w h:(float)h r:(float)r g:(float)g b:(float)b a:(float)a {
    const float x1 = x + w, y1 = y + h;
    const float v[6][6] = {{x, y, r, g, b, a}, {x1, y, r, g, b, a}, {x, y1, r, g, b, a},
                           {x1, y, r, g, b, a}, {x1, y1, r, g, b, a}, {x, y1, r, g, b, a}};
    _uiVerts.insert(_uiVerts.end(), &v[0][0], &v[0][0] + 36);
}

- (void)digitsAtX:(float)x y:(float)y cell:(float)cell value:(uint64_t)value count:(int)count r:(float)r g:(float)g b:(float)b {
    for (int i = count - 1; i >= 0; --i) {
        const int d = (int)(value % 10);
        value /= 10;
        const float gx = x + (float)i * cell * 4.0f;
        for (int row = 0; row < 5; ++row) {
            for (int col = 0; col < 3; ++col) {
                if (kDigits[d] & (1 << (14 - (row * 3 + col)))) [self rectX:gx + col * cell y:y + row * cell w:cell h:cell r:r g:g b:b a:1.0f];
            }
        }
    }
}

- (void)renderUIPanelFrame:(uint64_t)frameIndex time:(float)t uiTexture:(unsigned)uiTexture {
    if (!uiTexture) return;
    if (![self prepareFramebufferForColor:uiTexture width:kUIWidth height:kUIHeight depthIndex:-1]) return;

    _uiVerts.clear();
    const float W = kUIWidth, H = kUIHeight;
    [self rectX:0 y:0 w:W h:H r:0.10f g:0.12f b:0.16f a:0.90f];                 // body
    [self rectX:0 y:0 w:W h:8 r:0.35f g:0.75f b:0.95f a:1.0f];                  // border top
    [self rectX:0 y:H - 8 w:W h:8 r:0.35f g:0.75f b:0.95f a:1.0f];              // border bottom
    [self rectX:0 y:0 w:8 h:H r:0.35f g:0.75f b:0.95f a:1.0f];                  // border left
    [self rectX:W - 8 y:0 w:8 h:H r:0.35f g:0.75f b:0.95f a:1.0f];              // border right
    [self rectX:8 y:8 w:W - 16 h:88 r:0.13f g:0.32f b:0.72f a:1.0f];            // title bar
    // Title text-like bars.
    const float titleW[6] = {150, 90, 210, 70, 120, 60};
    float tx = 44;
    for (int i = 0; i < 6; ++i) {
        [self rectX:tx y:40 w:titleW[i] h:26 r:0.95f g:0.97f b:1.0f a:0.95f];
        tx += titleW[i] + 18;
    }
    [self digitsAtX:W - 8 - 6 * 44.0f - 32 y:30 cell:10 value:frameIndex % 1000000ull count:6 r:1.0f g:0.92f b:0.35f];
    // Orientation markers (top-left red, top-right green, bottom-right blue, bottom-left yellow).
    const float m = 56, pad = 22, top = 112;
    [self rectX:pad y:top w:m h:m r:0.92f g:0.16f b:0.14f a:1];
    [self rectX:W - pad - m y:top w:m h:m r:0.16f g:0.80f b:0.26f a:1];
    [self rectX:W - pad - m y:H - pad - m w:m h:m r:0.22f g:0.38f b:0.96f a:1];
    [self rectX:pad y:H - pad - m w:m h:m r:0.96f g:0.86f b:0.16f a:1];
    // Paragraph-like text bars.
    uint32_t seed = 12345u;
    for (int row = 0; row < 8; ++row) {
        seed = seed * 1664525u + 1013904223u;
        const float w = 380.0f + (float)((seed >> 16) % 620u);
        [self rectX:120 y:124 + row * 38.0f w:w h:20 r:0.82f g:0.86f b:0.92f a:0.85f];
    }
    // Animated progress bar.
    const float prog = std::fmod(t * 0.25f, 1.0f);
    [self rectX:120 y:452 w:W - 240 h:32 r:0.04f g:0.05f b:0.07f a:1.0f];
    [self rectX:124 y:456 w:(W - 248) * prog h:24 r:0.25f g:0.85f b:0.40f a:1.0f];
    // Button-like rectangles; the "hovered" one cycles.
    const int hot = (int)(t) % 4;
    const float bw = 236, bh = 96, gap = 32, bx0 = 120, by = 528;
    const float cols[4][3] = {{0.20f, 0.45f, 0.95f}, {0.20f, 0.70f, 0.45f}, {0.90f, 0.55f, 0.15f}, {0.85f, 0.25f, 0.30f}};
    for (int i = 0; i < 4; ++i) {
        const float lift = i == hot ? 1.0f : 0.72f;
        [self rectX:bx0 + i * (bw + gap) y:by w:bw h:bh r:cols[i][0] * lift g:cols[i][1] * lift b:cols[i][2] * lift a:1.0f];
        [self rectX:bx0 + i * (bw + gap) + 40 y:by + 36 w:bw - 80 h:22 r:1 g:1 b:1 a:0.92f];
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, kUIWidth, kUIHeight);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(_uiProgram);
    glUniform2f(_uiSize, W, H);
    glBindVertexArray(_uiVAO);
    glBindBuffer(GL_ARRAY_BUFFER, _uiVBO);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(_uiVerts.size() * sizeof(float)), _uiVerts.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(_uiVerts.size() / 6));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
}

#pragma mark - engine client frame

- (BOOL)renderFrame:(const XRFrameInfo*)frame targets:(const struct D3D8GLES_XRTargets*)targetsPtr output:(GXHostFrameOutput*)out {
    memset(out, 0, sizeof(*out));
    if (!_ok || frame->eye_count == 0 || !targetsPtr) return NO;
    const struct D3D8GLES_XRTargets& targets = *targetsPtr;

    float wfb[16];
    float hx = 0, hz = 0;
    const BOOL hasBoard = XRPresentation_GetTabletopPlacement(wfb, &hx, &hz);
    const float t = (float)(CACurrentMediaTime() - _t0);
    const simd_float4x4 worldFromBoard = hasBoard ? M(wfb) : matrix_identity_float4x4;

    bool any = false;
    for (uint32_t i = 0; i < frame->eye_count && i < XR_MAX_EYES; ++i) {
        const XREyeView& eye = frame->eyes[i];
        const int slot = targets.atlas ? D3D8GLES_XRT_STEREO_LEFT : (i == 0 ? D3D8GLES_XRT_STEREO_LEFT : D3D8GLES_XRT_STEREO_RIGHT);
        const struct D3D8GLES_XRHostTarget& target = targets.slot[slot];
        if (!target.glTexture) continue;
        const int di = targets.atlas ? 0 : (int)i;
        if (![self prepareFramebufferForColor:target.glTexture width:target.width height:target.height depthIndex:di]) continue;

        const int* rect = targets.eyeRect[i];
        glViewport(rect[0], rect[1], rect[2], rect[3]);
        if (targets.atlas) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(rect[0], rect[1], rect[2], rect[3]);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(0, 0, 0, 0);  // alpha 0 = passthrough
        glClearDepthf(0.0f);       // reverse-Z: 0 = far
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        const simd_float3 eyePos = simd_make_float3(eye.pose.position.x, eye.pose.position.y, eye.pose.position.z);
        [self drawSceneWithClipFromWorld:M(eye.clip_from_world) eyePosition:eyePos worldFromBoard:worldFromBoard hasBoard:hasBoard time:t];
        glDisable(GL_SCISSOR_TEST);
        any = true;
    }
    out->stereoValid = any;
    if (hasBoard) {
        out->hasFocus = true;
        out->focus[0] = wfb[12]; out->focus[1] = wfb[13]; out->focus[2] = wfb[14];
    }

    const bool uiTarget = targets.slot[D3D8GLES_XRT_UI].glTexture != 0;
    if (uiTarget) [self renderUIPanelFrame:frame->frame_index time:t uiTexture:targets.slot[D3D8GLES_XRT_UI].glTexture];

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    _frames++;
    if ((_frames % 120) == 1) {
        GLenum e;
        while ((e = glGetError()) != GL_NO_ERROR) {
            _glErrorCount++;
            GXXR_LOG("glGetError = 0x%x (frame %llu)", e, (unsigned long long)_frames);
        }
    }

    // The UI panel stands to the right of the board (clear of the launcher window that hangs over the far edge),
    // turned to face the player's viewpoint 1.25 m in front of the board center, 0.64 x 0.36 m (16:9).
    if (uiTarget && hasBoard) {
        const float px = 0.74f, pz = 0.0f;
        const simd_float4 p = simd_mul(worldFromBoard, simd_make_float4(px, 0.20f, pz, 1.0f));
        const simd_float3x3 boardRot = simd_matrix(simd_normalize(worldFromBoard.columns[0].xyz), simd_normalize(worldFromBoard.columns[1].xyz),
                                                   simd_normalize(worldFromBoard.columns[2].xyz));
        const simd_quatf yaw = simd_quaternion(-std::atan2(px, 1.25f), simd_make_float3(0, 1, 0));
        const simd_quatf q = simd_mul(simd_quaternion(boardRot), yaw);
        GXHostLayer& l = out->layers[out->layerCount++];
        memset(&l, 0, sizeof(l));
        snprintf(l.name, sizeof(l.name), "ui-panel");
        l.target = D3D8GLES_XRT_UI;
        l.position[0] = p.x; l.position[1] = p.y; l.position[2] = p.z;
        l.orientation[0] = q.vector.x; l.orientation[1] = q.vector.y; l.orientation[2] = q.vector.z; l.orientation[3] = q.vector.w;
        l.size[0] = 0.64f; l.size[1] = 0.36f;
        l.flags = GX_LAYER_FLIP_Y | GX_LAYER_PREMULTIPLIED;
    }
    return any || out->layerCount > 0;
}

@end
