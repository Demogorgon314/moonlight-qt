#pragma once

#include "applemediaprotocol.h"
#include "applevideodecoder.h"

#include <QImage>
#include <QList>
#include <QMutex>
#include <QString>

#include <functional>
#include <memory>
#include <atomic>

struct SDL_Window;

struct AppleVideoPresentationTimings
{
    QList<double> gpuMilliseconds;
    QList<double> submitToDisplayMilliseconds;
};

// Shared with native completion handlers so delayed callbacks cannot access a
// destroyed renderer. Timestamps within each sample must use the same clock.
class AppleVideoPresentationFeedback
{
public:
    void recordGpu(bool succeeded, double startedAt, double endedAt);
    void recordPresentation(double submittedAt, double presentedAt);
    bool hasCompletedFrame() const { return m_Completed.load(); }
    AppleVideoPresentationTimings takeTimings();

private:
    std::atomic_bool m_Completed{false};
    QMutex m_Mutex;
    AppleVideoPresentationTimings m_Timings;
};

// Platform presentation seam for Apple High Performance streams. The session
// supplies decoded tiles and canvas geometry; native devices, textures,
// synchronization, colour conversion, and swap-chain policy stay inside the
// selected adapter.
class AppleVideoRenderer
{
public:
    enum class RenderResult
    {
        // A complete frame was submitted; on-screen timing is asynchronous.
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
    // Adapters without GPU completion feedback use successful submission as
    // readiness. This is distinct from the on-screen timing measurements.
    virtual bool hasCompletedFrame() const { return true; }
    virtual AppleVideoPresentationTimings takePresentationTimings() { return {}; }
    virtual RenderResult render(const AppleCanvas& canvas,
                                const QList<int>& tileHeights,
                                QString* error = nullptr) = 0;
    // Native adapters may pace the session's presentation thread from the
    // display clock. Other adapters remain frame-arrival driven.
    virtual bool startDisplayLink(const std::function<void()>& callback)
    {
        (void)callback;
        return false;
    }
    virtual void setDisplayLinkPaused(bool paused) { (void)paused; }
    virtual void stopDisplayLink() { }
    virtual void clear() = 0;
};

// Selects and initializes the native adapter without exposing its device types
// to AppleScreenSharingSession. The decoder context keeps any shared hardware
// device alive for as long as the renderer needs it.
std::unique_ptr<AppleVideoRenderer> createAppleVideoRenderer(
        SDL_Window* window,
        const std::shared_ptr<AppleVideoBackendContext>& decoderContext,
        QString* error = nullptr);
