// GXXRStatusPanel.mm - see header. Objective-C++ with ARC.
#import "GXXRStatusPanel.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <QuartzCore/QuartzCore.h>

#include <cmath>

namespace {
constexpr int kCardW = 768, kCardH = 288;
constexpr int kSpinW = 256;
constexpr int kTextureRing = 3;              // the GPU may still read the previous card while a new one is written
constexpr CFTimeInterval kMinRedraw = 0.25;
}

@implementation GXXRStatusPanel {
    id<MTLDevice> _device;
    id<MTLTexture> _card[kTextureRing];
    NSUInteger _cardIndex;
    id<MTLTexture> _spinner;
    NSString* _drawnTitle;
    NSString* _drawnDetail;
    CFTimeInterval _drawnAt;
    NSMutableArray<GXXRCompositeLayer*>* _layers;
}

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device {
    self = [super init];
    if (!self) return nil;
    _device = device;
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:kCardW height:kCardH mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    for (int i = 0; i < kTextureRing; ++i) {
        _card[i] = [device newTextureWithDescriptor:d];
        _card[i].label = @"GXXR status card";
        if (!_card[i]) return nil;
    }
    MTLTextureDescriptor* sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:kSpinW height:kSpinW mipmapped:NO];
    sd.usage = MTLTextureUsageShaderRead;
    sd.storageMode = MTLStorageModeShared;
    _spinner = [device newTextureWithDescriptor:sd];
    _spinner.label = @"GXXR spinner";
    if (!_spinner) return nil;
    [self drawSpinner];
    return self;
}

static CGContextRef MakeContext(int w, int h, void** dataOut) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    void* data = calloc((size_t)w * h, 4);
    CGContextRef ctx = CGBitmapContextCreate(data, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    *dataOut = data;
    return ctx;
}

- (void)drawSpinner {
    void* data = nullptr;
    CGContextRef ctx = MakeContext(kSpinW, kSpinW, &data);
    CGContextSetLineCap(ctx, kCGLineCapRound);
    const CGFloat c = kSpinW * 0.5, r = kSpinW * 0.36;
    const int segments = 24;
    for (int i = 0; i < segments; ++i) {
        const CGFloat a = (CGFloat)i / segments;               // 0 = tail (transparent), 1 = head
        const CGFloat ang = -2.0 * M_PI * a;                   // sweep clockwise
        CGContextSetRGBStrokeColor(ctx, 0.35, 0.78, 1.0, a * a);
        CGContextSetLineWidth(ctx, 14);
        CGContextMoveToPoint(ctx, c + r * std::cos(ang), c + r * std::sin(ang));
        const CGFloat ang2 = -2.0 * M_PI * ((CGFloat)(i + 1) / segments);
        CGContextAddLineToPoint(ctx, c + r * std::cos(ang2), c + r * std::sin(ang2));
        CGContextStrokePath(ctx);
    }
    [_spinner replaceRegion:MTLRegionMake2D(0, 0, kSpinW, kSpinW) mipmapLevel:0 withBytes:data bytesPerRow:(NSUInteger)kSpinW * 4];
    CGContextRelease(ctx);
    free(data);
}

static void DrawText(CGContextRef ctx, NSString* text, CGFloat size, bool bold, CGFloat alpha, CGRect rect) {
    CTFontRef font = CTFontCreateWithName(bold ? CFSTR("Helvetica-Bold") : CFSTR("Helvetica"), size, nullptr);
    CGColorRef color = CGColorCreateGenericRGB(0.93, 0.96, 1.0, alpha);
    NSDictionary* attrs = @{(__bridge id)kCTFontAttributeName : (__bridge id)font, (__bridge id)kCTForegroundColorAttributeName : (__bridge id)color};
    NSAttributedString* as = [[NSAttributedString alloc] initWithString:text attributes:attrs];
    CTFramesetterRef fs = CTFramesetterCreateWithAttributedString((__bridge CFAttributedStringRef)as);
    CGPathRef path = CGPathCreateWithRect(rect, nullptr);
    CTFrameRef frame = CTFramesetterCreateFrame(fs, CFRangeMake(0, 0), path, nullptr);
    CTFrameDraw(frame, ctx);
    CFRelease(frame);
    CGPathRelease(path);
    CFRelease(fs);
    CGColorRelease(color);
    CFRelease(font);
}

- (void)redrawTitle:(NSString*)title detail:(NSString*)detail {
    void* data = nullptr;
    CGContextRef ctx = MakeContext(kCardW, kCardH, &data);
    const CGRect all = CGRectMake(0, 0, kCardW, kCardH);
    CGPathRef card = CGPathCreateWithRoundedRect(CGRectInset(all, 6, 6), 34, 34, nullptr);
    CGContextSetRGBFillColor(ctx, 0.05, 0.07, 0.11, 0.90);
    CGContextAddPath(ctx, card);
    CGContextFillPath(ctx);
    CGContextSetRGBStrokeColor(ctx, 0.35, 0.78, 1.0, 0.85);
    CGContextSetLineWidth(ctx, 5);
    CGContextAddPath(ctx, card);
    CGContextStrokePath(ctx);
    CGPathRelease(card);
    DrawText(ctx, title, 44, true, 1.0, CGRectMake(44, kCardH - 44 - 58, kCardW - 88, 58));
    DrawText(ctx, detail, 27, false, 0.86, CGRectMake(44, 28, kCardW - 88, kCardH - 44 - 58 - 36));
    _cardIndex = (_cardIndex + 1) % kTextureRing;
    [_card[_cardIndex] replaceRegion:MTLRegionMake2D(0, 0, kCardW, kCardH) mipmapLevel:0 withBytes:data bytesPerRow:(NSUInteger)kCardW * 4];
    CGContextRelease(ctx);
    free(data);
    _drawnTitle = [title copy];
    _drawnDetail = [detail copy];
    _drawnAt = CACurrentMediaTime();
}

- (NSArray<GXXRCompositeLayer*>*)layersWithTitle:(NSString*)title
                                          detail:(NSString*)detail
                                         spinner:(BOOL)spinner
                                            time:(CFTimeInterval)time
                                          center:(simd_float3)center
                                          viewer:(simd_float3)viewer {
    const bool changed = !_drawnTitle || ![_drawnTitle isEqualToString:title] || ![_drawnDetail isEqualToString:detail];
    if (changed && CACurrentMediaTime() - _drawnAt >= kMinRedraw) [self redrawTitle:title detail:detail];
    if (!_drawnTitle) return @[];

    const float dx = viewer.x - center.x, dz = viewer.z - center.z;
    const simd_quatf face = simd_quaternion(std::atan2(dx, dz), simd_make_float3(0, 1, 0));
    const float cardW = 0.36f, cardH = cardW * kCardH / kCardW;
    NSMutableArray<GXXRCompositeLayer*>* layers = [NSMutableArray arrayWithCapacity:2];
    [layers addObject:[GXXRCompositeLayer layerWithName:@"status-card" texture:_card[_cardIndex] position:center orientation:face
                                              sizeMeters:simd_make_float2(cardW, cardH) flipY:NO]];
    if (spinner) {
        const simd_float3 up = simd_make_float3(0, 1, 0);
        const simd_quatf spin = simd_quaternion(-(float)std::fmod(time * 3.2, 2.0 * M_PI), simd_make_float3(0, 0, 1));
        [layers addObject:[GXXRCompositeLayer layerWithName:@"status-spinner" texture:_spinner position:center + up * (cardH * 0.5f + 0.075f)
                                                 orientation:simd_mul(face, spin) sizeMeters:simd_make_float2(0.11f, 0.11f) flipY:NO]];
    }
    return layers;
}

@end
