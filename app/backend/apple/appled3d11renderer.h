#pragma once

#include "applemediaprotocol.h"
#include "applevideodecoder.h"

#include <QList>
#include <QString>

#include <memory>

class QImage;
struct SDL_Window;

// Owns the low-latency D3D11 presentation path for Apple High Performance
// sessions. SDL owns only the native window and event handling; keeping the
// swap chain here lets us preserve 4:4:4 video and cap DXGI's presentation
// queue at one frame.
class AppleD3D11Renderer
{
public:
    enum class RenderResult
    {
        Presented,
        Busy,
        Failed,
    };

    AppleD3D11Renderer();
    ~AppleD3D11Renderer();

    AppleD3D11Renderer(const AppleD3D11Renderer&) = delete;
    AppleD3D11Renderer& operator=(const AppleD3D11Renderer&) = delete;

    bool initialize(SDL_Window* window,
                    void* decoderDevice = nullptr,
                    QString* error = nullptr);
    bool usesFrameLatencyWaitableObject() const;
    bool outputSize(int* width, int* height) const;
    bool upload(const AppleDecodedTile& frame, QString* error = nullptr);
    bool uploadOverlay(const QImage& image, QString* error = nullptr);
    void clearOverlay();
    RenderResult render(const AppleCanvas& canvas,
                        const QList<int>& tileHeights,
                        const QList<int>& tileBoundaries,
                        int left,
                        int top,
                        int contentWidth,
                        int outputWidth,
                        int outputHeight,
                        QString* error = nullptr);
    void clear();

private:
    class Implementation;
    std::unique_ptr<Implementation> m_Implementation;
};
