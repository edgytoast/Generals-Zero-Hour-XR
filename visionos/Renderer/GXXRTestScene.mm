// GXXRTestScene.mm - see header.
#import "GXXRTestScene.h"

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <cstring>

static simd_float4x4 M(const float* f) {
    simd_float4x4 m;
    memcpy(&m, f, sizeof(float) * 16);
    return m;
}

@implementation GXXRTestScene {
    GXXRMetalRenderer* _renderer;
    BOOL _external;
    CFTimeInterval _t0;
    id<MTLTexture> _offColor[XR_MAX_EYES];
    id<MTLTexture> _offDepth[XR_MAX_EYES];
}

- (instancetype)initWithRenderer:(GXXRMetalRenderer*)renderer externalEyeTextures:(BOOL)external {
    self = [super init];
    if (self) {
        _renderer = renderer;
        _external = external;
        _t0 = CACurrentMediaTime();
    }
    return self;
}

- (void)ensureOffscreenForEye:(uint32_t)eye width:(NSUInteger)w height:(NSUInteger)h {
    if (_offColor[eye] && _offColor[eye].width == w && _offColor[eye].height == h) return;
    MTLTextureDescriptor* cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:_renderer.colorFormat width:w height:h mipmapped:NO];
    cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    cd.storageMode = MTLStorageModePrivate;
    _offColor[eye] = [_renderer.device newTextureWithDescriptor:cd];
    _offColor[eye].label = [NSString stringWithFormat:@"GXXR fake-engine eye %u color", eye];
    MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:_renderer.depthFormat width:w height:h mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget;
    dd.storageMode = MTLStorageModePrivate;
    _offDepth[eye] = [_renderer.device newTextureWithDescriptor:dd];
    _offDepth[eye].label = [NSString stringWithFormat:@"GXXR fake-engine eye %u depth", eye];
}

- (XRFrameResult)renderFrame:(const XRFrameInfo*)frame {
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)frame->command_buffer;
    if (!cb || frame->eye_count == 0) return XR_FRAME_SKIP;

    float wfb[16];
    float hx = 0, hz = 0;
    const bool hasBoard = XRPresentation_GetTabletopPlacement(wfb, &hx, &hz);
    const float t = (float)(CACurrentMediaTime() - _t0);
    const simd_float4x4 worldFromBoard = hasBoard ? M(wfb) : matrix_identity_float4x4;

    for (uint32_t i = 0; i < frame->eye_count; ++i) {
        const XREyeView& eye = frame->eyes[i];
        const simd_float3 eyePos = simd_make_float3(eye.pose.position.x, eye.pose.position.y, eye.pose.position.z);
        const simd_float4x4 clipFromWorld = M(eye.clip_from_world);

        if (!_external) {
            id<MTLTexture> color = (__bridge id<MTLTexture>)eye.color_target;
            id<MTLTexture> depth = (__bridge id<MTLTexture>)eye.depth_target;
            MTLViewport vp = {(double)eye.viewport.x, (double)eye.viewport.y, (double)eye.viewport.width, (double)eye.viewport.height, 0.0, 1.0};
            [_renderer encodeTestSceneInto:cb color:color colorSlice:eye.array_slice depth:depth viewport:vp
                             clipFromWorld:clipFromWorld eyePosition:eyePos worldFromBoard:worldFromBoard
                                  hasBoard:hasBoard time:t clear:eye.first_use_of_target];
        } else {
            const NSUInteger w = (NSUInteger)eye.viewport.width, h = (NSUInteger)eye.viewport.height;
            [self ensureOffscreenForEye:i width:w height:h];
            MTLViewport vp = {0.0, 0.0, (double)w, (double)h, 0.0, 1.0};
            [_renderer encodeTestSceneInto:cb color:_offColor[i] colorSlice:0 depth:_offDepth[i] viewport:vp
                             clipFromWorld:clipFromWorld eyePosition:eyePos worldFromBoard:worldFromBoard
                                  hasBoard:hasBoard time:t clear:YES];
            XREyeSubmit s = {};
            s.eye_index = i;
            s.texture = (__bridge void*)_offColor[i];
            s.width = (uint32_t)w;
            s.height = (uint32_t)h;
            s.flags = XR_SUBMIT_PREMULTIPLIED_ALPHA;  // our shader writes premultiplied color
            s.render_pose = eye.pose;
            s.render_fov = eye.fov;
            XRPresentation_SubmitEyeTexture(&s);
        }
    }
    return _external ? XR_FRAME_SUBMITTED_TEXTURES : XR_FRAME_RENDERED_DIRECT;
}

@end
