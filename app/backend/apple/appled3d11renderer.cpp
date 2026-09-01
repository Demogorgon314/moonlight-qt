#include "appled3d11renderer.h"

#include "applecontrolfeatures.h"

#include "SDL.h"
#include "SDL_syswm.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QImage>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/frame.h>
}

using Microsoft::WRL::ComPtr;

namespace {

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
}

QString d3dError(const QString& operation, HRESULT result)
{
    return QCoreApplication::translate(
                   "AppleD3D11Renderer", "%1 failed (0x%2).")
            .arg(operation)
            .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
}

QByteArray shaderBytecode(const QString& name)
{
    QFile file(QStringLiteral(":/data/") + name);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

struct Vertex
{
    float x;
    float y;
    float u;
    float v;
};

struct alignas(16) ColorConversionConstants
{
    float matrix[12] = {};
    float offsets[3] = {};
    float hlgMode = 0.0f;
    float chromaOffset[2] = {};
    float chromaMaximum[2] = {1.0f, 1.0f};
};

static_assert(sizeof(ColorConversionConstants) % 16 == 0);

ColorConversionConstants colorConversion(
        AppleDecodedTile::ColorSpace colorSpace,
        AppleDecodedTile::ColorRange colorRange)
{
    static constexpr std::array<float, 9> Bt601 = {
        1.0f, 1.0f, 1.0f,
        0.0f, -0.3441f, 1.7720f,
        1.4020f, -0.7141f, 0.0f,
    };
    static constexpr std::array<float, 9> Bt709 = {
        1.0f, 1.0f, 1.0f,
        0.0f, -0.1873f, 1.8556f,
        1.5748f, -0.4681f, 0.0f,
    };
    static constexpr std::array<float, 9> Bt2020 = {
        1.0f, 1.0f, 1.0f,
        0.0f, -0.1646f, 1.8814f,
        1.4746f, -0.5714f, 0.0f,
    };
    std::array<float, 9> matrix = Bt709;
    if (colorSpace == AppleDecodedTile::ColorSpace::Bt601) {
        matrix = Bt601;
    }
    else if (colorSpace == AppleDecodedTile::ColorSpace::Bt2020) {
        matrix = Bt2020;
    }

    const bool fullRange = colorRange == AppleDecodedTile::ColorRange::Full;
    const float yScale = fullRange ? 1.0f : 255.0f / (235.0f - 16.0f);
    const float chromaScale = fullRange ? 1.0f : 255.0f / (240.0f - 16.0f);
    for (int index = 0; index < 3; ++index) {
        matrix[index] *= yScale;
    }
    for (int index = 3; index < 9; ++index) {
        matrix[index] *= chromaScale;
    }

    ColorConversionConstants constants;
    constants.offsets[0] = fullRange ? 0.0f : 16.0f / 255.0f;
    constants.offsets[1] = 128.0f / 255.0f;
    constants.offsets[2] = 128.0f / 255.0f;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            constants.matrix[row * 4 + column] = matrix[column * 3 + row];
        }
    }
    return constants;
}

} // namespace

class AppleD3D11Renderer::Implementation
{
public:
    ~Implementation()
    {
        if (frameLatencyWaitableObject != nullptr) {
            CloseHandle(frameLatencyWaitableObject);
        }
    }

    struct TileTexture
    {
        int width = 0;
        int height = 0;
        int textureWidth = 0;
        int textureHeight = 0;
        bool hardwareSurface = false;
        AppleDecodedTile::ColorSpace colorSpace =
                AppleDecodedTile::ColorSpace::Bt709;
        AppleDecodedTile::ColorRange colorRange =
                AppleDecodedTile::ColorRange::Limited;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> resourceView;
    };

    HWND window = nullptr;
    int outputWidth = 0;
    int outputHeight = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<IDXGISwapChain2> lowLatencySwapChain;
    HANDLE frameLatencyWaitableObject = nullptr;
    UINT swapChainFlags = 0;
    ComPtr<ID3D11RenderTargetView> renderTarget;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> videoPixelShader;
    ComPtr<ID3D11PixelShader> overlayPixelShader;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> vertexBuffer;
    ComPtr<ID3D11Buffer> indexBuffer;
    ComPtr<ID3D11Buffer> constantBuffer;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> opaqueBlendState;
    ComPtr<ID3D11BlendState> overlayBlendState;
    ComPtr<ID3D11DepthStencilState> depthStencilState;
    ComPtr<ID3D11RasterizerState> rasterizerState;
    QHash<int, TileTexture> tiles;
    int overlayWidth = 0;
    int overlayHeight = 0;
    ComPtr<ID3D11Texture2D> overlayTexture;
    ComPtr<ID3D11ShaderResourceView> overlayResourceView;

    bool updateOutputSize()
    {
        RECT clientRect = {};
        if (window == nullptr || !GetClientRect(window, &clientRect)) {
            return false;
        }
        outputWidth = qMax<LONG>(0, clientRect.right - clientRect.left);
        outputHeight = qMax<LONG>(0, clientRect.bottom - clientRect.top);
        return outputWidth > 0 && outputHeight > 0;
    }

    bool createRenderTarget(QString* error)
    {
        ComPtr<ID3D11Texture2D> backBuffer;
        const HRESULT result = swapChain->GetBuffer(
                0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(result)) {
            setError(error, d3dError(QStringLiteral("IDXGISwapChain::GetBuffer"),
                                     result));
            return false;
        }
        D3D11_TEXTURE2D_DESC description = {};
        backBuffer->GetDesc(&description);
        outputWidth = static_cast<int>(description.Width);
        outputHeight = static_cast<int>(description.Height);
        const HRESULT viewResult = device->CreateRenderTargetView(
                backBuffer.Get(), nullptr, &renderTarget);
        if (FAILED(viewResult)) {
            setError(error, d3dError(
                                    QStringLiteral("CreateRenderTargetView"),
                                    viewResult));
            return false;
        }
        return true;
    }

    bool resizeIfNeeded(QString* error)
    {
        const int previousWidth = outputWidth;
        const int previousHeight = outputHeight;
        if (!updateOutputSize()) {
            return false;
        }
        if (renderTarget != nullptr && outputWidth == previousWidth &&
                outputHeight == previousHeight) {
            return true;
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);
        renderTarget.Reset();
        const HRESULT result = swapChain->ResizeBuffers(
                0, static_cast<UINT>(outputWidth),
                static_cast<UINT>(outputHeight), DXGI_FORMAT_UNKNOWN,
                swapChainFlags);
        if (FAILED(result)) {
            setError(error, d3dError(QStringLiteral("ResizeBuffers"), result));
            return false;
        }
        return createRenderTarget(error);
    }

    bool updateVertices(const Vertex (&vertices)[4], QString* error)
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT result = context->Map(
                vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(result)) {
            setError(error, d3dError(QStringLiteral("Map vertex buffer"),
                                     result));
            return false;
        }
        std::memcpy(mapped.pData, vertices, sizeof(vertices));
        context->Unmap(vertexBuffer.Get(), 0);
        return true;
    }
};

AppleD3D11Renderer::AppleD3D11Renderer()
    : m_Implementation(std::make_unique<Implementation>())
{
}

AppleD3D11Renderer::~AppleD3D11Renderer() = default;

bool AppleD3D11Renderer::initialize(SDL_Window* window,
                                    void* decoderDevice,
                                    QString* error)
{
    Implementation& implementation = *m_Implementation;
    SDL_SysWMinfo windowInfo = {};
    SDL_VERSION(&windowInfo.version);
    if (window == nullptr || !SDL_GetWindowWMInfo(window, &windowInfo) ||
            windowInfo.subsystem != SDL_SYSWM_WINDOWS) {
        setError(error, QCoreApplication::translate(
                                "AppleD3D11Renderer",
                                "SDL did not expose a native Windows window."));
        return false;
    }
    implementation.window = windowInfo.info.win.window;

    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT result = S_OK;
    if (decoderDevice != nullptr) {
        implementation.device = static_cast<ID3D11Device*>(decoderDevice);
        implementation.device->GetImmediateContext(&implementation.context);
    }
    else {
        result = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                featureLevels, static_cast<UINT>(std::size(featureLevels)),
                D3D11_SDK_VERSION, &implementation.device, &featureLevel,
                &implementation.context);
        if (result == E_INVALIDARG) {
            result = D3D11CreateDevice(
                    nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    featureLevels + 1, 1, D3D11_SDK_VERSION,
                    &implementation.device, &featureLevel,
                    &implementation.context);
        }
    }
    if (FAILED(result)) {
        setError(error, d3dError(QStringLiteral("D3D11CreateDevice"), result));
        return false;
    }

    ComPtr<IDXGIDevice> baseDxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(implementation.device.As(&baseDxgiDevice)) ||
            FAILED(baseDxgiDevice->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        setError(error, QCoreApplication::translate(
                                "AppleD3D11Renderer",
                                "The D3D11 adapter did not expose a DXGI factory."));
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 swapChainDescription = {};
    swapChainDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDescription.SampleDesc.Count = 1;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.BufferCount = 2;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDescription.Flags =
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> swapChain;
    result = factory->CreateSwapChainForHwnd(
            implementation.device.Get(), implementation.window,
            &swapChainDescription, nullptr, nullptr, &swapChain);
    if (SUCCEEDED(result) &&
            SUCCEEDED(swapChain.As(&implementation.lowLatencySwapChain)) &&
            SUCCEEDED(implementation.lowLatencySwapChain->SetMaximumFrameLatency(1))) {
        implementation.frameLatencyWaitableObject =
                implementation.lowLatencySwapChain->GetFrameLatencyWaitableObject();
    }
    if (FAILED(result) || implementation.frameLatencyWaitableObject == nullptr) {
        if (implementation.frameLatencyWaitableObject != nullptr) {
            CloseHandle(implementation.frameLatencyWaitableObject);
            implementation.frameLatencyWaitableObject = nullptr;
        }
        implementation.lowLatencySwapChain.Reset();
        swapChain.Reset();
        swapChainDescription.Flags = 0;
        result = factory->CreateSwapChainForHwnd(
                implementation.device.Get(), implementation.window,
                &swapChainDescription, nullptr, nullptr, &swapChain);
    }
    if (FAILED(result)) {
        swapChain.Reset();
        swapChainDescription.BufferCount = 1;
        swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        swapChainDescription.Flags = 0;
        result = factory->CreateSwapChainForHwnd(
                implementation.device.Get(), implementation.window,
                &swapChainDescription, nullptr, nullptr, &swapChain);
    }
    if (FAILED(result) || FAILED(swapChain.As(&implementation.swapChain))) {
        setError(error, d3dError(QStringLiteral("CreateSwapChainForHwnd"),
                                 result));
        return false;
    }
    implementation.swapChainFlags = swapChainDescription.Flags;
    if (implementation.frameLatencyWaitableObject == nullptr) {
        ComPtr<IDXGIDevice1> dxgiDevice;
        if (SUCCEEDED(implementation.device.As(&dxgiDevice))) {
            // Legacy swap chains have only the device-wide queue limit. The
            // waitable path uses the per-swap-chain latency contract instead.
            dxgiDevice->SetMaximumFrameLatency(1);
        }
    }
    factory->MakeWindowAssociation(implementation.window,
                                   DXGI_MWA_NO_ALT_ENTER);
    if (!implementation.createRenderTarget(error)) {
        return false;
    }

    const QByteArray vertexBytecode = shaderBytecode(
            QStringLiteral("d3d11_vertex.fxc"));
    const QByteArray videoBytecode = shaderBytecode(
            QStringLiteral("d3d11_ayuv_pixel.fxc"));
    const QByteArray overlayBytecode = shaderBytecode(
            QStringLiteral("d3d11_overlay_pixel.fxc"));
    if (vertexBytecode.isEmpty() || videoBytecode.isEmpty() ||
            overlayBytecode.isEmpty() ||
            FAILED(implementation.device->CreateVertexShader(
                    vertexBytecode.constData(), vertexBytecode.size(), nullptr,
                    &implementation.vertexShader)) ||
            FAILED(implementation.device->CreatePixelShader(
                    videoBytecode.constData(), videoBytecode.size(), nullptr,
                    &implementation.videoPixelShader)) ||
            FAILED(implementation.device->CreatePixelShader(
                    overlayBytecode.constData(), overlayBytecode.size(), nullptr,
                    &implementation.overlayPixelShader))) {
        setError(error, QCoreApplication::translate(
                                "AppleD3D11Renderer",
                                "The D3D11 presentation shaders could not be loaded."));
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC inputElements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    result = implementation.device->CreateInputLayout(
            inputElements, static_cast<UINT>(std::size(inputElements)),
            vertexBytecode.constData(), vertexBytecode.size(),
            &implementation.inputLayout);
    if (FAILED(result)) {
        setError(error, d3dError(QStringLiteral("CreateInputLayout"), result));
        return false;
    }

    D3D11_BUFFER_DESC vertexDescription = {};
    vertexDescription.ByteWidth = sizeof(Vertex) * 4;
    vertexDescription.Usage = D3D11_USAGE_DYNAMIC;
    vertexDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertexDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    result = implementation.device->CreateBuffer(
            &vertexDescription, nullptr, &implementation.vertexBuffer);
    if (FAILED(result)) {
        setError(error, d3dError(QStringLiteral("Create vertex buffer"), result));
        return false;
    }

    const quint32 indices[] = {0, 1, 2, 3, 2, 1};
    D3D11_BUFFER_DESC indexDescription = {};
    indexDescription.ByteWidth = sizeof(indices);
    indexDescription.Usage = D3D11_USAGE_IMMUTABLE;
    indexDescription.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA indexData = {};
    indexData.pSysMem = indices;
    result = implementation.device->CreateBuffer(
            &indexDescription, &indexData, &implementation.indexBuffer);
    if (FAILED(result)) {
        setError(error, d3dError(QStringLiteral("Create index buffer"), result));
        return false;
    }

    D3D11_BUFFER_DESC constantDescription = {};
    constantDescription.ByteWidth = sizeof(ColorConversionConstants);
    constantDescription.Usage = D3D11_USAGE_DYNAMIC;
    constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    result = implementation.device->CreateBuffer(
            &constantDescription, nullptr, &implementation.constantBuffer);
    if (FAILED(result)) {
        setError(error, d3dError(QStringLiteral("Create colour buffer"), result));
        return false;
    }

    D3D11_SAMPLER_DESC samplerDescription = {};
    samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
    result = implementation.device->CreateSamplerState(
            &samplerDescription, &implementation.sampler);
    if (FAILED(result)) {
        setError(error, d3dError(QStringLiteral("Create sampler"), result));
        return false;
    }

    D3D11_BLEND_DESC opaqueBlendDescription = {};
    opaqueBlendDescription.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
    result = implementation.device->CreateBlendState(
            &opaqueBlendDescription, &implementation.opaqueBlendState);
    if (FAILED(result)) {
        setError(error, d3dError(QStringLiteral("Create opaque blend state"),
                                 result));
        return false;
    }
    D3D11_BLEND_DESC overlayBlendDescription = {};
    D3D11_RENDER_TARGET_BLEND_DESC& overlayBlend =
            overlayBlendDescription.RenderTarget[0];
    overlayBlend.BlendEnable = TRUE;
    overlayBlend.SrcBlend = D3D11_BLEND_ONE;
    overlayBlend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    overlayBlend.BlendOp = D3D11_BLEND_OP_ADD;
    overlayBlend.SrcBlendAlpha = D3D11_BLEND_ONE;
    overlayBlend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    overlayBlend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    overlayBlend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    result = implementation.device->CreateBlendState(
            &overlayBlendDescription, &implementation.overlayBlendState);
    if (FAILED(result)) {
        setError(error, d3dError(QStringLiteral("Create overlay blend state"),
                                 result));
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depthDescription = {};
    depthDescription.DepthEnable = FALSE;
    depthDescription.StencilEnable = FALSE;
    result = implementation.device->CreateDepthStencilState(
            &depthDescription, &implementation.depthStencilState);
    if (FAILED(result)) {
        setError(error, d3dError(QStringLiteral("Create depth state"), result));
        return false;
    }
    D3D11_RASTERIZER_DESC rasterizerDescription = {};
    rasterizerDescription.FillMode = D3D11_FILL_SOLID;
    rasterizerDescription.CullMode = D3D11_CULL_NONE;
    rasterizerDescription.DepthClipEnable = TRUE;
    result = implementation.device->CreateRasterizerState(
            &rasterizerDescription, &implementation.rasterizerState);
    if (FAILED(result)) {
        setError(error, d3dError(QStringLiteral("Create rasterizer"), result));
        return false;
    }
    return true;
}

QString AppleD3D11Renderer::name() const
{
    return QStringLiteral("D3D11VA/DXGI");
}

bool AppleD3D11Renderer::usesLowLatencyPresentation() const
{
    return m_Implementation != nullptr &&
           m_Implementation->frameLatencyWaitableObject != nullptr;
}

bool AppleD3D11Renderer::outputSize(int* width, int* height) const
{
    if (m_Implementation == nullptr || width == nullptr || height == nullptr) {
        return false;
    }
    RECT clientRect = {};
    if (m_Implementation->window == nullptr ||
            !GetClientRect(m_Implementation->window, &clientRect)) {
        return false;
    }
    *width = qMax<LONG>(0, clientRect.right - clientRect.left);
    *height = qMax<LONG>(0, clientRect.bottom - clientRect.top);
    return *width > 0 && *height > 0;
}

bool AppleD3D11Renderer::upload(const AppleDecodedTile& frame, QString* error)
{
    if (!frame.isValid() ||
            (frame.pixelFormat != AppleDecodedTile::PixelFormat::Vuya &&
             frame.pixelFormat != AppleDecodedTile::PixelFormat::D3d11Ayuv)) {
        setError(error, QCoreApplication::translate(
                                "AppleD3D11Renderer",
                                "The decoded tile is not AYUV/VUYA 4:4:4."));
        return false;
    }
    Implementation& implementation = *m_Implementation;
    Implementation::TileTexture& tile = implementation.tiles[frame.tileIndex];

    if (frame.pixelFormat == AppleDecodedTile::PixelFormat::D3d11Ayuv) {
        AVFrame* hardwareFrame = frame.hardwareFrame.get();
        ID3D11Texture2D* texture = hardwareFrame != nullptr
                ? reinterpret_cast<ID3D11Texture2D*>(hardwareFrame->data[0])
                : nullptr;
        if (texture == nullptr) {
            setError(error, QCoreApplication::translate(
                                    "AppleD3D11Renderer",
                                    "The decoded AYUV surface is unavailable."));
            return false;
        }
        ComPtr<ID3D11Device> textureDevice;
        texture->GetDevice(&textureDevice);
        if (textureDevice.Get() != implementation.device.Get()) {
            setError(error, QCoreApplication::translate(
                                    "AppleD3D11Renderer",
                                    "The AYUV surface belongs to a different D3D11 device."));
            return false;
        }
        D3D11_TEXTURE2D_DESC textureDescription = {};
        texture->GetDesc(&textureDescription);
        if (textureDescription.Format != DXGI_FORMAT_AYUV) {
            setError(error, QCoreApplication::translate(
                                    "AppleD3D11Renderer",
                                    "The hardware decoder did not return an AYUV surface."));
            return false;
        }
        if (!tile.hardwareSurface || tile.texture == nullptr ||
                tile.textureWidth != static_cast<int>(textureDescription.Width) ||
                tile.textureHeight != static_cast<int>(textureDescription.Height)) {
            tile = {};
            tile.width = frame.width;
            tile.height = frame.height;
            tile.textureWidth = static_cast<int>(textureDescription.Width);
            tile.textureHeight = static_cast<int>(textureDescription.Height);
            tile.hardwareSurface = true;
            D3D11_TEXTURE2D_DESC displayTextureDescription = {};
            displayTextureDescription.Width = textureDescription.Width;
            displayTextureDescription.Height = textureDescription.Height;
            displayTextureDescription.MipLevels = 1;
            displayTextureDescription.ArraySize = 1;
            displayTextureDescription.Format = DXGI_FORMAT_AYUV;
            displayTextureDescription.SampleDesc.Count = 1;
            displayTextureDescription.Usage = D3D11_USAGE_DEFAULT;
            displayTextureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            HRESULT result = implementation.device->CreateTexture2D(
                    &displayTextureDescription, nullptr, &tile.texture);
            if (SUCCEEDED(result)) {
                D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription = {};
                viewDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                viewDescription.Texture2D.MostDetailedMip = 0;
                viewDescription.Texture2D.MipLevels = 1;
                result = implementation.device->CreateShaderResourceView(
                        tile.texture.Get(), &viewDescription,
                        &tile.resourceView);
            }
            if (FAILED(result)) {
                setError(error, d3dError(
                                        QStringLiteral("Create AYUV display texture"),
                                        result));
                tile = {};
                return false;
            }
        }
        tile.width = frame.width;
        tile.height = frame.height;
        tile.colorSpace = frame.colorSpace;
        tile.colorRange = frame.colorRange;
        const UINT sourceSubresource = D3D11CalcSubresource(
                0,
                static_cast<UINT>(reinterpret_cast<quintptr>(
                        hardwareFrame->data[1])),
                1);
        // Keep decoded surfaces in the GPU domain, but copy them into four
        // presentation-owned textures. Holding decoder-pool slices as the
        // displayed frame eventually exhausts FFmpeg's fixed D3D11VA pool.
        implementation.context->CopySubresourceRegion(
                tile.texture.Get(), 0, 0, 0, 0,
                texture, sourceSubresource, nullptr);
        return true;
    }

    if (tile.width != frame.width || tile.height != frame.height ||
            tile.texture == nullptr || tile.hardwareSurface) {
        tile = {};
        tile.width = frame.width;
        tile.height = frame.height;
        tile.textureWidth = frame.width;
        tile.textureHeight = frame.height;
        D3D11_TEXTURE2D_DESC textureDescription = {};
        textureDescription.Width = frame.width;
        textureDescription.Height = frame.height;
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = 1;
        textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Usage = D3D11_USAGE_DEFAULT;
        textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT result = implementation.device->CreateTexture2D(
                &textureDescription, nullptr, &tile.texture);
        if (SUCCEEDED(result)) {
            result = implementation.device->CreateShaderResourceView(
                    tile.texture.Get(), nullptr, &tile.resourceView);
        }
        if (FAILED(result)) {
            setError(error, d3dError(QStringLiteral("Create 4:4:4 tile texture"),
                                     result));
            tile = {};
            return false;
        }
    }
    tile.colorSpace = frame.colorSpace;
    tile.colorRange = frame.colorRange;
    implementation.context->UpdateSubresource(
            tile.texture.Get(), 0, nullptr, frame.pixels.constData(),
            frame.stride, 0);
    return true;
}

bool AppleD3D11Renderer::uploadOverlay(const QImage& image, QString* error)
{
    if (image.isNull() || image.format() != QImage::Format_ARGB32_Premultiplied) {
        setError(error, QCoreApplication::translate(
                                "AppleD3D11Renderer",
                                "The performance overlay has an unsupported format."));
        return false;
    }
    Implementation& implementation = *m_Implementation;
    if (implementation.overlayWidth != image.width() ||
            implementation.overlayHeight != image.height() ||
            implementation.overlayTexture == nullptr) {
        D3D11_TEXTURE2D_DESC description = {};
        description.Width = image.width();
        description.Height = image.height();
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        // The overlay changes once per statistics sample while the previous
        // frame may still be in flight. A dynamic texture lets WRITE_DISCARD
        // rename its storage instead of synchronizing with the GPU reader.
        description.Usage = D3D11_USAGE_DYNAMIC;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Texture2D> overlayTexture;
        ComPtr<ID3D11ShaderResourceView> overlayResourceView;
        HRESULT result = implementation.device->CreateTexture2D(
                &description, nullptr, &overlayTexture);
        if (SUCCEEDED(result)) {
            result = implementation.device->CreateShaderResourceView(
                    overlayTexture.Get(), nullptr, &overlayResourceView);
        }
        if (FAILED(result)) {
            setError(error, d3dError(QStringLiteral("Create overlay texture"),
                                     result));
            return false;
        }
        implementation.overlayTexture = std::move(overlayTexture);
        implementation.overlayResourceView = std::move(overlayResourceView);
        implementation.overlayWidth = image.width();
        implementation.overlayHeight = image.height();
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT mapResult = implementation.context->Map(
            implementation.overlayTexture.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(mapResult)) {
        setError(error, d3dError(QStringLiteral("Map overlay texture"),
                                 mapResult));
        return false;
    }
    const int rowBytes = image.width() * 4;
    for (int row = 0; row < image.height(); ++row) {
        std::memcpy(static_cast<quint8*>(mapped.pData) + row * mapped.RowPitch,
                    image.constScanLine(row), rowBytes);
    }
    implementation.context->Unmap(implementation.overlayTexture.Get(), 0);
    return true;
}

AppleD3D11Renderer::RenderResult AppleD3D11Renderer::render(
        const AppleCanvas& canvas,
        const QList<int>& tileHeights,
        QString* error)
{
    if (m_Implementation == nullptr || !canvas.isUsable() ||
            tileHeights.size() < canvas.tileCount) {
        setError(error, QCoreApplication::translate(
                "AppleD3D11Renderer",
                "The D3D11 presentation layout is invalid."));
        return RenderResult::Failed;
    }
    Implementation& implementation = *m_Implementation;
    if (!implementation.resizeIfNeeded(error)) {
        return RenderResult::Failed;
    }
    if (implementation.frameLatencyWaitableObject != nullptr &&
            WaitForSingleObjectEx(
                    implementation.frameLatencyWaitableObject, 0, FALSE) !=
                    WAIT_OBJECT_0) {
        return RenderResult::Busy;
    }
    // Use the actual back-buffer dimensions after a resize. DXGI may round
    // these during a DPI transition, so presentation geometry belongs here
    // rather than in the protocol session.
    const int outputWidth = implementation.outputWidth;
    const int outputHeight = implementation.outputHeight;
    const double scale = qMin(static_cast<double>(outputWidth) / canvas.width,
                              static_cast<double>(outputHeight) / canvas.height);
    const int contentWidth = qRound(canvas.width * scale);
    const int contentHeight = qRound(canvas.height * scale);
    const int left = (outputWidth - contentWidth) / 2;
    const int top = (outputHeight - contentHeight) / 2;
    const QList<int> tileBoundaries =
            AppleMediaLayout::verticalTileBoundaries(
                    canvas, tileHeights, contentHeight);

    ID3D11RenderTargetView* renderTarget = implementation.renderTarget.Get();
    implementation.context->OMSetRenderTargets(1, &renderTarget, nullptr);
    implementation.context->OMSetDepthStencilState(
            implementation.depthStencilState.Get(), 0);
    implementation.context->RSSetState(implementation.rasterizerState.Get());
    const D3D11_VIEWPORT viewport = {
        0.0f, 0.0f, static_cast<float>(outputWidth),
        static_cast<float>(outputHeight), 0.0f, 1.0f,
    };
    implementation.context->RSSetViewports(1, &viewport);
    implementation.context->IASetInputLayout(implementation.inputLayout.Get());
    implementation.context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT vertexStride = sizeof(Vertex);
    UINT vertexOffset = 0;
    ID3D11Buffer* vertexBuffer = implementation.vertexBuffer.Get();
    implementation.context->IASetVertexBuffers(
            0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
    implementation.context->IASetIndexBuffer(
            implementation.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
    implementation.context->VSSetShader(
            implementation.vertexShader.Get(), nullptr, 0);
    ID3D11SamplerState* sampler = implementation.sampler.Get();
    implementation.context->PSSetSamplers(0, 1, &sampler);
    const float black[] = {0.0f, 0.0f, 0.0f, 1.0f};
    implementation.context->ClearRenderTargetView(renderTarget, black);
    implementation.context->OMSetBlendState(
            implementation.opaqueBlendState.Get(), nullptr, 0xffffffff);
    implementation.context->PSSetShader(
            implementation.videoPixelShader.Get(), nullptr, 0);
    ID3D11Buffer* constantBuffer = implementation.constantBuffer.Get();
    implementation.context->PSSetConstantBuffers(0, 1, &constantBuffer);

    int logicalTop = 0;
    for (int tileIndex = 0; tileIndex < canvas.tileCount; ++tileIndex) {
        const auto tileIterator = implementation.tiles.constFind(tileIndex);
        const int tileHeight = tileHeights.value(tileIndex);
        const int validHeight = qMin(tileHeight, canvas.height - logicalTop);
        logicalTop += tileHeight;
        if (tileIterator == implementation.tiles.cend() || validHeight <= 0) {
            continue;
        }
        const Implementation::TileTexture& tile = tileIterator.value();
        const float x0 = left * 2.0f / outputWidth - 1.0f;
        const float x1 = (left + contentWidth) * 2.0f / outputWidth - 1.0f;
        const int destinationTop = top + tileBoundaries.value(tileIndex);
        const int destinationBottom = top + tileBoundaries.value(tileIndex + 1);
        const float y0 = 1.0f - destinationTop * 2.0f / outputHeight;
        const float y1 = 1.0f - destinationBottom * 2.0f / outputHeight;
        const float vMaximum = qMin(1.0f,
                                    validHeight /
                                            static_cast<float>(tile.textureHeight));
        const float uMaximum = qMin(1.0f,
                                    tile.width /
                                            static_cast<float>(tile.textureWidth));
        const Vertex vertices[] = {
            {x0, y0, 0.0f, 0.0f},
            {x0, y1, 0.0f, vMaximum},
            {x1, y0, uMaximum, 0.0f},
            {x1, y1, uMaximum, vMaximum},
        };
        if (!implementation.updateVertices(vertices, error)) {
            return RenderResult::Failed;
        }
        const ColorConversionConstants constants = colorConversion(
                tile.colorSpace, tile.colorRange);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT result = implementation.context->Map(
                implementation.constantBuffer.Get(), 0,
                D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(result)) {
            setError(error, d3dError(QStringLiteral("Map colour buffer"),
                                     result));
            return RenderResult::Failed;
        }
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        implementation.context->Unmap(implementation.constantBuffer.Get(), 0);
        ID3D11ShaderResourceView* resource = tile.resourceView.Get();
        implementation.context->PSSetShaderResources(0, 1, &resource);
        implementation.context->DrawIndexed(6, 0, 0);
    }

    if (implementation.overlayResourceView != nullptr) {
        const QPoint overlayTopLeft = ApplePerformanceOverlayPolicy().topLeft(
                QSize(outputWidth, outputHeight),
                QSize(implementation.overlayWidth,
                      implementation.overlayHeight));
        const float x0 = overlayTopLeft.x() * 2.0f / outputWidth - 1.0f;
        const float x1 = (overlayTopLeft.x() + implementation.overlayWidth) * 2.0f /
                        outputWidth - 1.0f;
        const float y0 = 1.0f - overlayTopLeft.y() * 2.0f / outputHeight;
        const float y1 = 1.0f -
                (overlayTopLeft.y() + implementation.overlayHeight) * 2.0f /
                        outputHeight;
        const Vertex overlayVertices[] = {
            {x0, y0, 0.0f, 0.0f},
            {x0, y1, 0.0f, 1.0f},
            {x1, y0, 1.0f, 0.0f},
            {x1, y1, 1.0f, 1.0f},
        };
        if (!implementation.updateVertices(overlayVertices, error)) {
            return RenderResult::Failed;
        }
        implementation.context->OMSetBlendState(
                implementation.overlayBlendState.Get(), nullptr, 0xffffffff);
        implementation.context->PSSetShader(
                implementation.overlayPixelShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* overlayResource =
                implementation.overlayResourceView.Get();
        implementation.context->PSSetShaderResources(
                0, 1, &overlayResource);
        implementation.context->DrawIndexed(6, 0, 0);
    }

    ID3D11ShaderResourceView* noResource = nullptr;
    implementation.context->PSSetShaderResources(0, 1, &noResource);
    // A latency-1 waitable swap chain admits rendering only after the previous
    // frame has left the queue. This mirrors Swift's two-drawable asynchronous
    // Metal path: the newest desktop state reaches the next vblank without an
    // older rendered frame in front of it, while sync interval 1 prevents tears.
    const UINT presentFlags = implementation.frameLatencyWaitableObject != nullptr
            ? 0 : DXGI_PRESENT_DO_NOT_WAIT;
    const HRESULT presentResult = implementation.swapChain->Present(
            1, presentFlags);
    if (presentResult == DXGI_ERROR_WAS_STILL_DRAWING) {
        return RenderResult::Busy;
    }
    if (FAILED(presentResult)) {
        setError(error, d3dError(QStringLiteral("Present"), presentResult));
        return RenderResult::Failed;
    }
    return RenderResult::Presented;
}

void AppleD3D11Renderer::clearOverlay()
{
    if (m_Implementation != nullptr) {
        m_Implementation->overlayTexture.Reset();
        m_Implementation->overlayResourceView.Reset();
        m_Implementation->overlayWidth = 0;
        m_Implementation->overlayHeight = 0;
    }
}

void AppleD3D11Renderer::clear()
{
    if (m_Implementation != nullptr) {
        m_Implementation->tiles.clear();
    }
    clearOverlay();
}
