#include "applescreensharingsession.h"

#include "applefiletransferdialog.h"
#include "applefiledrag.h"
#include "applefiletransferprogress.h"
#ifdef Q_OS_WIN
#include "applefiledrag_win.h"
#endif

#include "appleauthenticator.h"
#include "appleaudiostream.h"
#include "applecredentialstore.h"
#include "applekeyboardmapper.h"
#include "applevideorenderer.h"
#include "applemediatransport.h"
#include "settings/streamingpreferences.h"
#include "streaming/localstreamruntime.h"

#include "SDL.h"

#include <QCoreApplication>
#include <QClipboard>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QMetaObject>
#include <QMimeData>
#include <QMutexLocker>
#include <QPainter>
#include <QPointer>
#include <QQuickWindow>
#include <QRunnable>
#include <QSemaphore>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <QThreadPool>
#include <QTemporaryDir>
#include <QTimer>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include "SDL_syswm.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <utility>

namespace {

constexpr qint64 InitialVideoTimeoutMs = 12000;
constexpr qint64 ReceiverReportIntervalMs = 1000;
constexpr qint64 RateControlIntervalMs = 50;
constexpr qint64 KeyFrameRetryIntervalMs = 1500;
constexpr qint64 DecoderStallMs = 5000;
constexpr qint64 PerformanceReportIntervalMs = 1000;
constexpr int RealtimeMediaPollTimeoutMs = 1;
constexpr int PerformanceOverlayLineCount = 9;
constexpr int PerformanceOverlayReservedCharacters = 96;
constexpr quint32 FixedLanBandwidthKilobitsPerSecond = 60001;
constexpr int MaximumReconnectAttempts = 3;

QString appleVideoDecoderBackendName(AppleVideoDecoderBackend backend)
{
    switch (backend) {
    case AppleVideoDecoderBackend::D3D11va:
        return QStringLiteral("D3D11VA");
    case AppleVideoDecoderBackend::VideoToolbox:
        return QStringLiteral("VideoToolbox");
    case AppleVideoDecoderBackend::Software:
    default:
        return QStringLiteral("software");
    }
}

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

struct IntervalStatistics
{
    double average = 0.0;
    double percentile95 = 0.0;
    double jitter = 0.0;
};

IntervalStatistics calculateIntervalStatistics(const QVector<double>& intervals)
{
    IntervalStatistics statistics;
    if (intervals.isEmpty()) {
        return statistics;
    }

    QVector<double> sorted = intervals;
    double sum = 0.0;
    for (double interval : sorted) {
        sum += interval;
    }
    statistics.average = sum / sorted.size();
    std::sort(sorted.begin(), sorted.end());
    const int percentileIndex = qBound(
            0,
            static_cast<int>(std::ceil(sorted.size() * 0.95)) - 1,
            sorted.size() - 1);
    statistics.percentile95 = sorted.at(percentileIndex);

    double squaredDeviation = 0.0;
    for (double interval : intervals) {
        const double deviation = interval - statistics.average;
        squaredDeviation += deviation * deviation;
    }
    statistics.jitter = std::sqrt(squaredDeviation / intervals.size());
    return statistics;
}

quint32 currentMicroseconds()
{
    return static_cast<quint32>(
            SDL_GetPerformanceCounter() * 1000000ULL / SDL_GetPerformanceFrequency());
}

quint64 steadyNanoseconds()
{
    return static_cast<quint64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
}

double cursorDpiScale(SDL_Window* window)
{
#ifdef Q_OS_WIN
    if (window != nullptr) {
        SDL_SysWMinfo windowInfo = {};
        SDL_VERSION(&windowInfo.version);
        if (SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
                windowInfo.subsystem == SDL_SYSWM_WINDOWS &&
                windowInfo.info.win.window != nullptr) {
            const UINT dpi = GetDpiForWindow(windowInfo.info.win.window);
            if (dpi > 0) {
                return static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI;
            }
        }
    }
#elif defined(Q_OS_DARWIN)
    if (window != nullptr) {
        int pointWidth = 0;
        int pointHeight = 0;
        int pixelWidth = 0;
        int pixelHeight = 0;
        SDL_GetWindowSize(window, &pointWidth, &pointHeight);
        SDL_Metal_GetDrawableSize(window, &pixelWidth, &pixelHeight);
        if (pointWidth > 0 && pointHeight > 0 &&
                pixelWidth > 0 && pixelHeight > 0) {
            const double horizontalScale =
                    static_cast<double>(pixelWidth) / pointWidth;
            const double verticalScale =
                    static_cast<double>(pixelHeight) / pointHeight;
            return qMax(1.0, (horizontalScale + verticalScale) / 2.0);
        }
    }
#else
    Q_UNUSED(window)
#endif
    return 1.0;
}

#ifdef Q_OS_WIN
HWND nativeHandleForWindow(SDL_Window* window)
{
    if (window == nullptr) {
        return nullptr;
    }
    SDL_SysWMinfo windowInfo = {};
    SDL_VERSION(&windowInfo.version);
    if (SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
            windowInfo.subsystem == SDL_SYSWM_WINDOWS &&
            windowInfo.info.win.window != nullptr) {
        return windowInfo.info.win.window;
    }
    return nullptr;
}

bool nativeHandleMatchesWindow(SDL_Window* window, HWND nativeHandle)
{
    return nativeHandle != nullptr &&
            nativeHandleForWindow(window) == nativeHandle;
}
#endif

void recordMaximum(std::atomic<quint64>& value, quint64 candidate)
{
    quint64 previous = value.load(std::memory_order_relaxed);
    while (previous < candidate &&
           !value.compare_exchange_weak(
                   previous, candidate,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
    }
}

quint8 appleButtonForSdl(quint8 button)
{
    switch (button) {
    case SDL_BUTTON_LEFT: return 1 << 0;
    case SDL_BUTTON_RIGHT: return 1 << 1;
    case SDL_BUTTON_MIDDLE: return 1 << 2;
    default: return 0;
    }
}

QImage renderMoonlightPerformanceOverlay(
        const QList<ApplePerformanceOverlayTextRun>& runs,
        int outputWidth)
{
    if (runs.isEmpty() || outputWidth <= 0) {
        return {};
    }

    static const QString moonlightFontFamily = []() {
        const int fontId = QFontDatabase::addApplicationFont(
                QStringLiteral(":/data/ModeSeven.ttf"));
        const QStringList families =
                QFontDatabase::applicationFontFamilies(fontId);
        return families.isEmpty()
                ? QStringLiteral("Consolas") : families.first();
    }();
    constexpr int padding = 4;
    const int availableWidth = qMax(1, outputWidth - padding * 2);

    const auto fontForRun = [](
            const ApplePerformanceOverlayTextRun& run,
            double scale) {
        QFont font(moonlightFontFamily);
        font.setStyleHint(QFont::Monospace);
        font.setPixelSize(qMax(8, qRound(run.pixelSize * scale)));
        font.setBold(run.bold);
        return font;
    };
    const auto measure = [&runs, &fontForRun](
            double scale, int* width, int* ascent, int* descent) {
        *width = 0;
        *ascent = 0;
        *descent = 0;
        for (const ApplePerformanceOverlayTextRun& run : runs) {
            const QFontMetrics metrics(fontForRun(run, scale));
            *width += metrics.horizontalAdvance(run.text);
            *ascent = qMax(*ascent, metrics.ascent());
            *descent = qMax(*descent, metrics.descent());
        }
    };

    int contentWidth = 0;
    int maximumAscent = 0;
    int maximumDescent = 0;
    measure(1.0, &contentWidth, &maximumAscent, &maximumDescent);
    double scale = contentWidth > availableWidth
            ? static_cast<double>(availableWidth) / contentWidth
            : 1.0;
    measure(scale, &contentWidth, &maximumAscent, &maximumDescent);
    while (contentWidth > availableWidth && scale > 0.45) {
        scale *= 0.97;
        measure(scale, &contentWidth, &maximumAscent, &maximumDescent);
    }

    QImage image(qMin(outputWidth, contentWidth + padding * 2),
                 maximumAscent + maximumDescent + padding * 2,
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(0, 0, 0, 102));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(QColor(189, 249, 231));
    int x = padding;
    const int baseline = padding + maximumAscent;
    for (const ApplePerformanceOverlayTextRun& run : runs) {
        const QFont font = fontForRun(run, scale);
        painter.setFont(font);
        painter.drawText(x, baseline, run.text);
        x += QFontMetrics(font).horizontalAdvance(run.text);
    }
    painter.end();
    return image;
}

} // namespace

#ifdef Q_OS_WIN
// SDL's Windows system-key grab is implemented with WH_KEYBOARD_LL. With the
// SDL3 backend used through sdl2-compat, the hook can suppress LWIN/RWIN before
// their SDL keyboard events reach this session. Install after SDL updates its
// grab so this session can forward Command explicitly while still preventing
// the local Start menu from opening.
class AppleWindowsKeyboardHook
{
public:
    AppleWindowsKeyboardHook()
        : m_EventType(SDL_RegisterEvents(1))
    {
    }

    ~AppleWindowsKeyboardHook()
    {
        stop();
    }

    void update(SDL_Window* primary,
                bool primaryCapture,
                SDL_Window* secondary,
                bool secondaryCapture)
    {
        stop();
        m_Primary = targetForWindow(primary, primaryCapture);
        m_Secondary = targetForWindow(secondary, secondaryCapture);
        if (m_EventType == InvalidEventType ||
                (!m_Primary.capture && !m_Secondary.capture)) {
            return;
        }

        s_ActiveHook = this;
        m_Hook = SetWindowsHookExW(
                WH_KEYBOARD_LL,
                &AppleWindowsKeyboardHook::keyboardHookProc,
                GetModuleHandleW(nullptr),
                0);
        if (m_Hook == nullptr) {
            s_ActiveHook = nullptr;
            qWarning().nospace()
                    << "[DEBUG-APPLE-WIN-HOOK] installation failed error="
                    << GetLastError();
            return;
        }
        qInfo().nospace()
                << "[DEBUG-APPLE-WIN-HOOK] installed primary="
                << m_Primary.windowId << "/" << m_Primary.capture
                << " secondary=" << m_Secondary.windowId
                << "/" << m_Secondary.capture;
    }

    bool decodeEvent(const SDL_Event& event,
                     bool* isDown,
                     bool* isRight,
                     quint32* windowId) const
    {
        if (event.type != m_EventType || isDown == nullptr ||
                isRight == nullptr || windowId == nullptr) {
            return false;
        }
        *isDown = (event.user.code & DownFlag) != 0;
        *isRight = (event.user.code & RightFlag) != 0;
        *windowId = event.user.windowID;
        return true;
    }

private:
    struct WindowTarget
    {
        HWND nativeHandle = nullptr;
        quint32 windowId = 0;
        bool capture = false;
    };

    static constexpr Uint32 InvalidEventType = static_cast<Uint32>(-1);
    static constexpr Sint32 DownFlag = 1 << 0;
    static constexpr Sint32 RightFlag = 1 << 1;

    static WindowTarget targetForWindow(SDL_Window* window, bool capture)
    {
        WindowTarget target;
        target.nativeHandle = nativeHandleForWindow(window);
        target.windowId = window != nullptr ? SDL_GetWindowID(window) : 0;
        target.capture = capture && target.nativeHandle != nullptr &&
                target.windowId != 0;
        return target;
    }

    const WindowTarget* capturedTarget(HWND foreground) const
    {
        if (m_Primary.capture && m_Primary.nativeHandle == foreground) {
            return &m_Primary;
        }
        if (m_Secondary.capture && m_Secondary.nativeHandle == foreground) {
            return &m_Secondary;
        }
        return nullptr;
    }

    static LRESULT CALLBACK keyboardHookProc(
            int code, WPARAM message, LPARAM data)
    {
        AppleWindowsKeyboardHook* hook = s_ActiveHook;
        if (code == HC_ACTION && hook != nullptr) {
            const auto* keyboard =
                    reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            const bool isWin = keyboard->vkCode == VK_LWIN ||
                    keyboard->vkCode == VK_RWIN;
            const bool isDown = message == WM_KEYDOWN ||
                    message == WM_SYSKEYDOWN;
            const bool isUp = message == WM_KEYUP ||
                    message == WM_SYSKEYUP;
            const WindowTarget* target =
                    hook->capturedTarget(GetForegroundWindow());
            if (isWin && (isDown || isUp) && target != nullptr) {
                SDL_Event event = {};
                event.type = hook->m_EventType;
                event.user.timestamp = SDL_GetTicks();
                event.user.windowID = target->windowId;
                event.user.code = (isDown ? DownFlag : 0) |
                        (keyboard->vkCode == VK_RWIN ? RightFlag : 0);
                if (SDL_PushEvent(&event) == 1) {
                    return 1;
                }
            }
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    void stop()
    {
        if (m_Hook != nullptr) {
            UnhookWindowsHookEx(m_Hook);
            m_Hook = nullptr;
        }
        if (s_ActiveHook == this) {
            s_ActiveHook = nullptr;
        }
    }

    inline static AppleWindowsKeyboardHook* s_ActiveHook = nullptr;
    Uint32 m_EventType = InvalidEventType;
    HHOOK m_Hook = nullptr;
    WindowTarget m_Primary;
    WindowTarget m_Secondary;
};
#endif

// A second display is a distinct AVConference stream, not another tile group
// in the primary decoder. This module owns every mutable decoder/network state
// for that stream and exposes only one polling operation to the session loop.
class AppleSecondaryVideoStream
{
public:
    AppleSecondaryVideoStream(AppleScreenSharingSession* session,
                              AppleVideoNegotiation negotiation,
                              bool preferHardware,
                              QString* error)
        : m_Session(session),
          m_Negotiation(std::move(negotiation)),
          m_Decryptor(m_Negotiation.serverKey, error),
          m_Feedback(m_Negotiation.viewerKey, error),
          m_PreferHardware(preferHardware)
    {
        m_Clock.start();
    }

    bool isValid() const
    {
        return m_Negotiation.isUsable() && m_Decryptor.isValid() &&
                m_Feedback.isValid();
    }

    bool poll(AppleMediaTransport& media,
              std::atomic_bool* cancelled,
              bool* receivedDatagram,
              QString* error)
    {
        if (receivedDatagram != nullptr) {
            *receivedDatagram = false;
        }
        const qint64 now = m_Clock.elapsed();
        QByteArray datagram;
        QString receiveError;
        if (media.receiveVideo(m_Negotiation.mediaStreamIndex,
                               &datagram, 0, cancelled, &receiveError)) {
            if (receivedDatagram != nullptr) {
                *receivedDatagram = true;
            }
            if (!processDatagram(media, datagram, now, error)) {
                return false;
            }
        }
        else if (!receiveError.isEmpty()) {
            if (error != nullptr) {
                *error = receiveError;
            }
            return false;
        }

        if (m_FirstPacketAt < 0 && now >= InitialVideoTimeoutMs) {
            if (error != nullptr) {
                *error = QCoreApplication::translate(
                        "AppleScreenSharingSession",
                        "The Mac negotiated a second display but sent no video packets.");
            }
            return false;
        }
        if (!sendPeriodicFeedback(media, now, error)) {
            return false;
        }
        m_Assembler.expire(now);
        return true;
    }

private:
    bool processDatagram(AppleMediaTransport& media,
                         const QByteArray& datagram,
                         qint64 now,
                         QString* error)
    {
        AppleRtpPacket packet;
        if (!m_Decryptor.decrypt(datagram, &packet, nullptr)) {
            return true;
        }
        ++m_ReceivedPacketCount;
        m_PerformanceBytes += static_cast<quint64>(datagram.size());
        if (!m_HasPreviousTimestamp) {
            m_PreviousTimestamp = packet.timestamp;
            m_HasPreviousTimestamp = true;
        }
        else if (packet.timestamp != m_PreviousTimestamp) {
            const quint32 delta = packet.timestamp - m_PreviousTimestamp;
            if (delta < 0x7fffffffU) {
                m_PreviousTimestamp = packet.timestamp;
                m_LastAcceptedTimestamp = packet.timestamp;
                m_LastAcceptedTimestampAt = now;
            }
        }
        if (m_FirstPacketAt < 0) {
            m_FirstPacketAt = now;
        }
        m_LastPacketAt = now;

        AppleHevcAccessUnit accessUnit;
        const bool completed = m_Assembler.process(packet, now, &accessUnit);
        if (m_Sources.isEmpty() && m_Assembler.parameterSets().isComplete()) {
            const QList<quint32> candidates = m_Assembler.primarySources(
                    m_Negotiation.canvas.tileCount);
            const bool complete = candidates.size() ==
                    m_Negotiation.canvas.tileCount &&
                    std::all_of(candidates.cbegin(), candidates.cend(),
                                [this](quint32 source) {
                return m_Assembler.completedSources().contains(source);
            });
            const bool settled = m_Assembler.totalPacketCount() >= 100 ||
                    now - m_FirstPacketAt >= 2300;
            if (complete && settled) {
                m_Sources = candidates;
                for (int index = 0; index < m_Sources.size(); ++index) {
                    m_SourceToTile.insert(m_Sources.at(index), index);
                }
                m_Decoder = std::make_unique<AppleHevcDecoder>(
                        m_PreferHardware, m_Negotiation.canvas.tileCount);
                if (!m_Decoder->open(error) ||
                        !requestKeyFrames(media, error)) {
                    return false;
                }
                m_LastKeyFrameAt = now;
                m_AwaitingRandomAccessPicture = true;
            }
        }

        if (!completed ||
                !m_SourceToTile.contains(accessUnit.synchronizationSource)) {
            return true;
        }
        if (m_AwaitingRandomAccessPicture) {
            if (!accessUnit.containsRandomAccessPicture()) {
                return true;
            }
            m_DecodingOrder.reset();
            m_AwaitingRandomAccessPicture = false;
        }
        const QList<AppleHevcAccessUnit> readyUnits =
                m_DecodingOrder.enqueue({accessUnit});
        for (const AppleHevcAccessUnit& ready : readyUnits) {
            const int tile = m_SourceToTile.value(
                    ready.synchronizationSource);
            m_FrameBatcher.recordSubmission(ready, tile);
            publishReadyBatches();
            QString decodeError;
            QElapsedTimer decodeClock;
            decodeClock.start();
            const quint64 decoderGeneration = m_Decoder->generation();
            QList<AppleDecodedTile> frames = m_Decoder->decode(
                    ready, m_Assembler.parameterSets(), tile, &decodeError);
            ++m_PerformanceDecodeCalls;
            m_PerformanceDecodeNanoseconds += decodeClock.nsecsElapsed();
            if (m_Decoder->generation() != decoderGeneration) {
                // A decoder replacement is a hard reference discontinuity.
                // Do not let the submitted tile remain at the head of the
                // atomic frame queue or feed inter pictures into the fresh
                // codec before the requested random-access picture arrives.
                m_FrameBatcher.reset();
                m_Assembler.discardIncomplete();
                m_DecodingOrder.reset();
                m_AwaitingRandomAccessPicture = true;
                m_EnteredRefreshState = true;
                if (!requestKeyFrames(media, error)) {
                    return false;
                }
                m_LastKeyFrameAt = now;
                qWarning().noquote()
                        << "Apple display 2 HEVC decoder reset:"
                        << decodeError;
                return true;
            }
            if (!frames.isEmpty()) {
                m_PerformanceDecodedTiles +=
                        static_cast<quint64>(frames.size());
                if (!m_PerformanceDecodedSourceTimestamps.contains(
                            ready.timestamp)) {
                    m_PerformanceDecodedSourceTimestamps.insert(
                            ready.timestamp);
                }
                m_LastDecodedAt = now;
                m_FrameBatcher.recordDecodedFrames(std::move(frames));
                publishReadyBatches();
                if (!m_NotifiedReady) {
                    m_NotifiedReady = true;
                    m_Session->m_EverMediaReady.store(true);
                    const QPointer<AppleScreenSharingSession> session = m_Session;
                    const AppleCanvas canvas = m_Negotiation.canvas;
                    const AppleVideoDecoderBackend backend =
                            m_Decoder->backend();
                    const bool fallback =
                            m_Decoder->hardwareFallbackOccurred();
                    const std::shared_ptr<AppleVideoBackendContext> context =
                            m_Decoder->presentationContext();
                    const int displayIndex = m_Negotiation.displayIndex;
                    QMetaObject::invokeMethod(
                            session,
                            [session, canvas, backend, fallback,
                             context, displayIndex]() {
                                if (session != nullptr) {
                                    session->mediaReady(canvas, backend,
                                                        fallback, context,
                                                        displayIndex);
                                }
                            },
                            Qt::QueuedConnection);
                }
            }
            else if (!decodeError.isEmpty()) {
                m_FrameBatcher.recordDecodeFailure(
                        ready.frameSequenceNumber, tile);
                publishReadyBatches();
                if (now - m_LastKeyFrameAt >= KeyFrameRetryIntervalMs) {
                    if (!requestKeyFrames(media, error)) {
                        return false;
                    }
                    if (!m_EnteredRefreshState) {
                        m_EnteredRefreshState = true;
                        m_Assembler.discardIncomplete();
                        m_DecodingOrder.reset();
                    }
                    m_LastKeyFrameAt = now;
                }
            }
        }
        return true;
    }

    void publishReadyBatches()
    {
        QList<QList<AppleDecodedTile>> batches =
                m_FrameBatcher.takeReadyBatches();
        for (QList<AppleDecodedTile>& batch : batches) {
            m_Session->queueDecodedFrames(
                    std::move(batch), m_Negotiation.displayIndex);
        }
    }

    bool sendPeriodicFeedback(AppleMediaTransport& media,
                              qint64 now,
                              QString* error)
    {
        if (m_Sources.isEmpty()) {
            return true;
        }
        const auto nacks = m_Assembler.takeNacks(now);
        for (auto iterator = nacks.cbegin(); iterator != nacks.cend(); ++iterator) {
            const QByteArray packet = AppleMediaWire::receiverReport(
                    m_Negotiation.synchronizationSource) +
                    AppleMediaWire::genericNack(
                            m_Negotiation.synchronizationSource,
                            iterator.key(), iterator.value());
            if (!sendFeedback(media, packet, error)) {
                return false;
            }
            m_PerformanceNacks +=
                    static_cast<quint64>(iterator.value().size());
        }
        if (now - m_LastReceiverReportAt >= ReceiverReportIntervalMs) {
            if (!sendFeedback(media,
                              AppleMediaWire::receiverReport(
                                      m_Negotiation.synchronizationSource),
                              error)) {
                return false;
            }
            m_LastReceiverReportAt = now;
        }
        if (m_HasPreviousTimestamp &&
                now - m_LastRateControlAt >= RateControlIntervalMs) {
            const quint32 delay = m_LastAcceptedTimestampAt >= 0
                    ? static_cast<quint32>(qMin(
                              now - m_LastAcceptedTimestampAt,
                              static_cast<qint64>(0xffff)))
                    : 0xffff;
            const quint16 echo = static_cast<quint16>(
                    (static_cast<quint64>(now) * 1024 / 1000) & 0xffff);
            if (!sendFeedback(media,
                              AppleMediaWire::rateControl(
                                      m_Negotiation.synchronizationSource,
                                      m_LastAcceptedTimestamp,
                                      FixedLanBandwidthKilobitsPerSecond,
                                      m_ReceivedPacketCount, delay, echo),
                              error)) {
                return false;
            }
            m_LastRateControlAt = now;
        }
        const bool awaiting = m_LastDecodedAt < 0 &&
                now - m_LastKeyFrameAt >= KeyFrameRetryIntervalMs;
        const bool stalled = m_LastDecodedAt >= 0 && m_LastPacketAt >= 0 &&
                now - m_LastDecodedAt >= DecoderStallMs &&
                now - m_LastPacketAt < 1500 &&
                now - m_LastKeyFrameAt >= DecoderStallMs;
        if (awaiting || stalled) {
            if (!requestKeyFrames(media, error)) {
                return false;
            }
            m_DecodingOrder.reset();
            if (awaiting) {
                m_Assembler.discardIncomplete();
            }
            m_LastKeyFrameAt = now;
        }
        reportPerformance(now);
        return true;
    }

    bool sendFeedback(AppleMediaTransport& media,
                      const QByteArray& packet,
                      QString* error)
    {
        const QByteArray protectedPacket = m_Feedback.protect(packet, error);
        return !protectedPacket.isEmpty() && media.sendVideoControl(
                m_Negotiation.mediaStreamIndex, protectedPacket, error);
    }

    bool requestKeyFrames(AppleMediaTransport& media, QString* error)
    {
        const QList<QByteArray> requests = AppleMediaWire::fullIntraRequests(
                m_Negotiation.synchronizationSource,
                m_Sources,
                m_KeyFrameSequence);
        for (const QByteArray& request : requests) {
            if (!sendFeedback(media, request, error)) {
                return false;
            }
        }
        m_PerformanceFirs += static_cast<quint64>(requests.size());
        m_KeyFrameSequence = static_cast<quint8>(
                m_KeyFrameSequence + requests.size());
        return true;
    }

    void reportPerformance(qint64 now)
    {
        if (now - m_PerformanceWindowStartedAt < PerformanceReportIntervalMs) {
            return;
        }
        const double seconds = qMax<qint64>(
                1, now - m_PerformanceWindowStartedAt) / 1000.0;
        const double sourceFramesPerSecond =
                m_PerformanceDecodedSourceTimestamps.size() / seconds;
        const double averageDecodeMilliseconds =
                m_PerformanceDecodeCalls == 0 ? 0.0
                : m_PerformanceDecodeNanoseconds / 1000000.0 /
                  m_PerformanceDecodeCalls;
        const QString backend = m_Decoder != nullptr
                ? appleVideoDecoderBackendName(m_Decoder->backend())
                : QStringLiteral("software");
        const QString summary = QStringLiteral(
                "DISPLAY 2 SOURCE %1 FPS   RX %2 Mbps   HEVC 4:4:4 %3 tiles/s @ %4 ms   %5   NACK %6   FIR %7")
                .arg(sourceFramesPerSecond, 0, 'f', 1)
                .arg(m_PerformanceBytes * 8.0 / seconds / 1000000.0,
                     0, 'f', 1)
                .arg(m_PerformanceDecodedTiles / seconds, 0, 'f', 1)
                .arg(averageDecodeMilliseconds, 0, 'f', 2)
                .arg(backend)
                .arg(m_PerformanceNacks)
                .arg(m_PerformanceFirs);
        qInfo().noquote() << "Apple High Performance" << summary;
        const QPointer<AppleScreenSharingSession> session = m_Session;
        if (session != nullptr) {
            QMetaObject::invokeMethod(
                    session,
                    [session, summary]() {
                        if (session != nullptr) {
                            session->updateSecondaryPerformanceStatistics(
                                    summary);
                        }
                    },
                    Qt::QueuedConnection);
        }
        m_PerformanceWindowStartedAt = now;
        m_PerformanceBytes = 0;
        m_PerformanceDecodedTiles = 0;
        m_PerformanceDecodeCalls = 0;
        m_PerformanceDecodeNanoseconds = 0;
        m_PerformanceNacks = 0;
        m_PerformanceFirs = 0;
        m_PerformanceDecodedSourceTimestamps.clear();
    }

    QPointer<AppleScreenSharingSession> m_Session;
    AppleVideoNegotiation m_Negotiation;
    AppleSrtpDecryptor m_Decryptor;
    AppleSrtcpEncryptor m_Feedback;
    AppleHevcAssembler m_Assembler;
    AppleHevcDecodingOrderQueue m_DecodingOrder;
    AppleDecodedFrameBatcher m_FrameBatcher;
    QList<quint32> m_Sources;
    QHash<quint32, int> m_SourceToTile;
    std::unique_ptr<AppleHevcDecoder> m_Decoder;
    QElapsedTimer m_Clock;
    qint64 m_FirstPacketAt = -1;
    qint64 m_LastPacketAt = -1;
    qint64 m_LastDecodedAt = -1;
    qint64 m_LastReceiverReportAt = 0;
    qint64 m_LastRateControlAt = 0;
    qint64 m_LastAcceptedTimestampAt = -1;
    qint64 m_LastKeyFrameAt = -KeyFrameRetryIntervalMs;
    quint32 m_ReceivedPacketCount = 0;
    quint32 m_PreviousTimestamp = 0;
    quint32 m_LastAcceptedTimestamp = 0;
    quint8 m_KeyFrameSequence = 0;
    qint64 m_PerformanceWindowStartedAt = 0;
    quint64 m_PerformanceBytes = 0;
    quint64 m_PerformanceDecodedTiles = 0;
    quint64 m_PerformanceDecodeCalls = 0;
    quint64 m_PerformanceDecodeNanoseconds = 0;
    quint64 m_PerformanceNacks = 0;
    quint64 m_PerformanceFirs = 0;
    QSet<quint32> m_PerformanceDecodedSourceTimestamps;
    bool m_PreferHardware = false;
    bool m_HasPreviousTimestamp = false;
    bool m_AwaitingRandomAccessPicture = true;
    bool m_EnteredRefreshState = false;
    bool m_NotifiedReady = false;
};

class AppleHighPerformanceSessionTask final : public QRunnable
{
public:
    AppleHighPerformanceSessionTask(AppleScreenSharingSession* session,
                                    AppleSavedConnection connection,
                                    std::atomic_bool* cancelled,
                                    bool preferHardware,
                                    QList<QSize> displaySizes)
        : m_Session(session),
          m_Connection(std::move(connection)),
          m_Cancelled(cancelled),
          m_PreferHardware(preferHardware),
          m_DisplaySizes(std::move(displaySizes)),
          m_DisplayCount(qBound(1, m_DisplaySizes.size(), 2))
    {
        while (m_DisplaySizes.size() < m_DisplayCount) {
            m_DisplaySizes.append(QSize(1440, 900));
        }
        m_DisplaySizes.resize(m_DisplayCount);
        for (QSize& size : m_DisplaySizes) {
            if (!size.isValid()) {
                size = QSize(1440, 900);
            }
        }
        setAutoDelete(true);
    }

    void run() override
    {
        QString error;
        bool succeeded = false;
        for (int attempt = 0; attempt <= MaximumReconnectAttempts &&
             !m_Cancelled->load(); ++attempt) {
            error.clear();
            succeeded = runConnectionAttempt(&error);
            if (succeeded || m_Cancelled->load() ||
                    !m_Session->m_EverMediaReady.load() ||
                    attempt >= MaximumReconnectAttempts) {
                break;
            }
            if (!prepareForRetry(attempt + 1, error, &error)) {
                break;
            }
        }

        const bool cancelled = m_Cancelled->load();
        const QPointer<AppleScreenSharingSession> session = m_Session;
        if (session != nullptr) {
            QMetaObject::invokeMethod(
                    session,
                    [session, succeeded, cancelled, error]() {
                        if (session != nullptr) {
                            session->complete(succeeded || cancelled, error);
                        }
                    },
                    Qt::QueuedConnection);
        }
    }

private:
    bool runConnectionAttempt(QString* error)
    {
        m_Session->m_ReconnectRequested.store(false);
        m_Session->m_NativePrecisionScrollSupported.store(false);
        m_Session->m_FileTransferSupported.store(false);
        m_Session->m_FileTransferService->reset();
        m_Session->m_FileTransferService->setControlling(
                !m_Session->m_Observing.load());
        AppleTcpTransport tcp;
        AppleAuthenticator authenticator;
        AppleAuthenticatedControl authenticated;
        bool succeeded = authenticator.authenticate(
                tcp,
                m_Connection.endpoint,
                m_Connection.trustedHostFingerprint,
                [reference = m_Connection.credentialReference,
                 connectionId = m_Connection.id](
                        AppleCredentials* credentials,
                        QString* credentialError) {
                    if (!AppleCredentialStore::isReferenceForConnection(
                                reference, connectionId)) {
                        if (credentialError != nullptr) {
                            *credentialError = QCoreApplication::translate(
                                    "AppleScreenSharingSession",
                                    "The saved credential binding is invalid.");
                        }
                        return false;
                    }
                    return AppleCredentialStore().load(
                            reference, credentials, credentialError);
                },
                &authenticated,
                m_Cancelled,
                error);
        if (succeeded) {
            const bool precisionScroll =
                    authenticated.supportsServerCommand(23);
            const bool fileTransfer =
                    authenticated.supportsServerCommand(32) &&
                    authenticated.supportsServerCommand(34);
            m_Session->m_NativePrecisionScrollSupported.store(
                    precisionScroll);
            m_Session->m_FileTransferSupported.store(fileTransfer);
            m_Session->m_FileTransferService->setAvailable(fileTransfer);
            qInfo() << "Apple Screen Sharing native precision scrolling:"
                    << (precisionScroll ? "supported" : "legacy fallback");
            qInfo() << "Apple Screen Sharing native file transfer:"
                    << (fileTransfer ? "supported" : "unavailable");
        }

        AppleControlChannel control;
        if (succeeded) {
            succeeded = control.negotiate(
                    tcp, authenticated.masterKey,
                    AppleWire::displayConfiguration(m_DisplaySizes),
                    m_Cancelled, error);
        }

        AppleMediaTransport media;
        AppleMediaNegotiationResult negotiation;
        AppleTextClipboardExchange clipboard;
        QString audioProbeError;
        const bool audioEnabled = AppleAudioStream::decoderIsSupported(
                &audioProbeError);
        if (succeeded) {
            succeeded = AppleMediaNegotiator().negotiate(
                    tcp, control, media, m_Connection.endpoint.port,
                    audioEnabled, m_DisplayCount, &negotiation,
                    m_Cancelled, error);
        }

        std::unique_ptr<AppleAudioStream> audio;
        if (succeeded && audioEnabled) {
            QString audioError;
            audio = std::make_unique<AppleAudioStream>(
                    negotiation.keys.audioServer, &audioError);
            if (!audio->isReady()) {
                qWarning() << "Apple Screen Sharing audio unavailable:"
                           << audioError;
                audio.reset();
            }
        }
        else if (succeeded && !audioProbeError.isEmpty()) {
            qWarning() << "Apple Screen Sharing audio decoder probe failed:"
                       << audioProbeError;
        }

        if (succeeded) {
            succeeded = control.sendEncrypted(
                    tcp,
                    AppleMediaWire::controlMode(m_Session->m_Observing.load()),
                    m_Cancelled,
                    error);
        }
        if (succeeded) {
            for (const QByteArray& message :
                 clipboard.setEligible(!m_Session->m_Observing.load())) {
                if (!control.sendEncrypted(
                            tcp, message, m_Cancelled, error)) {
                    succeeded = false;
                    break;
                }
            }
        }
        if (succeeded) {
            m_Session->m_ControlReady.store(true);
            const QPointer<AppleScreenSharingSession> session = m_Session;
            QMetaObject::invokeMethod(
                    session,
                    [session]() {
                        if (session != nullptr) {
                            session->updateControlSummary();
                            session->localClipboardChanged();
                        }
                    },
                    Qt::QueuedConnection);
            succeeded = runMediaLoop(
                    tcp, control, media, negotiation, clipboard,
                    audio.get(), error);
        }

        m_Session->m_ControlReady.store(false);
        m_Session->m_FileTransferService->setAvailable(false);
        media.close();
        tcp.close();
        return succeeded;
    }

    bool prepareForRetry(int attempt,
                         const QString& reason,
                         QString* error)
    {
        while (m_Session->m_SystemSuspended.load() &&
               !m_Cancelled->load()) {
            QThread::msleep(50);
        }
        if (m_Cancelled->load()) {
            return false;
        }

        const QPointer<AppleScreenSharingSession> retrySession = m_Session;
        if (retrySession == nullptr) {
            return false;
        }
        const auto presentationPrepared = std::make_shared<QSemaphore>();
        const bool resetQueued = QMetaObject::invokeMethod(
                retrySession,
                [retrySession, attempt, reason, presentationPrepared]() {
                    if (retrySession != nullptr) {
                        retrySession->prepareForReconnect(attempt, reason);
                    }
                    presentationPrepared->release();
                },
                Qt::QueuedConnection);
        bool resetCompleted = false;
        for (int waited = 0; resetQueued && waited < 5000 &&
             !m_Cancelled->load(); waited += 50) {
            if (presentationPrepared->tryAcquire(1, 50)) {
                resetCompleted = true;
                break;
            }
        }
        if (!resetCompleted || m_Cancelled->load()) {
            if (!m_Cancelled->load() && error != nullptr) {
                *error = QCoreApplication::translate(
                        "AppleScreenSharingSession",
                        "Video presentation did not reset in time for reconnection.");
            }
            return false;
        }

        const int delayMilliseconds = 500 << qMin(attempt - 1, 2);
        for (int waited = 0; waited < delayMilliseconds &&
             !m_Cancelled->load(); waited += 50) {
            QThread::msleep(50);
        }
        return !m_Cancelled->load();
    }

    bool runMediaLoop(AppleTcpTransport& tcp,
                      AppleControlChannel& control,
                      AppleMediaTransport& media,
                      const AppleMediaNegotiationResult& negotiation,
                      AppleTextClipboardExchange& clipboard,
                      AppleAudioStream* audio,
                      QString* error)
    {
        AppleSrtpDecryptor decryptor(negotiation.keys.videoServer, error);
        AppleSrtcpEncryptor feedback(negotiation.keys.videoViewer, error);
        if (!decryptor.isValid() || !feedback.isValid()) {
            return false;
        }
        std::unique_ptr<AppleSecondaryVideoStream> secondaryVideo;
        if (negotiation.videos.size() > 1) {
            secondaryVideo = std::make_unique<AppleSecondaryVideoStream>(
                    m_Session, negotiation.videos.at(1),
                    m_PreferHardware, error);
            if (!secondaryVideo->isValid()) {
                return false;
            }
        }
        AppleHevcAssembler assembler;
        AppleHevcDecodingOrderQueue decodingOrder;
        AppleDecodedFrameBatcher frameBatcher;
        QList<quint32> sources;
        QHash<quint32, int> sourceToTile;
        std::unique_ptr<AppleHevcDecoder> decoder;
        QElapsedTimer clock;
        clock.start();
        qint64 firstPacketAt = -1;
        qint64 videoWaitStartedAt = 0;
        qint64 lastPacketAt = -1;
        qint64 lastDecodedAt = -1;
        qint64 lastReceiverReportAt = 0;
        qint64 lastRateControlAt = 0;
        qint64 lastAcceptedTimestampAt = -1;
        qint64 lastKeyFrameAt = -KeyFrameRetryIntervalMs;
        quint32 receivedPacketCount = 0;
        quint32 previousTimestamp = 0;
        quint32 lastAcceptedTimestamp = 0;
        bool hasPreviousTimestamp = false;
        quint8 keyFrameSequence = 0;
        bool notifiedMediaReady = false;
        AppleVideoDecoderBackend decoderBackend =
                AppleVideoDecoderBackend::Software;
        bool hardwareFallback = false;
        bool awaitingRandomAccessPicture = true;
        bool hasEnteredDecodeRefreshState = false;
        qint64 performanceWindowStartedAt = 0;
        quint64 performancePackets = 0;
        quint64 performanceBytes = 0;
        quint64 performanceAccessUnits = 0;
        quint64 performanceDecodedTiles = 0;
        quint64 performanceDecodeCalls = 0;
        quint64 performanceNacks = 0;
        quint64 performanceFirs = 0;
        QSet<quint32> performanceDecodedSourceTimestamps;
        std::optional<quint32> lastPerformanceSourceTimestamp;
        qint64 lastPerformanceSourceFrameAt = -1;
        QVector<double> performanceSourceFrameIntervals;
        QVector<double> performanceRtpFrameIntervals;
        QVector<double> performanceControlSendLatencies;
        qint64 performanceDecodeNanoseconds = 0;
        AppleAudioStatistics previousAudioStatistics;
        bool observing = m_Session->m_Observing.load();
        AppleCanvas activeCanvas = negotiation.canvas;
        bool hasReceivedInitialLayout = false;
        qint64 dynamicResolutionResponsePendingAt = -1;

        for (const QByteArray& pending : negotiation.pendingMessages) {
            const AppleControlEvents events =
                    AppleControlEventParser::parse(pending);
            if (!events.displayLayouts.isEmpty()) {
                hasReceivedInitialLayout = true;
            }
            if (!events.cursorUpdates.isEmpty() ||
                    !events.displayLayouts.isEmpty()) {
                const QPointer<AppleScreenSharingSession> session = m_Session;
                QMetaObject::invokeMethod(
                        session,
                        [session, events]() {
                            if (session != nullptr) {
                                session->applyControlEvents(events);
                            }
                        },
                        Qt::QueuedConnection);
            }
        }

        auto resetVideoPipeline = [&]() {
            assembler = AppleHevcAssembler();
            decodingOrder.reset();
            frameBatcher.reset();
            sources.clear();
            sourceToTile.clear();
            if (decoder) {
                decoder->flush();
            }
            firstPacketAt = -1;
            videoWaitStartedAt = clock.elapsed();
            lastPacketAt = -1;
            lastDecodedAt = -1;
            lastAcceptedTimestampAt = -1;
            hasPreviousTimestamp = false;
            awaitingRandomAccessPicture = true;
            hasEnteredDecodeRefreshState = false;
            lastKeyFrameAt = -KeyFrameRetryIntervalMs;
        };

        const auto recordControlSent = [&](const AppleOutboundControl& outbound) {
            if (outbound.queuedAtNanoseconds == 0) {
                return;
            }
            const quint64 sentAt = steadyNanoseconds();
            if (sentAt >= outbound.queuedAtNanoseconds) {
                performanceControlSendLatencies.append(
                        (sentAt - outbound.queuedAtNanoseconds) / 1000000.0);
            }
        };

        const auto dispatchFileTransferEvents = [&]() {
            QList<AppleFileTransferEvent> fileEvents =
                    m_Session->m_FileTransferService->takeEvents();
            if (fileEvents.isEmpty()) return;
            const QPointer<AppleScreenSharingSession> session = m_Session;
            QMetaObject::invokeMethod(
                    session,
                    [session, fileEvents = std::move(fileEvents)]() mutable {
                        if (session != nullptr) {
                            session->applyFileTransferEvents(
                                    std::move(fileEvents));
                        }
                    },
                    Qt::QueuedConnection);
        };

        while (!m_Cancelled->load()) {
            if (m_Session->m_ReconnectRequested.exchange(false)) {
                if (error != nullptr) {
                    *error = QCoreApplication::translate(
                            "AppleScreenSharingSession",
                            "The session is reconnecting after a system or network interruption.");
                }
                return false;
            }
            for (const AppleOutboundControl& outbound :
                 m_Session->takePendingControls()) {
                QList<QByteArray> messages;
                switch (outbound.kind) {
                case AppleOutboundControl::Kind::Input:
                    if (!observing &&
                            !control.sendEncryptedInput(
                                    tcp,
                                    outbound.input.header,
                                    outbound.input.plaintextBlock,
                                    m_Cancelled,
                                    error)) {
                        return false;
                    }
                    if (!observing) {
                        recordControlSent(outbound);
                    }
                    continue;
                case AppleOutboundControl::Kind::Message:
                    messages.append(outbound.message);
                    break;
                case AppleOutboundControl::Kind::LocalClipboardText:
                    messages = clipboard.advertiseLocalText(
                            outbound.text, error);
                    break;
                case AppleOutboundControl::Kind::SetObserving:
                    messages.append(AppleMediaWire::controlMode(
                            outbound.observing));
                    messages.append(clipboard.setEligible(
                            !outbound.observing));
                    observing = outbound.observing;
                    break;
                }
                for (const QByteArray& message : std::as_const(messages)) {
                    if (!message.isEmpty() &&
                            !control.sendEncrypted(
                                    tcp, message, m_Cancelled, error)) {
                        return false;
                    }
                    if (!message.isEmpty() &&
                            static_cast<quint8>(message.at(0)) == 0x1d) {
                        dynamicResolutionResponsePendingAt = clock.elapsed();
                        qInfo().nospace()
                                << "Apple Screen Sharing dynamic resolution "
                                   "control sent: "
                                << message.size() << " plaintext bytes";
                    }
                }
                recordControlSent(outbound);
            }

            // File payload is deliberately lower priority than input and
            // display control. Send one complete logical file-copy message
            // per loop; its encrypted-record fragments must remain adjacent
            // just like the Swift writer's atomic send(messages) operation.
            for (const QByteArray& message :
                 m_Session->m_FileTransferService->takeOutbound(1)) {
                if (!control.sendEncrypted(
                            tcp, message, m_Cancelled, error)) {
                    return false;
                }
            }
            dispatchFileTransferEvents();

            if (tcp.hasPendingData()) {
                QByteArray message;
                if (!control.receiveEncrypted(tcp, &message, m_Cancelled, error)) {
                    return false;
                }
                QString fileDiagnostic;
                if (m_Session->m_FileTransferService->receive(
                            message, &fileDiagnostic)) {
                    if (!fileDiagnostic.isEmpty()) {
                        qWarning().noquote()
                                << "Apple file transfer:" << fileDiagnostic;
                    }
                    dispatchFileTransferEvents();
                }
                const AppleControlEvents events =
                        AppleControlEventParser::parse(message);
                for (const AppleDisplayLayout& layout : events.displayLayouts) {
                    if (!control.sendEncrypted(
                                tcp,
                                AppleMediaWire::framebufferUpdateRequest(),
                                m_Cancelled,
                                error)) {
                        return false;
                    }
                    if (!hasReceivedInitialLayout) {
                        hasReceivedInitialLayout = true;
                        continue;
                    }
                    AppleCanvas layoutCanvas{
                        layout.backingWidth,
                        layout.backingHeight,
                        activeCanvas.tileCount,
                    };
                    if (m_DisplayCount == 1 && layoutCanvas.isUsable() &&
                            layoutCanvas != activeCanvas) {
                        activeCanvas = layoutCanvas;
                        resetVideoPipeline();
                        const QPointer<AppleScreenSharingSession> session = m_Session;
                        QMetaObject::invokeMethod(
                                session,
                                [session, layoutCanvas]() {
                                    if (session != nullptr) {
                                        session->applyCanvas(layoutCanvas);
                                    }
                                },
                                Qt::QueuedConnection);
                        if (!control.sendEncrypted(
                                    tcp, negotiation.configuration,
                                    m_Cancelled, error)) {
                            return false;
                        }
                    }
                }
                AppleCanvas mediaCanvas;
                const bool hasMediaCanvas =
                        AppleMediaWire::parseCanvas(message, &mediaCanvas) &&
                        mediaCanvas.isUsable();
                if (dynamicResolutionResponsePendingAt >= 0) {
                    qInfo().nospace()
                            << "Apple Screen Sharing dynamic response: type=0x"
                            << QString::number(
                                       message.isEmpty() ? 0
                                               : static_cast<quint8>(
                                                         message.at(0)),
                                       16)
                            << ", bytes=" << message.size()
                            << ", layouts=" << events.displayLayouts.size()
                            << ", mediaCanvas="
                            << (hasMediaCanvas
                                        ? QStringLiteral("%1x%2")
                                                  .arg(mediaCanvas.width)
                                                  .arg(mediaCanvas.height)
                                        : QStringLiteral("none"));
                    if (!events.displayLayouts.isEmpty() || hasMediaCanvas) {
                        dynamicResolutionResponsePendingAt = -1;
                    }
                }
                if (m_DisplayCount == 1 && hasMediaCanvas &&
                        mediaCanvas.isUsable() && mediaCanvas != activeCanvas) {
                    activeCanvas = mediaCanvas;
                    const QPointer<AppleScreenSharingSession> session = m_Session;
                    QMetaObject::invokeMethod(
                            session,
                            [session, mediaCanvas]() {
                                if (session != nullptr) {
                                    session->applyCanvas(mediaCanvas);
                                }
                            },
                            Qt::QueuedConnection);
                }
                AppleTextClipboardResult clipboardResult =
                        clipboard.receive(message, error);
                for (const QByteArray& response :
                     std::as_const(clipboardResult.outboundMessages)) {
                    if (!control.sendEncrypted(
                                tcp, response, m_Cancelled, error)) {
                        return false;
                    }
                }
                if (!events.cursorUpdates.isEmpty() ||
                        !events.displayLayouts.isEmpty()) {
                    const QPointer<AppleScreenSharingSession> session = m_Session;
                    QMetaObject::invokeMethod(
                            session,
                            [session, events]() {
                                if (session != nullptr) {
                                    session->applyControlEvents(events);
                                }
                            },
                            Qt::QueuedConnection);
                }
                if (clipboardResult.receivedText.has_value()) {
                    const QPointer<AppleScreenSharingSession> session = m_Session;
                    const QString text = *clipboardResult.receivedText;
                    QMetaObject::invokeMethod(
                            session,
                            [session, text]() {
                                if (session != nullptr) {
                                    session->applyRemoteClipboardText(text);
                                }
                            },
                            Qt::QueuedConnection);
                }
            }
            else if (!tcp.isConnected()) {
                if (error != nullptr) {
                    *error = QCoreApplication::translate(
                            "AppleScreenSharingSession",
                            "The Mac closed the Screen Sharing control connection.");
                }
                return false;
            }

            const qint64 now = clock.elapsed();
            if (dynamicResolutionResponsePendingAt >= 0 &&
                    now - dynamicResolutionResponsePendingAt >= 3000) {
                qWarning() << "Apple Screen Sharing dynamic resolution timed out waiting for a layout or media canvas response";
                dynamicResolutionResponsePendingAt = -1;
            }
            if (audio != nullptr) {
                audio->process(media.drainControl(),
                               m_Session->m_AudioMuted.load());
            }
            else {
                media.drainControl();
            }
            QByteArray datagram;
            QString receiveError;
            bool primaryReceived = false;
            if (media.receiveVideo(&datagram,
                                   secondaryVideo ? 0 :
                                                    RealtimeMediaPollTimeoutMs,
                                   m_Cancelled,
                                   &receiveError)) {
                primaryReceived = true;
                AppleRtpPacket packet;
                if (decryptor.decrypt(datagram, &packet, nullptr)) {
                    ++receivedPacketCount;
                    ++performancePackets;
                    performanceBytes += static_cast<quint64>(datagram.size());
                    if (!hasPreviousTimestamp) {
                        previousTimestamp = packet.timestamp;
                        hasPreviousTimestamp = true;
                    }
                    else if (packet.timestamp != previousTimestamp) {
                        const quint32 delta = packet.timestamp - previousTimestamp;
                        if (delta < 0x7fffffffU) {
                            previousTimestamp = packet.timestamp;
                            lastAcceptedTimestamp = packet.timestamp;
                            lastAcceptedTimestampAt = now;
                        }
                    }
                    if (firstPacketAt < 0) {
                        firstPacketAt = now;
                    }
                    lastPacketAt = now;
                    AppleHevcAccessUnit accessUnit;
                    const bool completed = assembler.process(packet, now, &accessUnit);
                    if (completed) {
                        ++performanceAccessUnits;
                    }

                    if (sources.isEmpty() && assembler.parameterSets().isComplete()) {
                        const QList<quint32> candidates = assembler.primarySources(
                                activeCanvas.tileCount);
                        const bool candidatesComplete = candidates.size() ==
                                activeCanvas.tileCount &&
                                std::all_of(candidates.cbegin(), candidates.cend(),
                                            [&assembler](quint32 source) {
                            return assembler.completedSources().contains(source);
                        });
                        const bool burstSettled = assembler.totalPacketCount() >= 100 ||
                                (firstPacketAt >= 0 && now - firstPacketAt >= 2300);
                        if (candidatesComplete && burstSettled) {
                            sources = candidates;
                            for (int index = 0; index < sources.size(); ++index) {
                                sourceToTile.insert(sources.at(index), index);
                            }
                            if (!decoder) {
                                decoder = std::make_unique<AppleHevcDecoder>(
                                        m_PreferHardware,
                                        activeCanvas.tileCount);
                                if (!decoder->open(error)) {
                                    return false;
                                }
                            }
                            decoderBackend = decoder->backend();
                            hardwareFallback = decoder->hardwareFallbackOccurred();
                            if (!requestKeyFrames(media, feedback,
                                                  negotiation.offers.videoSynchronizationSource,
                                                  sources, &keyFrameSequence, error)) {
                                return false;
                            }
                            performanceFirs += static_cast<quint64>(sources.size());
                            lastKeyFrameAt = now;
                            awaitingRandomAccessPicture = true;
                        }
                    }

                    if (completed && sourceToTile.contains(
                                accessUnit.synchronizationSource)) {
                        if (awaitingRandomAccessPicture) {
                            if (!accessUnit.containsRandomAccessPicture()) {
                                continue;
                            }
                            // The decoder has no valid reference history until
                            // the first post-FIR random-access picture. Start
                            // DON admission at this clean boundary instead of
                            // feeding stale inter pictures into a new context.
                            decodingOrder.reset();
                            awaitingRandomAccessPicture = false;
                        }
                        const QList<AppleHevcAccessUnit> readyUnits =
                                decodingOrder.enqueue({accessUnit});
                        for (const AppleHevcAccessUnit& ready : readyUnits) {
                            const int tile = sourceToTile.value(
                                    ready.synchronizationSource);
                            frameBatcher.recordSubmission(ready, tile);
                            QList<QList<AppleDecodedTile>> readyBatches =
                                    frameBatcher.takeReadyBatches();
                            for (QList<AppleDecodedTile>& batch : readyBatches) {
                                m_Session->queueDecodedFrames(std::move(batch));
                            }
                            QString decodeError;
                            QElapsedTimer decodeClock;
                            decodeClock.start();
                            ++performanceDecodeCalls;
                            const quint64 decoderGeneration =
                                    decoder->generation();
                            QList<AppleDecodedTile> frames = decoder->decode(
                                    ready, assembler.parameterSets(), tile,
                                    &decodeError);
                            performanceDecodeNanoseconds += decodeClock.nsecsElapsed();
                            performanceDecodedTiles +=
                                    static_cast<quint64>(frames.size());
                            if (decoder->generation() != decoderGeneration) {
                                // FFmpeg has no VideoToolbox-style completion
                                // callback for an accepted sample that later
                                // produces no frame. A codec replacement is
                                // therefore an explicit discontinuity across
                                // the decoder, DON queue, and tile batcher.
                                frames.clear();
                                frameBatcher.reset();
                                assembler.discardIncomplete();
                                decodingOrder.reset();
                                awaitingRandomAccessPicture = true;
                                hasEnteredDecodeRefreshState = true;
                                decoderBackend = decoder->backend();
                                hardwareFallback =
                                        decoder->hardwareFallbackOccurred();
                                if (!requestKeyFrames(
                                            media, feedback,
                                            negotiation.offers.videoSynchronizationSource,
                                            sources, &keyFrameSequence, error)) {
                                    return false;
                                }
                                performanceFirs += static_cast<quint64>(
                                        sources.size());
                                lastKeyFrameAt = now;
                                qWarning().noquote()
                                        << "Apple HEVC decoder reset:"
                                        << decodeError
                                        << "backend="
                                        << appleVideoDecoderBackendName(
                                                   decoderBackend);
                                continue;
                            }
                            for (const AppleDecodedTile& frame : frames) {
                                if (!performanceDecodedSourceTimestamps.contains(
                                            frame.rtpTimestamp)) {
                                    performanceDecodedSourceTimestamps.insert(
                                            frame.rtpTimestamp);
                                    const qint64 sourceFrameAt = clock.elapsed();
                                    if (lastPerformanceSourceTimestamp.has_value() &&
                                            *lastPerformanceSourceTimestamp !=
                                                    frame.rtpTimestamp &&
                                            lastPerformanceSourceFrameAt >= 0) {
                                        performanceSourceFrameIntervals.append(
                                                sourceFrameAt -
                                                lastPerformanceSourceFrameAt);
                                        const quint32 rtpDelta =
                                                frame.rtpTimestamp -
                                                *lastPerformanceSourceTimestamp;
                                        if (rtpDelta < 0x7fffffffU) {
                                            performanceRtpFrameIntervals.append(
                                                    rtpDelta * 1000.0 / 90000.0);
                                        }
                                    }
                                    lastPerformanceSourceTimestamp = frame.rtpTimestamp;
                                    lastPerformanceSourceFrameAt = sourceFrameAt;
                                }
                            }
                            hardwareFallback = decoder->hardwareFallbackOccurred();
                            decoderBackend = decoder->backend();
                            if (!frames.isEmpty()) {
                                lastDecodedAt = clock.elapsed();
                                frameBatcher.recordDecodedFrames(std::move(frames));
                                readyBatches = frameBatcher.takeReadyBatches();
                                for (QList<AppleDecodedTile>& batch : readyBatches) {
                                    m_Session->queueDecodedFrames(std::move(batch));
                                }
                                if (!notifiedMediaReady) {
                                    notifiedMediaReady = true;
                                    m_Session->m_EverMediaReady.store(true);
                                    const QPointer<AppleScreenSharingSession> session = m_Session;
                                    const AppleCanvas canvas = activeCanvas;
                                    const AppleVideoDecoderBackend decoderBackend =
                                            decoder->backend();
                                    const std::shared_ptr<AppleVideoBackendContext>
                                            decoderContext =
                                                    decoder->presentationContext();
                                    QMetaObject::invokeMethod(
                                            session,
                                            [session, canvas, decoderBackend,
                                             hardwareFallback, decoderContext]() {
                                                if (session != nullptr) {
                                                    session->mediaReady(canvas,
                                                                        decoderBackend,
                                                                        hardwareFallback,
                                                                        decoderContext);
                                                }
                                            },
                                            Qt::QueuedConnection);
                                }
                            }
                            else if (!decodeError.isEmpty()) {
                                frameBatcher.recordDecodeFailure(
                                        ready.frameSequenceNumber, tile);
                                readyBatches = frameBatcher.takeReadyBatches();
                                for (QList<AppleDecodedTile>& batch : readyBatches) {
                                    m_Session->queueDecodedFrames(std::move(batch));
                                }
                                if (now - lastKeyFrameAt >=
                                        KeyFrameRetryIntervalMs) {
                                    if (!requestKeyFrames(
                                                media, feedback,
                                                negotiation.offers.videoSynchronizationSource,
                                                sources, &keyFrameSequence, error)) {
                                        return false;
                                    }
                                    performanceFirs += static_cast<quint64>(
                                            sources.size());
                                    // Native AVConference discards buffered
                                    // access units only when first entering
                                    // refresh recovery. It keeps submitting
                                    // later units so a usable reference can
                                    // recover the decoder without a freeze.
                                    if (!hasEnteredDecodeRefreshState) {
                                        hasEnteredDecodeRefreshState = true;
                                        assembler.discardIncomplete();
                                        decodingOrder.reset();
                                    }
                                    lastKeyFrameAt = now;
                                }
                            }
                        }
                    }
                }
            }
            else if (!receiveError.isEmpty()) {
                if (error != nullptr) {
                    *error = receiveError;
                }
                return false;
            }
            bool secondaryReceived = false;
            if (secondaryVideo &&
                    !secondaryVideo->poll(media, m_Cancelled,
                                          &secondaryReceived, error)) {
                return false;
            }
            if (secondaryVideo && !primaryReceived && !secondaryReceived) {
                QThread::usleep(1000);
            }
            if (firstPacketAt < 0 &&
                    now - videoWaitStartedAt >= InitialVideoTimeoutMs) {
                if (error != nullptr) {
                    *error = QCoreApplication::translate(
                            "AppleScreenSharingSession",
                            "The Mac negotiated media but sent no video packets.");
                }
                return false;
            }

            if (!sources.isEmpty()) {
                const auto nacks = assembler.takeNacks(now);
                for (auto iterator = nacks.cbegin(); iterator != nacks.cend(); ++iterator) {
                    const quint32 sender = negotiation.offers.videoSynchronizationSource;
                    const QByteArray nack = AppleMediaWire::receiverReport(sender) +
                            AppleMediaWire::genericNack(
                                    sender, iterator.key(), iterator.value());
                    if (!sendFeedback(media, feedback, nack, error)) {
                        return false;
                    }
                    performanceNacks +=
                            static_cast<quint64>(iterator.value().size());
                }
                if (now - lastReceiverReportAt >= ReceiverReportIntervalMs) {
                    if (!sendFeedback(media, feedback,
                                      AppleMediaWire::receiverReport(
                                              negotiation.offers.videoSynchronizationSource),
                                      error)) {
                        return false;
                    }
                    lastReceiverReportAt = now;
                }
                if (hasPreviousTimestamp &&
                        now - lastRateControlAt >= RateControlIntervalMs) {
                    const quint32 feedbackDelay = lastAcceptedTimestampAt >= 0
                            ? static_cast<quint32>(qMin(
                                      now - lastAcceptedTimestampAt,
                                      static_cast<qint64>(0xffff)))
                            : 0xffff;
                    const quint16 echoTimestamp = static_cast<quint16>(
                            (static_cast<quint64>(now) * 1024 / 1000) & 0xffff);
                    if (!sendFeedback(media, feedback,
                                      AppleMediaWire::rateControl(
                                              negotiation.offers.videoSynchronizationSource,
                                              lastAcceptedTimestamp,
                                              FixedLanBandwidthKilobitsPerSecond,
                                              receivedPacketCount,
                                              feedbackDelay,
                                              echoTimestamp),
                                      error)) {
                        return false;
                    }
                    lastRateControlAt = now;
                }
                const bool awaitingFirstFrame = lastDecodedAt < 0 &&
                        now - lastKeyFrameAt >= KeyFrameRetryIntervalMs;
                const bool stalled = lastDecodedAt >= 0 && lastPacketAt >= 0 &&
                        now - lastDecodedAt >= DecoderStallMs &&
                        now - lastPacketAt < 1500 &&
                        now - lastKeyFrameAt >= DecoderStallMs;
                if (awaitingFirstFrame || stalled) {
                    if (!requestKeyFrames(media, feedback,
                                          negotiation.offers.videoSynchronizationSource,
                                          sources, &keyFrameSequence, error)) {
                        return false;
                    }
                    performanceFirs += static_cast<quint64>(sources.size());
                    decodingOrder.reset();
                    if (awaitingFirstFrame) {
                        assembler.discardIncomplete();
                    }
                    lastKeyFrameAt = now;
                }
            }
            assembler.expire(now);

            const qint64 performanceNow = clock.elapsed();
            if (performanceNow - performanceWindowStartedAt >=
                    PerformanceReportIntervalMs) {
                const double seconds = qMax<qint64>(
                        1, performanceNow - performanceWindowStartedAt) / 1000.0;
                const double sourceFramesPerSecond =
                        performanceDecodedSourceTimestamps.size() / seconds;
                const double averageDecodeMilliseconds = performanceDecodeCalls == 0
                        ? 0.0
                        : performanceDecodeNanoseconds / 1000000.0 /
                          performanceDecodeCalls;
                const IntervalStatistics sourceCadence =
                        calculateIntervalStatistics(
                                performanceSourceFrameIntervals);
                const IntervalStatistics rtpCadence =
                        calculateIntervalStatistics(
                                performanceRtpFrameIntervals);
                const IntervalStatistics controlLatency =
                        calculateIntervalStatistics(
                                performanceControlSendLatencies);
                const quint64 coalescedPointerMotions =
                        m_Session->m_PointerMotionsCoalesced.exchange(
                                0, std::memory_order_relaxed);
                const quint64 maximumControlDepth =
                        m_Session->m_MaxPendingControlDepth.exchange(
                                0, std::memory_order_relaxed);
                const QString backend =
                        appleVideoDecoderBackendName(decoderBackend);
                const QString mediaSummary = QStringLiteral(
                        "SOURCE %1 FPS   RX %2 Mbps   HEVC 4:4:4 %3 tiles/s @ %4 ms   %5\n"
                        "ARRIVAL %6/%7 ms avg/p95   RTP %8/%9 ms avg/p95   JITTER %10 ms   NACK %11   FIR %12\n"
                        "CONTROL SEND %13/%14 ms avg/p95   POINTER MERGED %15/s   QUEUE MAX %16")
                        .arg(sourceFramesPerSecond, 0, 'f', 1)
                        .arg(performanceBytes * 8.0 / seconds / 1000000.0,
                             0, 'f', 1)
                        .arg(performanceDecodedTiles / seconds, 0, 'f', 1)
                        .arg(averageDecodeMilliseconds, 0, 'f', 2)
                        .arg(backend)
                        .arg(sourceCadence.average, 0, 'f', 1)
                        .arg(sourceCadence.percentile95, 0, 'f', 1)
                        .arg(rtpCadence.average, 0, 'f', 1)
                        .arg(rtpCadence.percentile95, 0, 'f', 1)
                        .arg(sourceCadence.jitter, 0, 'f', 1)
                        .arg(performanceNacks)
                        .arg(performanceFirs)
                        .arg(controlLatency.average, 0, 'f', 2)
                        .arg(controlLatency.percentile95, 0, 'f', 2)
                        .arg(coalescedPointerMotions / seconds, 0, 'f', 1)
                        .arg(maximumControlDepth);
                ApplePerformanceOverlayMetrics overlayMetrics;
                overlayMetrics.canvasSize = QSize(activeCanvas.width,
                                                   activeCanvas.height);
                overlayMetrics.receivedFramesPerSecond =
                        sourceFramesPerSecond;
                overlayMetrics.decodedFramesPerSecond =
                        performanceDecodedTiles / seconds /
                        qMax(1, activeCanvas.tileCount);
                overlayMetrics.networkMegabitsPerSecond =
                        performanceBytes * 8.0 / seconds / 1000000.0;
                overlayMetrics.decodeMilliseconds =
                        averageDecodeMilliseconds;
                overlayMetrics.decoderBackend = backend;
                overlayMetrics.hasMediaSample = true;
                if (audio != nullptr) {
                    const AppleAudioStatistics audioStatistics =
                            audio->statistics();
                    const quint64 received = audioStatistics.receivedPackets -
                            previousAudioStatistics.receivedPackets;
                    const quint64 decoded = audioStatistics.decodedPackets -
                            previousAudioStatistics.decodedPackets;
                    const quint64 dropped = audioStatistics.droppedPackets -
                            previousAudioStatistics.droppedPackets;
                    const quint64 decodeAttempts =
                            audioStatistics.decodeAttempts -
                            previousAudioStatistics.decodeAttempts;
                    const quint64 decodeNanoseconds =
                            audioStatistics.decodeNanoseconds -
                            previousAudioStatistics.decodeNanoseconds;
                    const quint64 underflows =
                            audioStatistics.playbackUnderflows -
                            previousAudioStatistics.playbackUnderflows;
                    const double audioDecodeMilliseconds = decodeAttempts == 0
                            ? 0.0
                            : decodeNanoseconds / 1000000.0 / decodeAttempts;
                    const QString audioSummary = QStringLiteral(
                            "AUDIO AAC-ELD %1/%2 pkt/s   DROP %3   DECODE %4 ms   QUEUE %5 ms   XRUN %6%7")
                            .arg(decoded / seconds, 0, 'f', 1)
                            .arg(received / seconds, 0, 'f', 1)
                            .arg(dropped)
                            .arg(audioDecodeMilliseconds, 0, 'f', 2)
                            .arg(audioStatistics.queuedMilliseconds)
                            .arg(underflows)
                            .arg(m_Session->m_AudioMuted.load()
                                 ? QStringLiteral("   MUTED") : QString());
                    previousAudioStatistics = audioStatistics;
                    const QPointer<AppleScreenSharingSession> audioSession =
                            m_Session;
                    QMetaObject::invokeMethod(
                            audioSession,
                            [audioSession, audioSummary]() {
                                if (audioSession != nullptr) {
                                    audioSession->updateAudioStatistics(
                                            audioSummary);
                                }
                            },
                            Qt::QueuedConnection);
                }
                qInfo().nospace()
                        << "Apple High Performance media: rx="
                        << QString::number(performancePackets / seconds, 'f', 1)
                        << " packets/s, "
                        << QString::number(performanceBytes * 8.0 / seconds /
                                           1000000.0, 'f', 1)
                        << " Mbit/s, assembled="
                        << QString::number(performanceAccessUnits / seconds, 'f', 1)
                        << " tiles/s, decoded="
                        << QString::number(performanceDecodedTiles / seconds, 'f', 1)
                        << " tiles/s, source="
                        << QString::number(sourceFramesPerSecond, 'f', 1)
                        << " fps, avg decode="
                        << QString::number(averageDecodeMilliseconds, 'f', 2)
                        << " ms, backend="
                        << appleVideoDecoderBackendName(decoderBackend)
                        << ", source cadence avg/p95/jitter="
                        << QString::number(sourceCadence.average, 'f', 1) << "/"
                        << QString::number(sourceCadence.percentile95, 'f', 1) << "/"
                        << QString::number(sourceCadence.jitter, 'f', 1)
                        << " ms, RTP cadence avg/p95="
                        << QString::number(rtpCadence.average, 'f', 1) << "/"
                        << QString::number(rtpCadence.percentile95, 'f', 1)
                        << " ms, NACK=" << performanceNacks
                        << ", FIR=" << performanceFirs
                        << ", control send avg/p95="
                        << QString::number(controlLatency.average, 'f', 2)
                        << "/"
                        << QString::number(controlLatency.percentile95, 'f', 2)
                        << " ms, pointer merged="
                        << QString::number(
                                   coalescedPointerMotions / seconds, 'f', 1)
                        << "/s, queue max=" << maximumControlDepth;
                const QPointer<AppleScreenSharingSession> session = m_Session;
                if (session != nullptr) {
                    QMetaObject::invokeMethod(
                            session,
                            [session, mediaSummary, overlayMetrics]() {
                                if (session != nullptr) {
                                    session->updatePerformanceStatistics(
                                            mediaSummary, overlayMetrics);
                                }
                            },
                            Qt::QueuedConnection);
                }
                performanceWindowStartedAt = performanceNow;
                performancePackets = 0;
                performanceBytes = 0;
                performanceAccessUnits = 0;
                performanceDecodedTiles = 0;
                performanceDecodeCalls = 0;
                performanceNacks = 0;
                performanceFirs = 0;
                performanceDecodedSourceTimestamps.clear();
                performanceSourceFrameIntervals.clear();
                performanceRtpFrameIntervals.clear();
                performanceControlSendLatencies.clear();
                performanceDecodeNanoseconds = 0;
            }
        }
        return true;
    }

    static bool sendFeedback(AppleMediaTransport& media,
                             AppleSrtcpEncryptor& encryptor,
                             const QByteArray& packet,
                             QString* error)
    {
        const QByteArray protectedPacket = encryptor.protect(packet, error);
        return !protectedPacket.isEmpty() &&
               media.sendVideoControl(protectedPacket, error);
    }

    static bool requestKeyFrames(AppleMediaTransport& media,
                                 AppleSrtcpEncryptor& encryptor,
                                 quint32 sender,
                                 const QList<quint32>& sources,
                                 quint8* sequence,
                                 QString* error)
    {
        const quint8 initialSequence = sequence != nullptr ? *sequence : 0;
        const QList<QByteArray> requests = AppleMediaWire::fullIntraRequests(
                sender, sources, initialSequence);
        for (const QByteArray& request : requests) {
            if (!sendFeedback(media, encryptor, request, error)) {
                return false;
            }
        }
        if (sequence != nullptr) {
            *sequence = static_cast<quint8>(initialSequence + requests.size());
        }
        return true;
    }

    QPointer<AppleScreenSharingSession> m_Session;
    AppleSavedConnection m_Connection;
    std::atomic_bool* m_Cancelled;
    bool m_PreferHardware;
    QList<QSize> m_DisplaySizes;
    int m_DisplayCount;
};

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
        // Keep presentation independent from Qt's main event loop. Native
        // window moves, resizes, and unrelated UI work can temporarily suspend
        // main-thread timers, while Swift's display-link path continues in a
        // common run-loop mode. The one-millisecond poll also bounds a native
        // non-blocking presentation retry without adding a frame of buffering.
        while (!isInterruptionRequested()) {
            m_Session->renderLatestFrames();
            m_Session->renderSecondaryFrames();
            QThread::usleep(1000);
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
    m_KeyboardMapper = std::make_unique<AppleKeyboardMapper>(
            preferences->swapWinAltKeys);
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
    const bool preferHardware = QSettings().value(
            QStringLiteral("appleScreenSharing/preferHardwareDecode"), true).toBool();
    m_WorkerPool.start(
             new AppleHighPerformanceSessionTask(
                     this, m_Connection, &m_Cancelled, preferHardware,
                     m_InitialDisplaySizes));
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

void AppleScreenSharingSession::queueDecodedFrames(QList<AppleDecodedTile> frames,
                                                   int displayIndex)
{
    QMutexLocker locker(&m_FrameMutex);
    bool acceptedBatch = false;
    for (AppleDecodedTile& frame : frames) {
        if (frame.isValid()) {
            if (displayIndex == 1) {
                m_SecondaryLatestFrames.insert(
                        frame.tileIndex, std::move(frame));
            }
            else {
                m_LatestFrames.insert(frame.tileIndex, std::move(frame));
            }
            acceptedBatch = true;
        }
    }
    if (acceptedBatch) {
        if (displayIndex == 1) {
            ++m_SecondaryPendingFrameBatches;
            m_SecondaryPresentationNeeded.store(true);
        }
        else {
            ++m_PendingFrameBatches;
        }
    }
}

QList<AppleOutboundControl> AppleScreenSharingSession::takePendingControls()
{
    QMutexLocker locker(&m_InputMutex);
    QList<AppleOutboundControl> result = std::move(m_PendingControls);
    m_PendingControls.clear();
    return result;
}

#ifdef Q_OS_WIN
void AppleScreenSharingSession::ensureWindowsFileDragLifecycle()
{
    if (m_LocalFileDragLifecycle != nullptr) return;
    m_LocalFileDragLifecycle =
            std::make_shared<AppleLocalFileDragLifecycle>(
                    [this](const AppleFileDragPoint& point) {
                        return remotePoint(
                                point.x,
                                point.y,
                                point.displayIndex).has_value();
                    },
                    [this](const QStringList& paths) {
                        QList<QByteArray> messages;
                        QString error;
                        if (!m_FileTransferService->beginLocalDrop(
                                    paths, &messages, &error)) {
                            qWarning().noquote()
                                    << "Apple local file drag could not start:"
                                    << error;
                            return false;
                        }
                        for (QByteArray& message : messages) {
                            AppleOutboundControl outbound;
                            outbound.kind = AppleOutboundControl::Kind::Message;
                            outbound.message = std::move(message);
                            queueControl(std::move(outbound));
                        }
                        qInfo() << "Apple local file drag entered the remote display";
                        return true;
                    },
                    [this](const AppleFileDragPoint& point,
                           AppleLocalFileDragPointerAction action) {
                        const bool pressed = action !=
                                AppleLocalFileDragPointerAction::Release;
                        if (!remotePoint(
                                    point.x,
                                    point.y,
                                    point.displayIndex).has_value()) {
                            if (!pressed && m_LocalFileDragPointerActive) {
                                queueLocalFileDragPointer(
                                        m_LastLocalFileDragX,
                                        m_LastLocalFileDragY,
                                        m_LastLocalFileDragDisplayIndex,
                                        false,
                                        false);
                                m_LocalFileDragPointerActive = false;
                            }
                            return;
                        }
                        m_LastLocalFileDragX = point.x;
                        m_LastLocalFileDragY = point.y;
                        m_LastLocalFileDragDisplayIndex = point.displayIndex;
                        m_LocalFileDragPointerActive = pressed;
                        m_LastMouseX = point.x;
                        m_LastMouseY = point.y;
                        queueLocalFileDragPointer(
                                point.x,
                                point.y,
                                point.displayIndex,
                                pressed,
                                action ==
                                        AppleLocalFileDragPointerAction::Move);
                    },
                    [this]() {
                        QList<QByteArray> messages;
                        m_FileTransferService->cancelLocalDrop(&messages);
                        for (QByteArray& message : messages) {
                            AppleOutboundControl outbound;
                            outbound.kind = AppleOutboundControl::Kind::Message;
                            outbound.message = std::move(message);
                            queueControl(std::move(outbound));
                        }
                    });
}

void AppleScreenSharingSession::installWindowsFileDropTarget(
        SDL_Window* window,
        int displayIndex)
{
    if (window == nullptr) return;
    ensureWindowsFileDragLifecycle();
    void* const nativeWindow = nativeHandleForWindow(window);
    auto target = std::make_unique<AppleWindowsFileDropTarget>(
            nativeWindow,
            displayIndex,
            m_LocalFileDragLifecycle);
    if (!target->isValid()) {
        qWarning().noquote()
                << "Apple native Windows file drop unavailable:"
                << target->errorString();
        return;
    }
    m_WindowsFileDropTargets.push_back(std::move(target));
    if (m_WindowsRemoteFileDragSource != nullptr) {
        m_WindowsRemoteFileDragSource->addStreamWindow(nativeWindow);
    }
    qInfo() << "Apple native Windows file drop target enabled for display"
            << displayIndex + 1;
}
#endif

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
        m_SecondaryPresentationNeeded.store(true);
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
    updateKeyboardGrabState(window);
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
    qInfo().nospace()
            << "Apple High Performance presentation scheduler="
               "dedicated-high-priority, media-poll-max="
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

void AppleScreenSharingSession::applyCanvas(const AppleCanvas& canvas)
{
    if (!canvas.isUsable()) {
        return;
    }
    {
        QMutexLocker locker(&m_FrameMutex);
        m_Canvas = canvas;
        m_LatestFrames.clear();
        m_TileHeights.clear();
        m_PendingFrameBatches = 0;
        m_AwaitingPresentationBatches = 0;
        m_AwaitingDecodeSubmissions.clear();
    }
    {
        QMutexLocker locker(&m_PerformanceMutex);
        m_PerformanceMetrics.canvasSize = QSize(canvas.width, canvas.height);
    }
    requestPerformanceOverlayUpdate();
    m_PresentationNeeded.store(true);
    qInfo().nospace() << "Apple Screen Sharing canvas="
                      << canvas.width << "x" << canvas.height
                      << " tiles=" << canvas.tileCount;
}

void AppleScreenSharingSession::updatePerformanceStatistics(
        const QString& summary,
        const ApplePerformanceOverlayMetrics& metrics)
{
    {
        QMutexLocker locker(&m_PerformanceMutex);
        m_PerformanceMediaSummary = summary;
        m_PerformanceMetrics.canvasSize = metrics.canvasSize;
        m_PerformanceMetrics.receivedFramesPerSecond =
                metrics.receivedFramesPerSecond;
        m_PerformanceMetrics.decodedFramesPerSecond =
                metrics.decodedFramesPerSecond;
        m_PerformanceMetrics.networkMegabitsPerSecond =
                metrics.networkMegabitsPerSecond;
        m_PerformanceMetrics.decodeMilliseconds =
                metrics.decodeMilliseconds;
        m_PerformanceMetrics.decoderBackend = metrics.decoderBackend;
        m_PerformanceMetrics.hasMediaSample = metrics.hasMediaSample;
    }
    requestPerformanceOverlayUpdate();
}

void AppleScreenSharingSession::updateSecondaryPerformanceStatistics(
        const QString& summary)
{
    {
        QMutexLocker locker(&m_PerformanceMutex);
        m_SecondaryPerformanceSummary = summary;
    }
    requestPerformanceOverlayUpdate();
}

void AppleScreenSharingSession::updateAudioStatistics(const QString& summary)
{
    {
        QMutexLocker locker(&m_PerformanceMutex);
        m_AudioSummary = summary;
    }
    requestPerformanceOverlayUpdate();
}

void AppleScreenSharingSession::requestPerformanceOverlayUpdate()
{
    if (!m_PerformanceOverlayVisible.load()) {
        return;
    }
    m_PerformanceOverlayUpdateNeeded.store(true);
    m_PresentationNeeded.store(true);
}

void AppleScreenSharingSession::togglePerformanceOverlay()
{
    const bool visible = !m_PerformanceOverlayVisible.load();
    m_PerformanceOverlayVisible.store(visible);
    // Clearing and texture upload run on the presentation thread together with
    // the native render call, so the stream hotkey cannot race the immediate
    // context from SDL's GUI-thread event pump.
    m_PerformanceOverlayUpdateNeeded.store(true);
    m_PresentationNeeded.store(true);
    qInfo() << "Apple performance overlay"
            << (visible ? "enabled" : "disabled")
            << "for this stream";
}

void AppleScreenSharingSession::applyControlEvents(
        const AppleControlEvents& events)
{
    for (const AppleCursorUpdate& update : events.cursorUpdates) {
        ++m_RemoteCursorUpdateCount;
        m_RemoteCursorStore.apply(update);
    }
    if (!events.cursorUpdates.isEmpty()) {
        refreshRemoteCursor(cursorWindow(), true);
    }
    for (const AppleDisplayLayout& layout : events.displayLayouts) {
        if (m_MediaDisplayIds.isEmpty()) {
            for (const AppleDisplayRect& display : layout.displays) {
                m_MediaDisplayIds.append(display.id);
            }
        }
        m_DisplayLayout = layout;
        qInfo().nospace()
                << "Apple Screen Sharing display layout: "
                << layout.displays.size() << " display(s), backing="
                << layout.backingWidth << "x" << layout.backingHeight;
    }
    if (!events.cursorUpdates.isEmpty()) {
        updateControlSummary();
    }
}

SDL_Window* AppleScreenSharingSession::cursorWindow() const
{
    SDL_Window* primaryWindow = m_Runtime != nullptr
            ? m_Runtime->streamWindow() : nullptr;
    SDL_Window* focusedWindow = SDL_GetMouseFocus();
    if (focusedWindow != nullptr &&
            (focusedWindow == primaryWindow ||
             focusedWindow == m_SecondaryWindow)) {
        return focusedWindow;
    }
    return primaryWindow != nullptr ? primaryWindow : m_SecondaryWindow;
}

void AppleScreenSharingSession::refreshRemoteCursor(
        SDL_Window* window,
        bool force)
{
    const std::optional<AppleCursorImage> source =
            m_RemoteCursorStore.selectedImage();
    if (!source.has_value()) {
        useDefaultRemoteCursor();
        return;
    }

    const double dpiScale = cursorDpiScale(window);
    if (!force && m_ActiveRemoteCursor != nullptr &&
            qFuzzyCompare(dpiScale, m_ActiveRemoteCursorScale)) {
        return;
    }

    const AppleCursorImage cursorImage = source->scaledForDpi(dpiScale);
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
            const_cast<char*>(cursorImage.rgba.constData()),
            cursorImage.width,
            cursorImage.height,
            32,
            cursorImage.width * 4,
            SDL_PIXELFORMAT_RGBA32);
    SDL_Cursor* cursor = nullptr;
    if (surface != nullptr) {
        cursor = SDL_CreateColorCursor(
                surface,
                cursorImage.hotspotX,
                cursorImage.hotspotY);
        SDL_FreeSurface(surface);
    }
    if (cursor == nullptr) {
        qWarning().nospace()
                << "Failed to create Apple remote cursor: " << SDL_GetError();
        useDefaultRemoteCursor();
        return;
    }

    SDL_Cursor* previous = m_ActiveRemoteCursor;
    SDL_SetCursor(cursor);
    m_ActiveRemoteCursor = cursor;
    m_ActiveRemoteCursorScale = dpiScale;
    SDL_ShowCursor(SDL_ENABLE);
    if (previous != nullptr) {
        SDL_FreeCursor(previous);
    }

    const std::optional<quint32> selectedId = m_RemoteCursorStore.selectedId();
    qInfo().nospace()
            << "Apple remote cursor id="
            << (selectedId.has_value() ? QString::number(*selectedId)
                                       : QStringLiteral("none"))
            << ", points=" << source->width << "x" << source->height
            << ", dpi=" << QString::number(dpiScale, 'f', 2)
            << "x, raster=" << cursorImage.width << "x"
            << cursorImage.height;
}

void AppleScreenSharingSession::useDefaultRemoteCursor()
{
    SDL_Cursor* previous = m_ActiveRemoteCursor;
    m_ActiveRemoteCursor = nullptr;
    m_ActiveRemoteCursorScale = 0.0;
    if (previous == nullptr) {
        return;
    }
    SDL_SetCursor(SDL_GetDefaultCursor());
    SDL_ShowCursor(SDL_ENABLE);
    SDL_FreeCursor(previous);
}

void AppleScreenSharingSession::applyRemoteClipboardText(const QString& text)
{
    if (m_Observing.load()) {
        return;
    }
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr ||
            AppleLocalClipboardTracker::containsFiles(clipboard->mimeData())) {
        qInfo() << "Apple text clipboard preserved a local file clipboard";
        return;
    }
    m_LocalClipboardTracker.expectRemoteText(text);
    clipboard->setText(text);
    qInfo().nospace()
            << "Apple text clipboard received " << text.toUtf8().size()
            << " UTF-8 bytes";
}

void AppleScreenSharingSession::activateRemoteFileDragIfEligible(
        bool pointerInsideStream)
{
    if (m_RemoteFileDragGate == nullptr) return;
    bool leftButtonDown = (m_MouseButtons & 1) != 0;
#ifdef Q_OS_WIN
    if (m_WindowsRemoteFileDragSource != nullptr &&
            m_WindowsRemoteFileDragSource->isValid()) {
        // SDL can lose its button state as soon as the cursor leaves the
        // client area. The async type-32 promise must use physical Windows
        // state and the actual HWND client rectangle instead.
        leftButtonDown = m_WindowsRemoteFileDragSource->leftButtonDown();
        pointerInsideStream =
                m_WindowsRemoteFileDragSource->pointerInsideWindow();
    }
#endif
    const std::optional<AppleRemoteFileDrag> drag =
            m_RemoteFileDragGate->takeIfEligible(
                    leftButtonDown, pointerInsideStream);
    if (!drag.has_value()) return;

    const QPointer<AppleScreenSharingSession> session(this);
    QTimer::singleShot(0, this, [session, drag]() {
        if (session == nullptr || session->m_Cancelled.load()) return;
#ifdef Q_OS_WIN
        if (session->m_WindowsRemoteFileDragSource != nullptr &&
                session->m_WindowsRemoteFileDragSource->isValid()) {
            const QString temporaryRoot = QStandardPaths::writableLocation(
                    QStandardPaths::TempLocation);
            auto staging = std::make_shared<QTemporaryDir>(
                    QDir(temporaryRoot).filePath(
                            QStringLiteral("Moonlight-AppleDrag-XXXXXX")));
            if (!staging->isValid()) {
                const QString error = tr("Couldn’t create temporary storage for the remote drag.");
                qWarning().noquote() << "Apple promised-file drag:" << error;
                session->addLaunchWarning(error);
                return;
            }
            // Explorer may complete the copy asynchronously after DoDragDrop
            // returns, so cleanup is delayed after a successful drop.
            staging->setAutoRemove(false);
            const QString stagingPath = staging->path();
            const std::shared_ptr<AppleFileTransferService> service =
                    session->m_FileTransferService;
            // OLE owns the physical mouse-up once DoDragDrop begins. Transfer
            // ownership before entering its nested message loop.
            const AppleRemoteFileDragInputTransition began =
                    session->m_RemoteFileDragInputState->nativeDragBegan(
                            session->m_MouseButtons);
            session->m_MouseButtons = began.buttons;
            qInfo() << "Apple remote promised-file drag entered Windows Explorer";
            QString startError;
            const bool started = session->m_WindowsRemoteFileDragSource->begin(
                    *drag,
                    [service, drag, staging](
                            const std::atomic_bool& nativeCancelled,
                            QStringList* paths,
                            QString* error) {
                        return service != nullptr &&
                                service->materializeRemoteDrag(
                                        *drag,
                                        staging->path(),
                                        nativeCancelled,
                                        paths,
                                        error);
                    },
                    [session, stagingPath](
                            AppleWindowsRemoteFileDragResult result,
                            const QString& error) {
                        if (session == nullptr) {
                            QDir(stagingPath).removeRecursively();
                            return;
                        }
                        QMetaObject::invokeMethod(
                                session,
                                [session, stagingPath, result, error]() {
                                    if (session == nullptr) {
                                        QDir(stagingPath).removeRecursively();
                                        return;
                                    }
                                    if (result == AppleWindowsRemoteFileDragResult::Failed) {
                                        qWarning().noquote()
                                                << "Apple promised-file drag failed:"
                                                << error;
                                        if (!error.isEmpty()) {
                                            session->addLaunchWarning(error);
                                        }
                                    }
                                    else if (result == AppleWindowsRemoteFileDragResult::Dropped) {
                                        qInfo() << "Apple promised files were dropped through Windows Explorer";
                                    }
                                    const int cleanupDelay =
                                            result == AppleWindowsRemoteFileDragResult::Dropped
                                            ? 10 * 60 * 1000 : 0;
                                    QTimer::singleShot(
                                            cleanupDelay,
                                            QCoreApplication::instance(),
                                            [stagingPath]() {
                                                QDir(stagingPath).removeRecursively();
                                            });
                                },
                                Qt::QueuedConnection);
                    },
                    &startError);
            if (started) {
                const AppleRemoteFileDragInputTransition ended =
                        session->m_RemoteFileDragInputState->nativeDragEnded(
                                session->m_MouseButtons);
                session->m_MouseButtons = ended.buttons;
                return;
            }
            const AppleRemoteFileDragInputTransition failed =
                    session->m_RemoteFileDragInputState->nativeDragStartFailed(
                            session->m_MouseButtons);
            session->m_MouseButtons = failed.buttons;
            QDir(stagingPath).removeRecursively();
            qWarning().noquote()
                    << "Apple promised-file drag could not start:"
                    << startError;
            return;
        }
#endif
        // Keep a portable fallback for platforms without a native promised-
        // file source. Windows normally takes the OLE branch above.
        QSettings settings;
        const QString fallbackDirectory =
                QStandardPaths::writableLocation(
                        QStandardPaths::DownloadLocation);
        const QString initialDirectory = settings.value(
                QStringLiteral("appleScreenSharing/fileTransferDownloadDirectory"),
                fallbackDirectory).toString();
        void* ownerWindow = nullptr;
#ifdef Q_OS_WIN
        ownerWindow = nativeHandleForWindow(
                session->m_Runtime != nullptr
                        ? session->m_Runtime->streamWindow() : nullptr);
#endif
        const QString destination = chooseAppleFileTransferDirectory(
                tr("Save files from %1").arg(session->m_Connection.displayName),
                initialDirectory,
                ownerWindow);
        if (destination.isEmpty()) {
            qInfo() << "Apple remote file drag was declined after leaving the stream";
            return;
        }
        settings.setValue(
                QStringLiteral("appleScreenSharing/fileTransferDownloadDirectory"),
                destination);
        QString error;
        if (!session->m_FileTransferService->acceptRemoteDrag(
                    *drag, destination, &error)) {
            qWarning().noquote()
                    << "Apple remote file transfer could not start:"
                    << error;
            session->addLaunchWarning(error);
        }
    });
}

void AppleScreenSharingSession::applyFileTransferEvents(
        QList<AppleFileTransferEvent> events)
{
    for (const AppleFileTransferEvent& event : std::as_const(events)) {
        if (event.kind == AppleFileTransferEvent::Kind::RemoteDrag) {
            m_RemoteFileDragGate->update(event.remoteDrag);
            if (!event.remoteDrag.sourcePaths.isEmpty()) {
                SDL_Window* window = m_Runtime != nullptr
                        ? m_Runtime->streamWindow() : nullptr;
                int globalX = 0;
                int globalY = 0;
                SDL_GetGlobalMouseState(&globalX, &globalY);
                int windowX = 0;
                int windowY = 0;
                int width = 0;
                int height = 0;
                if (window != nullptr) {
                    SDL_GetWindowPosition(window, &windowX, &windowY);
                    SDL_GetWindowSize(window, &width, &height);
                }
                const bool inside = window != nullptr &&
                        globalX >= windowX && globalY >= windowY &&
                        globalX < windowX + width &&
                        globalY < windowY + height;
                activateRemoteFileDragIfEligible(inside);
            }
            continue;
        }

        const bool incoming = event.direction ==
                AppleFileTransferEvent::Direction::FromRemote;
        if (!m_FileTransferProgressWindow) {
            const QPointer<AppleScreenSharingSession> session(this);
            m_FileTransferProgressWindow =
                    std::make_unique<AppleFileTransferProgressWindow>(
                            [session](quint32 sessionId, bool paused) {
                                if (session != nullptr) {
                                    session->m_FileTransferService->setPaused(
                                            sessionId, paused);
                                }
                            },
                            [session](quint32 sessionId) {
                                if (session != nullptr) {
                                    session->m_FileTransferService->cancel(
                                            sessionId);
                                }
                            });
        }
        if (event.kind == AppleFileTransferEvent::Kind::Started ||
                event.kind == AppleFileTransferEvent::Kind::Progress ||
                event.kind == AppleFileTransferEvent::Kind::Paused) {
            m_ActiveFileTransferSessionId = event.sessionId;
            m_ActiveFileTransferPaused =
                    event.kind == AppleFileTransferEvent::Kind::Paused;
        }
        QString state;
        switch (event.kind) {
        case AppleFileTransferEvent::Kind::Started:
            state = incoming ? tr("receiving") : tr("sending");
            break;
        case AppleFileTransferEvent::Kind::Progress:
            state = tr("%1%2")
                    .arg(qRound(event.progress * 100))
                    .arg(QLatin1Char('%'));
            if (event.bytesPerSecond > 0) {
                state += tr(" · %1 MB/s")
                        .arg(event.bytesPerSecond / 1000000.0, 0, 'f', 1);
            }
            break;
        case AppleFileTransferEvent::Kind::Paused:
            state = tr("paused");
            break;
        case AppleFileTransferEvent::Kind::Completing:
            state = tr("finishing");
            break;
        case AppleFileTransferEvent::Kind::Completed:
            state = tr("completed");
            break;
        case AppleFileTransferEvent::Kind::Failed:
            state = tr("failed");
            break;
        case AppleFileTransferEvent::Kind::Cancelled:
            state = tr("cancelled");
            break;
        case AppleFileTransferEvent::Kind::RemoteDrag:
            break;
        }

        AppleFileTransferProgressEntry progressEntry;
        progressEntry.sessionId = event.sessionId;
        progressEntry.name = event.name;
        progressEntry.remoteName = m_Connection.displayName;
        progressEntry.incoming = incoming;
        progressEntry.path = event.path;
        progressEntry.progress = event.progress;
        progressEntry.bytesPerSecond = event.bytesPerSecond;
        progressEntry.hasProgress =
                event.kind == AppleFileTransferEvent::Kind::Progress ||
                event.kind == AppleFileTransferEvent::Kind::Completed;
        switch (event.kind) {
        case AppleFileTransferEvent::Kind::Started:
            progressEntry.state = incoming
                    ? AppleFileTransferProgressState::Receiving
                    : AppleFileTransferProgressState::Sending;
            break;
        case AppleFileTransferEvent::Kind::Progress:
            progressEntry.state = incoming
                    ? AppleFileTransferProgressState::Receiving
                    : AppleFileTransferProgressState::Sending;
            break;
        case AppleFileTransferEvent::Kind::Paused:
            progressEntry.state = AppleFileTransferProgressState::Paused;
            break;
        case AppleFileTransferEvent::Kind::Completing:
            progressEntry.state = AppleFileTransferProgressState::Completing;
            progressEntry.progress = 1.0;
            progressEntry.hasProgress = true;
            break;
        case AppleFileTransferEvent::Kind::Completed:
            progressEntry.state = AppleFileTransferProgressState::Completed;
            progressEntry.progress = 1.0;
            break;
        case AppleFileTransferEvent::Kind::Failed:
            progressEntry.state = AppleFileTransferProgressState::Failed;
            progressEntry.errorText = event.errorText.isEmpty()
                    ? event.path : event.errorText;
            progressEntry.path.clear();
            break;
        case AppleFileTransferEvent::Kind::Cancelled:
            progressEntry.state = AppleFileTransferProgressState::Cancelled;
            break;
        case AppleFileTransferEvent::Kind::RemoteDrag:
            break;
        }
        m_FileTransferProgressWindow->update(progressEntry);

        const QString summary = tr("FILE %1 %2 · %3")
                .arg(incoming ? QStringLiteral("↓") : QStringLiteral("↑"),
                     event.name,
                     state);
        {
            QMutexLocker locker(&m_PerformanceMutex);
            m_FileTransferSummary = summary;
        }
        qInfo().noquote() << "Apple Screen Sharing" << summary;
        requestPerformanceOverlayUpdate();
        if (event.kind == AppleFileTransferEvent::Kind::Completed ||
                event.kind == AppleFileTransferEvent::Kind::Failed ||
                event.kind == AppleFileTransferEvent::Kind::Cancelled) {
            if (m_ActiveFileTransferSessionId == event.sessionId) {
                m_ActiveFileTransferSessionId = 0;
                m_ActiveFileTransferPaused = false;
            }
            QPointer<AppleScreenSharingSession> guard(this);
            QTimer::singleShot(4000, this, [guard, summary]() {
                if (guard == nullptr) return;
                {
                    QMutexLocker locker(&guard->m_PerformanceMutex);
                    if (guard->m_FileTransferSummary != summary) return;
                    guard->m_FileTransferSummary.clear();
                }
                guard->requestPerformanceOverlayUpdate();
            });
        }
    }
}

void AppleScreenSharingSession::updateControlSummary()
{
    const QString summary = QStringLiteral(
            "MODE %1   CLIPBOARD %2   FILES %3   CURSOR %4   DISPLAY %5%6   O:MODE M:MUTE P:PAUSE X:CANCEL")
            .arg(m_Observing.load() ? QStringLiteral("OBSERVE")
                                    : QStringLiteral("CONTROL"))
            .arg(!m_Observing.load() && m_ControlReady.load()
                         ? QStringLiteral("ON") : QStringLiteral("OFF"))
            .arg(!m_Observing.load() && m_FileTransferSupported.load()
                         ? QStringLiteral("ON") : QStringLiteral("OFF"))
            .arg(m_RemoteCursorUpdateCount)
            .arg(m_DisplayCount)
            .arg(m_DynamicResolutionEnabled
                         ? QStringLiteral(" DYNAMIC") : QString());
    {
        QMutexLocker locker(&m_PerformanceMutex);
        m_ControlSummary = summary;
    }
    if (m_Runtime && m_Runtime->streamWindow() != nullptr) {
        const QString title = tr("%1 — Apple Screen Sharing [%2]")
                .arg(m_Connection.displayName,
                     m_Observing.load() ? tr("Observe") : tr("Control"));
        SDL_SetWindowTitle(m_Runtime->streamWindow(), title.toUtf8().constData());
    }
    requestPerformanceOverlayUpdate();
}

void AppleScreenSharingSession::localClipboardChanged()
{
    refreshLocalClipboard(false);
}

void AppleScreenSharingSession::refreshLocalClipboard(bool windowFocusGained)
{
    const QClipboard* clipboard = QGuiApplication::clipboard();
    const QMimeData* mime = clipboard != nullptr ? clipboard->mimeData() : nullptr;
    if (!m_ControlReady.load() || m_Observing.load()) {
        return;
    }
    const std::optional<QString> text = windowFocusGained
            ? m_LocalClipboardTracker.windowFocusGained(mime)
            : m_LocalClipboardTracker.dataChanged(mime);
    if (!text.has_value()) {
        return;
    }
    AppleOutboundControl outbound;
    outbound.kind = AppleOutboundControl::Kind::LocalClipboardText;
    outbound.text = *text;
    queueControl(std::move(outbound));
    qInfo().nospace()
            << "Apple text clipboard advertised " << text->toUtf8().size()
            << " UTF-8 bytes after "
            << (windowFocusGained ? "stream-window focus" : "local change");
}

void AppleScreenSharingSession::toggleControlMode()
{
    const bool observing = !m_Observing.load();
    if (observing) {
        releaseAllKeys();
    }
    m_Observing.store(observing);
    m_FileTransferService->setControlling(!observing);
    m_MouseButtons = 0;
    AppleOutboundControl outbound;
    outbound.kind = AppleOutboundControl::Kind::SetObserving;
    outbound.observing = observing;
    queueControl(std::move(outbound));
    updateControlSummary();
    if (!observing) {
        localClipboardChanged();
    }
    qInfo() << "Apple Screen Sharing mode changed to"
            << (observing ? "observe" : "control");
}

void AppleScreenSharingSession::toggleAudioMute()
{
    const bool muted = !m_AudioMuted.load();
    m_AudioMuted.store(muted);
    updateControlSummary();
    qInfo() << "Apple Screen Sharing audio"
            << (muted ? "muted" : "unmuted");
}

void AppleScreenSharingSession::scheduleDynamicResolution(
        SDL_Window* window,
        int width,
        int height,
        bool waitsForViewportToSettle)
{
    if (!m_DynamicResolutionEnabled || m_DisplayCount != 1 ||
            m_DynamicResolutionTimer == nullptr) {
        return;
    }
    const QSize size = AppleDynamicResolution::normalizedSizeForDpi(
            width, height, cursorDpiScale(window));
    if (!size.isValid()) {
        return;
    }
    if (size == m_LastRequestedDynamicResolution ||
            (m_Canvas.width == size.width() * 2 &&
             m_Canvas.height == size.height() * 2)) {
        m_PendingDynamicResolution = {};
        m_DynamicResolutionTimer->stop();
        return;
    }
    if (size == m_PendingDynamicResolution &&
            m_DynamicResolutionTimer->isActive()) {
        return;
    }
    m_PendingDynamicResolution = size;
    if (m_LiveResizing) {
        m_DynamicResolutionTimer->stop();
        return;
    }
    const quint64 now = steadyNanoseconds() / 1000000ULL;
    const quint64 earliest = m_LastDynamicResolutionRequestAt == 0
            ? now : m_LastDynamicResolutionRequestAt + 2500;
    const quint64 debounce = waitsForViewportToSettle ? 500 : 0;
    const int interval = static_cast<int>(qMax<quint64>(
            debounce, earliest > now ? earliest - now : 0));
    m_DynamicResolutionTimer->start(interval);
}

void AppleScreenSharingSession::sendPendingDynamicResolution()
{
    if (!m_DynamicResolutionEnabled ||
            !m_PendingDynamicResolution.isValid()) {
        return;
    }
    if (!m_ControlReady.load()) {
        m_DynamicResolutionTimer->start(100);
        return;
    }
    const QSize size = m_PendingDynamicResolution;
    m_PendingDynamicResolution = {};
    const QByteArray message = AppleWire::displayConfiguration({size});
    if (message.isEmpty()) {
        return;
    }
    AppleOutboundControl outbound;
    outbound.kind = AppleOutboundControl::Kind::Message;
    outbound.message = message;
    queueControl(std::move(outbound));
    m_LastRequestedDynamicResolution = size;
    m_LastDynamicResolutionRequestAt = steadyNanoseconds() / 1000000ULL;
    qInfo().nospace() << "Apple Screen Sharing requested dynamic resolution "
                      << size.width() << "x" << size.height();
}

void AppleScreenSharingSession::updatePerformanceOverlayTexture()
{
    if (m_VideoRenderer == nullptr) {
        return;
    }
    if (!m_PerformanceOverlayVisible.load()) {
        m_VideoRenderer->clearOverlay();
        m_PerformanceOverlaySize = {};
        m_PresentationNeeded.store(true);
        return;
    }
    QElapsedTimer updateTimer;
    updateTimer.start();

    QString mediaSummary;
    QString secondaryMediaSummary;
    QString presentationSummary;
    QString controlSummary;
    QString audioSummary;
    QString fileTransferSummary;
    ApplePerformanceOverlayMetrics performanceMetrics;
    {
        QMutexLocker locker(&m_PerformanceMutex);
        mediaSummary = m_PerformanceMediaSummary;
        secondaryMediaSummary = m_SecondaryPerformanceSummary;
        presentationSummary = m_PerformancePresentationSummary;
        controlSummary = m_ControlSummary;
        audioSummary = m_AudioSummary;
        fileTransferSummary = m_FileTransferSummary;
        performanceMetrics = m_PerformanceMetrics;
    }
    if (!performanceMetrics.canvasSize.isValid()) {
        QMutexLocker locker(&m_FrameMutex);
        performanceMetrics.canvasSize = QSize(m_Canvas.width, m_Canvas.height);
    }

    const bool moonlightStyle = m_PerformanceOverlayStyle ==
            ApplePerformanceOverlayStyle::Moonlight;
    QList<ApplePerformanceOverlayTextRun> moonlightRuns;
    QStringList lines;
    if (moonlightStyle) {
        moonlightRuns = appleMoonlightPerformanceRuns(performanceMetrics);
    }
    else {
        if (!controlSummary.isEmpty()) {
            lines.append(controlSummary);
        }
        if (!mediaSummary.isEmpty()) {
            lines.append(mediaSummary.split('\n', Qt::SkipEmptyParts));
        }
        if (!secondaryMediaSummary.isEmpty()) {
            lines.append(secondaryMediaSummary);
        }
        if (!audioSummary.isEmpty()) {
            lines.append(audioSummary);
        }
        if (!fileTransferSummary.isEmpty()) {
            lines.append(fileTransferSummary);
        }
        if (!presentationSummary.isEmpty()) {
            lines.append(presentationSummary.split(
                    '\n', Qt::SkipEmptyParts));
        }
        if (lines.isEmpty()) {
            lines.append(QStringLiteral(
                    "APPLE HIGH PERFORMANCE   Measuring..."));
        }
        while (lines.size() < PerformanceOverlayLineCount) {
            lines.append(QString());
        }
    }

    int outputWidth = 0;
    int outputHeight = 0;
    if (!m_VideoRenderer->outputSize(&outputWidth, &outputHeight) ||
            outputWidth <= 0 || outputHeight <= 0) {
        return;
    }

    QImage image;
    if (moonlightStyle) {
        image = renderMoonlightPerformanceOverlay(
                moonlightRuns, outputWidth);
    }
    else {
        QFont font(QStringLiteral("Consolas"));
        font.setPixelSize(qBound(14, outputHeight / 54, 24));
        font.setStyleHint(QFont::Monospace);
        const QFontMetrics metrics(font);
        const int horizontalPadding = qMax(14, metrics.height());
        const int verticalPadding = qMax(8, metrics.height() / 2);
        const int lineSpacing = qMax(2, metrics.height() / 6);
        int widestLine = 0;
        for (const QString& line : std::as_const(lines)) {
            widestLine = qMax(widestLine,
                              metrics.horizontalAdvance(line));
        }
        const int reservedContentWidth = metrics.horizontalAdvance(
                QString(PerformanceOverlayReservedCharacters,
                        QLatin1Char('M')));
        const int requestedImageWidth =
                qMax(widestLine, reservedContentWidth) +
                horizontalPadding * 2;
        const int imageWidth = qBound(
                1,
                qMax(requestedImageWidth,
                     m_PerformanceOverlaySize.first),
                qMax(1, outputWidth));
        const int lineCount = lines.size();
        const int imageHeight = qMin(
                outputHeight,
                verticalPadding * 2 + metrics.height() * lineCount +
                        lineSpacing * qMax(0, lineCount - 1));
        image = QImage(imageWidth, imageHeight,
                       QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(7, 11, 16, 218));
        painter.drawRoundedRect(image.rect(), 8, 8);
        painter.setBrush(QColor(0, 231, 196, 255));
        painter.drawRoundedRect(QRect(0, 0, 5, imageHeight), 2, 2);
        painter.setFont(font);
        int baseline = verticalPadding + metrics.ascent();
        for (int index = 0; index < lines.size(); ++index) {
            painter.setPen(index == 0
                                   ? QColor(126, 255, 229)
                                   : QColor(242, 246, 250));
            painter.drawText(horizontalPadding, baseline, lines.at(index));
            baseline += metrics.height() + lineSpacing;
        }
        painter.end();
    }
    if (image.isNull()) {
        return;
    }

    QString overlayError;
    if (!m_VideoRenderer->uploadOverlay(image, &overlayError)) {
        qWarning().nospace()
                << "Apple High Performance overlay upload failed: "
                << overlayError;
    }
    else {
        m_PerformanceOverlaySize = {image.width(), image.height()};
        m_PresentationNeeded.store(true);
    }
    m_MaxOverlayUpdateMilliseconds = qMax(
            m_MaxOverlayUpdateMilliseconds,
            updateTimer.nsecsElapsed() / 1000000.0);
}

void AppleScreenSharingSession::pollSdlEvents()
{
    const auto displayIndexForWindow = [this](quint32 windowId) {
        return m_SecondaryWindow != nullptr &&
                SDL_GetWindowID(m_SecondaryWindow) == windowId ? 1 : 0;
    };
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
#ifdef Q_OS_WIN
        bool winKeyDown = false;
        bool rightWinKey = false;
        quint32 winKeyWindowId = 0;
        if (m_WindowsKeyboardHook != nullptr &&
                m_WindowsKeyboardHook->decodeEvent(
                        event, &winKeyDown, &rightWinKey,
                        &winKeyWindowId)) {
            qInfo().nospace()
                    << "[DEBUG-APPLE-WIN-HOOK] "
                    << (winKeyDown ? "down" : "up")
                    << " " << (rightWinKey ? "right" : "left")
                    << " window=" << winKeyWindowId;
            queueKey(
                    winKeyDown,
                    rightWinKey ? SDLK_RGUI : SDLK_LGUI,
                    rightWinKey ? SDL_SCANCODE_RGUI : SDL_SCANCODE_LGUI,
                    rightWinKey ? KMOD_RGUI : KMOD_LGUI,
                    systemKeyCaptureRequestedForWindow(winKeyWindowId));
            continue;
        }
#endif
        switch (event.type) {
        case SDL_QUIT:
            interrupt();
            break;
#if SDL_VERSION_ATLEAST(2, 0, 5)
        case SDL_DROPBEGIN:
            m_PendingLocalDropPaths.clear();
            SDL_GetMouseState(&m_LastMouseX, &m_LastMouseY);
            break;
        case SDL_DROPFILE:
            if (event.drop.file != nullptr) {
                m_PendingLocalDropPaths.append(
                        QString::fromUtf8(event.drop.file));
                SDL_free(event.drop.file);
            }
            break;
        case SDL_DROPCOMPLETE: {
#ifdef Q_OS_WIN
            if (!m_WindowsFileDropTargets.empty()) {
                m_PendingLocalDropPaths.clear();
                break;
            }
#endif
            if (m_PendingLocalDropPaths.isEmpty()) break;
            QList<QByteArray> messages;
            QString error;
            if (!m_FileTransferService->beginLocalDrop(
                        std::exchange(m_PendingLocalDropPaths, {}),
                        &messages,
                        &error)) {
                qWarning().noquote()
                        << "Apple local file transfer could not start:"
                        << error;
                addLaunchWarning(error);
                break;
            }
            for (QByteArray& message : messages) {
                AppleOutboundControl outbound;
                outbound.kind = AppleOutboundControl::Kind::Message;
                outbound.message = std::move(message);
                queueControl(std::move(outbound));
            }
            // SDL publishes the file list only after the native drop has
            // concluded. Mirror Swift's concluded-before-begin-completes path:
            // advertise the drag first, then synthesize the remote press and
            // release in that exact control-queue order so Finder asks for the
            // destination with its type-30 response.
            SDL_GetMouseState(&m_LastMouseX, &m_LastMouseY);
            const int displayIndex = displayIndexForWindow(
                    event.drop.windowID);
            // Finder rejects file-drag pointer frames whose click count is
            // zero. Native Apple viewers retain one through both down and up.
            queuePointer(m_LastMouseX, m_LastMouseY, 1, 1, displayIndex);
            queuePointer(m_LastMouseX, m_LastMouseY, 1, 0, displayIndex);
            qInfo() << "Apple local file drag advertised to the Mac";
            break;
        }
#endif
        case SDL_MOUSEMOTION:
            m_LastMouseX = event.motion.x;
            m_LastMouseY = event.motion.y;
            queuePointer(m_LastMouseX, m_LastMouseY, 0, 0,
                         displayIndexForWindow(event.motion.windowID));
            // Motion events are inside the SDL window. A pending Mac drag must
            // remain inert here so ordinary Finder rearrangement never opens
            // a Windows download dialog.
            activateRemoteFileDragIfEligible(true);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            const quint8 button = appleButtonForSdl(event.button.button);
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (button == 1 && m_RemoteFileDragInputState != nullptr) {
                    const AppleRemoteFileDragInputTransition transition =
                            m_RemoteFileDragInputState->localLeftButtonChanged(
                                    true, m_MouseButtons);
                    m_MouseButtons = transition.buttons;
                }
                else {
                    m_MouseButtons |= button;
                }
            }
            else {
                if (button == 1 && m_RemoteFileDragGate != nullptr &&
                        m_RemoteFileDragGate->hasPending()) {
                    SDL_Window* eventWindow = event.button.windowID == 0
                            ? nullptr
                            : SDL_GetWindowFromID(event.button.windowID);
                    int width = 0;
                    int height = 0;
                    if (eventWindow != nullptr) {
                        SDL_GetWindowSize(eventWindow, &width, &height);
                    }
                    const bool inside = eventWindow != nullptr &&
                            event.button.x >= 0 && event.button.y >= 0 &&
                            event.button.x < width && event.button.y < height;
                    activateRemoteFileDragIfEligible(inside);
                    if (inside) m_RemoteFileDragGate->clear();
                }
                if (button == 1 && m_RemoteFileDragInputState != nullptr) {
                    const AppleRemoteFileDragInputTransition transition =
                            m_RemoteFileDragInputState->localLeftButtonChanged(
                                    false, m_MouseButtons);
                    m_MouseButtons = transition.buttons;
                    if (!transition.forwardToRemote) {
                        m_LastMouseX = event.button.x;
                        m_LastMouseY = event.button.y;
                        break;
                    }
                }
                else {
                    m_MouseButtons &= ~button;
                }
            }
            m_LastMouseX = event.button.x;
            m_LastMouseY = event.button.y;
            queuePointer(m_LastMouseX, m_LastMouseY, event.button.clicks, 0,
                         displayIndexForWindow(event.button.windowID));
            break;
        }
        case SDL_MOUSEWHEEL: {
            const int displayIndex = displayIndexForWindow(
                    event.wheel.windowID);
            m_LastMouseX = event.wheel.mouseX;
            m_LastMouseY = event.wheel.mouseY;
            if (m_NativePrecisionScrollSupported.load()) {
                queueScroll(
                        m_LastMouseX,
                        m_LastMouseY,
                        event.wheel.x,
                        event.wheel.y,
                        event.wheel.preciseX,
                        event.wheel.preciseY,
                        event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED,
                        displayIndex);
                break;
            }
            quint8 wheel = 0;
            if (event.wheel.y > 0) wheel |= 1 << 3;
            if (event.wheel.y < 0) wheel |= 1 << 4;
            if (event.wheel.x > 0) wheel |= 1 << 6;
            if (event.wheel.x < 0) wheel |= 1 << 5;
            if (wheel != 0) {
                queuePointer(m_LastMouseX, m_LastMouseY, 0, wheel,
                             displayIndex);
                queuePointer(m_LastMouseX, m_LastMouseY, 0, 0,
                             displayIndex);
            }
            break;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            const bool commandClipboardKey =
                    event.key.keysym.scancode == SDL_SCANCODE_LGUI ||
                    event.key.keysym.scancode == SDL_SCANCODE_RGUI ||
                    event.key.keysym.scancode == SDL_SCANCODE_C ||
                    event.key.keysym.scancode == SDL_SCANCODE_V;
            if (commandClipboardKey) {
                SDL_Window* keyboardFocus = SDL_GetKeyboardFocus();
                qInfo().nospace()
                        << "[DEBUG-APPLE-CMD-CV-EVENT] "
                        << (event.type == SDL_KEYDOWN ? "down" : "up")
                        << " window=" << event.key.windowID
                        << " focus=" << (keyboardFocus != nullptr
                                ? SDL_GetWindowID(keyboardFocus) : 0)
                        << " sym=" << event.key.keysym.sym
                        << " scan=" << event.key.keysym.scancode
                        << " mod=0x" << Qt::hex
                        << static_cast<quint32>(event.key.keysym.mod)
                        << Qt::dec
                        << " repeat=" << static_cast<int>(event.key.repeat)
                        << " capture="
                        << systemKeyCaptureRequestedForWindow(
                                event.key.windowID);
            }
            if (event.key.keysym.sym == SDLK_s &&
                    (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) ==
                            (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                    togglePerformanceOverlay();
                }
                break;
            }
            if (event.key.keysym.sym == SDLK_o &&
                    (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) ==
                            (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                    toggleControlMode();
                }
                break;
            }
            if (event.key.keysym.sym == SDLK_m &&
                    (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) ==
                            (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                    toggleAudioMute();
                }
                break;
            }
            if (event.key.keysym.sym == SDLK_p &&
                    (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) ==
                            (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
                        m_ActiveFileTransferSessionId != 0 &&
                        m_FileTransferService->setPaused(
                                m_ActiveFileTransferSessionId,
                                !m_ActiveFileTransferPaused)) {
                    m_ActiveFileTransferPaused =
                            !m_ActiveFileTransferPaused;
                }
                break;
            }
            if (event.key.keysym.sym == SDLK_x &&
                    (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) ==
                            (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
                        m_ActiveFileTransferSessionId != 0) {
                    m_FileTransferService->cancel(
                            m_ActiveFileTransferSessionId);
                }
                break;
            }
            if (event.type == SDL_KEYUP || event.key.repeat == 0) {
                queueKey(event.type == SDL_KEYDOWN,
                         event.key.keysym.sym,
                         event.key.keysym.scancode,
                         event.key.keysym.mod,
                         systemKeyCaptureRequestedForWindow(
                                 event.key.windowID));
            }
            break;
        }
        case SDL_WINDOWEVENT: {
            const int displayIndex = displayIndexForWindow(event.window.windowID);
            SDL_Window* changedWindow = displayIndex == 1
                    ? m_SecondaryWindow : m_Runtime->streamWindow();
            if (event.window.event == SDL_WINDOWEVENT_MOVED ||
                    event.window.event == SDL_WINDOWEVENT_RESTORED ||
                    event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                captureWindowGeometry(
                        changedWindow,
                        displayIndex == 1 ? AppleWindowRole::Secondary
                                          : AppleWindowRole::Primary);
            }
            if (event.window.event == SDL_WINDOWEVENT_ENTER ||
                    event.window.event == SDL_WINDOWEVENT_MOVED ||
                    event.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) {
                refreshRemoteCursor(changedWindow, false);
            }
            if (event.window.event == SDL_WINDOWEVENT_LEAVE) {
                activateRemoteFileDragIfEligible(false);
            }
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                updateKeyboardGrabState(changedWindow);
                // Match the native client: copying normally occurs while the
                // stream window is inactive, so becoming key is the reliable
                // point to sample the current pasteboard. QClipboard's native
                // notification can be missed while SDL owns the foreground
                // window and must remain only the fast path.
                refreshLocalClipboard(true);
            }
            else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                // System shortcuts can move focus before SDL delivers their
                // key-up events. Match both native iScreenSharing and the
                // Moonlight input path by releasing the exact remote keys now.
                releaseAllKeys();
                if (m_MouseButtons != 0) {
                    m_MouseButtons = 0;
                    queuePointer(m_LastMouseX, m_LastMouseY, 0, 0,
                                 displayIndex);
                }
            }
            switch (event.window.event) {
            case SDL_WINDOWEVENT_EXPOSED:
            case SDL_WINDOWEVENT_SHOWN:
            case SDL_WINDOWEVENT_RESTORED:
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                updateKeyboardGrabState(changedWindow);
                m_PresentationNeeded.store(true);
                if (displayIndex == 1) {
                    m_SecondaryPresentationNeeded.store(true);
                }
                else if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    scheduleDynamicResolution(
                            changedWindow,
                            event.window.data1,
                            event.window.data2);
                }
                break;
            default:
                break;
            }
            break;
        }
        default:
            break;
        }
    }
}

void AppleScreenSharingSession::queueScroll(
        int windowX,
        int windowY,
        qint32 deltaX,
        qint32 deltaY,
        double preciseDeltaX,
        double preciseDeltaY,
        bool flipped,
        int displayIndex)
{
    if (m_Observing.load()) {
        return;
    }
    const auto point = remotePoint(windowX, windowY, displayIndex);
    if (!point.has_value()) {
        return;
    }
    std::optional<quint32> displayId;
    if (m_DisplayCount > 1 &&
            displayIndex >= 0 && displayIndex < m_MediaDisplayIds.size()) {
        displayId = m_MediaDisplayIds.at(displayIndex);
    }

    QMutexLocker locker(&m_InputMutex);
    if (displayId != m_SelectedInputDisplayId) {
        AppleOutboundControl selection;
        selection.kind = AppleOutboundControl::Kind::Message;
        selection.queuedAtNanoseconds = steadyNanoseconds();
        selection.message = displayId.has_value()
                ? AppleMediaWire::selectDisplay(*displayId)
                : AppleMediaWire::selectCombinedDisplays();
        m_PendingControls.append(std::move(selection));
        m_SelectedInputDisplayId = displayId;
    }
    AppleOutboundControl input;
    input.kind = AppleOutboundControl::Kind::Message;
    input.queuedAtNanoseconds = steadyNanoseconds();
    input.message = AppleMediaWire::scrollWheelEvent(
            AppleMediaWire::scrollWheelDeltas(
                    deltaX,
                    deltaY,
                    preciseDeltaX,
                    preciseDeltaY,
                    flipped,
                    ++m_ScrollEventCount,
                    m_ScrollSpeedMultiplier),
            point->first,
            point->second);
    m_PendingControls.append(std::move(input));
    recordMaximum(m_MaxPendingControlDepth,
                  static_cast<quint64>(m_PendingControls.size()));
}

void AppleScreenSharingSession::queuePointer(
        int windowX,
        int windowY,
        int clickCount,
        quint8 extraButtons,
        int displayIndex)
{
    if (m_Observing.load()) {
        return;
    }
    queuePointerFrame(
            windowX,
            windowY,
            m_MouseButtons | extraButtons,
            clickCount,
            displayIndex,
            clickCount == 0 && extraButtons == 0
                    ? AppleOutboundControl::Coalescing::PointerMotion
                    : AppleOutboundControl::Coalescing::None);
}

void AppleScreenSharingSession::queueLocalFileDragPointer(
        int windowX,
        int windowY,
        int displayIndex,
        bool pressed,
        bool moving)
{
    if (m_Observing.load()) return;
    queuePointerFrame(
            windowX,
            windowY,
            pressed ? static_cast<quint8>(1) : static_cast<quint8>(0),
            1,
            displayIndex,
            moving ? AppleOutboundControl::Coalescing::FileDragMotion
                   : AppleOutboundControl::Coalescing::None);
}

void AppleScreenSharingSession::queuePointerFrame(
        int windowX,
        int windowY,
        quint8 buttons,
        int clickCount,
        int displayIndex,
        AppleOutboundControl::Coalescing coalescing)
{
    const auto point = remotePoint(windowX, windowY, displayIndex);
    if (!point.has_value()) {
        return;
    }
    const quint32 now = currentMicroseconds();
    const quint32 delta = now - m_PreviousInputTimestamp;
    m_PreviousInputTimestamp = now;

    std::optional<quint32> displayId;
    if (m_DisplayCount > 1 &&
            displayIndex >= 0 && displayIndex < m_MediaDisplayIds.size()) {
        displayId = m_MediaDisplayIds.at(displayIndex);
    }

    QMutexLocker locker(&m_InputMutex);
    if (displayId != m_SelectedInputDisplayId) {
        AppleOutboundControl selection;
        selection.kind = AppleOutboundControl::Kind::Message;
        selection.queuedAtNanoseconds = steadyNanoseconds();
        selection.message = displayId.has_value()
                ? AppleMediaWire::selectDisplay(*displayId)
                : AppleMediaWire::selectCombinedDisplays();
        m_PendingControls.append(std::move(selection));
        m_SelectedInputDisplayId = displayId;
    }
    AppleOutboundControl input;
    input.kind = AppleOutboundControl::Kind::Input;
    input.queuedAtNanoseconds = steadyNanoseconds();
    input.timestampDeltaMicroseconds = delta;
    input.coalescing = coalescing;
    input.input = AppleMediaWire::pointerEvent(
            buttons,
            point->first,
            point->second,
            clickCount,
            delta);
    if (input.coalescing != AppleOutboundControl::Coalescing::None &&
            !m_PendingControls.isEmpty() &&
            m_PendingControls.last().coalescing == input.coalescing) {
        const quint32 accumulatedDelta =
                m_PendingControls.last().timestampDeltaMicroseconds + delta;
        input.timestampDeltaMicroseconds = accumulatedDelta;
        input.input = AppleMediaWire::pointerEvent(
                buttons,
                point->first,
                point->second,
                clickCount,
                accumulatedDelta);
        m_PendingControls.last() = std::move(input);
        m_PointerMotionsCoalesced.fetch_add(1, std::memory_order_relaxed);
    }
    else {
        m_PendingControls.append(std::move(input));
    }
    recordMaximum(m_MaxPendingControlDepth,
                  static_cast<quint64>(m_PendingControls.size()));
}

void AppleScreenSharingSession::queueKey(
        bool isDown,
        int sdlKeycode,
        int sdlScancode,
        int sdlModifiers,
        bool systemKeyCaptureRequested)
{
    if (m_Observing.load() || m_KeyboardMapper == nullptr) {
        return;
    }
    int platformGuiModifiers = KMOD_NONE;
#ifdef Q_OS_WIN
    const bool leftWinDown =
            (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0;
    const bool rightWinDown =
            (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
    if (leftWinDown) {
        platformGuiModifiers |= KMOD_LGUI;
    }
    if (rightWinDown) {
        platformGuiModifiers |= KMOD_RGUI;
    }
    if (sdlScancode == SDL_SCANCODE_C ||
            sdlScancode == SDL_SCANCODE_V ||
            platformGuiModifiers != KMOD_NONE) {
        qInfo().nospace()
                << "[DEBUG-APPLE-WIN-GUI-STATE] scan=" << sdlScancode
                << " sdl=0x" << Qt::hex << sdlModifiers
                << " native=0x" << platformGuiModifiers << Qt::dec
                << " capture=" << systemKeyCaptureRequested;
    }
#endif
    const QList<AppleRemoteKeyEvent> keys =
            m_KeyboardMapper->updateWithModifiers(
            isDown,
            sdlKeycode,
            sdlScancode,
            sdlModifiers,
            platformGuiModifiers,
            systemKeyCaptureRequested);
    for (const AppleRemoteKeyEvent& key : keys) {
        queueRemoteKey(key);
    }
}

void AppleScreenSharingSession::queueRemoteKey(
        const AppleRemoteKeyEvent& key)
{
    if (key.symbol == 0xffeb || key.symbol == 0xffec ||
            key.symbol == 'c' || key.symbol == 'v') {
        qInfo().nospace()
                << "[DEBUG-APPLE-CMD-CV-WIRE] "
                << (key.isDown ? "down" : "up")
                << " symbol=0x" << Qt::hex << key.symbol << Qt::dec
                << " keyCode=" << key.keyCode
                << " keyboardType=" << key.keyboardType;
    }
    const quint32 now = currentMicroseconds();
    const quint32 delta = now - m_PreviousInputTimestamp;
    m_PreviousInputTimestamp = now;
    AppleOutboundControl outbound;
    outbound.kind = AppleOutboundControl::Kind::Input;
    outbound.timestampDeltaMicroseconds = delta;
    outbound.input = AppleMediaWire::keyEvent(
                key.isDown,
                key.symbol,
                delta,
                key.keyboardType,
                key.keyCode);
    queueControl(std::move(outbound));
}

void AppleScreenSharingSession::releaseAllKeys()
{
    if (m_KeyboardMapper == nullptr) {
        return;
    }
    const QList<AppleRemoteKeyEvent> releases =
            m_KeyboardMapper->releaseAll();
    if (releases.isEmpty()) {
        return;
    }
    qInfo() << "Apple Screen Sharing releasing"
            << releases.size() << "pressed remote key(s)";
    if (m_Observing.load()) {
        return;
    }
    for (const AppleRemoteKeyEvent& key : releases) {
        queueRemoteKey(key);
    }
}

void AppleScreenSharingSession::updateKeyboardGrabState(SDL_Window* window)
{
    if (window == nullptr) {
        return;
    }
    bool shouldGrab = m_CaptureSystemKeysMode !=
            static_cast<int>(StreamingPreferences::CSK_OFF);
    if (shouldGrab && m_CaptureSystemKeysMode ==
            static_cast<int>(StreamingPreferences::CSK_FULLSCREEN) &&
            (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0) {
        shouldGrab = false;
    }

    bool* active = nullptr;
    const bool isPrimary = m_Runtime != nullptr &&
            window == m_Runtime->streamWindow();
    if (isPrimary) {
        active = &m_PrimaryKeyboardGrabActive;
    }
    else if (window == m_SecondaryWindow) {
        active = &m_SecondaryKeyboardGrabActive;
    }
    if (active == nullptr) {
        return;
    }

    const bool wasActive = *active;
#if SDL_VERSION_ATLEAST(2, 0, 15)
    // Reapply on focus gain because some SDL platform adapters ignore a grab
    // requested while the stream window is still hidden.
    SDL_SetWindowKeyboardGrab(window, shouldGrab ? SDL_TRUE : SDL_FALSE);
#else
    shouldGrab = false;
#endif
    *active = shouldGrab;
#ifdef Q_OS_WIN
    if (m_WindowsKeyboardHook != nullptr) {
        m_WindowsKeyboardHook->update(
                m_Runtime != nullptr ? m_Runtime->streamWindow() : nullptr,
                m_PrimaryKeyboardGrabActive,
                m_SecondaryWindow,
                m_SecondaryKeyboardGrabActive);
    }
#endif
    if (wasActive == shouldGrab) {
        return;
    }
    qInfo().nospace()
            << "Apple Screen Sharing system-key capture "
            << (shouldGrab ? "enabled" : "disabled")
            << " for " << (isPrimary ? "primary" : "secondary")
            << " window";
}

bool AppleScreenSharingSession::systemKeyCaptureRequestedForWindow(
        quint32 windowId) const
{
    SDL_Window* primary = m_Runtime != nullptr
            ? m_Runtime->streamWindow() : nullptr;
    if (primary != nullptr && SDL_GetWindowID(primary) == windowId) {
        // A Win key can move Windows focus before the queued SDL event is
        // consumed. The event's window ID plus our requested grab is the stable
        // ownership signal; focus loss separately releases every remote key.
        return m_PrimaryKeyboardGrabActive;
    }
    if (m_SecondaryWindow != nullptr &&
            SDL_GetWindowID(m_SecondaryWindow) == windowId) {
        return m_SecondaryKeyboardGrabActive;
    }
    return false;
}

void AppleScreenSharingSession::queueControl(AppleOutboundControl control)
{
    if (control.queuedAtNanoseconds == 0) {
        control.queuedAtNanoseconds = steadyNanoseconds();
    }
    QMutexLocker locker(&m_InputMutex);
    m_PendingControls.append(std::move(control));
    recordMaximum(m_MaxPendingControlDepth,
                  static_cast<quint64>(m_PendingControls.size()));
}

std::optional<QPair<quint16, quint16>> AppleScreenSharingSession::remotePoint(
        int windowX,
        int windowY,
        int displayIndex) const
{
    const AppleCanvas canvas = displayIndex == 1
            ? m_SecondaryCanvas : m_Canvas;
    SDL_Window* window = displayIndex == 1
            ? m_SecondaryWindow : m_Runtime->streamWindow();
    if (!canvas.isUsable() || window == nullptr) {
        return std::nullopt;
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    const double scale = qMin(static_cast<double>(width) / canvas.width,
                              static_cast<double>(height) / canvas.height);
    const double contentWidth = canvas.width * scale;
    const double contentHeight = canvas.height * scale;
    const double left = (width - contentWidth) / 2.0;
    const double top = (height - contentHeight) / 2.0;
    if (windowX < left || windowY < top ||
            windowX >= left + contentWidth || windowY >= top + contentHeight) {
        return std::nullopt;
    }
    const int x = qBound(0, static_cast<int>((windowX - left) / scale),
                         canvas.width - 1);
    const int y = qBound(0, static_cast<int>((windowY - top) / scale),
                         canvas.height - 1);
    return QPair<quint16, quint16>(
            static_cast<quint16>(qBound(0, x, 65535)),
            static_cast<quint16>(qBound(0, y, 65535)));
}

void AppleScreenSharingSession::renderLatestFrames()
{
    if (m_VideoRenderer == nullptr) {
        return;
    }
    const quint64 renderLoopAtNanoseconds = steadyNanoseconds();
    if (m_LastRenderLoopAtNanoseconds != 0 &&
            renderLoopAtNanoseconds >= m_LastRenderLoopAtNanoseconds) {
        m_MaxRenderLoopGapMilliseconds = qMax(
                m_MaxRenderLoopGapMilliseconds,
                (renderLoopAtNanoseconds - m_LastRenderLoopAtNanoseconds) /
                        1000000.0);
    }
    m_LastRenderLoopAtNanoseconds = renderLoopAtNanoseconds;
    if (m_PerformanceOverlayUpdateNeeded.exchange(false)) {
        updatePerformanceOverlayTexture();
    }
    QHash<int, AppleDecodedTile> frames;
    quint64 pendingFrameBatches = 0;
    AppleCanvas canvas;
    {
        QMutexLocker locker(&m_FrameMutex);
        canvas = m_Canvas;
        frames = std::move(m_LatestFrames);
        m_LatestFrames.clear();
        pendingFrameBatches = m_PendingFrameBatches;
        m_PendingFrameBatches = 0;
    }
    if (!canvas.isUsable()) {
        return;
    }
    if (pendingFrameBatches > 0) {
        m_AwaitingPresentationBatches += pendingFrameBatches;
    }
    for (auto iterator = frames.begin(); iterator != frames.end(); ++iterator) {
        AppleDecodedTile& frame = iterator.value();
        if (frame.isValid()) {
            QString uploadError;
            if (!m_VideoRenderer ||
                    !m_VideoRenderer->upload(frame, &uploadError)) {
                qWarning().nospace()
                        << "Apple High Performance tile upload failed: "
                        << uploadError;
                continue;
            }
            m_TextureSizes.insert(
                    frame.tileIndex, {frame.width, frame.height});
            m_TextureFormats.insert(
                    frame.tileIndex, static_cast<quint32>(
                            frame.pixelFormat));
            m_TileHeights.insert(frame.tileIndex, frame.height);
            m_AwaitingDecodeSubmissions.insert(
                    frame.tileIndex, frame.decodeSubmittedAtNanoseconds);
            m_PresentationNeeded.store(true);
            continue;
        }
        qWarning().nospace()
                << "Apple High Performance ignored non-4:4:4 decoded tile "
                << frame.tileIndex;
    }

    // Clear the request before rendering so a frame or window event arriving
    // during the render remains set for the next iteration.
    if (!m_PresentationNeeded.exchange(false)) {
        return;
    }

    const int fallbackHeight = (canvas.height + canvas.tileCount - 1) /
            canvas.tileCount;
    QList<int> tileHeights;
    tileHeights.reserve(canvas.tileCount);
    for (int tile = 0; tile < canvas.tileCount; ++tile) {
        tileHeights.append(m_TileHeights.value(tile, fallbackHeight));
    }
    QString renderError;
    QElapsedTimer renderTimer;
    renderTimer.start();
    const AppleVideoRenderer::RenderResult renderResult =
            m_VideoRenderer->render(
                    canvas,
                    tileHeights,
                    &renderError);
    m_RenderCallDurations.append(renderTimer.nsecsElapsed() / 1000000.0);
    if (renderResult == AppleVideoRenderer::RenderResult::Busy) {
        ++m_PresentationBusyCount;
        m_PresentationNeeded.store(true);
        return;
    }
    if (renderResult == AppleVideoRenderer::RenderResult::Failed) {
        qWarning().nospace()
                << "Apple High Performance 4:4:4 render failed: "
                << renderError;
        m_AwaitingPresentationBatches = 0;
        m_AwaitingDecodeSubmissions.clear();
        return;
    }
    ++m_PresentationCount;
    m_PresentedTileUpdates += static_cast<quint64>(
            m_AwaitingDecodeSubmissions.size());
    const quint64 presentationNow = SDL_GetTicks64();
    if (m_AwaitingPresentationBatches > 0) {
        ++m_DisplayedFrameBatches;
        m_DroppedFrameBatches += m_AwaitingPresentationBatches - 1;
        if (m_LastDisplayedFrameAt != 0 &&
                presentationNow >= m_LastDisplayedFrameAt) {
            m_DisplayFrameIntervals.append(
                    static_cast<double>(presentationNow -
                                        m_LastDisplayedFrameAt));
        }
        m_LastDisplayedFrameAt = presentationNow;
        const quint64 displayedAtNanoseconds = steadyNanoseconds();
        for (quint64 decodeSubmittedAtNanoseconds :
             std::as_const(m_AwaitingDecodeSubmissions)) {
            if (decodeSubmittedAtNanoseconds != 0 &&
                    displayedAtNanoseconds >=
                            decodeSubmittedAtNanoseconds) {
                m_SubmitToDisplayLatencies.append(
                        (displayedAtNanoseconds -
                         decodeSubmittedAtNanoseconds) / 1000000.0);
            }
        }
    }
    m_AwaitingPresentationBatches = 0;
    m_AwaitingDecodeSubmissions.clear();
    if (m_PresentationWindowStartedAt == 0) {
        m_PresentationWindowStartedAt = presentationNow;
    }
    else if (presentationNow - m_PresentationWindowStartedAt >=
             static_cast<quint64>(PerformanceReportIntervalMs)) {
        const double seconds = qMax<quint64>(
                1, presentationNow - m_PresentationWindowStartedAt) / 1000.0;
        const IntervalStatistics displayCadence =
                calculateIntervalStatistics(m_DisplayFrameIntervals);
        const IntervalStatistics submitToDisplay =
                calculateIntervalStatistics(m_SubmitToDisplayLatencies);
        const IntervalStatistics renderCalls =
                calculateIntervalStatistics(m_RenderCallDurations);
        const QString presentationSummary = QStringLiteral(
                "DISPLAY %1 FPS   VSYNC %2 Hz   TILE UPDATES %3/s   COALESCED %4\n"
                "FRAME TIME %5 ms avg   %6 p95   JITTER %7 ms\n"
                        "DECODE TO PRESENT %8 ms avg   %9 p95\n"
                "PRESENT CALL %10 ms avg   %11 p95   BUSY %12")
                .arg(m_DisplayedFrameBatches / seconds, 0, 'f', 1)
                .arg(m_PresentationCount / seconds, 0, 'f', 1)
                .arg(m_PresentedTileUpdates / seconds, 0, 'f', 1)
                .arg(m_DroppedFrameBatches)
                .arg(displayCadence.average, 0, 'f', 1)
                .arg(displayCadence.percentile95, 0, 'f', 1)
                .arg(displayCadence.jitter, 0, 'f', 1)
                .arg(submitToDisplay.average, 0, 'f', 1)
                .arg(submitToDisplay.percentile95, 0, 'f', 1)
                .arg(renderCalls.average, 0, 'f', 2)
                .arg(renderCalls.percentile95, 0, 'f', 2)
                .arg(m_PresentationBusyCount);
        {
            QMutexLocker locker(&m_PerformanceMutex);
            m_PerformancePresentationSummary = presentationSummary;
            m_PerformanceMetrics.presentedFramesPerSecond =
                    m_DisplayedFrameBatches / seconds;
            m_PerformanceMetrics.renderMilliseconds = renderCalls.average;
            m_PerformanceMetrics.hasPresentationSample = true;
        }
        qInfo().nospace()
                << "Apple High Performance presentation: "
                << QString::number(m_PresentationCount / seconds, 'f', 1)
                << " presents/s, "
                << QString::number(m_PresentedTileUpdates / seconds, 'f', 1)
                << " tile updates/s, displayed="
                << QString::number(m_DisplayedFrameBatches / seconds, 'f', 1)
                << " fps, coalesced=" << m_DroppedFrameBatches
                << ", frame interval avg/p95/jitter="
                << QString::number(displayCadence.average, 'f', 1) << "/"
                << QString::number(displayCadence.percentile95, 'f', 1) << "/"
                << QString::number(displayCadence.jitter, 'f', 1)
                << " ms, decode-to-present avg/p95="
                << QString::number(submitToDisplay.average, 'f', 1) << "/"
                << QString::number(submitToDisplay.percentile95, 'f', 1)
                << " ms, present-call avg/p95="
                << QString::number(renderCalls.average, 'f', 2) << "/"
                << QString::number(renderCalls.percentile95, 'f', 2)
                << " ms, busy=" << m_PresentationBusyCount
                << ", render-loop max="
                << QString::number(m_MaxRenderLoopGapMilliseconds, 'f', 2)
                << " ms, overlay-update max="
                << QString::number(m_MaxOverlayUpdateMilliseconds, 'f', 2)
                << " ms";
        m_PresentationWindowStartedAt = presentationNow;
        m_PresentationCount = 0;
        m_PresentedTileUpdates = 0;
        m_DisplayedFrameBatches = 0;
        m_DroppedFrameBatches = 0;
        m_PresentationBusyCount = 0;
        m_DisplayFrameIntervals.clear();
        m_SubmitToDisplayLatencies.clear();
        m_RenderCallDurations.clear();
        m_MaxRenderLoopGapMilliseconds = 0.0;
        m_MaxOverlayUpdateMilliseconds = 0.0;
        requestPerformanceOverlayUpdate();
    }
}

void AppleScreenSharingSession::destroyPresentation()
{
#ifdef Q_OS_WIN
    m_WindowsRemoteFileDragSource.reset();
    m_WindowsFileDropTargets.clear();
    m_LocalFileDragLifecycle.reset();
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
    if (m_PresentationThread != nullptr) {
        m_PresentationThread->requestInterruption();
        m_PresentationThread->wait();
        m_PresentationThread.reset();
    }
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
    m_LastRenderLoopAtNanoseconds = 0;
    m_MaxRenderLoopGapMilliseconds = 0.0;
    m_MaxOverlayUpdateMilliseconds = 0.0;
    persistWindowGeometry(m_Runtime ? m_Runtime->streamWindow() : nullptr,
                          AppleWindowRole::Primary);
    persistWindowGeometry(m_SecondaryWindow, AppleWindowRole::Secondary);
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

void AppleScreenSharingSession::renderSecondaryFrames()
{
    if (m_SecondaryVideoRenderer == nullptr) {
        return;
    }
    QHash<int, AppleDecodedTile> frames;
    AppleCanvas canvas;
    {
        QMutexLocker locker(&m_FrameMutex);
        canvas = m_SecondaryCanvas;
        frames = std::move(m_SecondaryLatestFrames);
        m_SecondaryLatestFrames.clear();
        m_SecondaryPendingFrameBatches = 0;
    }
    if (!canvas.isUsable()) {
        return;
    }
    for (auto iterator = frames.begin(); iterator != frames.end(); ++iterator) {
        AppleDecodedTile& frame = iterator.value();
        if (!frame.isValid()) {
            continue;
        }
        QString uploadError;
        if (m_SecondaryVideoRenderer->upload(frame, &uploadError)) {
            m_SecondaryTileHeights.insert(frame.tileIndex, frame.height);
            m_SecondaryPresentationNeeded.store(true);
        }
        else {
            qWarning().nospace()
                    << "Apple High Performance display 2 upload failed: "
                    << uploadError;
        }
    }
    if (!m_SecondaryPresentationNeeded.exchange(false)) {
        return;
    }
    const int fallbackHeight = (canvas.height + canvas.tileCount - 1) /
            canvas.tileCount;
    QList<int> tileHeights;
    tileHeights.reserve(canvas.tileCount);
    for (int tile = 0; tile < canvas.tileCount; ++tile) {
        tileHeights.append(m_SecondaryTileHeights.value(
                tile, fallbackHeight));
    }
    QString renderError;
    const AppleVideoRenderer::RenderResult result =
            m_SecondaryVideoRenderer->render(
                    canvas,
                    tileHeights,
                    &renderError);
    if (result == AppleVideoRenderer::RenderResult::Busy) {
        m_SecondaryPresentationNeeded.store(true);
    }
    else if (result == AppleVideoRenderer::RenderResult::Failed) {
        qWarning().nospace()
                << "Apple High Performance display 2 render failed: "
                << renderError;
    }
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
