#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QUuid>

#include <algorithm>
#include <optional>

struct AppleCanvas
{
    int width = 0;
    int height = 0;
    int tileCount = 0;

    bool isUsable() const
    {
        return width > 0 && height > 0 && tileCount > 0 && tileCount <= 16;
    }

    bool operator==(const AppleCanvas& other) const
    {
        return width == other.width && height == other.height &&
               tileCount == other.tileCount;
    }

    bool operator!=(const AppleCanvas& other) const
    {
        return !(*this == other);
    }
};

namespace AppleMediaLayout {

// Returns tileCount + 1 shared pixel boundaries. Adjacent tiles consume the
// same boundary value so fractional scaling cannot leave a cleared row between
// them.
QList<int> verticalTileBoundaries(const AppleCanvas& canvas,
                                  const QList<int>& tileHeights,
                                  int outputHeight);

} // namespace AppleMediaLayout

struct AppleMediaKeys
{
    static constexpr int BlobLength = 46;

    QByteArray audioViewer;
    QByteArray audioServer;
    QByteArray videoViewer;
    QByteArray videoServer;
    QByteArray secondaryVideoViewer;
    QByteArray secondaryVideoServer;

    bool isValid() const;
    bool hasSecondaryVideo() const;
};

struct AppleMediaOffers
{
    QByteArray video;
    QByteArray audio;
    quint32 videoSynchronizationSource = 0;
    quint32 audioSynchronizationSource = 0;
};

struct AppleMediaPorts
{
    quint16 audio = 0;
    quint16 video = 0;
    QList<quint16> videos;

    QList<quint16> videoPorts() const
    {
        return !videos.isEmpty() ? videos
                                 : (video != 0 ? QList<quint16>{video}
                                               : QList<quint16>{});
    }

    bool isUsable() const
    {
        const QList<quint16> effective = videoPorts();
        return audio != 0 && !effective.isEmpty() &&
                std::all_of(effective.cbegin(), effective.cend(),
                            [](quint16 value) { return value != 0; });
    }
};

struct AppleInputEncryptionRequest
{
    QByteArray header;
    QByteArray plaintextBlock;

    bool isValid() const
    {
        return header.size() == 2 && plaintextBlock.size() == 16;
    }
};

struct AppleScrollWheelEvent
{
    qint16 deltaX = 0;
    qint16 deltaY = 0;
    qint16 deltaZ = 0;
    qint32 fixedDeltaX = 0;
    qint32 fixedDeltaY = 0;
    qint32 fixedDeltaZ = 0;
    qint32 pointDeltaX = 0;
    qint32 pointDeltaY = 0;
    qint32 pointDeltaZ = 0;
    quint32 scrollPhase = 0;
    quint32 momentumPhase = 0;
    quint32 scrollCount = 0;
    quint32 flags = 0;
};

struct AppleRtpReceptionReport
{
    quint32 source = 0;
    quint8 fractionLost = 0;
    qint32 cumulativePacketsLost = 0;
    quint32 extendedHighestSequence = 0;
    quint32 interarrivalJitter = 0;
    quint32 lastSenderReport = 0;
    quint32 delaySinceLastSenderReport = 0;
};

struct AppleVideoFrameLossFeedback
{
    quint32 mediaSource = 0;
    quint32 rtpTimestamp = 0;
    quint16 expectedPacketCount = 0;
    quint16 lostPacketCount = 0;

    quint16 packedLoss() const
    {
        return static_cast<quint16>(((expectedPacketCount & 0x3f) << 6) |
                                    (lostPacketCount & 0x3f));
    }
};

enum class AppleVideoBandwidthProbeActivity
{
    Active,
    Boundary,
    Suppressed,
};

struct AppleVideoRateControlInfo
{
    quint32 rtpTimestamp = 0;
    quint32 estimatedBandwidthKilobitsPerSecond = 0;
    quint16 burstyLoss = 0;
    quint32 receivedPacketCount = 0;
    quint32 feedbackDelayMilliseconds = 0;
    quint16 echoTimestamp = 0;
    double oneWayReceiveDelaySeconds = 0.0;
};

class AppleVideoRateControlEstimator
{
public:
    // Arrival times share one monotonic nanosecond clock. Probe packets update
    // capacity while every packet advances the native RCTL playout fields.
    void observe(quint32 rtpTimestamp,
                 qint64 arrivalNanoseconds,
                 int packetSize,
                 AppleVideoBandwidthProbeActivity activity);
    std::optional<AppleVideoRateControlInfo> feedback(
            qint64 nowNanoseconds) const;

private:
    enum class EstimateState
    {
        Initial,
        Stable,
        InsufficientProbeWindow,
        PendingUp,
        PendingDown,
        Committed,
    };

    struct ProbeGroup
    {
        quint32 timestamp = 0;
        qint64 referenceArrivalNanoseconds = 0;
        qint64 lastArrivalNanoseconds = 0;
        quint64 bytesAfterReference = 0;
        int packetsAfterReference = 0;
    };

    void updateTimestampStatistics(quint32 rtpTimestamp,
                                   qint64 arrivalNanoseconds);
    void updateBandwidthEstimate(quint32 rtpTimestamp,
                                 qint64 arrivalNanoseconds,
                                 int packetSize);
    void finalizeBandwidthProbe();
    void finalize(const ProbeGroup& group);
    void applyCandidate(double candidateBitsPerSecond);
    void clearPendingEstimate(EstimateState state);
    void updateOneWayReceiveDelay(quint32 timestamp,
                                  qint64 arrivalNanoseconds);

    std::optional<ProbeGroup> m_ProbeGroup;
    std::optional<double> m_EstimatedBandwidth;
    EstimateState m_EstimateState = EstimateState::Stable;
    int m_PendingDirection = 0;
    double m_PendingBandwidth = 0.0;
    int m_PendingCount = 0;
    int m_ConsecutiveShortProbeCount = 0;
    bool m_DidReceiveVideo = false;
    quint32 m_TotalPacketsReceived = 0;
    std::optional<quint32> m_PreviousTimestamp;
    quint32 m_LastAcceptedTimestamp = 0;
    quint32 m_LastAcceptedPacketCount = 0;
    qint64 m_LastAcceptedArrivalNanoseconds = -1;
    std::optional<quint32> m_DelayPreviousTimestamp;
    quint64 m_DelayWrapOffset = 0;
    std::optional<quint64> m_FirstUnwrappedTimestamp;
    qint64 m_FirstDelayArrivalNanoseconds = -1;
    double m_ShortDelay = 0.0;
    double m_LongDelay = 0.0;
    double m_OneWayReceiveDelay = 0.0;
};

class AppleFrameUpdatePauseState
{
public:
    std::optional<quint32> setMiniaturized(bool miniaturized,
                                           int endpointWindowCount);
    bool isPaused() const { return m_IsPaused; }

private:
    bool m_IsPaused = false;
};

namespace AppleMediaWire {

AppleMediaOffers createOffers(bool audioEnabled,
                              const QString& operatingSystemVersion,
                              QString* error = nullptr);
QByteArray createOffer(int mode,
                       quint32 synchronizationSource,
                       quint64 timestampNanoseconds,
                       const QUuid& callId,
                       bool audioEnabled,
                       const QString& operatingSystemVersion,
                       QString* error = nullptr);
QByteArray configuration(const AppleMediaOffers& offers,
                         const AppleMediaKeys& keys,
                         const QUuid& callId,
                         QString* error = nullptr);
QByteArray configuration(const AppleMediaOffers& offers,
                         const AppleMediaOffers* secondaryOffers,
                         const AppleMediaKeys& keys,
                         const QUuid& callId,
                         QString* error = nullptr);

QByteArray framebufferUpdateRequest();
QByteArray autoFramebufferUpdate(quint32 intervalMilliseconds = 0);
QByteArray controlMode(bool observing);
QByteArray selectCombinedDisplays();
QByteArray selectDisplay(quint32 displayId);
AppleScrollWheelEvent scrollWheelDeltas(qint32 deltaX,
                                        qint32 deltaY,
                                        double preciseDeltaX,
                                        double preciseDeltaY,
                                        bool flipped,
                                        quint32 scrollCount,
                                        double speedMultiplier);
QByteArray scrollWheelEvent(const AppleScrollWheelEvent& event,
                            quint16 x,
                            quint16 y);

bool parsePorts(const QByteArray& answer, AppleMediaPorts* ports);
bool parseCanvas(const QByteArray& answer, AppleCanvas* canvas);
QList<AppleCanvas> parseCanvases(const QByteArray& answer);
bool containsMediaAnswer(const QByteArray& answer);

AppleInputEncryptionRequest pointerEvent(quint8 buttons,
                                         quint16 x,
                                         quint16 y,
                                         int clickCount,
                                         quint32 timestampDelta);
AppleInputEncryptionRequest keyEvent(bool isDown,
                                     quint32 keySymbol,
                                     quint32 timestampDelta,
                                     quint16 keyboardType,
                                     quint16 keyCode);

QByteArray receiverReport(quint32 sender);
QByteArray receiverReport(quint32 sender,
                          const AppleRtpReceptionReport& report);
QByteArray genericNack(quint32 sender,
                       quint32 mediaSource,
                       const QList<quint16>& lostSequences);
QByteArray frameLossFeedback(
        quint32 sender,
        const AppleVideoFrameLossFeedback& feedback);
QByteArray fullIntraRequest(quint32 sender,
                            quint32 mediaSource,
                            quint8 sequence);
QList<QByteArray> fullIntraRequests(quint32 sender,
                                    const QList<quint32>& mediaSources,
                                    quint8 initialSequence);
QByteArray rateControl(quint32 sender,
                       quint32 rtpTimestamp,
                       quint32 estimatedBandwidthKilobitsPerSecond,
                       quint32 receivedPacketCount,
                       quint32 feedbackDelayMilliseconds,
                       quint16 echoTimestamp);
QByteArray rateControl(quint32 sender,
                       const AppleVideoRateControlInfo& info);

} // namespace AppleMediaWire

struct AppleRtpPacket
{
    struct FramePacketInfo
    {
        quint16 totalPacketsPerFrame = 0;
        quint16 frameSequenceNumber = 0;

        bool operator==(const FramePacketInfo& other) const
        {
            return totalPacketsPerFrame == other.totalPacketsPerFrame &&
                   frameSequenceNumber == other.frameSequenceNumber;
        }
    };

    QByteArray header;
    QByteArray payload;
    quint16 sequenceNumber = 0;
    quint32 timestamp = 0;
    quint32 synchronizationSource = 0;
    quint8 payloadType = 0;
    bool marker = false;

    std::optional<FramePacketInfo> framePacketInfo() const;
};

class AppleSrtpDecryptor
{
public:
    AppleSrtpDecryptor() = default;
    explicit AppleSrtpDecryptor(const QByteArray& keyBlob, QString* error = nullptr);

    bool isValid() const;
    bool decrypt(const QByteArray& datagram,
                 AppleRtpPacket* packet,
                 QString* error = nullptr);

private:
    struct SourceState
    {
        quint32 rolloverCounter = 0;
        quint16 maximumSequence = 0;
        bool initialized = false;
    };

    QByteArray initializationVector(quint32 source, quint64 index) const;
    bool authenticates(const QByteArray& datagram,
                       int bodyLength,
                       quint32 rollover) const;

    QByteArray m_CipherKey;
    QByteArray m_AuthenticationKey;
    QByteArray m_Salt;
    QHash<quint32, SourceState> m_States;
};

class AppleSrtcpEncryptor
{
public:
    AppleSrtcpEncryptor() = default;
    explicit AppleSrtcpEncryptor(const QByteArray& keyBlob,
                                 QString* error = nullptr);

    bool isValid() const;
    QByteArray protect(const QByteArray& packet, QString* error = nullptr);

private:
    QByteArray m_CipherKey;
    QByteArray m_AuthenticationKey;
    QByteArray m_Salt;
    quint32 m_Index = 0;
};

struct AppleHevcParameterSets
{
    QByteArray video;
    QByteArray sequence;
    QList<QByteArray> pictures;

    bool isComplete() const
    {
        return !video.isEmpty() && !sequence.isEmpty() && !pictures.isEmpty();
    }
};

struct AppleHevcAccessUnit
{
    enum class SubframeBoundary
    {
        NotLast,
        Last,
        Unknown,
    };

    quint32 synchronizationSource = 0;
    quint32 timestamp = 0;
    std::optional<quint16> decodingOrderNumber;
    std::optional<quint16> frameSequenceNumber;
    std::optional<quint16> totalPacketsPerFrame;
    QList<QByteArray> nalUnits;
    SubframeBoundary subframeBoundary = SubframeBoundary::Unknown;

    bool containsVideoSlice() const;
    bool containsRandomAccessPicture() const;
};

// The four tile SSRCs are subframes of one HEVC decoding timeline. Keep DON
// admission global so a late datagram from one tile cannot move the shared
// decoder backwards and invalidate references established by another tile.
class AppleHevcDecodingOrderQueue
{
public:
    QList<AppleHevcAccessUnit> enqueue(
            const QList<AppleHevcAccessUnit>& accessUnits);
    void reset();

private:
    static QList<AppleHevcAccessUnit> circularlyOrdered(
            const QList<AppleHevcAccessUnit>& accessUnits);

    std::optional<quint16> m_ExpectedDecodingOrderNumber;
};

class AppleHevcAssembler
{
public:
    bool process(const AppleRtpPacket& packet,
                 qint64 nowMilliseconds,
                 AppleHevcAccessUnit* accessUnit,
                 qint64 arrivalNanoseconds = -1);
    void expire(qint64 nowMilliseconds);
    void discardIncomplete();

    const AppleHevcParameterSets& parameterSets() const { return m_ParameterSets; }
    int totalPacketCount() const;
    QHash<quint32, int> packetCounts() const { return m_SourcePacketCounts; }
    QSet<quint32> completedSources() const { return m_CompletedSources; }
    QList<quint32> primarySources(int tileCount,
                                  const QHash<quint32, int>& baseline = {},
                                  const QSet<quint32>& excluded = {}) const;
    // Returns a complete, sufficiently active source group without allowing a
    // stalled decoder to bounce back to a source group it already abandoned.
    QList<quint32> replacementSources(
            int tileCount,
            const QList<quint32>& currentSources,
            const QSet<quint32>& abandonedSources,
            int minimumPacketsPerSource) const;
    QHash<quint32, QList<quint16>> takeNacks(qint64 nowMilliseconds);
    std::optional<AppleRtpReceptionReport> receptionReport(quint32 source);
    std::optional<AppleVideoRateControlInfo> rateControlFeedback(
            qint64 nowNanoseconds) const;
    QList<AppleVideoFrameLossFeedback> frameLossFeedbackDue(
            qint64 nowMilliseconds) const;
    void markFrameLossFeedbackSent(
            const AppleVideoFrameLossFeedback& feedback,
            qint64 nowMilliseconds);

private:
    struct PendingPacket
    {
        quint16 sequence = 0;
        QByteArray payload;
        bool marker = false;
    };

    struct PendingAccessUnit
    {
        qint64 firstSeenAt = 0;
        std::optional<quint16> decodingOrderNumber;
        std::optional<quint16> frameSequenceNumber;
        std::optional<quint16> totalPacketsPerFrame;
        QList<PendingPacket> packets;
    };

    struct MissingPacket
    {
        qint64 firstDetectedAt = 0;
        qint64 lastNackAt = -1;
    };

    struct ReceptionState
    {
        quint32 baseExtendedSequence = 0;
        quint32 maximumExtendedSequence = 0;
        quint32 receivedPackets = 1;
        quint32 previousExpectedPackets = 0;
        quint32 previousReceivedPackets = 0;
        std::optional<qint64> previousTransit;
        double jitter = 0.0;
    };

    struct CachedFrameLoss
    {
        quint32 rtpTimestamp = 0;
        quint16 frameSequenceNumber = 0;
        quint16 expectedPacketCount = 0;
        QSet<quint16> receivedSequences;
        qint64 lastSentAt = -1;
    };

    static QList<PendingPacket> sequenceOrdered(const QList<PendingPacket>& packets);
    static QList<QByteArray> reassemble(const QList<PendingPacket>& packets);
    static std::optional<quint16> firstDecodingOrderNumber(const QByteArray& payload);
    static AppleHevcAccessUnit::SubframeBoundary subframeBoundary(
            const QList<QByteArray>& units);
    void harvest(const QList<QByteArray>& units);
    void observeSequence(const AppleRtpPacket& packet,
                         qint64 nowMilliseconds,
                         qint64 arrivalNanoseconds);
    void updateReceptionState(const AppleRtpPacket& packet,
                              qint64 arrivalNanoseconds);
    void observeRateControl(const AppleRtpPacket& packet,
                            qint64 arrivalNanoseconds);
    void cacheFrameLossIfNeeded(quint32 source,
                                quint32 timestamp,
                                const PendingAccessUnit& group);
    void correctCachedFrameLoss(const AppleRtpPacket& packet);
    static quint32 extendedSequence(quint16 sequence, quint32 maximum);

    QHash<quint64, PendingAccessUnit> m_Groups;
    QHash<quint32, int> m_SourcePacketCounts;
    QSet<quint32> m_CompletedSources;
    QHash<quint32, quint16> m_MaximumSequence;
    QHash<quint32, QHash<quint16, MissingPacket>> m_MissingPackets;
    QHash<quint32, ReceptionState> m_ReceptionStates;
    QHash<quint32, CachedFrameLoss> m_CachedFrameLosses;
    AppleVideoRateControlEstimator m_RateControlEstimator;
    std::optional<quint32> m_ActiveProbeTimestamp;
    qint64 m_LastProbeEndNanoseconds = -1;
    AppleHevcParameterSets m_ParameterSets;
    std::optional<quint32> m_PlayoutTimestamp;
};
