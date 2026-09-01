#pragma once

#include "applemediaprotocol.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <memory>

class AppleAacEldDecoder
{
public:
    AppleAacEldDecoder();
    ~AppleAacEldDecoder();

    AppleAacEldDecoder(const AppleAacEldDecoder&) = delete;
    AppleAacEldDecoder& operator=(const AppleAacEldDecoder&) = delete;

    bool open(QString* error = nullptr);
    bool decode(const QByteArray& accessUnit,
                QByteArray* interleavedFloatPcm,
                int* frameCount,
                QString* error = nullptr);
    void close();
    bool isOpen() const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

struct AppleAudioStatistics
{
    quint64 receivedPackets = 0;
    quint64 decodedPackets = 0;
    quint64 droppedPackets = 0;
    quint64 decodeAttempts = 0;
    quint64 decodeNanoseconds = 0;
    quint64 playbackUnderflows = 0;
    int queuedMilliseconds = 0;
};

// Owns the complete audio path. Callers only supply control-port datagrams and
// a mute state; SRTP, AAC-ELD and device buffering remain an internal contract.
class AppleAudioStream
{
public:
    static constexpr int SampleRate = 48000;
    static constexpr int ChannelCount = 2;
    static constexpr int FramesPerPacket = 480;

    explicit AppleAudioStream(const QByteArray& serverKey,
                              QString* error = nullptr);
    ~AppleAudioStream();

    AppleAudioStream(const AppleAudioStream&) = delete;
    AppleAudioStream& operator=(const AppleAudioStream&) = delete;

    static bool isAudioRtp(const QByteArray& datagram);
    static bool decoderIsSupported(QString* error = nullptr);

    bool isReady() const;
    void process(const QList<QByteArray>& datagrams, bool muted);
    AppleAudioStatistics statistics() const;
    void close();

private:
    struct Private;
    std::unique_ptr<Private> d;
};
