#include "applevideorenderer.h"

#include <QCoreApplication>
#include <QMutexLocker>
#include <cmath>

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

void AppleVideoPresentationFeedback::recordGpu(
        bool succeeded, double startedAt, double endedAt)
{
    if (!succeeded) {
        return;
    }
    m_Completed.store(true);
    if (!std::isfinite(startedAt) || !std::isfinite(endedAt) ||
            startedAt <= 0 || endedAt < startedAt) {
        return;
    }
    QMutexLocker locker(&m_Mutex);
    if (m_Timings.gpuMilliseconds.size() >= 256) {
        m_Timings.gpuMilliseconds.removeFirst();
    }
    m_Timings.gpuMilliseconds.append((endedAt - startedAt) * 1000.0);
}

void AppleVideoPresentationFeedback::recordPresentation(
        double submittedAt, double presentedAt)
{
    if (!std::isfinite(submittedAt) || !std::isfinite(presentedAt) ||
            submittedAt <= 0 || presentedAt <= 0 || presentedAt < submittedAt) {
        return;
    }
    QMutexLocker locker(&m_Mutex);
    if (m_Timings.submitToDisplayMilliseconds.size() >= 256) {
        m_Timings.submitToDisplayMilliseconds.removeFirst();
    }
    m_Timings.submitToDisplayMilliseconds.append(
            (presentedAt - submittedAt) * 1000.0);
}

AppleVideoPresentationTimings AppleVideoPresentationFeedback::takeTimings()
{
    QMutexLocker locker(&m_Mutex);
    AppleVideoPresentationTimings result = std::move(m_Timings);
    m_Timings = {};
    return result;
}

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
