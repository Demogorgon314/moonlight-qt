#include "applescreensharingsession.h"
#include "applescreensharingsession_p.h"

#include "applefiledrag.h"
#include "applefiletransferprogress.h"
#ifdef Q_OS_WIN
#include "applefiledrag_win.h"
#include "applewindowskeyboardhook_p.h"
#endif

#include "appleaudiostream.h"
#include "applecredentialstore.h"
#include "applekeyboardmapper.h"
#ifdef Q_OS_DARWIN
#include "applefiledrag_mac.h"
#include "applemacinputbridge.h"
#endif
#include "applevideorenderer.h"
#include "settings/streamingpreferences.h"
#include "streaming/localstreamruntime.h"

#include "SDL.h"

#include <QCoreApplication>
#include <QClipboard>
#include <QDebug>
#include <QGuiApplication>
#include <QMutexLocker>
#include <QPointer>
#include <QQuickWindow>
#include <QSettings>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_DARWIN
#include <Carbon/Carbon.h>
#endif

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <memory>
#include <utility>

using AppleScreenSharingSessionPrivate::appleVideoDecoderBackendName;
using AppleScreenSharingSessionPrivate::cursorDpiScale;
using AppleScreenSharingSessionPrivate::MaximumReconnectAttempts;
using AppleScreenSharingSessionPrivate::RealtimeMediaPollTimeoutMs;
#ifdef Q_OS_WIN
using AppleScreenSharingSessionPrivate::nativeHandleForWindow;
using AppleScreenSharingSessionPrivate::nativeHandleMatchesWindow;
#endif

namespace {

Uint32 appleVideoWindowFlags()
{
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                   SDL_WINDOW_HIDDEN;
#ifdef Q_OS_DARWIN
    flags |= SDL_WINDOW_METAL;
#endif
    return flags;
}

QList<QRect> sdlUsableDisplayBounds()
{
    QList<QRect> displays;
    const int displayCount = SDL_GetNumVideoDisplays();
    for (int index = 0; index < displayCount; ++index) {
        SDL_Rect bounds;
        if (SDL_GetDisplayUsableBounds(index, &bounds) == 0 &&
                bounds.w > 0 && bounds.h > 0) {
            displays.append(QRect(bounds.x, bounds.y, bounds.w, bounds.h));
        }
    }
    return displays;
}

const char* appleWindowRoleName(AppleWindowRole role)
{
    return role == AppleWindowRole::Primary ? "primary" : "secondary";
}

#ifdef Q_OS_DARWIN
std::optional<quint32> nativeSpecialKeySymbol(quint16 keyCode)
{
    switch (keyCode) {
    case 53: return 0xff1b;
    case 48: return 0xff09;
    case 51: return 0xff08;
    case 36: return 0xff0d;
    case 117: return 0xffff;
    case 114: return 0xff63;
    case 115: return 0xff50;
    case 119: return 0xff57;
    case 116: return 0xff55;
    case 121: return 0xff56;
    case 123: return 0xff51;
    case 126: return 0xff52;
    case 124: return 0xff53;
    case 125: return 0xff54;
    case 122: return 0xffbe;
    case 120: return 0xffbf;
    case 99: return 0xffc0;
    case 118: return 0xffc1;
    case 96: return 0xffc2;
    case 97: return 0xffc3;
    case 98: return 0xffc4;
    case 100: return 0xffc5;
    case 101: return 0xffc6;
    case 109: return 0xffc7;
    case 103: return 0xffc8;
    case 111: return 0xffc9;
    default: return std::nullopt;
    }
}

std::optional<quint32> nativeModifierKeySymbol(quint16 keyCode)
{
    switch (keyCode) {
    case 56: return 0xffe1;
    case 60: return 0xffe2;
    case 59: return 0xffe3;
    case 62: return 0xffe4;
    case 58: return 0xffe9;
    case 61: return 0xffea;
    case 55: return 0xffeb;
    case 54: return 0xffec;
    case 57: return 0xffe5;
    default: return std::nullopt;
    }
}
#endif

#ifdef Q_OS_DARWIN
quint8 appleButtonForNative(unsigned char buttonNumber)
{
    switch (buttonNumber) {
    case 0: return 1 << 0;
    case 1: return 1 << 1;
    case 2: return 1 << 2;
    default: return 0;
    }
}
#endif

} // namespace

class ApplePresentationThread final : public QThread
{
public:
    explicit ApplePresentationThread(AppleScreenSharingSession* session)
        : m_Session(session)
    {
        setObjectName(QStringLiteral("Apple video presentation"));
    }

protected:
    void run() override
    {
        while (!isInterruptionRequested()) {
            const bool signalled = m_Session->m_PresentationWake.tryAcquire(
                    1, 100);
            if (!signalled && m_Session->m_DisplayLinkActive.load()) {
                continue;
            }
            if (isInterruptionRequested()) {
                break;
            }
            m_Session->renderLatestFrames();
            m_Session->renderSecondaryFrames();
            if (!m_Session->m_DisplayLinkActive.load() &&
                    (m_Session->m_PresentationNeeded.load() ||
                     m_Session->m_SecondaryPresentationNeeded.load())) {
                // Non-Metal renderers retry a temporarily busy swap chain
                // without restoring the old unconditional 1 ms poll.
                QThread::msleep(1);
                m_Session->wakePresentation();
            }
        }
    }

private:
    AppleScreenSharingSession* const m_Session;
};

AppleScreenSharingSession::AppleScreenSharingSession(
        AppleSavedConnection connection,
        QObject* parent)
    : StreamSession(parent),
      m_Connection(std::move(connection)),
      m_Runtime(std::make_unique<LocalStreamRuntime>()),
      m_FileTransferService(std::make_shared<AppleFileTransferService>()),
      m_RemoteFileDragGate(std::make_unique<AppleRemoteFileDragGate>()),
      m_RemoteFileDragInputState(
              std::make_unique<AppleRemoteFileDragInputState>())
{
    m_ClipboardSharingEnabled.store(m_Connection.sharedClipboardEnabled);
    m_WorkerPool.setMaxThreadCount(1);
    m_WorkerPool.setExpiryTimeout(-1);
}

AppleScreenSharingSession::~AppleScreenSharingSession()
{
    m_Cancelled.store(true);
    m_FileTransferService->close();
    if (m_Runtime) {
        m_Runtime->requestStop();
    }
    m_WorkerPool.waitForDone();
    if (m_NativeEventFilterInstalled && QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->removeNativeEventFilter(this);
        m_NativeEventFilterInstalled = false;
    }
    destroyPresentation();
}

void AppleScreenSharingSession::wakePresentation(bool displayLinkTick)
{
    if (!displayLinkTick && m_DisplayLinkActive.load()) {
        return;
    }
    if (m_PresentationWake.available() == 0) {
        m_PresentationWake.release();
    }
}

void AppleScreenSharingSession::setWindowMiniaturized(
        int displayIndex,
        bool miniaturized)
{
    std::atomic_bool& state = displayIndex == 1
            ? m_SecondaryWindowMiniaturized
            : m_PrimaryWindowMiniaturized;
    state.store(miniaturized);
    AppleVideoRenderer* renderer = displayIndex == 1
            ? m_SecondaryVideoRenderer.get() : m_VideoRenderer.get();
    if (renderer != nullptr) {
        renderer->setDisplayLinkPaused(miniaturized);
    }

    const auto interval = m_FrameUpdatePauseState.setMiniaturized(
            miniaturized, m_DisplayCount);
    if (interval.has_value()) {
        AppleOutboundControl control;
        control.message = AppleMediaWire::autoFramebufferUpdate(*interval);
        queueControl(std::move(control));
        qInfo() << "Apple remote frame updates"
                << (*interval == 0 ? "resumed" : "paused")
                << "for minimized window";
    }
    if (!miniaturized) {
        if (displayIndex == 1) {
            m_SecondaryPresentationNeeded.store(true);
        }
        else {
            m_PresentationNeeded.store(true);
        }
        wakePresentation();
    }
}

bool AppleScreenSharingSession::initializeSession(QQuickWindow* qtWindow)
{
    if (!m_Connection.isValid() || !m_Connection.isTrusted() ||
            !AppleCredentialStore::isReferenceForConnection(
                    m_Connection.credentialReference, m_Connection.id) ||
            qtWindow == nullptr) {
        return false;
    }
    m_QtWindow = qtWindow;
    QSettings settings;
    const StreamingPreferences* preferences = StreamingPreferences::get();
    m_RememberWindowPlacement = preferences->rememberWindowPosition;
    quint16 keyboardType = 0;
#ifdef Q_OS_DARWIN
    // Apple's high-performance input record contains the physical keyboard
    // type as well as the virtual key code. Match native AppKit key events.
    keyboardType = static_cast<quint16>(LMGetKbdType());
#endif
    m_KeyboardMapper = std::make_unique<AppleKeyboardMapper>(
            preferences->swapWinAltKeys, keyboardType);
    m_CaptureSystemKeysMode = static_cast<int>(
            preferences->captureSysKeysMode);
    // Match Moonlight's established streaming input behavior. These hints keep
    // Alt+Tab and Alt+F4 in SDL's key path while the Apple window owns the
    // explicit keyboard grab.
    SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");
#ifdef Q_OS_WIN
    SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, "1");
#endif
    m_ScrollSpeedMultiplier = qBound(
            25, preferences->appleScrollSpeedPercent, 150) / 50.0;
    const ApplePerformanceOverlayPolicy overlayPolicy =
            ApplePerformanceOverlayPolicy::fromSettings(
                    preferences->showPerformanceOverlay,
                    static_cast<int>(preferences->performanceStatsStyle));
    m_PerformanceOverlayVisible.store(overlayPolicy.visible);
    m_PerformanceOverlayStyle = overlayPolicy.style;
    m_DisplayCount = qBound(1, m_Connection.virtualDisplayCount, 2);
    m_DynamicResolutionEnabled = m_DisplayCount == 1 &&
            settings.value(QStringLiteral(
                    "appleScreenSharing/dynamicResolution"), true).toBool();
    const auto loadInitialViewport = [this, qtWindow](int displayIndex) {
        std::optional<QSize> viewport = m_WindowPlacementStore.loadViewport(
                m_Connection.id, displayIndex);
        if (viewport.has_value() || !m_RememberWindowPlacement) {
            return viewport;
        }

        // Older builds stored SDL's native-pixel window geometry only. This
        // one-time conversion is replaced with the exact per-window DPI value
        // as soon as the first updated session viewport is persisted.
        const AppleWindowRole role = displayIndex == 0
                ? AppleWindowRole::Primary : AppleWindowRole::Secondary;
        const std::optional<QRect> legacyGeometry =
                m_WindowPlacementStore.load(role);
        const double dpiScale = qtWindow->devicePixelRatio();
        if (!legacyGeometry.has_value() || !std::isfinite(dpiScale) ||
                dpiScale <= 0) {
            return viewport;
        }
        viewport = QSize(
                qMax(1, qRound(legacyGeometry->width() / dpiScale)),
                qMax(1, qRound(legacyGeometry->height() / dpiScale)));
        m_WindowPlacementStore.saveViewport(
                m_Connection.id, displayIndex, *viewport);
        return viewport;
    };
    const std::optional<QSize> primaryViewport = loadInitialViewport(0);
    const QSize fallbackDisplaySize =
            AppleDynamicResolution::initialDisplaySize(primaryViewport);
    m_InitialDisplaySizes.clear();
    for (int displayIndex = 0; displayIndex < m_DisplayCount; ++displayIndex) {
        const std::optional<QSize> storedViewport =
                loadInitialViewport(displayIndex);
        m_InitialDisplaySizes.append(storedViewport.has_value()
                ? AppleDynamicResolution::initialDisplaySize(storedViewport)
                : fallbackDisplaySize);
    }
    if (m_DisplayCount == 1 && !m_DynamicResolutionEnabled) {
        m_InitialDisplaySizes[0] = QSize(1920, 1080);
    }
    const QSize initialDisplaySize = m_InitialDisplaySizes.first();
    qInfo().nospace()
            << "Apple Screen Sharing initial display size="
            << initialDisplaySize.width() << "x" << initialDisplaySize.height()
            << (primaryViewport.has_value() ? " (stored viewport)"
                                            : " (Swift default)");
    LocalStreamRuntimeConfig runtimeConfig;
    runtimeConfig.streamWidth = initialDisplaySize.width();
    runtimeConfig.streamHeight = initialDisplaySize.height();
    if (!m_Runtime->initialize(qtWindow, runtimeConfig)) {
        return false;
    }
#ifdef Q_OS_WIN
    m_WindowsKeyboardHook = std::make_unique<AppleWindowsKeyboardHook>();
#endif
    if (QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->installNativeEventFilter(this);
        m_NativeEventFilterInstalled = true;
    }
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        connect(clipboard, &QClipboard::dataChanged,
                this, &AppleScreenSharingSession::localClipboardChanged,
                Qt::UniqueConnection);
    }
    updateControlSummary();
    QString audioProbeError;
    if (!AppleAudioStream::decoderIsSupported(&audioProbeError)) {
        addLaunchWarning(tr("AAC-ELD audio is unavailable: %1")
                         .arg(audioProbeError));
    }
    return true;
}

std::optional<QRect> AppleScreenSharingSession::restoredWindowGeometry(
        AppleWindowRole role) const
{
    if (!m_RememberWindowPlacement) {
        return std::nullopt;
    }

    const auto saved = m_WindowPlacementStore.load(role);
    if (!saved.has_value()) {
        return std::nullopt;
    }
    const QRect restored = AppleWindowPlacement::constrainToVisibleDisplays(
            *saved, sdlUsableDisplayBounds());
    if (!restored.isValid()) {
        return std::nullopt;
    }
    qInfo().nospace()
            << "Apple Screen Sharing restored " << appleWindowRoleName(role)
            << " window=[" << restored.x() << "," << restored.y() << " "
            << restored.width() << "x" << restored.height() << "]";
    return restored;
}

void AppleScreenSharingSession::captureWindowGeometry(
        SDL_Window* window,
        AppleWindowRole role)
{
    if (window == nullptr) {
        return;
    }

    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }
    const double dpiScale = cursorDpiScale(window);
    const QSize viewportSize(
            qMax(1, qRound(width / dpiScale)),
            qMax(1, qRound(height / dpiScale)));
    if (role == AppleWindowRole::Primary) {
        m_PrimaryViewportSize = viewportSize;
    }
    else {
        m_SecondaryViewportSize = viewportSize;
    }

    if (!m_RememberWindowPlacement) {
        return;
    }
    const quint32 flags = SDL_GetWindowFlags(window);
    if ((flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED |
                  SDL_WINDOW_FULLSCREEN)) != 0) {
        return;
    }

    int x = 0;
    int y = 0;
    SDL_GetWindowPosition(window, &x, &y);
    const QRect geometry(x, y, width, height);
    if (!geometry.isValid()) {
        return;
    }
    if (role == AppleWindowRole::Primary) {
        m_PrimaryWindowGeometry = geometry;
    }
    else {
        m_SecondaryWindowGeometry = geometry;
    }
}

void AppleScreenSharingSession::persistWindowGeometry(
        SDL_Window* window,
        AppleWindowRole role)
{
    captureWindowGeometry(window, role);
    const int displayIndex = role == AppleWindowRole::Primary ? 0 : 1;
    const auto& viewportSize = role == AppleWindowRole::Primary
            ? m_PrimaryViewportSize : m_SecondaryViewportSize;
    if (viewportSize.has_value() &&
            !m_WindowPlacementStore.saveViewport(
                    m_Connection.id, displayIndex, *viewportSize)) {
        qWarning() << "Could not save Apple Screen Sharing"
                   << appleWindowRoleName(role) << "viewport size";
    }
    if (!m_RememberWindowPlacement) {
        return;
    }
    const auto& geometry = role == AppleWindowRole::Primary
            ? m_PrimaryWindowGeometry : m_SecondaryWindowGeometry;
    if (!geometry.has_value()) {
        return;
    }
    if (!m_WindowPlacementStore.save(role, *geometry)) {
        qWarning() << "Could not save Apple Screen Sharing"
                   << appleWindowRoleName(role) << "window placement";
        return;
    }
    qInfo().nospace()
            << "Apple Screen Sharing saved " << appleWindowRoleName(role)
            << " window=[" << geometry->x() << "," << geometry->y() << " "
            << geometry->width() << "x" << geometry->height() << "]";
}

void AppleScreenSharingSession::startSession()
{
    emit stageStarting(tr("Apple authentication and media negotiation"));
    startHighPerformanceWorker();
}

void AppleScreenSharingSession::interruptSession()
{
    m_Cancelled.store(true);
    m_FileTransferService->close();
    if (m_Runtime) {
        m_Runtime->requestStop();
    }
}

void AppleScreenSharingSession::setShouldExitSession(bool)
{
    interrupt();
}

bool AppleScreenSharingSession::nativeEventFilter(
        const QByteArray& eventType,
        void* message,
        NativeEventResult*)
{
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG" && message != nullptr) {
        const MSG* nativeMessage = static_cast<const MSG*>(message);
        SDL_Window* streamWindow = m_Runtime != nullptr
                ? m_Runtime->streamWindow() : nullptr;
        if (nativeHandleMatchesWindow(streamWindow, nativeMessage->hwnd)) {
            if (nativeMessage->message == WM_ENTERSIZEMOVE) {
                m_LiveResizing = true;
                if (m_DynamicResolutionTimer != nullptr) {
                    m_DynamicResolutionTimer->stop();
                }
            }
            else if (nativeMessage->message == WM_EXITSIZEMOVE) {
                m_LiveResizing = false;
                int width = 0;
                int height = 0;
                SDL_GetWindowSize(streamWindow, &width, &height);
                persistWindowGeometry(streamWindow, AppleWindowRole::Primary);
                scheduleDynamicResolution(
                        streamWindow, width, height, false);
            }
        }
        if (nativeMessage->message == WM_POWERBROADCAST) {
            switch (nativeMessage->wParam) {
            case PBT_APMSUSPEND:
                m_SystemSuspended.store(true);
                m_ReconnectRequested.store(true);
                qInfo() << "Apple Screen Sharing detected Windows suspend";
                break;
            case PBT_APMRESUMEAUTOMATIC:
            case PBT_APMRESUMECRITICAL:
            case PBT_APMRESUMESUSPEND:
                m_SystemSuspended.store(false);
                m_ReconnectRequested.store(true);
                qInfo() << "Apple Screen Sharing detected Windows resume; reconnect requested";
                break;
            default:
                break;
            }
        }
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
#endif
    return false;
}

void AppleScreenSharingSession::mediaReady(
        const AppleCanvas& canvas,
        AppleVideoDecoderBackend decoderBackend,
        bool hardwareFallbackOccurred,
        std::shared_ptr<AppleVideoBackendContext> decoderContext,
        int displayIndex)
{
    if (displayIndex == 1) {
        if (m_SecondaryMediaReady || m_Cancelled.load() ||
                !canvas.isUsable()) {
            return;
        }
        {
            QMutexLocker locker(&m_FrameMutex);
            m_SecondaryCanvas = canvas;
        }
        const auto restored = restoredWindowGeometry(AppleWindowRole::Secondary);
        const int width = restored.has_value()
                ? restored->width() : qBound(800, canvas.width, 1600);
        const int height = restored.has_value()
                ? restored->height() : qBound(450, canvas.height, 1000);
        const int x = restored.has_value()
                ? restored->x() : SDL_WINDOWPOS_CENTERED;
        const int y = restored.has_value()
                ? restored->y() : SDL_WINDOWPOS_CENTERED;
        const QByteArray title = tr("%1 — Apple Screen Sharing — Display 2")
                .arg(m_Connection.displayName).toUtf8();
        m_SecondaryWindow = SDL_CreateWindow(
                title.constData(),
                x,
                y,
                width,
                height,
                appleVideoWindowFlags());
        if (m_SecondaryWindow == nullptr) {
            m_Cancelled.store(true);
            emit displayLaunchError(tr(
                    "Couldn’t create the second Apple Screen Sharing display window."));
            return;
        }
        captureWindowGeometry(m_SecondaryWindow, AppleWindowRole::Secondary);
        QString rendererError;
        m_SecondaryVideoRenderer = createAppleVideoRenderer(
                m_SecondaryWindow, decoderContext, &rendererError);
        if (m_SecondaryVideoRenderer == nullptr) {
            SDL_DestroyWindow(m_SecondaryWindow);
            m_SecondaryWindow = nullptr;
            m_Cancelled.store(true);
            emit displayLaunchError(tr(
                    "Couldn’t initialize the second 4:4:4 display: %1")
                                    .arg(rendererError));
            return;
        }
#ifdef Q_OS_WIN
        installWindowsFileDropTarget(m_SecondaryWindow, 1);
#endif
        m_SecondaryMediaReady = true;
        m_EverMediaReady.store(true);
        if (m_PresentationThread != nullptr &&
                m_SecondaryVideoRenderer->startDisplayLink([this]() {
                    wakePresentation(true);
                })) {
            m_DisplayLinkActive.store(true);
        }
        qInfo().nospace()
                << "Apple High Performance display 2 renderer="
                << m_SecondaryVideoRenderer->name() << ", "
                   "canvas=" << canvas.width << "x" << canvas.height
                << ", decoder="
                << appleVideoDecoderBackendName(decoderBackend)
                << (hardwareFallbackOccurred ? " (fallback)" : "");
        SDL_ShowWindow(m_SecondaryWindow);
        SDL_RaiseWindow(m_SecondaryWindow);
        updateKeyboardGrabState(m_SecondaryWindow);
        if ((SDL_GetWindowFlags(m_SecondaryWindow) &
             SDL_WINDOW_INPUT_FOCUS) != 0) {
            setClipboardWindowFocused(
                    SDL_GetWindowID(m_SecondaryWindow), true);
        }
        m_SecondaryPresentationNeeded.store(true);
        wakePresentation();
        return;
    }
    if (m_MediaReady || m_Cancelled.load() || !canvas.isUsable()) {
        return;
    }
    applyCanvas(canvas);
    const auto restored = restoredWindowGeometry(AppleWindowRole::Primary);
    const int width = restored.has_value()
            ? restored->width() : qBound(800, canvas.width, 1600);
    const int height = restored.has_value()
            ? restored->height() : qBound(450, canvas.height, 1000);
    const int x = restored.has_value()
            ? restored->x() : SDL_WINDOWPOS_CENTERED;
    const int y = restored.has_value()
            ? restored->y() : SDL_WINDOWPOS_CENTERED;
    SDL_Window* window = m_Runtime->createStreamWindow(
            tr("%1 — Apple Screen Sharing").arg(m_Connection.displayName),
            x,
            y,
            width,
            height,
            appleVideoWindowFlags());
    if (window == nullptr) {
        m_Cancelled.store(true);
        emit displayLaunchError(tr("Couldn’t create the Apple Screen Sharing video window."));
        return;
    }
#ifdef Q_OS_WIN
    void* const nativeWindow = nativeHandleForWindow(window);
    installWindowsFileDropTarget(window, 0);
    m_WindowsRemoteFileDragSource =
            std::make_unique<AppleWindowsRemoteFileDragSource>(nativeWindow);
    if (!m_WindowsRemoteFileDragSource->isValid()) {
        qWarning() << "Apple native Windows promised-file drag source unavailable";
        m_WindowsRemoteFileDragSource.reset();
    }
    else {
        if (m_SecondaryWindow != nullptr) {
            m_WindowsRemoteFileDragSource->addStreamWindow(
                    nativeHandleForWindow(m_SecondaryWindow));
        }
        qInfo() << "Apple native Windows promised-file drag source enabled";
    }
#endif
    captureWindowGeometry(window, AppleWindowRole::Primary);
    QString rendererError;
    m_VideoRenderer = createAppleVideoRenderer(
            window, decoderContext, &rendererError);
    if (m_VideoRenderer == nullptr) {
        m_Cancelled.store(true);
        emit displayLaunchError(tr(
                "Couldn’t initialize lossless 4:4:4 presentation: %1")
                                        .arg(rendererError));
        return;
    }
#ifdef Q_OS_DARWIN
    // Attach only after renderer initialization so the bridge owns the final
    // SDL content view. This mirrors the native Swift canvas: one view owns
    // keyboard, pointer, and scroll delivery through shortcut transitions.
    m_MacRemoteFileDragSource =
            std::make_unique<AppleMacRemoteFileDragSource>(window);
    if (!m_MacRemoteFileDragSource->isValid()) {
        qWarning() << "Apple native macOS promised-file drag source unavailable";
        m_MacRemoteFileDragSource.reset();
    }
    else {
        qInfo() << "Apple native macOS promised-file drag source enabled";
    }
    ensureLocalFileDragLifecycle();
    m_AppleMacInputBridge = std::make_unique<AppleMacInputBridge>(
            window,
            [this](const AppleMacKeyEvent& event) {
                if (event.type != AppleMacKeyEvent::Type::Modifier &&
                        event.controlDown && event.optionDown &&
                        event.shiftDown && !event.commandDown) {
                    char32_t character =
                            event.charactersIgnoringModifiers.empty()
                            ? U'\0'
                            : event.charactersIgnoringModifiers.front();
                    if (character >= U'A' && character <= U'Z') {
                        character += U'a' - U'A';
                    }
                    // Preserve the shortcuts on input sources where Option
                    // turns the key into a dead key and AppKit reports no
                    // charactersIgnoringModifiers value.
                    if (character == U'\0') {
                        if (event.keyCode == 8) character = U'c';
                        if (event.keyCode == 5) character = U'g';
                        if (event.keyCode == 9) character = U'v';
                    }
                    if (character == U'c' || character == U'g' ||
                            character == U'v') {
                        if (event.type == AppleMacKeyEvent::Type::Down &&
                                !event.isRepeat) {
                            if (character == U'c') toggleClipboardSharing();
                            if (character == U'g') requestRemoteClipboard();
                            if (character == U'v') sendLocalClipboard();
                        }
                        return;
                    }
                }
                if (m_Observing.load()) {
                    return;
                }

                const quint16 keyboardType = event.keyboardType != 0
                        ? event.keyboardType
                        : static_cast<quint16>(LMGetKbdType());
                const auto sendKey = [this, keyboardType](
                        bool isDown, quint32 symbol, quint16 keyCode) {
                    queueRemoteKey(AppleRemoteKeyEvent{
                            isDown, symbol, keyboardType, keyCode,
                    });
                };

                if (event.type == AppleMacKeyEvent::Type::Modifier) {
                    const auto symbol = nativeModifierKeySymbol(event.keyCode);
                    if (symbol.has_value()) {
                        sendKey(event.modifierDown, *symbol, event.keyCode);
                    }
                }
                else if (const auto symbol =
                         nativeSpecialKeySymbol(event.keyCode);
                         symbol.has_value()) {
                    sendKey(event.type == AppleMacKeyEvent::Type::Down,
                            *symbol,
                            event.keyCode);
                }
                else {
                    const bool hasCommandModifier = event.commandDown ||
                            event.controlDown || event.optionDown;
                    const std::u32string& characters = hasCommandModifier
                            ? event.charactersIgnoringModifiers
                            : event.characters;
                    if (event.type == AppleMacKeyEvent::Type::Down) {
                        if (event.controlDown &&
                                !event.controlEventObserved) {
                            sendKey(true, 0xffe3, 59);
                        }
                        if (event.optionDown &&
                                !event.optionEventObserved) {
                            sendKey(true, 0xffe9, 58);
                        }
                        if (event.commandDown &&
                                !event.commandEventObserved) {
                            sendKey(true, 0xffeb, 55);
                        }
                        for (char32_t character : characters) {
                            sendKey(true, character, event.keyCode);
                            if (!hasCommandModifier) {
                                sendKey(false, character, event.keyCode);
                            }
                        }
                    }
                    else if (hasCommandModifier) {
                        for (char32_t character : characters) {
                            sendKey(false, character, event.keyCode);
                        }
                        if (event.commandDown &&
                                !event.commandEventObserved) {
                            sendKey(false, 0xffeb, 55);
                        }
                        if (event.optionDown &&
                                !event.optionEventObserved) {
                            sendKey(false, 0xffe9, 58);
                        }
                        if (event.controlDown &&
                                !event.controlEventObserved) {
                            sendKey(false, 0xffe3, 59);
                        }
                    }
                }
            },
            [this, window](const AppleMacPointerEvent& event) {
                switch (event.type) {
                case AppleMacPointerEvent::Type::Motion:
                    handleMouseMotion(event.x, event.y, 0);
                    break;
                case AppleMacPointerEvent::Type::ButtonDown:
                case AppleMacPointerEvent::Type::ButtonUp:
                    handleMouseButton(
                            event.type ==
                                    AppleMacPointerEvent::Type::ButtonDown,
                            appleButtonForNative(event.buttonNumber),
                            event.clickCount,
                            event.x,
                            event.y,
                            0,
                            window);
                    break;
                case AppleMacPointerEvent::Type::Scroll:
                    m_LastMouseX = event.x;
                    m_LastMouseY = event.y;
                    if (m_NativePrecisionScrollSupported.load() &&
                            event.hasNativeScrollEvent) {
                        queueScroll(
                                event.x,
                                event.y,
                                event.deltaX,
                                event.deltaY,
                                event.preciseDeltaX,
                                event.preciseDeltaY,
                                event.scrollingDirectionInverted,
                                0,
                                &event.nativeScrollEvent);
                    }
                    else {
                        const int deltaX = event.hasNativeScrollEvent
                                ? event.nativeScrollEvent.deltaX
                                : event.deltaX;
                        const int deltaY = event.hasNativeScrollEvent
                                ? event.nativeScrollEvent.deltaY
                                : event.deltaY;
                        quint8 wheel = 0;
                        if (deltaY > 0) wheel |= 1 << 3;
                        if (deltaY < 0) wheel |= 1 << 4;
                        if (deltaX > 0) wheel |= 1 << 6;
                        if (deltaX < 0) wheel |= 1 << 5;
                        if (wheel != 0) {
                            queuePointer(event.x, event.y, 0, wheel, 0);
                            queuePointer(event.x, event.y, 0, 0, 0);
                        }
                    }
                    break;
                }
            },
            [this](const void* nativeEvent, bool pointerInsideView) {
                return activateRemoteFileDragIfEligible(
                        pointerInsideView, nativeEvent);
            },
            [this]() {
                interrupt();
            },
            m_LocalFileDragLifecycle,
            0,
            [this](AppleMacClipboardCommand command) {
                switch (command) {
                case AppleMacClipboardCommand::ToggleSharing:
                    toggleClipboardSharing();
                    break;
                case AppleMacClipboardCommand::Receive:
                    requestRemoteClipboard();
                    break;
                case AppleMacClipboardCommand::Send:
                    sendLocalClipboard();
                    break;
                }
            });
    if (!m_AppleMacInputBridge->isValid()) {
        addLaunchWarning(tr("Native macOS input could not be attached."));
        m_AppleMacInputBridge.reset();
    }
    else {
        qInfo() << "Apple macOS window-owned input adapter enabled";
    }
#endif
    updateKeyboardGrabState(window);
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0) {
        setClipboardWindowFocused(SDL_GetWindowID(window), true);
    }
    qInfo().nospace()
            << "Apple High Performance renderer="
            << m_VideoRenderer->name()
            << ", vsync=true, maximum-frame-latency=1, decoder="
            << appleVideoDecoderBackendName(decoderBackend)
            << ", low-latency-presentation="
            << (m_VideoRenderer->usesLowLatencyPresentation()
                        ? "true" : "false");
    if (hardwareFallbackOccurred) {
        addLaunchWarning(tr("Hardware HEVC decoding was unavailable or failed; the session continued with software decoding."));
    }
    else if (decoderBackend != AppleVideoDecoderBackend::Software) {
        addLaunchWarning(tr("Hardware HEVC decoding is active (%1).")
                         .arg(appleVideoDecoderBackendName(decoderBackend)));
    }
    else {
        addLaunchWarning(tr("HEVC software decoding is active."));
    }

    updatePerformanceOverlayTexture();
    m_PresentationThread = std::make_unique<ApplePresentationThread>(this);
    m_PresentationThread->start(QThread::HighPriority);
    const bool usesDisplayLink = m_VideoRenderer->startDisplayLink([this]() {
        wakePresentation(true);
    });
    m_DisplayLinkActive.store(usesDisplayLink);
    wakePresentation();
    qInfo().nospace()
            << "Apple High Performance presentation scheduler="
            << (usesDisplayLink ? "macOS-display-link"
                                : "event-driven-high-priority")
            << ", media-poll-max="
            << RealtimeMediaPollTimeoutMs << "ms";

    m_EventTimer = new QTimer(this);
    m_EventTimer->setTimerType(Qt::PreciseTimer);
    // SDL input and window events remain on the GUI thread. Video presentation
    // is isolated above so a native modal window loop cannot freeze playback.
    m_EventTimer->setInterval(1);
    connect(m_EventTimer, &QTimer::timeout,
            this, &AppleScreenSharingSession::pollSdlEvents);
    m_EventTimer->start();
    if (m_DynamicResolutionEnabled) {
        m_DynamicResolutionTimer = new QTimer(this);
        m_DynamicResolutionTimer->setSingleShot(true);
        m_DynamicResolutionTimer->setTimerType(Qt::CoarseTimer);
        connect(m_DynamicResolutionTimer, &QTimer::timeout,
                this, &AppleScreenSharingSession::sendPendingDynamicResolution);
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window, &width, &height);
        scheduleDynamicResolution(window, width, height);
    }
    m_MediaReady = true;
    m_EverMediaReady.store(true);
    updateControlSummary();
    setRunning();
    emit connectionStarted();
    QPointer<AppleScreenSharingSession> guard(this);
    QTimer::singleShot(360, this, [guard]() {
        if (guard == nullptr || guard->m_Cancelled.load() ||
                guard->m_Runtime->streamWindow() == nullptr) {
            return;
        }
        if (guard->m_QtWindow != nullptr) {
            guard->m_QtWindow->hide();
        }
        SDL_ShowWindow(guard->m_Runtime->streamWindow());
        SDL_RaiseWindow(guard->m_Runtime->streamWindow());
        guard->updateKeyboardGrabState(guard->m_Runtime->streamWindow());
    });
}

void AppleScreenSharingSession::destroyPresentation()
{
#ifdef Q_OS_WIN
    m_WindowsRemoteFileDragSource.reset();
    m_WindowsFileDropTargets.clear();
    m_WindowsKeyboardHook.reset();
#endif
    releaseAllKeys();
#ifdef Q_OS_WIN
    // Restore SDL's normal application behavior outside a streaming session.
    SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, "0");
#endif
    if (m_EventTimer != nullptr) {
        m_EventTimer->stop();
        m_EventTimer->deleteLater();
        m_EventTimer = nullptr;
    }
    if (m_DynamicResolutionTimer != nullptr) {
        m_DynamicResolutionTimer->stop();
        m_DynamicResolutionTimer->deleteLater();
        m_DynamicResolutionTimer = nullptr;
    }
    m_LiveResizing = false;
    if (m_VideoRenderer != nullptr) {
        m_VideoRenderer->stopDisplayLink();
    }
    if (m_SecondaryVideoRenderer != nullptr) {
        m_SecondaryVideoRenderer->stopDisplayLink();
    }
    m_DisplayLinkActive.store(false);
    if (m_PresentationThread != nullptr) {
        m_PresentationThread->requestInterruption();
        m_PresentationWake.release();
        m_PresentationThread->wait();
        m_PresentationThread.reset();
    }
    m_PresentationWake.tryAcquire(m_PresentationWake.available());
    m_Textures.clear();
    m_TextureSizes.clear();
    m_TextureFormats.clear();
    {
        QMutexLocker locker(&m_FrameMutex);
        m_LatestFrames.clear();
        m_SecondaryLatestFrames.clear();
        m_PendingFrameBatches = 0;
        m_SecondaryPendingFrameBatches = 0;
    }
    m_AwaitingPresentationBatches = 0;
    m_AwaitingDecodeSubmissions.clear();
    m_PerformanceOverlayUpdateNeeded.store(false);
    m_PresentationNeeded.store(true);
    m_PrimaryWindowMiniaturized.store(false);
    m_SecondaryWindowMiniaturized.store(false);
    m_ClipboardFocusedWindows.clear();
    m_ClipboardAutomaticEligible.store(false);
    m_FrameUpdatePauseState = AppleFrameUpdatePauseState();
    m_LastRenderLoopAtNanoseconds = 0;
    m_MaxRenderLoopGapMilliseconds = 0.0;
    m_MaxOverlayUpdateMilliseconds = 0.0;
    persistWindowGeometry(m_Runtime ? m_Runtime->streamWindow() : nullptr,
                          AppleWindowRole::Primary);
    persistWindowGeometry(m_SecondaryWindow, AppleWindowRole::Secondary);
#ifdef Q_OS_DARWIN
    m_AppleMacInputBridge.reset();
    m_MacRemoteFileDragSource.reset();
#endif
    m_LocalFileDragLifecycle.reset();
    m_LocalFileDragPointerActive = false;
    m_VideoRenderer.reset();
    m_SecondaryVideoRenderer.reset();
    if (m_SecondaryWindow != nullptr) {
        SDL_DestroyWindow(m_SecondaryWindow);
        m_SecondaryWindow = nullptr;
    }
    m_SecondaryTileHeights.clear();
    m_SecondaryCanvas = {};
    m_SecondaryMediaReady = false;
    m_PrimaryKeyboardGrabActive = false;
    m_SecondaryKeyboardGrabActive = false;
    m_PerformanceOverlayTexture = nullptr;
    m_PerformanceOverlaySize = {};
    m_Renderer = nullptr;
    useDefaultRemoteCursor();
    m_RemoteCursorStore.clear();
    if (m_Runtime) {
        m_Runtime->shutdown();
    }
    if (m_QtWindow != nullptr) {
        m_QtWindow->show();
        m_QtWindow->raise();
    }
}

void AppleScreenSharingSession::prepareForReconnect(
        int attempt,
        const QString& reason)
{
    if (m_Cancelled.load()) {
        return;
    }
    qWarning().nospace()
            << "Apple Screen Sharing reconnect " << attempt << "/"
            << MaximumReconnectAttempts << ": " << reason;
    if (m_FileTransferProgressWindow) {
        m_FileTransferProgressWindow->failActive(reason);
    }
    if (m_RemoteFileDragGate) m_RemoteFileDragGate->clear();
    if (m_RemoteFileDragInputState) m_RemoteFileDragInputState->reset();
    m_ActiveFileTransferSessionId = 0;
    m_ActiveFileTransferPaused = false;
    emit stageStarting(tr("Reconnecting to the Mac (%1/%2)")
                       .arg(attempt)
                       .arg(MaximumReconnectAttempts));
    m_ControlReady.store(false);
    destroyPresentation();
    m_MediaReady = false;
    m_SecondaryMediaReady = false;
    m_DisplayLayout = {};
    m_MediaDisplayIds.clear();
    m_LocalClipboardTracker.reset();
    m_PendingDynamicResolution = {};
    m_LastRequestedDynamicResolution = {};
    m_LastDynamicResolutionRequestAt = 0;
    {
        QMutexLocker locker(&m_InputMutex);
        m_SelectedInputDisplayId.reset();
        m_PendingControls.clear();
    }
    {
        QMutexLocker locker(&m_PerformanceMutex);
        m_PerformanceMediaSummary.clear();
        m_SecondaryPerformanceSummary.clear();
        m_PerformancePresentationSummary.clear();
        m_AudioSummary.clear();
        m_FileTransferSummary.clear();
    }
    m_PresentationWindowStartedAt = 0;
    m_PresentationCount = 0;
    m_PresentedTileUpdates = 0;
    m_DisplayedFrameBatches = 0;
    m_DroppedFrameBatches = 0;
    m_PresentationBusyCount = 0;
    m_LastDisplayedFrameAt = 0;
    m_DisplayFrameIntervals.clear();
    m_SubmitToDisplayLatencies.clear();
    m_RenderCallDurations.clear();
    m_MaxRenderLoopGapMilliseconds = 0.0;
    m_MaxOverlayUpdateMilliseconds = 0.0;
    LocalStreamRuntimeConfig runtimeConfig;
    const QSize initialDisplaySize = m_InitialDisplaySizes.isEmpty()
            ? QSize(1440, 900) : m_InitialDisplaySizes.first();
    runtimeConfig.streamWidth = initialDisplaySize.width();
    runtimeConfig.streamHeight = initialDisplaySize.height();
    if (m_QtWindow == nullptr ||
            !m_Runtime->initialize(m_QtWindow, runtimeConfig)) {
        m_Cancelled.store(true);
        emit displayLaunchError(tr(
                "Couldn’t reinitialize video presentation after reconnecting."));
    }
    updateControlSummary();
}

void AppleScreenSharingSession::complete(bool success, const QString& error)
{
    destroyPresentation();
    if (!success && !m_Cancelled.load() && !error.isEmpty()) {
        emit displayLaunchError(error);
    }
    finishSession(success ? 0 : -1);
    publishReadyForDeletion();
}
