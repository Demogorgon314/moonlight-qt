#pragma once

#include "appleconnectionstore.h"
#include "applemediaprotocol.h"
#include "applevideodecoder.h"
#include "streaming/streamsession.h"

#include <QHash>
#include <QMutex>
#include <QPointer>
#include <QVector>

#include <atomic>
#include <memory>
#include <optional>

class LocalStreamRuntime;
class QTimer;
class AppleHighPerformanceSessionTask;
struct SDL_Renderer;
struct SDL_Texture;

class AppleScreenSharingSession final : public StreamSession
{
    Q_OBJECT

public:
    AppleScreenSharingSession(AppleSavedConnection connection,
                              QObject* parent = nullptr);
    ~AppleScreenSharingSession() override;

    void complete(bool success, const QString& error);

protected:
    bool initializeSession(QQuickWindow* qtWindow) override;
    void startSession() override;
    void interruptSession() override;
    void setShouldExitSession(bool quitHostActivity) override;

private:
    friend class AppleHighPerformanceSessionTask;

    void queueDecodedFrames(QList<AppleDecodedTile> frames);
    QList<AppleInputEncryptionRequest> takePendingInputs();
    void mediaReady(const AppleCanvas& canvas,
                    bool hardwareDecoderActive,
                    bool hardwareFallbackOccurred);
    void updatePerformanceStatistics(const QString& summary);
    void pollSdlEvents();
    void renderLatestFrames();
    void updatePerformanceOverlayTexture();
    void queuePointer(int windowX, int windowY, int clickCount = 0,
                      quint8 extraButtons = 0);
    void queueKey(bool isDown, int sdlKeycode, int sdlScancode);
    std::optional<QPair<quint16, quint16>> remotePoint(int windowX,
                                                       int windowY) const;
    void destroyPresentation();

    AppleSavedConnection m_Connection;
    std::atomic_bool m_Cancelled{false};
    QPointer<QQuickWindow> m_QtWindow;
    std::unique_ptr<LocalStreamRuntime> m_Runtime;
    QTimer* m_EventTimer = nullptr;
    SDL_Renderer* m_Renderer = nullptr;
    QHash<int, SDL_Texture*> m_Textures;
    QHash<int, QPair<int, int>> m_TextureSizes;
    QHash<int, quint32> m_TextureFormats;
    SDL_Texture* m_PerformanceOverlayTexture = nullptr;
    QPair<int, int> m_PerformanceOverlaySize;
    QHash<int, AppleDecodedTile> m_LatestFrames;
    QHash<int, int> m_TileHeights;
    quint64 m_PendingFrameBatches = 0;
    mutable QMutex m_FrameMutex;
    QList<AppleInputEncryptionRequest> m_PendingInputs;
    QMutex m_InputMutex;
    AppleCanvas m_Canvas;
    quint8 m_MouseButtons = 0;
    int m_LastMouseX = 0;
    int m_LastMouseY = 0;
    quint32 m_PreviousInputTimestamp = 0;
    quint64 m_PresentationWindowStartedAt = 0;
    quint64 m_PresentationCount = 0;
    quint64 m_PresentedTileUpdates = 0;
    quint64 m_DisplayedFrameBatches = 0;
    quint64 m_DroppedFrameBatches = 0;
    quint64 m_LastDisplayedFrameAt = 0;
    QVector<double> m_DisplayFrameIntervals;
    QString m_PerformanceMediaSummary;
    QString m_PerformancePresentationSummary;
    bool m_MediaReady = false;
};
