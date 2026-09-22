// GXXRMetalRenderer.h - Metal drawing helpers for the visionOS shell.
//
// Two jobs:
//  1. Draw the temporary stereo test tabletop into any color/depth target
//     (the compositor drawable directly, or an offscreen per-eye texture that
//     emulates what the real GL-on-Metal engine will hand over).
//  2. Composite an externally produced per-eye MTLTexture into a compositor
//     drawable view (the path the real engine will use).
//
// Depth is REVERSE-Z (1 = near, 0 = far) because Compositor Services requires it.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <simd/simd.h>

NS_ASSUME_NONNULL_BEGIN

/// A texture presented as a world-anchored quad (the engine's UI / world / game textures, or any
/// named host texture). The quad is centered on `position`, oriented by `orientation` (local +Z is
/// the front face) and is `sizeMeters` wide/high. GL textures are bottom-up: set `flipY`.
@interface GXXRCompositeLayer : NSObject
@property(nonatomic, copy) NSString* name;
@property(nonatomic, strong) id<MTLTexture> texture;
@property(nonatomic) simd_float3 position;
@property(nonatomic) simd_quatf orientation;
@property(nonatomic) simd_float2 sizeMeters;
@property(nonatomic) BOOL flipY;
/// Crop of the source texture shown on the quad: x,y origin and w,h size in [0,1] (GL bottom-up UVs before flipY). Default the whole texture.
@property(nonatomic) simd_float4 uvRect;
/// Source alpha is premultiplied (default YES: the engine writes premultiplied coverage).
@property(nonatomic) BOOL premultipliedAlpha;
/// Source RGB is gamma (sRGB) encoded in a non-sRGB pixel format (default: derived from the texture's
/// pixel format). Sampling decodes it to linear.
@property(nonatomic) BOOL gammaEncoded;
@property(nonatomic, readonly) simd_float4x4 worldFromQuad;

+ (instancetype)layerWithName:(NSString*)name
                      texture:(id<MTLTexture>)texture
                     position:(simd_float3)position
                  orientation:(simd_quatf)orientation
                   sizeMeters:(simd_float2)sizeMeters
                        flipY:(BOOL)flipY;
@end

@interface GXXRMetalRenderer : NSObject

/// YES for the *_sRGB pixel formats (hardware decodes on sampling).
+ (BOOL)isSRGBFormat:(MTLPixelFormat)format;

@property(nonatomic, readonly) id<MTLDevice> device;
@property(nonatomic, readonly) MTLPixelFormat colorFormat;
@property(nonatomic, readonly) MTLPixelFormat depthFormat;

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                            colorFormat:(MTLPixelFormat)colorFormat
                            depthFormat:(MTLPixelFormat)depthFormat;

/// Encodes one render pass that draws the test tabletop scene.
/// @param clear YES clears color to transparent black (alpha 0 shows passthrough) and depth to 0 (far).
- (void)encodeTestSceneInto:(id<MTLCommandBuffer>)commandBuffer
                      color:(id<MTLTexture>)color
                 colorSlice:(NSUInteger)slice
                      depth:(id<MTLTexture>)depth
                   viewport:(MTLViewport)viewport
              clipFromWorld:(simd_float4x4)clipFromWorld
                eyePosition:(simd_float3)eyePosition
             worldFromBoard:(simd_float4x4)worldFromBoard
                  hasBoard:(BOOL)hasBoard
                       time:(float)timeSeconds
                      clear:(BOOL)clear;

/// Encodes one render pass that draws `source` over the whole viewport (premultiplied alpha).
/// `flags` are the GXXR_COMPOSITE_* bits from ShaderTypes.h.
- (void)encodeCompositeInto:(id<MTLCommandBuffer>)commandBuffer
                     source:(id<MTLTexture>)source
                      flags:(uint32_t)flags
                      color:(id<MTLTexture>)color
                 colorSlice:(NSUInteger)slice
                      depth:(id<MTLTexture>)depth
                   viewport:(MTLViewport)viewport
                      clear:(BOOL)clear;

/// Fullscreen composite of (a rectangle of) `source` with a constant reverse-Z depth written for every
/// pixel with alpha > 0 (0 = far; alpha-0 pixels always keep depth 0 so passthrough shows).
/// `uvRect` = {originX, originY, width, height} in [0,1] (use {0,0,1,1} for the whole texture; atlas eyes use half).
- (void)encodeEyeCompositeInto:(id<MTLCommandBuffer>)commandBuffer
                        source:(id<MTLTexture>)source
                         flags:(uint32_t)flags
                        uvRect:(simd_float4)uvRect
                 constantDepth:(float)constantDepth
                         color:(id<MTLTexture>)color
                    colorSlice:(NSUInteger)slice
                         depth:(id<MTLTexture>)depth
                      viewport:(MTLViewport)viewport
                         clear:(BOOL)clear;

/// Draws world-anchored quads (premultiplied blend, reverse-Z depth test + write) into a view's target
/// with the eye's `clipFromWorld`. Loads the existing color/depth (encode it after the eye composite).
- (void)encodeLayers:(NSArray<GXXRCompositeLayer*>*)layers
                into:(id<MTLCommandBuffer>)commandBuffer
               color:(id<MTLTexture>)color
          colorSlice:(NSUInteger)slice
               depth:(id<MTLTexture>)depth
            viewport:(MTLViewport)viewport
       clipFromWorld:(simd_float4x4)clipFromWorld;

/// Encodes an empty pass that just clears the target (used when a frame is skipped but the drawable must still be presented).
- (void)encodeClearInto:(id<MTLCommandBuffer>)commandBuffer
                  color:(id<MTLTexture>)color
             colorSlice:(NSUInteger)slice
                  depth:(id<MTLTexture>)depth;

@end

NS_ASSUME_NONNULL_END
