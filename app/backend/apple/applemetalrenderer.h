#pragma once

#include "applevideorenderer.h"

#include <memory>

// VideoToolbox/Metal adapter for Apple High Performance streams on macOS.
// It composites the sender's NV24 tiles atomically into a synchronized
// CAMetalLayer while admitting at most one GPU presentation at a time.
class AppleMetalRenderer final : public AppleVideoRenderer
{
public:
    AppleMetalRenderer();
    ~AppleMetalRenderer() override;

    AppleMetalRenderer(const AppleMetalRenderer&) = delete;
    AppleMetalRenderer& operator=(const AppleMetalRenderer&) = delete;

    bool initialize(
            SDL_Window* window,
            const std::shared_ptr<AppleVideoBackendContext>& decoderContext,
            QString* error = nullptr);

    QString name() const override;
    bool usesLowLatencyPresentation() const override;
    bool outputSize(int* width, int* height) const override;
    bool upload(const AppleDecodedTile& frame,
                QString* error = nullptr) override;
    bool uploadOverlay(const QImage& image,
                       QString* error = nullptr) override;
    void clearOverlay() override;
    RenderResult render(const AppleCanvas& canvas,
                        const QList<int>& tileHeights,
                        QString* error = nullptr) override;
    bool startDisplayLink(const std::function<void()>& callback) override;
    void setDisplayLinkPaused(bool paused) override;
    void stopDisplayLink() override;
    void clear() override;

private:
    class Implementation;
    std::unique_ptr<Implementation> m_Implementation;
};
