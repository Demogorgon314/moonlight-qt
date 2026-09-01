#include "applevideorenderer.h"

#include <QCoreApplication>

#ifdef Q_OS_WIN
#include "appled3d11renderer.h"
#endif

#ifdef Q_OS_DARWIN
#include "applemetalrenderer.h"
#endif

namespace {

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
}

} // namespace

std::unique_ptr<AppleVideoRenderer> createAppleVideoRenderer(
        SDL_Window* window,
        const std::shared_ptr<AppleVideoBackendContext>& decoderContext,
        QString* error)
{
#ifdef Q_OS_WIN
    auto renderer = std::make_unique<AppleD3D11Renderer>();
    void* decoderDevice = decoderContext != nullptr
            ? decoderContext->nativeDevice.get() : nullptr;
    if (!renderer->initialize(window, decoderDevice, error)) {
        return nullptr;
    }
    return renderer;
#elif defined(Q_OS_DARWIN)
    auto renderer = std::make_unique<AppleMetalRenderer>();
    if (!renderer->initialize(window, decoderContext, error)) {
        return nullptr;
    }
    return renderer;
#else
    Q_UNUSED(window);
    Q_UNUSED(decoderContext);
    setError(error, QCoreApplication::translate(
            "AppleVideoRenderer",
            "Apple Screen Sharing video presentation is unsupported on this platform."));
    return nullptr;
#endif
}
