#include "applemetalrenderer.h"

#include "applecontrolfeatures.h"

#include "SDL.h"

#include <QCoreApplication>
#include <QDebug>
#include <QHash>
#include <QImage>
#include <QPoint>
#include <QSize>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CADisplayLink.h>
#import <dispatch/dispatch.h>

extern "C" {
#include <libavutil/frame.h>
}

namespace {

constexpr OSType VideoToolboxNv24FullRange = 0x34343466; // '444f'
constexpr OSType VideoToolboxNv24VideoRange = 0x34343476; // '444v'

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
}

QString metalError(const QString& operation, NSError* error)
{
    const char* description = error != nil
            ? error.localizedDescription.UTF8String : nullptr;
    return QCoreApplication::translate(
                   "AppleMetalRenderer", "%1 failed: %2")
            .arg(operation,
                 description != nullptr
                         ? QString::fromUtf8(description)
                         : QCoreApplication::translate(
                                   "AppleMetalRenderer", "Unknown Metal error"));
}

struct Vertex
{
    float x;
    float y;
    float u;
    float v;
};

struct ColorConversion
{
    float yOffset;
    float chromaOffset;
    float yScale;
    float chromaScale;
    float redV;
    float greenU;
    float greenV;
    float blueU;
};

ColorConversion colorConversion(const AppleDecodedTile& tile)
{
    ColorConversion conversion = {
        16.0f / 255.0f,
        128.0f / 255.0f,
        255.0f / 219.0f,
        255.0f / 224.0f,
        1.5748f,
        -0.187324f,
        -0.468124f,
        1.8556f,
    };
    if (tile.colorRange == AppleDecodedTile::ColorRange::Full) {
        conversion.yOffset = 0.0f;
        conversion.yScale = 1.0f;
        conversion.chromaScale = 1.0f;
    }
    if (tile.colorSpace == AppleDecodedTile::ColorSpace::Bt601) {
        conversion.redV = 1.4020f;
        conversion.greenU = -0.344136f;
        conversion.greenV = -0.714136f;
        conversion.blueU = 1.7720f;
    }
    else if (tile.colorSpace == AppleDecodedTile::ColorSpace::Bt2020) {
        conversion.redV = 1.4746f;
        conversion.greenU = -0.164553f;
        conversion.greenV = -0.571353f;
        conversion.blueU = 1.8814f;
    }
    return conversion;
}

const char* shaderSource()
{
    return R"METAL(
#include <metal_stdlib>
using namespace metal;

struct VertexInput {
    float2 position;
    float2 textureCoordinate;
};

struct VertexOutput {
    float4 position [[position]];
    float2 textureCoordinate;
};

struct ColorConversion {
    float yOffset;
    float chromaOffset;
    float yScale;
    float chromaScale;
    float redV;
    float greenU;
    float greenV;
    float blueU;
};

vertex VertexOutput appleVertex(
        uint index [[vertex_id]],
        constant VertexInput* vertices [[buffer(0)]]) {
    VertexOutput output;
    output.position = float4(vertices[index].position, 0.0, 1.0);
    output.textureCoordinate = vertices[index].textureCoordinate;
    return output;
}

float4 convertYuv(float y, float u, float v,
                  constant ColorConversion& conversion) {
    y = (y - conversion.yOffset) * conversion.yScale;
    u = (u - conversion.chromaOffset) * conversion.chromaScale;
    v = (v - conversion.chromaOffset) * conversion.chromaScale;
    return float4(
        y + conversion.redV * v,
        y + conversion.greenU * u + conversion.greenV * v,
        y + conversion.blueU * u,
        1.0);
}

fragment float4 appleNV24Fragment(
        VertexOutput input [[stage_in]],
        texture2d<float> luma [[texture(0)]],
        texture2d<float> chroma [[texture(1)]],
        constant ColorConversion& conversion [[buffer(0)]]) {
    constexpr sampler sampleFilter(address::clamp_to_edge, filter::linear);
    const float y = luma.sample(sampleFilter, input.textureCoordinate).r;
    const float2 uv = chroma.sample(sampleFilter, input.textureCoordinate).rg;
    return convertYuv(y, uv.x, uv.y, conversion);
}

fragment float4 appleVUYAFragment(
        VertexOutput input [[stage_in]],
        texture2d<float> packed [[texture(0)]],
        constant ColorConversion& conversion [[buffer(0)]]) {
    constexpr sampler sampleFilter(address::clamp_to_edge, filter::linear);
    const float4 vuya = packed.sample(sampleFilter, input.textureCoordinate);
    return convertYuv(vuya.b, vuya.g, vuya.r, conversion);
}

fragment float4 appleOverlayFragment(
        VertexOutput input [[stage_in]],
        texture2d<float> overlay [[texture(0)]]) {
    constexpr sampler sampleFilter(address::clamp_to_edge, filter::linear);
    return overlay.sample(sampleFilter, input.textureCoordinate);
}
)METAL";
}

id<MTLRenderPipelineState> makePipeline(
        id<MTLDevice> device,
        id<MTLLibrary> library,
        NSString* fragmentName,
        bool blends,
        NSError** error)
{
    MTLRenderPipelineDescriptor* descriptor =
            [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
    id<MTLFunction> vertexFunction =
            [library newFunctionWithName:@"appleVertex"];
    id<MTLFunction> fragmentFunction =
            [library newFunctionWithName:fragmentName];
    descriptor.vertexFunction = vertexFunction;
    descriptor.fragmentFunction = fragmentFunction;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (blends) {
        MTLRenderPipelineColorAttachmentDescriptor* attachment =
                descriptor.colorAttachments[0];
        attachment.blendingEnabled = YES;
        attachment.sourceRGBBlendFactor = MTLBlendFactorOne;
        attachment.destinationRGBBlendFactor =
                MTLBlendFactorOneMinusSourceAlpha;
        attachment.rgbBlendOperation = MTLBlendOperationAdd;
        attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
        attachment.destinationAlphaBlendFactor =
                MTLBlendFactorOneMinusSourceAlpha;
        attachment.alphaBlendOperation = MTLBlendOperationAdd;
    }
    id<MTLRenderPipelineState> pipeline =
            [device newRenderPipelineStateWithDescriptor:descriptor error:error];
    [vertexFunction release];
    [fragmentFunction release];
    return pipeline;
}

} // namespace

@interface AppleMetalDisplayLinkTarget : NSObject
{
    std::function<void()> _callback;
}

- (instancetype)initWithCallback:(const std::function<void()>&)callback;
- (void)displayLinkDidFire:(CADisplayLink*)displayLink
        API_AVAILABLE(macos(14.0));

@end

@implementation AppleMetalDisplayLinkTarget

- (instancetype)initWithCallback:(const std::function<void()>&)callback
{
    self = [super init];
    if (self != nil) {
        _callback = callback;
    }
    return self;
}

- (void)displayLinkDidFire:(CADisplayLink*)displayLink
        API_AVAILABLE(macos(14.0))
{
    (void)displayLink;
    if (_callback) {
        _callback();
    }
}

@end

class AppleMetalRenderer::Implementation
{
public:
    struct TileResource
    {
        AppleDecodedTile metadata;
        id<MTLTexture> packedTexture = nil;
    };

    ~Implementation()
    {
        stopDisplayLink();
        // Session shutdown first stops the presentation thread. Waiting for the
        // single admitted command buffer here makes all retained AVFrames and
        // CVPixelBuffers safe to release before the Metal view disappears.
        if (inFlightGate != nullptr &&
                dispatch_semaphore_wait(inFlightGate,
                                        dispatch_time(DISPATCH_TIME_NOW,
                                                      2 * NSEC_PER_SEC)) == 0) {
            dispatch_semaphore_signal(inFlightGate);
        }
        clearTiles();
        [overlayTexture release];
        [nv24Pipeline release];
        [vuyaPipeline release];
        [overlayPipeline release];
        [commandQueue release];
        if (textureCache != nullptr) {
            CFRelease(textureCache);
        }
        if (metalView != nullptr) {
            SDL_Metal_DestroyView(metalView);
        }
        [device release];
#if !OS_OBJECT_USE_OBJC
        if (inFlightGate != nullptr) {
            dispatch_release(inFlightGate);
        }
#endif
    }

    void clearTiles()
    {
        for (auto iterator = tiles.begin(); iterator != tiles.end(); ++iterator) {
            [iterator.value().packedTexture release];
            iterator.value().packedTexture = nil;
        }
        tiles.clear();
    }

    bool startDisplayLink(const std::function<void()>& callback)
    { @autoreleasepool {
        if (displayLink != nil || legacyDisplayLink != nullptr || !callback) {
            return displayLink != nil || legacyDisplayLink != nullptr;
        }
        if (@available(macOS 14.0, *)) {
            if (metalView == nullptr) {
                return false;
            }
            NSView* view = reinterpret_cast<NSView*>(metalView);
            NSWindow* nativeWindow = view.window;
            if (nativeWindow == nil) {
                return false;
            }
            displayLinkTarget = [[AppleMetalDisplayLinkTarget alloc]
                    initWithCallback:callback];
            displayLink = [[nativeWindow
                    displayLinkWithTarget:displayLinkTarget
                                  selector:@selector(displayLinkDidFire:)] retain];
            if (displayLink == nil) {
                [displayLinkTarget release];
                displayLinkTarget = nil;
                return false;
            }
            [displayLink addToRunLoop:[NSRunLoop mainRunLoop]
                              forMode:NSRunLoopCommonModes];
            return true;
        }

        displayLinkCallback = callback;
        NSView* view = reinterpret_cast<NSView*>(metalView);
        NSScreen* screen = view.window.screen;
        CVReturn result;
        if (screen != nil) {
            const CGDirectDisplayID displayId =
                    [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
            result = CVDisplayLinkCreateWithCGDisplay(
                    displayId, &legacyDisplayLink);
        }
        else {
            result = CVDisplayLinkCreateWithActiveCGDisplays(
                    &legacyDisplayLink);
        }
        if (result != kCVReturnSuccess || legacyDisplayLink == nullptr ||
                CVDisplayLinkSetOutputCallback(
                        legacyDisplayLink, legacyDisplayLinkDidFire,
                        this) != kCVReturnSuccess ||
                CVDisplayLinkStart(legacyDisplayLink) != kCVReturnSuccess) {
            if (legacyDisplayLink != nullptr) {
                CVDisplayLinkRelease(legacyDisplayLink);
                legacyDisplayLink = nullptr;
            }
            displayLinkCallback = {};
            return false;
        }
        return true;
    }}

    void setDisplayLinkPaused(bool paused)
    {
        if (@available(macOS 14.0, *)) {
            [displayLink setPaused:paused];
        }
        if (legacyDisplayLink != nullptr) {
            if (paused && CVDisplayLinkIsRunning(legacyDisplayLink)) {
                CVDisplayLinkStop(legacyDisplayLink);
            }
            else if (!paused && !CVDisplayLinkIsRunning(legacyDisplayLink)) {
                CVDisplayLinkStart(legacyDisplayLink);
            }
        }
    }

    void stopDisplayLink()
    {
        if (@available(macOS 14.0, *)) {
            [displayLink invalidate];
            [displayLink release];
            displayLink = nil;
        }
        if (legacyDisplayLink != nullptr) {
            CVDisplayLinkStop(legacyDisplayLink);
            CVDisplayLinkRelease(legacyDisplayLink);
            legacyDisplayLink = nullptr;
        }
        [displayLinkTarget release];
        displayLinkTarget = nil;
        displayLinkCallback = {};
    }

    static CVReturn legacyDisplayLinkDidFire(
            CVDisplayLinkRef,
            const CVTimeStamp*,
            const CVTimeStamp*,
            CVOptionFlags,
            CVOptionFlags*,
            void* context)
    {
        Implementation* implementation =
                static_cast<Implementation*>(context);
        if (implementation->displayLinkCallback) {
            implementation->displayLinkCallback();
        }
        return kCVReturnSuccess;
    }

    SDL_Window* window = nullptr;
    QString lastFrameRetryError;
    std::shared_ptr<AppleVideoPresentationFeedback> presentationFeedback =
            std::make_shared<AppleVideoPresentationFeedback>();
    SDL_MetalView metalView = nullptr;
    CAMetalLayer* layer = nil;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> commandQueue = nil;
    id<MTLRenderPipelineState> nv24Pipeline = nil;
    id<MTLRenderPipelineState> vuyaPipeline = nil;
    id<MTLRenderPipelineState> overlayPipeline = nil;
    CVMetalTextureCacheRef textureCache = nullptr;
    QHash<int, TileResource> tiles;
    id<MTLTexture> overlayTexture = nil;
    int overlayWidth = 0;
    int overlayHeight = 0;
    dispatch_semaphore_t inFlightGate = nullptr;
    id displayLink = nil;
    AppleMetalDisplayLinkTarget* displayLinkTarget = nil;
    CVDisplayLinkRef legacyDisplayLink = nullptr;
    std::function<void()> displayLinkCallback;
};

AppleMetalRenderer::AppleMetalRenderer()
    : m_Implementation(std::make_unique<Implementation>())
{
}

AppleMetalRenderer::~AppleMetalRenderer() = default;

bool AppleMetalRenderer::initialize(
        SDL_Window* window,
        const std::shared_ptr<AppleVideoBackendContext>&,
        QString* error)
{ @autoreleasepool {
    if (window == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleMetalRenderer", "The Metal presentation window is unavailable."));
        return false;
    }
    Implementation& implementation = *m_Implementation;
    implementation.window = window;
    implementation.metalView = SDL_Metal_CreateView(window);
    if (implementation.metalView == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleMetalRenderer", "SDL could not create a Metal view: %1")
                               .arg(QString::fromUtf8(SDL_GetError())));
        return false;
    }
    implementation.layer = reinterpret_cast<CAMetalLayer*>(
            SDL_Metal_GetLayer(implementation.metalView));
    implementation.device = MTLCreateSystemDefaultDevice();
    if (implementation.layer == nil || implementation.device == nil) {
        setError(error, QCoreApplication::translate(
                "AppleMetalRenderer", "No Metal presentation device is available."));
        return false;
    }
    implementation.layer.device = implementation.device;
    implementation.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    implementation.layer.framebufferOnly = YES;
    implementation.layer.displaySyncEnabled = YES;
    implementation.layer.presentsWithTransaction = NO;
    if (@available(macOS 10.13.2, *)) {
        implementation.layer.maximumDrawableCount = 2;
        implementation.layer.allowsNextDrawableTimeout = YES;
    }

    implementation.commandQueue = [implementation.device newCommandQueue];
    NSError* compileError = nil;
    NSString* source = [NSString stringWithUTF8String:shaderSource()];
    id<MTLLibrary> library = [implementation.device
            newLibraryWithSource:source
                         options:nil
                           error:&compileError];
    if (implementation.commandQueue == nil || library == nil) {
        setError(error, metalError(QStringLiteral("Metal shader compilation"),
                                   compileError));
        [library release];
        return false;
    }
    implementation.nv24Pipeline = makePipeline(
            implementation.device, library, @"appleNV24Fragment", false,
            &compileError);
    implementation.vuyaPipeline = makePipeline(
            implementation.device, library, @"appleVUYAFragment", false,
            &compileError);
    implementation.overlayPipeline = makePipeline(
            implementation.device, library, @"appleOverlayFragment", true,
            &compileError);
    [library release];
    if (implementation.nv24Pipeline == nil ||
            implementation.vuyaPipeline == nil ||
            implementation.overlayPipeline == nil) {
        setError(error, metalError(QStringLiteral("Metal pipeline creation"),
                                   compileError));
        return false;
    }
    const CVReturn cacheResult = CVMetalTextureCacheCreate(
            kCFAllocatorDefault, nullptr, implementation.device, nullptr,
            &implementation.textureCache);
    if (cacheResult != kCVReturnSuccess ||
            implementation.textureCache == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleMetalRenderer",
                "Couldn’t create the VideoToolbox Metal texture cache (%1).")
                               .arg(cacheResult));
        return false;
    }
    implementation.inFlightGate = dispatch_semaphore_create(1);
    if (implementation.inFlightGate == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleMetalRenderer",
                "Couldn’t create the low-latency Metal presentation gate."));
        return false;
    }
    return true;
}}

QString AppleMetalRenderer::name() const
{
    return QStringLiteral("VideoToolbox/Metal");
}

bool AppleMetalRenderer::usesLowLatencyPresentation() const
{
    return m_Implementation != nullptr &&
           m_Implementation->inFlightGate != nullptr;
}

bool AppleMetalRenderer::outputSize(int* width, int* height) const
{
    if (m_Implementation == nullptr || m_Implementation->window == nullptr ||
            width == nullptr || height == nullptr) {
        return false;
    }
    SDL_Metal_GetDrawableSize(m_Implementation->window, width, height);
    return *width > 0 && *height > 0;
}

bool AppleMetalRenderer::upload(const AppleDecodedTile& frame, QString* error)
{ @autoreleasepool {
    if (!frame.isValid() || m_Implementation == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleMetalRenderer", "The decoded 4:4:4 tile is invalid."));
        return false;
    }
    Implementation& implementation = *m_Implementation;
    Implementation::TileResource next;
    next.metadata = frame;
    if (frame.pixelFormat == AppleDecodedTile::PixelFormat::Vuya) {
        MTLTextureDescriptor* descriptor =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                                              MTLPixelFormatRGBA8Unorm
                                                                 width:frame.width
                                                                height:frame.height
                                                             mipmapped:NO];
        descriptor.storageMode = MTLStorageModeManaged;
        descriptor.usage = MTLTextureUsageShaderRead;
        next.packedTexture =
                [implementation.device newTextureWithDescriptor:descriptor];
        if (next.packedTexture == nil) {
            setError(error, QCoreApplication::translate(
                    "AppleMetalRenderer", "Couldn’t allocate a VUYA Metal texture."));
            return false;
        }
        [next.packedTexture replaceRegion:MTLRegionMake2D(
                                                  0, 0, frame.width, frame.height)
                                  mipmapLevel:0
                                    withBytes:frame.pixels.constData()
                                  bytesPerRow:frame.stride];
    }
    else if (frame.pixelFormat ==
             AppleDecodedTile::PixelFormat::VideoToolboxNv24) {
        AVFrame* hardwareFrame = frame.hardwareFrame.get();
        CVPixelBufferRef pixelBuffer = hardwareFrame != nullptr
                ? reinterpret_cast<CVPixelBufferRef>(hardwareFrame->data[3])
                : nullptr;
        const OSType pixelFormat = pixelBuffer != nullptr
                ? CVPixelBufferGetPixelFormatType(pixelBuffer) : 0;
        if (pixelBuffer == nullptr || CVPixelBufferGetPlaneCount(pixelBuffer) < 2 ||
                (pixelFormat != VideoToolboxNv24FullRange &&
                 pixelFormat != VideoToolboxNv24VideoRange)) {
            setError(error, QCoreApplication::translate(
                    "AppleMetalRenderer",
                    "VideoToolbox did not return a native NV24 surface."));
            return false;
        }
        next.metadata.colorRange = pixelFormat == VideoToolboxNv24FullRange
                ? AppleDecodedTile::ColorRange::Full
                : AppleDecodedTile::ColorRange::Limited;
    }
    else {
        setError(error, QCoreApplication::translate(
                "AppleMetalRenderer",
                "The decoded tile is not a Metal-compatible 4:4:4 surface."));
        return false;
    }

    auto previous = implementation.tiles.find(frame.tileIndex);
    if (previous != implementation.tiles.end()) {
        [previous.value().packedTexture release];
    }
    implementation.tiles.insert(frame.tileIndex, std::move(next));
    return true;
}}

bool AppleMetalRenderer::uploadOverlay(const QImage& image, QString* error)
{ @autoreleasepool {
    if (image.isNull() || m_Implementation == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleMetalRenderer", "The performance overlay image is invalid."));
        return false;
    }
    const QImage converted = image.convertToFormat(
            QImage::Format_ARGB32_Premultiplied);
    MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                                          MTLPixelFormatBGRA8Unorm
                                                             width:converted.width()
                                                            height:converted.height()
                                                         mipmapped:NO];
    descriptor.storageMode = MTLStorageModeManaged;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture =
            [m_Implementation->device newTextureWithDescriptor:descriptor];
    if (texture == nil) {
        setError(error, QCoreApplication::translate(
                "AppleMetalRenderer", "Couldn’t allocate the overlay Metal texture."));
        return false;
    }
    [texture replaceRegion:MTLRegionMake2D(
                                   0, 0, converted.width(), converted.height())
             mipmapLevel:0
               withBytes:converted.constBits()
             bytesPerRow:converted.bytesPerLine()];
    [m_Implementation->overlayTexture release];
    m_Implementation->overlayTexture = texture;
    m_Implementation->overlayWidth = converted.width();
    m_Implementation->overlayHeight = converted.height();
    return true;
}}

void AppleMetalRenderer::clearOverlay()
{
    if (m_Implementation == nullptr) {
        return;
    }
    [m_Implementation->overlayTexture release];
    m_Implementation->overlayTexture = nil;
    m_Implementation->overlayWidth = 0;
    m_Implementation->overlayHeight = 0;
}

AppleVideoRenderer::RenderResult AppleMetalRenderer::render(
        const AppleCanvas& canvas,
        const QList<int>& tileHeights,
        QString* error)
{ @autoreleasepool {
    if (m_Implementation == nullptr || !canvas.isUsable() ||
            tileHeights.size() < canvas.tileCount) {
        setError(error, QCoreApplication::translate(
                "AppleMetalRenderer",
                "The Metal presentation layout is invalid."));
        return RenderResult::Failed;
    }
    Implementation& implementation = *m_Implementation;
    if (implementation.inFlightGate == nullptr ||
            dispatch_semaphore_wait(implementation.inFlightGate,
                                    DISPATCH_TIME_NOW) != 0) {
        return RenderResult::Busy;
    }
    int outputWidth = 0;
    int outputHeight = 0;
    if (!outputSize(&outputWidth, &outputHeight)) {
        dispatch_semaphore_signal(implementation.inFlightGate);
        return RenderResult::Busy;
    }
    implementation.layer.drawableSize = CGSizeMake(outputWidth, outputHeight);
    id<CAMetalDrawable> drawable = [implementation.layer nextDrawable];
    id<MTLCommandBuffer> commandBuffer =
            [implementation.commandQueue commandBuffer];
    if (drawable == nil || commandBuffer == nil) {
        dispatch_semaphore_signal(implementation.inFlightGate);
        return RenderResult::Busy;
    }
    MTLRenderPassDescriptor* pass =
            [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> encoder =
            [commandBuffer renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) {
        dispatch_semaphore_signal(implementation.inFlightGate);
        setError(error, QCoreApplication::translate(
                "AppleMetalRenderer", "Couldn’t create the Metal render encoder."));
        return RenderResult::Failed;
    }

    const auto retryFrame = [&](const QString& reason) {
        // Nothing from this command buffer may reach the screen unless every
        // visible tile was encoded. Keep the previous drawable and let the
        // session retry the retained tiles, even if the sender is now idle.
        [encoder endEncoding];
        dispatch_semaphore_signal(implementation.inFlightGate);
        setError(error, reason);
        if (reason != implementation.lastFrameRetryError) {
            qWarning().noquote() << "Apple Metal frame deferred:" << reason;
            implementation.lastFrameRetryError = reason;
        }
        return RenderResult::Busy;
    };

    const double scale = qMin(static_cast<double>(outputWidth) / canvas.width,
                              static_cast<double>(outputHeight) / canvas.height);
    const int contentWidth = qRound(canvas.width * scale);
    const int contentHeight = qRound(canvas.height * scale);
    const int left = (outputWidth - contentWidth) / 2;
    const int top = (outputHeight - contentHeight) / 2;
    const QList<int> tileBoundaries =
            AppleMediaLayout::verticalTileBoundaries(
                    canvas, tileHeights, contentHeight);

    int logicalTop = 0;
    for (int tileIndex = 0; tileIndex < canvas.tileCount; ++tileIndex) {
        const auto tileIterator = implementation.tiles.constFind(tileIndex);
        const int tileHeight = tileHeights.value(tileIndex);
        const int validHeight = qMin(tileHeight, canvas.height - logicalTop);
        logicalTop += tileHeight;
        if (validHeight <= 0) {
            continue;
        }
        if (tileIterator == implementation.tiles.cend()) {
            return retryFrame(QStringLiteral("Missing visible tile %1").arg(tileIndex));
        }
        const Implementation::TileResource& resource = tileIterator.value();
        const AppleDecodedTile& tile = resource.metadata;
        const float x0 = left * 2.0f / outputWidth - 1.0f;
        const float x1 = (left + contentWidth) * 2.0f / outputWidth - 1.0f;
        const int destinationTop = top + tileBoundaries.value(tileIndex);
        const int destinationBottom = top + tileBoundaries.value(tileIndex + 1);
        const float y0 = 1.0f - destinationTop * 2.0f / outputHeight;
        const float y1 = 1.0f - destinationBottom * 2.0f / outputHeight;
        const float uMaximum = qMin(1.0f,
                                    canvas.width /
                                            static_cast<float>(tile.width));
        const float vMaximum = qMin(1.0f,
                                    validHeight /
                                            static_cast<float>(tile.height));
        const Vertex vertices[] = {
            {x0, y0, 0.0f, 0.0f},
            {x0, y1, 0.0f, vMaximum},
            {x1, y0, uMaximum, 0.0f},
            {x1, y1, uMaximum, vMaximum},
        };
        const ColorConversion conversion = colorConversion(tile);
        [encoder setVertexBytes:vertices length:sizeof(vertices) atIndex:0];
        [encoder setFragmentBytes:&conversion
                           length:sizeof(conversion)
                          atIndex:0];
        if (tile.pixelFormat == AppleDecodedTile::PixelFormat::Vuya) {
            if (resource.packedTexture == nil) {
                return retryFrame(QStringLiteral("Missing packed texture for tile %1")
                                          .arg(tileIndex));
            }
            [encoder setRenderPipelineState:implementation.vuyaPipeline];
            [encoder setFragmentTexture:resource.packedTexture atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                        vertexStart:0
                        vertexCount:4];
            continue;
        }

        AVFrame* hardwareFrame = tile.hardwareFrame.get();
        CVPixelBufferRef pixelBuffer = hardwareFrame != nullptr
                ? reinterpret_cast<CVPixelBufferRef>(hardwareFrame->data[3])
                : nullptr;
        if (pixelBuffer == nullptr) {
            return retryFrame(QStringLiteral("Missing pixel buffer for tile %1")
                                      .arg(tileIndex));
        }
        CVMetalTextureRef lumaImage = nullptr;
        CVMetalTextureRef chromaImage = nullptr;
        const CVReturn lumaResult = CVMetalTextureCacheCreateTextureFromImage(
                kCFAllocatorDefault, implementation.textureCache, pixelBuffer,
                nullptr, MTLPixelFormatR8Unorm,
                CVPixelBufferGetWidthOfPlane(pixelBuffer, 0),
                CVPixelBufferGetHeightOfPlane(pixelBuffer, 0), 0, &lumaImage);
        const CVReturn chromaResult = CVMetalTextureCacheCreateTextureFromImage(
                kCFAllocatorDefault, implementation.textureCache, pixelBuffer,
                nullptr, MTLPixelFormatRG8Unorm,
                CVPixelBufferGetWidthOfPlane(pixelBuffer, 1),
                CVPixelBufferGetHeightOfPlane(pixelBuffer, 1), 1, &chromaImage);
        const bool mapped = lumaResult == kCVReturnSuccess &&
                chromaResult == kCVReturnSuccess &&
                lumaImage != nullptr && chromaImage != nullptr &&
                CVMetalTextureGetTexture(lumaImage) != nil &&
                CVMetalTextureGetTexture(chromaImage) != nil;
        if (mapped) {
            [encoder setRenderPipelineState:implementation.nv24Pipeline];
            [encoder setFragmentTexture:CVMetalTextureGetTexture(lumaImage)
                                atIndex:0];
            [encoder setFragmentTexture:CVMetalTextureGetTexture(chromaImage)
                                atIndex:1];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                        vertexStart:0
                        vertexCount:4];
        }
        if (lumaImage != nullptr) {
            CFRelease(lumaImage);
        }
        if (chromaImage != nullptr) {
            CFRelease(chromaImage);
        }
        if (!mapped) {
            return retryFrame(QStringLiteral(
                    "Tile %1 texture mapping failed (luma=%2, chroma=%3)")
                                      .arg(tileIndex).arg(lumaResult).arg(chromaResult));
        }
    }

    if (implementation.overlayTexture != nil) {
        const QPoint overlayTopLeft = ApplePerformanceOverlayPolicy().topLeft(
                QSize(outputWidth, outputHeight),
                QSize(implementation.overlayWidth,
                      implementation.overlayHeight));
        const float x0 = overlayTopLeft.x() * 2.0f / outputWidth - 1.0f;
        const float x1 = (overlayTopLeft.x() + implementation.overlayWidth) *
                         2.0f / outputWidth - 1.0f;
        const float y0 = 1.0f - overlayTopLeft.y() * 2.0f / outputHeight;
        const float y1 = 1.0f -
                (overlayTopLeft.y() + implementation.overlayHeight) *
                        2.0f / outputHeight;
        const Vertex vertices[] = {
            {x0, y0, 0.0f, 0.0f},
            {x0, y1, 0.0f, 1.0f},
            {x1, y0, 1.0f, 0.0f},
            {x1, y1, 1.0f, 1.0f},
        };
        [encoder setRenderPipelineState:implementation.overlayPipeline];
        [encoder setVertexBytes:vertices length:sizeof(vertices) atIndex:0];
        [encoder setFragmentTexture:implementation.overlayTexture atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                    vertexStart:0
                    vertexCount:4];
    }
    [encoder endEncoding];
    const auto feedback = implementation.presentationFeedback;
    const double submittedAt = CACurrentMediaTime();
    [drawable addPresentedHandler:^(id<MTLDrawable> presentedDrawable) {
        feedback->recordPresentation(submittedAt, presentedDrawable.presentedTime);
    }];
    [commandBuffer presentDrawable:drawable];
    dispatch_semaphore_t gate = implementation.inFlightGate;
    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        feedback->recordGpu(completed.status == MTLCommandBufferStatusCompleted,
                            completed.GPUStartTime, completed.GPUEndTime);
        dispatch_semaphore_signal(gate);
    }];
    [commandBuffer commit];
    implementation.lastFrameRetryError.clear();
    return RenderResult::Presented;
}}

bool AppleMetalRenderer::hasCompletedFrame() const
{
    return m_Implementation != nullptr &&
            m_Implementation->presentationFeedback->hasCompletedFrame();
}

AppleVideoPresentationTimings AppleMetalRenderer::takePresentationTimings()
{
    return m_Implementation != nullptr
            ? m_Implementation->presentationFeedback->takeTimings()
            : AppleVideoPresentationTimings{};
}

bool AppleMetalRenderer::startDisplayLink(
        const std::function<void()>& callback)
{
    return m_Implementation != nullptr &&
            m_Implementation->startDisplayLink(callback);
}

void AppleMetalRenderer::setDisplayLinkPaused(bool paused)
{
    if (m_Implementation != nullptr) {
        m_Implementation->setDisplayLinkPaused(paused);
    }
}

void AppleMetalRenderer::stopDisplayLink()
{
    if (m_Implementation != nullptr) {
        m_Implementation->stopDisplayLink();
    }
}

void AppleMetalRenderer::clear()
{
    if (m_Implementation == nullptr) {
        return;
    }
    m_Implementation->clearTiles();
    clearOverlay();
}
