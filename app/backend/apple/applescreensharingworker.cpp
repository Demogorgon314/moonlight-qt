#include "applescreensharingsession.h"
#include "applescreensharingsession_p.h"

#include "appleaudiostream.h"
#include "appleauthenticator.h"
#include "applecredentialstore.h"
#include "applemediatransport.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <QSemaphore>
#include <QSettings>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <utility>

using AppleScreenSharingSessionPrivate::appleVideoDecoderBackendName;
using AppleScreenSharingSessionPrivate::calculateIntervalStatistics;
using AppleScreenSharingSessionPrivate::IntervalStatistics;
using AppleScreenSharingSessionPrivate::MaximumReconnectAttempts;
using AppleScreenSharingSessionPrivate::PerformanceReportIntervalMs;
using AppleScreenSharingSessionPrivate::RealtimeMediaPollTimeoutMs;
using AppleScreenSharingSessionPrivate::steadyNanoseconds;

namespace {

constexpr qint64 InitialVideoTimeoutMs = 12000;
constexpr qint64 ReceiverReportIntervalMs = 1000;
constexpr qint64 RateControlIntervalMs = 50;
constexpr qint64 KeyFrameRetryIntervalMs = 1500;
constexpr qint64 DecoderStallMs = 5000;
constexpr qint64 SourceAdoptionStallMs = 2000;
constexpr int MinimumSourceAdoptionPackets = 5;

} // namespace

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
        m_PerformanceBytes += static_cast<quint64>(datagram.size());
        if (m_FirstPacketAt < 0) {
            m_FirstPacketAt = now;
        }
        m_LastPacketAt = now;

        AppleHevcAccessUnit accessUnit;
        const bool completed = m_Assembler.process(
                packet, now, &accessUnit,
                static_cast<qint64>(steadyNanoseconds()));
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
        if (!adoptFreshSourcesIfNeeded(media, now, error)) {
            return false;
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
        const QList<AppleVideoFrameLossFeedback> frameLosses =
                m_Assembler.frameLossFeedbackDue(now);
        for (const AppleVideoFrameLossFeedback& frameLoss : frameLosses) {
            if (!sendFeedback(media,
                              AppleMediaWire::frameLossFeedback(
                                      m_Negotiation.synchronizationSource,
                                      frameLoss),
                              error)) {
                return false;
            }
            m_Assembler.markFrameLossFeedbackSent(frameLoss, now);
        }
        if (now - m_LastReceiverReportAt >= ReceiverReportIntervalMs) {
            const auto report = m_Assembler.receptionReport(
                    m_Sources.first());
            if (report.has_value()) {
                if (!sendFeedback(media,
                                  AppleMediaWire::receiverReport(
                                          m_Negotiation.synchronizationSource,
                                          *report),
                                  error)) {
                    return false;
                }
                m_LastReceiverReportAt = now;
            }
        }
        if (now - m_LastRateControlAt >= RateControlIntervalMs) {
            const auto rateControl = m_Assembler.rateControlFeedback(
                    static_cast<qint64>(steadyNanoseconds()));
            if (rateControl.has_value()) {
                if (!sendFeedback(media,
                                  AppleMediaWire::rateControl(
                                          m_Negotiation.synchronizationSource,
                                          *rateControl),
                                  error)) {
                    return false;
                }
                m_LastRateControlAt = now;
            }
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

    bool adoptFreshSourcesIfNeeded(AppleMediaTransport& media,
                                   qint64 now,
                                   QString* error)
    {
        if (m_LastDecodedAt < 0 ||
                now - m_LastDecodedAt < SourceAdoptionStallMs) {
            return true;
        }
        const QList<quint32> candidates = m_Assembler.replacementSources(
                m_Negotiation.canvas.tileCount, m_Sources,
                m_AbandonedSources, MinimumSourceAdoptionPackets);
        if (candidates.isEmpty()) {
            return true;
        }

        for (quint32 source : std::as_const(m_Sources)) {
            m_AbandonedSources.insert(source);
        }
        m_Sources = candidates;
        m_SourceToTile.clear();
        for (int index = 0; index < m_Sources.size(); ++index) {
            m_SourceToTile.insert(m_Sources.at(index), index);
        }
        m_FrameBatcher.reset();
        m_DecodingOrder.reset();
        m_Assembler.discardIncomplete();
        m_Decoder->flush();
        if (!requestKeyFrames(media, error)) {
            return false;
        }
        m_AwaitingRandomAccessPicture = true;
        m_EnteredRefreshState = false;
        m_LastDecodedAt = now;
        m_LastKeyFrameAt = now;
        qInfo() << "Apple display 2 adopted fresh video sources"
                << m_Sources;
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
    QSet<quint32> m_AbandonedSources;
    QHash<quint32, int> m_SourceToTile;
    std::unique_ptr<AppleHevcDecoder> m_Decoder;
    QElapsedTimer m_Clock;
    qint64 m_FirstPacketAt = -1;
    qint64 m_LastPacketAt = -1;
    qint64 m_LastDecodedAt = -1;
    qint64 m_LastReceiverReportAt = 0;
    qint64 m_LastRateControlAt = 0;
    qint64 m_LastKeyFrameAt = -KeyFrameRetryIntervalMs;
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
        QSet<quint32> abandonedSources;
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
        qint64 lastKeyFrameAt = -KeyFrameRetryIntervalMs;
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
            abandonedSources.clear();
            sourceToTile.clear();
            if (decoder) {
                decoder->flush();
            }
            firstPacketAt = -1;
            videoWaitStartedAt = clock.elapsed();
            lastPacketAt = -1;
            lastDecodedAt = -1;
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
                    ++performancePackets;
                    performanceBytes += static_cast<quint64>(datagram.size());
                    if (firstPacketAt < 0) {
                        firstPacketAt = now;
                    }
                    lastPacketAt = now;
                    AppleHevcAccessUnit accessUnit;
                    const bool completed = assembler.process(
                            packet, now, &accessUnit,
                            static_cast<qint64>(steadyNanoseconds()));
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
                if (lastDecodedAt >= 0 &&
                        now - lastDecodedAt >= SourceAdoptionStallMs) {
                    const QList<quint32> candidates =
                            assembler.replacementSources(
                                    activeCanvas.tileCount, sources,
                                    abandonedSources,
                                    MinimumSourceAdoptionPackets);
                    if (!candidates.isEmpty()) {
                        for (quint32 source : std::as_const(sources)) {
                            abandonedSources.insert(source);
                        }
                        sources = candidates;
                        sourceToTile.clear();
                        for (int index = 0; index < sources.size(); ++index) {
                            sourceToTile.insert(sources.at(index), index);
                        }
                        frameBatcher.reset();
                        decodingOrder.reset();
                        assembler.discardIncomplete();
                        decoder->flush();
                        awaitingRandomAccessPicture = true;
                        hasEnteredDecodeRefreshState = false;
                        lastDecodedAt = now;
                        if (!requestKeyFrames(
                                    media, feedback,
                                    negotiation.offers.videoSynchronizationSource,
                                    sources, &keyFrameSequence, error)) {
                            return false;
                        }
                        performanceFirs += static_cast<quint64>(sources.size());
                        lastKeyFrameAt = now;
                        qInfo() << "Apple adopted fresh video sources"
                                << sources;
                    }
                }
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
                const QList<AppleVideoFrameLossFeedback> frameLosses =
                        assembler.frameLossFeedbackDue(now);
                for (const AppleVideoFrameLossFeedback& frameLoss :
                     frameLosses) {
                    if (!sendFeedback(
                                media, feedback,
                                AppleMediaWire::frameLossFeedback(
                                        negotiation.offers.videoSynchronizationSource,
                                        frameLoss),
                                error)) {
                        return false;
                    }
                    assembler.markFrameLossFeedbackSent(frameLoss, now);
                }
                if (now - lastReceiverReportAt >= ReceiverReportIntervalMs) {
                    const auto report = assembler.receptionReport(
                            sources.first());
                    if (report.has_value()) {
                        if (!sendFeedback(
                                    media, feedback,
                                    AppleMediaWire::receiverReport(
                                            negotiation.offers.videoSynchronizationSource,
                                            *report),
                                    error)) {
                            return false;
                        }
                        lastReceiverReportAt = now;
                    }
                }
                if (now - lastRateControlAt >= RateControlIntervalMs) {
                    const auto rateControl = assembler.rateControlFeedback(
                            static_cast<qint64>(steadyNanoseconds()));
                    if (rateControl.has_value()) {
                        if (!sendFeedback(
                                    media, feedback,
                                    AppleMediaWire::rateControl(
                                            negotiation.offers.videoSynchronizationSource,
                                            *rateControl),
                                    error)) {
                            return false;
                        }
                        lastRateControlAt = now;
                    }
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

void AppleScreenSharingSession::startHighPerformanceWorker()
{
    const bool preferHardware = QSettings().value(
            QStringLiteral("appleScreenSharing/preferHardwareDecode"), true).toBool();
    m_WorkerPool.start(
            new AppleHighPerformanceSessionTask(
                    this, m_Connection, &m_Cancelled, preferHardware,
                    m_InitialDisplaySizes));
}
