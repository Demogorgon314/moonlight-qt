#include "applescreensharingsession.h"

#include "appleauthenticator.h"
#include "applecredentialstore.h"
#include "applemediatransport.h"
#include "streaming/localstreamruntime.h"

#include "SDL.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QMetaObject>
#include <QMutexLocker>
#include <QPainter>
#include <QPointer>
#include <QQuickWindow>
#include <QRunnable>
#include <QSettings>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <cmath>
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
constexpr quint32 FixedLanBandwidthKilobitsPerSecond = 60001;

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

quint32 keySymbolForSdl(int keycode)
{
    if (keycode >= 0x20 && keycode <= 0xff) {
        return static_cast<quint32>(keycode);
    }
    switch (keycode) {
    case SDLK_BACKSPACE: return 0xff08;
    case SDLK_TAB: return 0xff09;
    case SDLK_RETURN: return 0xff0d;
    case SDLK_ESCAPE: return 0xff1b;
    case SDLK_HOME: return 0xff50;
    case SDLK_LEFT: return 0xff51;
    case SDLK_UP: return 0xff52;
    case SDLK_RIGHT: return 0xff53;
    case SDLK_DOWN: return 0xff54;
    case SDLK_PAGEUP: return 0xff55;
    case SDLK_PAGEDOWN: return 0xff56;
    case SDLK_END: return 0xff57;
    case SDLK_INSERT: return 0xff63;
    case SDLK_DELETE: return 0xffff;
    case SDLK_LSHIFT: return 0xffe1;
    case SDLK_RSHIFT: return 0xffe2;
    case SDLK_LCTRL: return 0xffe3;
    case SDLK_RCTRL: return 0xffe4;
    case SDLK_LALT: return 0xffe9;
    case SDLK_RALT: return 0xffea;
    case SDLK_LGUI: return 0xffeb;
    case SDLK_RGUI: return 0xffec;
    default:
        if (keycode >= SDLK_F1 && keycode <= SDLK_F24) {
            return 0xffbe + static_cast<quint32>(keycode - SDLK_F1);
        }
        return 0;
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

} // namespace

class AppleHighPerformanceSessionTask final : public QRunnable
{
public:
    AppleHighPerformanceSessionTask(AppleScreenSharingSession* session,
                                    AppleSavedConnection connection,
                                    std::atomic_bool* cancelled,
                                    bool preferHardware)
        : m_Session(session),
          m_Connection(std::move(connection)),
          m_Cancelled(cancelled),
          m_PreferHardware(preferHardware)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        QString error;
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
                &error);

        AppleControlChannel control;
        if (succeeded) {
            succeeded = control.negotiate(
                    tcp, authenticated.masterKey, m_Cancelled, &error);
        }

        AppleMediaTransport media;
        AppleMediaNegotiationResult negotiation;
        if (succeeded) {
            // AAC-ELD is independently optional. Until a decoder passes its
            // runtime probe, make the offer explicitly video-only so audio can
            // never destabilize the verified video path.
            succeeded = AppleMediaNegotiator().negotiate(
                    tcp,
                    control,
                    media,
                    m_Connection.endpoint.port,
                    false,
                    &negotiation,
                    m_Cancelled,
                    &error);
        }
        if (succeeded) {
            succeeded = control.sendEncrypted(
                    tcp, AppleMediaWire::controlMode(false), m_Cancelled, &error);
        }
        if (succeeded) {
            succeeded = runMediaLoop(tcp, control, media, negotiation, &error);
        }

        media.close();
        tcp.close();
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
    bool runMediaLoop(AppleTcpTransport& tcp,
                      AppleControlChannel& control,
                      AppleMediaTransport& media,
                      const AppleMediaNegotiationResult& negotiation,
                      QString* error)
    {
        AppleSrtpDecryptor decryptor(negotiation.keys.videoServer, error);
        AppleSrtcpEncryptor feedback(negotiation.keys.videoViewer, error);
        if (!decryptor.isValid() || !feedback.isValid()) {
            return false;
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
        bool hardwareActive = false;
        bool hardwareFallback = false;
        bool awaitingRandomAccessPicture = true;
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
        qint64 performanceDecodeNanoseconds = 0;

        while (!m_Cancelled->load()) {
            for (const AppleInputEncryptionRequest& input :
                 m_Session->takePendingInputs()) {
                if (!control.sendEncryptedInput(tcp,
                                                input.header,
                                                input.plaintextBlock,
                                                m_Cancelled,
                                                error)) {
                    return false;
                }
            }

            if (tcp.hasPendingData()) {
                QByteArray message;
                if (!control.receiveEncrypted(tcp, &message, m_Cancelled, error)) {
                    return false;
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
            QByteArray datagram;
            QString receiveError;
            if (media.receiveVideo(&datagram, 20, m_Cancelled, &receiveError)) {
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
                                negotiation.canvas.tileCount);
                        const bool candidatesComplete = candidates.size() ==
                                negotiation.canvas.tileCount &&
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
                            decoder = std::make_unique<AppleHevcDecoder>(
                                    m_PreferHardware);
                            if (!decoder->open(error)) {
                                return false;
                            }
                            hardwareActive = decoder->backend() ==
                                    AppleHevcDecoder::Backend::D3D11va;
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
                            QList<AppleDecodedTile> frames = decoder->decode(
                                    ready, assembler.parameterSets(), tile, &decodeError);
                            performanceDecodeNanoseconds += decodeClock.nsecsElapsed();
                            performanceDecodedTiles +=
                                    static_cast<quint64>(frames.size());
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
                                    }
                                    lastPerformanceSourceTimestamp = frame.rtpTimestamp;
                                    lastPerformanceSourceFrameAt = sourceFrameAt;
                                }
                            }
                            hardwareFallback = decoder->hardwareFallbackOccurred();
                            hardwareActive = decoder->backend() ==
                                    AppleHevcDecoder::Backend::D3D11va;
                            if (!frames.isEmpty()) {
                                lastDecodedAt = clock.elapsed();
                                frameBatcher.recordDecodedFrames(std::move(frames));
                                readyBatches = frameBatcher.takeReadyBatches();
                                for (QList<AppleDecodedTile>& batch : readyBatches) {
                                    m_Session->queueDecodedFrames(std::move(batch));
                                }
                                if (!notifiedMediaReady) {
                                    notifiedMediaReady = true;
                                    const QPointer<AppleScreenSharingSession> session = m_Session;
                                    const AppleCanvas canvas = negotiation.canvas;
                                    QMetaObject::invokeMethod(
                                            session,
                                            [session, canvas, hardwareActive, hardwareFallback]() {
                                                if (session != nullptr) {
                                                    session->mediaReady(canvas,
                                                                        hardwareActive,
                                                                        hardwareFallback);
                                                }
                                            },
                                            Qt::QueuedConnection);
                                }
                            }
                            else if (!decodeError.isEmpty() &&
                                     now - lastKeyFrameAt >= KeyFrameRetryIntervalMs) {
                                frameBatcher.recordDecodeFailure(
                                        ready.frameSequenceNumber, tile);
                                if (!requestKeyFrames(media, feedback,
                                                      negotiation.offers.videoSynchronizationSource,
                                                      sources, &keyFrameSequence, error)) {
                                    return false;
                                }
                                performanceFirs +=
                                        static_cast<quint64>(sources.size());
                                assembler.discardIncomplete();
                                decodingOrder.reset();
                                frameBatcher.reset();
                                awaitingRandomAccessPicture = true;
                                lastKeyFrameAt = now;
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
            if (firstPacketAt < 0 && now >= InitialVideoTimeoutMs) {
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
                    assembler.discardIncomplete();
                    decodingOrder.reset();
                    frameBatcher.reset();
                    awaitingRandomAccessPicture = true;
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
                const QString backend = hardwareActive
                        ? QStringLiteral("D3D11VA")
                        : QStringLiteral("software");
                const QString mediaSummary = QStringLiteral(
                        "SOURCE %1 FPS   RX %2 Mbps   HEVC %3 tiles/s @ %4 ms   %5\n"
                        "SOURCE TIME %6 ms avg   %7 p95   JITTER %8 ms   NACK %9   FIR %10")
                        .arg(sourceFramesPerSecond, 0, 'f', 1)
                        .arg(performanceBytes * 8.0 / seconds / 1000000.0,
                             0, 'f', 1)
                        .arg(performanceDecodedTiles / seconds, 0, 'f', 1)
                        .arg(averageDecodeMilliseconds, 0, 'f', 2)
                        .arg(backend)
                        .arg(sourceCadence.average, 0, 'f', 1)
                        .arg(sourceCadence.percentile95, 0, 'f', 1)
                        .arg(sourceCadence.jitter, 0, 'f', 1)
                        .arg(performanceNacks)
                        .arg(performanceFirs);
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
                        << (hardwareActive ? "D3D11VA" : "software")
                        << ", source cadence avg/p95/jitter="
                        << QString::number(sourceCadence.average, 'f', 1) << "/"
                        << QString::number(sourceCadence.percentile95, 'f', 1) << "/"
                        << QString::number(sourceCadence.jitter, 'f', 1)
                        << " ms, NACK=" << performanceNacks
                        << ", FIR=" << performanceFirs;
                const QPointer<AppleScreenSharingSession> session = m_Session;
                if (session != nullptr) {
                    QMetaObject::invokeMethod(
                            session,
                            [session, mediaSummary]() {
                                if (session != nullptr) {
                                    session->updatePerformanceStatistics(
                                            mediaSummary);
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
};

AppleScreenSharingSession::AppleScreenSharingSession(
        AppleSavedConnection connection,
        QObject* parent)
    : StreamSession(parent),
      m_Connection(std::move(connection)),
      m_Runtime(std::make_unique<LocalStreamRuntime>())
{
}

AppleScreenSharingSession::~AppleScreenSharingSession()
{
    m_Cancelled.store(true);
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
    LocalStreamRuntimeConfig runtimeConfig;
    runtimeConfig.streamWidth = 1440;
    runtimeConfig.streamHeight = 900;
    if (!m_Runtime->initialize(qtWindow, runtimeConfig)) {
        return false;
    }
    addLaunchWarning(tr("Apple High Performance audio is unavailable in this build; the session will run in explicit video-only mode."));
    return true;
}

void AppleScreenSharingSession::startSession()
{
    emit stageStarting(tr("Apple authentication and media negotiation"));
    const bool preferHardware = QSettings().value(
            QStringLiteral("appleScreenSharing/preferHardwareDecode"), true).toBool();
    QThreadPool::globalInstance()->start(
            new AppleHighPerformanceSessionTask(
                    this, m_Connection, &m_Cancelled, preferHardware));
}

void AppleScreenSharingSession::interruptSession()
{
    m_Cancelled.store(true);
    if (m_Runtime) {
        m_Runtime->requestStop();
    }
}

void AppleScreenSharingSession::setShouldExitSession(bool)
{
    interrupt();
}

void AppleScreenSharingSession::queueDecodedFrames(QList<AppleDecodedTile> frames)
{
    QMutexLocker locker(&m_FrameMutex);
    bool acceptedBatch = false;
    for (AppleDecodedTile& frame : frames) {
        if (frame.isValid()) {
            m_LatestFrames.insert(frame.tileIndex, std::move(frame));
            acceptedBatch = true;
        }
    }
    if (acceptedBatch) {
        ++m_PendingFrameBatches;
    }
}

QList<AppleInputEncryptionRequest> AppleScreenSharingSession::takePendingInputs()
{
    QMutexLocker locker(&m_InputMutex);
    QList<AppleInputEncryptionRequest> result = std::move(m_PendingInputs);
    m_PendingInputs.clear();
    return result;
}

void AppleScreenSharingSession::mediaReady(
        const AppleCanvas& canvas,
        bool hardwareDecoderActive,
        bool hardwareFallbackOccurred)
{
    if (m_MediaReady || m_Cancelled.load() || !canvas.isUsable()) {
        return;
    }
    m_Canvas = canvas;
    const int width = qBound(800, canvas.width, 1600);
    const int height = qBound(450, canvas.height, 1000);
    SDL_Window* window = m_Runtime->createStreamWindow(
            tr("%1 — Apple Screen Sharing").arg(m_Connection.displayName),
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            width,
            height,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
    if (window == nullptr) {
        m_Cancelled.store(true);
        emit displayLaunchError(tr("Couldn’t create the Apple Screen Sharing video window."));
        return;
    }
    m_Renderer = SDL_CreateRenderer(
            window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (m_Renderer == nullptr) {
        m_Renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (m_Renderer == nullptr) {
        m_Cancelled.store(true);
        emit displayLaunchError(tr("Couldn’t create the Apple Screen Sharing renderer."));
        return;
    }
    SDL_RendererInfo rendererInfo = {};
    const bool rendererInfoAvailable =
            SDL_GetRendererInfo(m_Renderer, &rendererInfo) == 0;
    const bool rendererUsesVsync = rendererInfoAvailable &&
            (rendererInfo.flags & SDL_RENDERER_PRESENTVSYNC) != 0;
    qInfo().nospace()
            << "Apple High Performance renderer="
            << (rendererInfoAvailable && rendererInfo.name != nullptr
                    ? rendererInfo.name : "unknown")
            << ", vsync=" << rendererUsesVsync;
    SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);
    if (hardwareFallbackOccurred) {
        addLaunchWarning(tr("D3D11VA HEVC decoding was unavailable or failed; the session continued with software decoding."));
    }
    else if (hardwareDecoderActive) {
        addLaunchWarning(tr("D3D11VA HEVC hardware decoding is active."));
    }
    else {
        addLaunchWarning(tr("HEVC software decoding is active."));
    }

    updatePerformanceOverlayTexture();

    m_EventTimer = new QTimer(this);
    m_EventTimer->setTimerType(Qt::PreciseTimer);
    // SDL_RenderPresent() already blocks until the next refresh when V-sync is
    // active. Adding another timer delay after that block lowers a nominal
    // 60 Hz stream substantially. A zero-interval Qt timer returns control to
    // the event loop between presents without inserting a second frame clock.
    m_EventTimer->setInterval(rendererUsesVsync ? 0 : 8);
    connect(m_EventTimer, &QTimer::timeout,
            this, &AppleScreenSharingSession::pollSdlEvents);
    m_EventTimer->start();
    m_MediaReady = true;
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
    });
}

void AppleScreenSharingSession::updatePerformanceStatistics(
        const QString& summary)
{
    m_PerformanceMediaSummary = summary;
    updatePerformanceOverlayTexture();
}

void AppleScreenSharingSession::updatePerformanceOverlayTexture()
{
    if (m_Renderer == nullptr) {
        return;
    }

    QStringList lines;
    if (!m_PerformanceMediaSummary.isEmpty()) {
        lines.append(m_PerformanceMediaSummary.split('\n', Qt::SkipEmptyParts));
    }
    if (!m_PerformancePresentationSummary.isEmpty()) {
        lines.append(m_PerformancePresentationSummary.split(
                '\n', Qt::SkipEmptyParts));
    }
    if (lines.isEmpty()) {
        lines.append(QStringLiteral("APPLE HIGH PERFORMANCE   Measuring..."));
    }

    int outputWidth = 0;
    int outputHeight = 0;
    if (SDL_GetRendererOutputSize(m_Renderer, &outputWidth, &outputHeight) != 0 ||
            outputWidth <= 0 || outputHeight <= 0) {
        return;
    }

    QFont font(QStringLiteral("Consolas"));
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(qBound(14, outputHeight / 54, 24));
    const QFontMetrics metrics(font);
    const int horizontalPadding = qMax(14, metrics.height());
    const int verticalPadding = qMax(8, metrics.height() / 2);
    const int lineSpacing = qMax(2, metrics.height() / 6);
    int widestLine = 0;
    for (const QString& line : std::as_const(lines)) {
        widestLine = qMax(widestLine, metrics.horizontalAdvance(line));
    }
    const int imageWidth = qBound(
            1, widestLine + horizontalPadding * 2, qMax(1, outputWidth - 32));
    const int imageHeight = qMin(
            outputHeight,
            verticalPadding * 2 + metrics.height() * lines.size() +
                    lineSpacing * qMax(0, lines.size() - 1));
    QImage image(imageWidth, imageHeight, QImage::Format_ARGB32_Premultiplied);
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

    const QPair<int, int> size(image.width(), image.height());
    if (m_PerformanceOverlayTexture == nullptr ||
            m_PerformanceOverlaySize != size) {
        if (m_PerformanceOverlayTexture != nullptr) {
            SDL_DestroyTexture(m_PerformanceOverlayTexture);
        }
        m_PerformanceOverlayTexture = SDL_CreateTexture(
                m_Renderer,
                SDL_PIXELFORMAT_BGRA32,
                SDL_TEXTUREACCESS_STREAMING,
                image.width(),
                image.height());
        m_PerformanceOverlaySize = size;
        if (m_PerformanceOverlayTexture != nullptr) {
            SDL_SetTextureBlendMode(
                    m_PerformanceOverlayTexture, SDL_BLENDMODE_BLEND);
        }
    }
    if (m_PerformanceOverlayTexture != nullptr) {
        SDL_UpdateTexture(m_PerformanceOverlayTexture,
                          nullptr,
                          image.constBits(),
                          image.bytesPerLine());
    }
}

void AppleScreenSharingSession::pollSdlEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            interrupt();
            break;
        case SDL_MOUSEMOTION:
            m_LastMouseX = event.motion.x;
            m_LastMouseY = event.motion.y;
            queuePointer(m_LastMouseX, m_LastMouseY);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            const quint8 button = appleButtonForSdl(event.button.button);
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                m_MouseButtons |= button;
            }
            else {
                m_MouseButtons &= ~button;
            }
            m_LastMouseX = event.button.x;
            m_LastMouseY = event.button.y;
            queuePointer(m_LastMouseX, m_LastMouseY, event.button.clicks);
            break;
        }
        case SDL_MOUSEWHEEL: {
            quint8 wheel = 0;
            if (event.wheel.y > 0) wheel |= 1 << 3;
            if (event.wheel.y < 0) wheel |= 1 << 4;
            if (event.wheel.x > 0) wheel |= 1 << 6;
            if (event.wheel.x < 0) wheel |= 1 << 5;
            if (wheel != 0) {
                queuePointer(m_LastMouseX, m_LastMouseY, 0, wheel);
                queuePointer(m_LastMouseX, m_LastMouseY);
            }
            break;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (event.type == SDL_KEYUP || event.key.repeat == 0) {
                queueKey(event.type == SDL_KEYDOWN,
                         event.key.keysym.sym,
                         event.key.keysym.scancode);
            }
            break;
        default:
            break;
        }
    }
    renderLatestFrames();
}

void AppleScreenSharingSession::queuePointer(
        int windowX,
        int windowY,
        int clickCount,
        quint8 extraButtons)
{
    const auto point = remotePoint(windowX, windowY);
    if (!point.has_value()) {
        return;
    }
    const quint32 now = currentMicroseconds();
    const quint32 delta = now - m_PreviousInputTimestamp;
    m_PreviousInputTimestamp = now;
    QMutexLocker locker(&m_InputMutex);
    m_PendingInputs.append(AppleMediaWire::pointerEvent(
            m_MouseButtons | extraButtons,
            point->first,
            point->second,
            clickCount,
            delta));
}

void AppleScreenSharingSession::queueKey(bool isDown,
                                         int sdlKeycode,
                                         int sdlScancode)
{
    const quint32 symbol = keySymbolForSdl(sdlKeycode);
    if (symbol == 0) {
        return;
    }
    const quint32 now = currentMicroseconds();
    const quint32 delta = now - m_PreviousInputTimestamp;
    m_PreviousInputTimestamp = now;
    QMutexLocker locker(&m_InputMutex);
    m_PendingInputs.append(AppleMediaWire::keyEvent(
            isDown,
            symbol,
            delta,
            0,
            static_cast<quint16>(qBound(0, sdlScancode, 65535))));
}

std::optional<QPair<quint16, quint16>> AppleScreenSharingSession::remotePoint(
        int windowX,
        int windowY) const
{
    if (!m_Canvas.isUsable() || m_Runtime->streamWindow() == nullptr) {
        return std::nullopt;
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(m_Runtime->streamWindow(), &width, &height);
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    const double scale = qMin(static_cast<double>(width) / m_Canvas.width,
                              static_cast<double>(height) / m_Canvas.height);
    const double contentWidth = m_Canvas.width * scale;
    const double contentHeight = m_Canvas.height * scale;
    const double left = (width - contentWidth) / 2.0;
    const double top = (height - contentHeight) / 2.0;
    if (windowX < left || windowY < top ||
            windowX >= left + contentWidth || windowY >= top + contentHeight) {
        return std::nullopt;
    }
    const int x = qBound(0, static_cast<int>((windowX - left) / scale),
                         m_Canvas.width - 1);
    const int y = qBound(0, static_cast<int>((windowY - top) / scale),
                         m_Canvas.height - 1);
    return QPair<quint16, quint16>(static_cast<quint16>(qMin(x, 65535)),
                                   static_cast<quint16>(qMin(y, 65535)));
}

void AppleScreenSharingSession::renderLatestFrames()
{
    if (m_Renderer == nullptr || !m_Canvas.isUsable()) {
        return;
    }
    QHash<int, AppleDecodedTile> frames;
    quint64 pendingFrameBatches = 0;
    {
        QMutexLocker locker(&m_FrameMutex);
        frames = std::move(m_LatestFrames);
        m_LatestFrames.clear();
        pendingFrameBatches = m_PendingFrameBatches;
        m_PendingFrameBatches = 0;
    }
    for (auto iterator = frames.begin(); iterator != frames.end(); ++iterator) {
        AppleDecodedTile& frame = iterator.value();
        const QPair<int, int> size(frame.width, frame.height);
        const quint32 textureFormat = SDL_PIXELFORMAT_NV12;
        if (!m_Textures.contains(frame.tileIndex) ||
                m_TextureSizes.value(frame.tileIndex) != size ||
                m_TextureFormats.value(frame.tileIndex) != textureFormat) {
            if (SDL_Texture* old = m_Textures.take(frame.tileIndex)) {
                SDL_DestroyTexture(old);
            }
            SDL_Texture* texture = SDL_CreateTexture(
                    m_Renderer,
                    textureFormat,
                    SDL_TEXTUREACCESS_STREAMING,
                    frame.width,
                    frame.height);
            if (texture == nullptr) {
                continue;
            }
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
            m_Textures.insert(frame.tileIndex, texture);
            m_TextureSizes.insert(frame.tileIndex, size);
            m_TextureFormats.insert(frame.tileIndex, textureFormat);
        }
        const uint8_t* luma = reinterpret_cast<const uint8_t*>(
                frame.pixels.constData());
        const uint8_t* chroma = reinterpret_cast<const uint8_t*>(
                frame.pixels.constData() + frame.chromaOffset);
        if (SDL_UpdateNVTexture(m_Textures.value(frame.tileIndex),
                                nullptr,
                                luma,
                                frame.stride,
                                chroma,
                                frame.chromaStride) != 0) {
            void* lockedPixels = nullptr;
            int texturePitch = 0;
            if (SDL_LockTexture(m_Textures.value(frame.tileIndex),
                                nullptr,
                                &lockedPixels,
                                &texturePitch) != 0) {
                qWarning().nospace()
                        << "Apple High Performance NV12 upload failed: "
                        << SDL_GetError();
                continue;
            }
            char* destination = static_cast<char*>(lockedPixels);
            const int lumaBytes = qMin(frame.stride, texturePitch);
            for (int row = 0; row < frame.height; ++row) {
                std::memcpy(destination + texturePitch * row,
                            frame.pixels.constData() + frame.stride * row,
                            lumaBytes);
            }
            const int chromaBytes = qMin(frame.chromaStride, texturePitch);
            const int chromaRows = (frame.height + 1) / 2;
            for (int row = 0; row < chromaRows; ++row) {
                std::memcpy(destination + texturePitch * (frame.height + row),
                            frame.pixels.constData() + frame.chromaOffset +
                                    frame.chromaStride * row,
                            chromaBytes);
            }
            SDL_UnlockTexture(m_Textures.value(frame.tileIndex));
        }
        m_TileHeights.insert(frame.tileIndex, frame.height);
    }

    int outputWidth = 0;
    int outputHeight = 0;
    SDL_GetRendererOutputSize(m_Renderer, &outputWidth, &outputHeight);
    const double scale = qMin(static_cast<double>(outputWidth) / m_Canvas.width,
                              static_cast<double>(outputHeight) / m_Canvas.height);
    const int contentWidth = qRound(m_Canvas.width * scale);
    const int contentHeight = qRound(m_Canvas.height * scale);
    const int left = (outputWidth - contentWidth) / 2;
    const int top = (outputHeight - contentHeight) / 2;

    SDL_SetRenderDrawColor(m_Renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_Renderer);
    int logicalTop = 0;
    const int fallbackHeight = (m_Canvas.height + m_Canvas.tileCount - 1) /
            m_Canvas.tileCount;
    QList<int> tileHeights;
    tileHeights.reserve(m_Canvas.tileCount);
    for (int tile = 0; tile < m_Canvas.tileCount; ++tile) {
        tileHeights.append(m_TileHeights.value(tile, fallbackHeight));
    }
    const QList<int> tileBoundaries =
            AppleMediaLayout::verticalTileBoundaries(
                    m_Canvas, tileHeights, contentHeight);
    for (int tile = 0; tile < m_Canvas.tileCount; ++tile) {
        const int tileHeight = tileHeights.at(tile);
        const int validHeight = qMin(tileHeight, m_Canvas.height - logicalTop);
        if (validHeight <= 0) {
            break;
        }
        SDL_Texture* texture = m_Textures.value(tile, nullptr);
        if (texture != nullptr) {
            const int textureWidth = m_TextureSizes.value(tile).first;
            SDL_Rect source = {0, 0, textureWidth, validHeight};
            // Both neighboring tiles must use the exact same rounded boundary.
            // Rounding each tile's offset and height independently can leave a
            // one-pixel clear strip between otherwise contiguous tiles.
            const int scaledTop = tileBoundaries.at(tile);
            const int scaledBottom = tileBoundaries.at(tile + 1);
            SDL_Rect destination = {
                left,
                top + scaledTop,
                contentWidth,
                qMax(1, scaledBottom - scaledTop),
            };
            SDL_RenderCopy(m_Renderer, texture, &source, &destination);
        }
        logicalTop += tileHeight;
    }
    if (m_PerformanceOverlayTexture != nullptr) {
        SDL_Rect overlayDestination = {
            16,
            16,
            m_PerformanceOverlaySize.first,
            m_PerformanceOverlaySize.second,
        };
        SDL_RenderCopy(m_Renderer,
                       m_PerformanceOverlayTexture,
                       nullptr,
                       &overlayDestination);
    }
    SDL_RenderPresent(m_Renderer);
    ++m_PresentationCount;
    m_PresentedTileUpdates += static_cast<quint64>(frames.size());
    const quint64 presentationNow = SDL_GetTicks64();
    if (pendingFrameBatches > 0) {
        ++m_DisplayedFrameBatches;
        m_DroppedFrameBatches += pendingFrameBatches - 1;
        if (m_LastDisplayedFrameAt != 0 &&
                presentationNow >= m_LastDisplayedFrameAt) {
            m_DisplayFrameIntervals.append(
                    static_cast<double>(presentationNow -
                                        m_LastDisplayedFrameAt));
        }
        m_LastDisplayedFrameAt = presentationNow;
    }
    if (m_PresentationWindowStartedAt == 0) {
        m_PresentationWindowStartedAt = presentationNow;
    }
    else if (presentationNow - m_PresentationWindowStartedAt >=
             static_cast<quint64>(PerformanceReportIntervalMs)) {
        const double seconds = qMax<quint64>(
                1, presentationNow - m_PresentationWindowStartedAt) / 1000.0;
        const IntervalStatistics displayCadence =
                calculateIntervalStatistics(m_DisplayFrameIntervals);
        m_PerformancePresentationSummary = QStringLiteral(
                "DISPLAY %1 FPS   VSYNC %2 Hz   TILE UPDATES %3/s   COALESCED %4\n"
                "FRAME TIME %5 ms avg   %6 p95   JITTER %7 ms")
                .arg(m_DisplayedFrameBatches / seconds, 0, 'f', 1)
                .arg(m_PresentationCount / seconds, 0, 'f', 1)
                .arg(m_PresentedTileUpdates / seconds, 0, 'f', 1)
                .arg(m_DroppedFrameBatches)
                .arg(displayCadence.average, 0, 'f', 1)
                .arg(displayCadence.percentile95, 0, 'f', 1)
                .arg(displayCadence.jitter, 0, 'f', 1);
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
                << QString::number(displayCadence.jitter, 'f', 1) << " ms";
        updatePerformanceOverlayTexture();
        m_PresentationWindowStartedAt = presentationNow;
        m_PresentationCount = 0;
        m_PresentedTileUpdates = 0;
        m_DisplayedFrameBatches = 0;
        m_DroppedFrameBatches = 0;
        m_DisplayFrameIntervals.clear();
    }
}

void AppleScreenSharingSession::destroyPresentation()
{
    if (m_EventTimer != nullptr) {
        m_EventTimer->stop();
        m_EventTimer->deleteLater();
        m_EventTimer = nullptr;
    }
    for (SDL_Texture* texture : std::as_const(m_Textures)) {
        SDL_DestroyTexture(texture);
    }
    m_Textures.clear();
    m_TextureSizes.clear();
    m_TextureFormats.clear();
    if (m_PerformanceOverlayTexture != nullptr) {
        SDL_DestroyTexture(m_PerformanceOverlayTexture);
        m_PerformanceOverlayTexture = nullptr;
    }
    m_PerformanceOverlaySize = {};
    if (m_Renderer != nullptr) {
        SDL_DestroyRenderer(m_Renderer);
        m_Renderer = nullptr;
    }
    if (m_Runtime) {
        m_Runtime->shutdown();
    }
    if (m_QtWindow != nullptr) {
        m_QtWindow->show();
        m_QtWindow->raise();
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
