#pragma once

#include "appleconnectionstore.h"
#include "applecontrolfeatures.h"
#include "applemediaprotocol.h"
#include "applevideodecoder.h"
#include "applevideorenderer.h"
#include "applewindowplacement.h"
#include "streaming/streamsession.h"

#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QMutex>
#include <QPointer>
#include <QSize>
#include <QThreadPool>
#include <QVector>

#include <atomic>
#include <memory>
#include <optional>

class LocalStreamRuntime;
class QTimer;
class AppleHighPerformanceSessionTask;
class AppleSecondaryVideoStream;
class ApplePresentationThread;
class AppleKeyboardMapper;
#ifdef Q_OS_WIN
class AppleWindowsKeyboardHook;
#endif
struct AppleRemoteKeyEvent;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Cursor;
struct SDL_Window;

struct AppleOutboundControl
{
    enum class Kind
    {
        Input,
        Message,
        LocalClipboardText,
        SetObserving,
    };

    Kind kind = Kind::Message;
    AppleInputEncryptionRequest input;
    QByteArray message;
    QString text;
    quint64 queuedAtNanoseconds = 0;
    quint32 timestampDeltaMicroseconds = 0;
    bool observing = false;
    bool coalesciblePointerMotion = false;
};

class AppleScreenSharingSession final : public StreamSession,
                                        public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    using NativeEventResult = qintptr;
#else
    using NativeEventResult = long;
#endif

    AppleScreenSharingSession(AppleSavedConnection connection,
                              QObject* parent = nullptr);
    ~AppleScreenSharingSession() override;

    void complete(bool success, const QString& error);
    bool nativeEventFilter(const QByteArray& eventType,
                           void* message,
                           NativeEventResult* result) override;

protected:
    bool initializeSession(QQuickWindow* qtWindow) override;
    void startSession() override;
    void interruptSession() override;
    void setShouldExitSession(bool quitHostActivity) override;

private:
    friend class AppleHighPerformanceSessionTask;
    friend class AppleSecondaryVideoStream;
    friend class ApplePresentationThread;

    void queueDecodedFrames(QList<AppleDecodedTile> frames,
                            int displayIndex = 0);
    QList<AppleOutboundControl> takePendingControls();
    void mediaReady(const AppleCanvas& canvas,
                    AppleVideoDecoderBackend decoderBackend,
                    bool hardwareFallbackOccurred,
                    std::shared_ptr<AppleVideoBackendContext> decoderContext,
                    int displayIndex = 0);
    void applyCanvas(const AppleCanvas& canvas);
    void updatePerformanceStatistics(
            const QString& summary,
            const ApplePerformanceOverlayMetrics& metrics);
    void updateSecondaryPerformanceStatistics(const QString& summary);
    void updateAudioStatistics(const QString& summary);
    void applyControlEvents(const AppleControlEvents& events);
    SDL_Window* cursorWindow() const;
    void refreshRemoteCursor(SDL_Window* window, bool force);
    void useDefaultRemoteCursor();
    void applyRemoteClipboardText(const QString& text);
    void updateControlSummary();
    void localClipboardChanged();
    void refreshLocalClipboard(bool windowFocusGained);
    void requestPerformanceOverlayUpdate();
    void togglePerformanceOverlay();
    void toggleControlMode();
    void toggleAudioMute();
    void scheduleDynamicResolution(SDL_Window* window,
                                   int width,
                                   int height,
                                   bool waitsForViewportToSettle = true);
    void sendPendingDynamicResolution();
    void pollSdlEvents();
    void renderLatestFrames();
    void renderSecondaryFrames();
    void updatePerformanceOverlayTexture();
    void queuePointer(int windowX, int windowY, int clickCount = 0,
                      quint8 extraButtons = 0, int displayIndex = 0);
    void queueScroll(int windowX,
                     int windowY,
                     qint32 deltaX,
                     qint32 deltaY,
                     double preciseDeltaX,
                     double preciseDeltaY,
                     bool flipped,
                     int displayIndex);
    void queueKey(bool isDown,
                  int sdlKeycode,
                  int sdlScancode,
                  int sdlModifiers,
                  bool systemKeyCaptureRequested);
    void queueRemoteKey(const AppleRemoteKeyEvent& key);
    void releaseAllKeys();
    void updateKeyboardGrabState(SDL_Window* window);
    bool systemKeyCaptureRequestedForWindow(quint32 windowId) const;
    void queueControl(AppleOutboundControl control);
    std::optional<QRect> restoredWindowGeometry(AppleWindowRole role) const;
    void captureWindowGeometry(SDL_Window* window, AppleWindowRole role);
    void persistWindowGeometry(SDL_Window* window, AppleWindowRole role);
    std::optional<QPair<quint16, quint16>> remotePoint(int windowX,
                                                       int windowY,
                                                       int displayIndex = 0) const;
    void destroyPresentation();
    void prepareForReconnect(int attempt, const QString& reason);

    AppleSavedConnection m_Connection;
    std::atomic_bool m_Cancelled{false};
    QThreadPool m_WorkerPool;
    QPointer<QQuickWindow> m_QtWindow;
    std::unique_ptr<LocalStreamRuntime> m_Runtime;
    std::unique_ptr<AppleKeyboardMapper> m_KeyboardMapper;
    AppleWindowPlacementStore m_WindowPlacementStore;
    std::optional<QRect> m_PrimaryWindowGeometry;
    std::optional<QRect> m_SecondaryWindowGeometry;
    std::optional<QSize> m_PrimaryViewportSize;
    std::optional<QSize> m_SecondaryViewportSize;
    QList<QSize> m_InitialDisplaySizes;
    QTimer* m_EventTimer = nullptr;
    QTimer* m_DynamicResolutionTimer = nullptr;
    std::unique_ptr<ApplePresentationThread> m_PresentationThread;
    SDL_Renderer* m_Renderer = nullptr;
    std::unique_ptr<AppleVideoRenderer> m_VideoRenderer;
    SDL_Window* m_SecondaryWindow = nullptr;
    std::unique_ptr<AppleVideoRenderer> m_SecondaryVideoRenderer;
    QHash<int, SDL_Texture*> m_Textures;
    QHash<int, QPair<int, int>> m_TextureSizes;
    QHash<int, quint32> m_TextureFormats;
    SDL_Texture* m_PerformanceOverlayTexture = nullptr;
    QPair<int, int> m_PerformanceOverlaySize;
    QHash<int, AppleDecodedTile> m_LatestFrames;
    QHash<int, AppleDecodedTile> m_SecondaryLatestFrames;
    QHash<int, int> m_TileHeights;
    QHash<int, int> m_SecondaryTileHeights;
    quint64 m_PendingFrameBatches = 0;
    quint64 m_SecondaryPendingFrameBatches = 0;
    quint64 m_AwaitingPresentationBatches = 0;
    QHash<int, quint64> m_AwaitingDecodeSubmissions;
    mutable QMutex m_FrameMutex;
    QList<AppleOutboundControl> m_PendingControls;
    QMutex m_InputMutex;
    QMutex m_PerformanceMutex;
    AppleCanvas m_Canvas;
    AppleCanvas m_SecondaryCanvas;
    AppleDisplayLayout m_DisplayLayout;
    QList<quint32> m_MediaDisplayIds;
    std::optional<quint32> m_SelectedInputDisplayId;
    quint8 m_MouseButtons = 0;
    int m_LastMouseX = 0;
    int m_LastMouseY = 0;
    quint32 m_PreviousInputTimestamp = 0;
    quint32 m_ScrollEventCount = 0;
    quint64 m_PresentationWindowStartedAt = 0;
    quint64 m_PresentationCount = 0;
    quint64 m_PresentedTileUpdates = 0;
    quint64 m_DisplayedFrameBatches = 0;
    quint64 m_DroppedFrameBatches = 0;
    quint64 m_PresentationBusyCount = 0;
    quint64 m_LastDisplayedFrameAt = 0;
    quint64 m_LastRenderLoopAtNanoseconds = 0;
    double m_MaxRenderLoopGapMilliseconds = 0.0;
    double m_MaxOverlayUpdateMilliseconds = 0.0;
    QVector<double> m_DisplayFrameIntervals;
    QVector<double> m_SubmitToDisplayLatencies;
    QVector<double> m_RenderCallDurations;
    QString m_PerformanceMediaSummary;
    QString m_SecondaryPerformanceSummary;
    QString m_PerformancePresentationSummary;
    QString m_ControlSummary;
    QString m_AudioSummary;
    ApplePerformanceOverlayMetrics m_PerformanceMetrics;
    ApplePerformanceOverlayStyle m_PerformanceOverlayStyle =
            ApplePerformanceOverlayStyle::Moonlight;
    AppleCursorStore m_RemoteCursorStore;
    SDL_Cursor* m_ActiveRemoteCursor = nullptr;
    double m_ActiveRemoteCursorScale = 0.0;
    quint64 m_RemoteCursorUpdateCount = 0;
    AppleLocalClipboardTracker m_LocalClipboardTracker;
    QSize m_PendingDynamicResolution;
    QSize m_LastRequestedDynamicResolution;
    quint64 m_LastDynamicResolutionRequestAt = 0;
    int m_DisplayCount = 1;
    bool m_DynamicResolutionEnabled = true;
    bool m_LiveResizing = false;
    bool m_RememberWindowPlacement = false;
    double m_ScrollSpeedMultiplier = 1.0;
    std::atomic_bool m_PerformanceOverlayVisible{false};
    std::atomic_bool m_PerformanceOverlayUpdateNeeded{false};
    std::atomic_bool m_PresentationNeeded{true};
    std::atomic_bool m_SecondaryPresentationNeeded{true};
    std::atomic_bool m_Observing{false};
    std::atomic_bool m_ControlReady{false};
    std::atomic_bool m_NativePrecisionScrollSupported{false};
    std::atomic_bool m_AudioMuted{false};
    std::atomic_bool m_EverMediaReady{false};
    std::atomic_bool m_ReconnectRequested{false};
    std::atomic_bool m_SystemSuspended{false};
    std::atomic<quint64> m_PointerMotionsCoalesced{0};
    std::atomic<quint64> m_MaxPendingControlDepth{0};
    bool m_MediaReady = false;
    bool m_SecondaryMediaReady = false;
    bool m_NativeEventFilterInstalled = false;
    int m_CaptureSystemKeysMode = 0;
    bool m_PrimaryKeyboardGrabActive = false;
    bool m_SecondaryKeyboardGrabActive = false;
#ifdef Q_OS_WIN
    std::unique_ptr<AppleWindowsKeyboardHook> m_WindowsKeyboardHook;
#endif
};
