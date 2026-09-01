#pragma once

#include "applemediaprotocol.h"
#include "applevideodecoder.h"

#include <QImage>
#include <QList>
#include <QString>

#include <memory>

struct SDL_Window;

// Platform presentation seam for Apple High Performance streams. The session
// supplies decoded tiles and canvas geometry; native devices, textures,
// synchronization, colour conversion, and swap-chain policy stay inside the
// selected adapter.
class AppleVideoRenderer
{
public:
    enum class RenderResult
    {
        Presented,
        Busy,
        Failed,
    };

    virtual ~AppleVideoRenderer() = default;

    virtual QString name() const = 0;
    virtual bool usesLowLatencyPresentation() const = 0;
    virtual bool outputSize(int* width, int* height) const = 0;
    virtual bool upload(const AppleDecodedTile& frame,
                        QString* error = nullptr) = 0;
    virtual bool uploadOverlay(const QImage& image,
                               QString* error = nullptr) = 0;
    virtual void clearOverlay() = 0;
    virtual RenderResult render(const AppleCanvas& canvas,
                                const QList<int>& tileHeights,
                                QString* error = nullptr) = 0;
    virtual void clear() = 0;
};

// Selects and initializes the native adapter without exposing its device types
// to AppleScreenSharingSession. The decoder context keeps any shared hardware
// device alive for as long as the renderer needs it.
std::unique_ptr<AppleVideoRenderer> createAppleVideoRenderer(
        SDL_Window* window,
        const std::shared_ptr<AppleVideoBackendContext>& decoderContext,
        QString* error = nullptr);
