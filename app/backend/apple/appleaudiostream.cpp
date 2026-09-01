#include "appleaudiostream.h"

#include "SDL.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QLibrary>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {

// AudioToolbox's magic cookie wraps this AudioSpecificConfig in an ESDS
// descriptor. FDK-AAC consumes the AudioSpecificConfig itself.
constexpr unsigned char AacEldAudioSpecificConfig[] = {
    0xf8, 0xe6, 0x51, 0x32, 0xe0, 0x00,
};

// Keep FDK-AAC behind a runtime boundary. The Apple feature is optional and
// its decoder has distribution terms which must not leak into feature-off
// builds. These declarations describe the stable public C ABI from
// aacdecoder_lib.h without making the normal Moonlight build depend on its
// development headers or import library.
using FdkHandle = void*;
using FdkError = int;
using FdkUInt = unsigned int;
using FdkByte = unsigned char;
using FdkPcm = short;

struct FdkStreamInfoPrefix
{
    int sampleRate;
    int frameSize;
    int numChannels;
};

static_assert(sizeof(FdkPcm) == 2,
              "FDK-AAC PCM ABI must remain signed 16-bit");

using FdkOpen = FdkHandle (*)(int transportFormat, FdkUInt layerCount);
using FdkConfigRaw = FdkError (*)(FdkHandle, FdkByte*[], const FdkUInt[]);
using FdkFill = FdkError (*)(FdkHandle, FdkByte*[],
                             const FdkUInt[], FdkUInt*);
using FdkDecodeFrame = FdkError (*)(FdkHandle, FdkPcm*, int, FdkUInt);
using FdkClose = void (*)(FdkHandle);
using FdkGetStreamInfo = FdkStreamInfoPrefix* (*)(FdkHandle);

constexpr FdkError FdkOk = 0;
constexpr FdkError FdkNotEnoughBits = 0x1002;
constexpr FdkError FdkDecodeErrorFirst = 0x4000;
constexpr FdkError FdkDecodeErrorLast = 0x4fff;
constexpr int FdkMp4Raw = 0;
constexpr int MaximumFdkPcmSamples = 2048 * 8;

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
}

QString fdkError(FdkError code)
{
    return QStringLiteral("FDK-AAC error 0x%1")
            .arg(static_cast<unsigned int>(code), 4, 16, QLatin1Char('0'));
}

} // namespace

struct AppleAacEldDecoder::Private
{
    QLibrary library;
    FdkHandle handle = nullptr;
    FdkOpen open = nullptr;
    FdkConfigRaw configRaw = nullptr;
    FdkFill fill = nullptr;
    FdkDecodeFrame decodeFrame = nullptr;
    FdkClose close = nullptr;
    FdkGetStreamInfo getStreamInfo = nullptr;
};

AppleAacEldDecoder::AppleAacEldDecoder()
    : d(std::make_unique<Private>())
{
}

AppleAacEldDecoder::~AppleAacEldDecoder()
{
    close();
}

bool AppleAacEldDecoder::open(QString* error)
{
    close();
    d->library.setFileName(QDir(QCoreApplication::applicationDirPath())
                           .filePath(QStringLiteral("fdk-aac.dll")));
    if (!d->library.load()) {
        setError(error, QCoreApplication::translate(
                "AppleAudioStream",
                "The AAC-ELD decoder is unavailable: %1")
                .arg(d->library.errorString()));
        return false;
    }

    d->open = reinterpret_cast<FdkOpen>(d->library.resolve("aacDecoder_Open"));
    d->configRaw = reinterpret_cast<FdkConfigRaw>(
            d->library.resolve("aacDecoder_ConfigRaw"));
    d->fill = reinterpret_cast<FdkFill>(d->library.resolve("aacDecoder_Fill"));
    d->decodeFrame = reinterpret_cast<FdkDecodeFrame>(
            d->library.resolve("aacDecoder_DecodeFrame"));
    d->close = reinterpret_cast<FdkClose>(d->library.resolve("aacDecoder_Close"));
    d->getStreamInfo = reinterpret_cast<FdkGetStreamInfo>(
            d->library.resolve("aacDecoder_GetStreamInfo"));
    if (d->open == nullptr || d->configRaw == nullptr || d->fill == nullptr ||
            d->decodeFrame == nullptr || d->close == nullptr ||
            d->getStreamInfo == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleAudioStream",
                "The AAC-ELD decoder DLL has an incompatible API."));
        close();
        return false;
    }

    d->handle = d->open(FdkMp4Raw, 1);
    if (d->handle == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleAudioStream", "Couldn’t allocate the AAC-ELD decoder."));
        close();
        return false;
    }

    FdkByte* configuration[] = {
        const_cast<FdkByte*>(AacEldAudioSpecificConfig),
    };
    const FdkUInt configurationSizes[] = {
        static_cast<FdkUInt>(sizeof(AacEldAudioSpecificConfig)),
    };
    const FdkError result = d->configRaw(
            d->handle, configuration, configurationSizes);
    if (result != FdkOk) {
        setError(error, QCoreApplication::translate(
                "AppleAudioStream", "Couldn’t open the AAC-ELD decoder: %1")
                .arg(fdkError(result)));
        close();
        return false;
    }
    return true;
}

bool AppleAacEldDecoder::decode(const QByteArray& accessUnit,
                                QByteArray* interleavedFloatPcm,
                                int* frameCount,
                                QString* error)
{
    if (!isOpen() || interleavedFloatPcm == nullptr ||
            accessUnit.isEmpty() || accessUnit.size() > 65535) {
        setError(error, QCoreApplication::translate(
                "AppleAudioStream", "The AAC-ELD access unit is invalid."));
        return false;
    }
    interleavedFloatPcm->clear();
    if (frameCount != nullptr) {
        *frameCount = 0;
    }
    FdkByte* input[] = {
        reinterpret_cast<FdkByte*>(const_cast<char*>(accessUnit.constData())),
    };
    const FdkUInt inputSizes[] = {
        static_cast<FdkUInt>(accessUnit.size()),
    };
    FdkUInt bytesValid = inputSizes[0];
    FdkError result = d->fill(d->handle, input, inputSizes, &bytesValid);
    if (result != FdkOk || bytesValid != 0) {
        setError(error, QCoreApplication::translate(
                "AppleAudioStream", "AAC-ELD packet submission failed: %1")
                .arg(fdkError(result)));
        return false;
    }

    std::array<FdkPcm, MaximumFdkPcmSamples> pcm = {};
    result = d->decodeFrame(d->handle, pcm.data(),
                            static_cast<int>(pcm.size()), 0);
    if (result == FdkNotEnoughBits) {
        return true;
    }
    if (result != FdkOk &&
            (result <= FdkDecodeErrorFirst || result > FdkDecodeErrorLast)) {
        setError(error, QCoreApplication::translate(
                "AppleAudioStream", "AAC-ELD frame decoding failed: %1")
                .arg(fdkError(result)));
        return false;
    }

    const FdkStreamInfoPrefix* stream = d->getStreamInfo(d->handle);
    if (stream == nullptr || stream->sampleRate != AppleAudioStream::SampleRate ||
            stream->numChannels != AppleAudioStream::ChannelCount ||
            stream->frameSize <= 0 ||
            stream->frameSize * stream->numChannels >
                    static_cast<int>(pcm.size())) {
        setError(error, QCoreApplication::translate(
                "AppleAudioStream",
                "The AAC-ELD decoder returned an unsupported PCM format."));
        return false;
    }

    const int sampleCount = stream->frameSize * stream->numChannels;
    QByteArray output(sampleCount * static_cast<int>(sizeof(float)), '\0');
    float* destination = reinterpret_cast<float*>(output.data());
    for (int sample = 0; sample < sampleCount; ++sample) {
        destination[sample] = static_cast<float>(pcm[sample]) / 32768.0f;
    }
    *interleavedFloatPcm = std::move(output);
    if (frameCount != nullptr) {
        *frameCount = stream->frameSize;
    }
    return true;
}

void AppleAacEldDecoder::close()
{
    if (d->handle != nullptr && d->close != nullptr) {
        d->close(d->handle);
    }
    d->handle = nullptr;
    d->open = nullptr;
    d->configRaw = nullptr;
    d->fill = nullptr;
    d->decodeFrame = nullptr;
    d->close = nullptr;
    d->getStreamInfo = nullptr;
    if (d->library.isLoaded()) {
        d->library.unload();
    }
}

bool AppleAacEldDecoder::isOpen() const
{
    return d->handle != nullptr;
}

struct AppleAudioStream::Private
{
    AppleSrtpDecryptor decryptor;
    AppleAacEldDecoder decoder;
    SDL_AudioDeviceID device = 0;
    bool ownsAudioSubsystem = false;
    bool muted = false;
    bool playbackStarted = false;
    bool queueWasEmpty = false;
    AppleAudioStatistics statistics;
};

AppleAudioStream::AppleAudioStream(const QByteArray& serverKey, QString* error)
    : d(std::make_unique<Private>())
{
    d->decryptor = AppleSrtpDecryptor(serverKey, error);
    if (!d->decryptor.isValid() || !d->decoder.open(error)) {
        return;
    }
    d->ownsAudioSubsystem = (SDL_WasInit(SDL_INIT_AUDIO) == 0);
    if (d->ownsAudioSubsystem && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        setError(error, QCoreApplication::translate(
                "AppleAudioStream", "Couldn’t initialize audio playback: %1")
                .arg(QString::fromUtf8(SDL_GetError())));
        return;
    }
    SDL_AudioSpec requested = {};
    SDL_AudioSpec obtained = {};
    requested.freq = SampleRate;
    requested.format = AUDIO_F32SYS;
    requested.channels = ChannelCount;
    requested.samples = FramesPerPacket * 3;
    d->device = SDL_OpenAudioDevice(nullptr, 0, &requested, &obtained, 0);
    if (d->device == 0 || obtained.freq != SampleRate ||
            obtained.format != AUDIO_F32SYS || obtained.channels != ChannelCount) {
        setError(error, QCoreApplication::translate(
                "AppleAudioStream", "Couldn’t open 48 kHz stereo audio playback: %1")
                .arg(QString::fromUtf8(SDL_GetError())));
        if (d->device != 0) {
            SDL_CloseAudioDevice(d->device);
            d->device = 0;
        }
        return;
    }
    SDL_PauseAudioDevice(d->device, 1);
    qInfo() << "Apple Screen Sharing AAC-ELD audio ready: 48000 Hz stereo,"
            << obtained.samples << "device samples";
}

AppleAudioStream::~AppleAudioStream()
{
    close();
}

bool AppleAudioStream::isAudioRtp(const QByteArray& datagram)
{
    if (datagram.size() < 12 ||
            (static_cast<quint8>(datagram.at(0)) & 0xc0) != 0x80) {
        return false;
    }
    return (static_cast<quint8>(datagram.at(1)) & 0x7f) == 101;
}

bool AppleAudioStream::decoderIsSupported(QString* error)
{
    AppleAacEldDecoder decoder;
    return decoder.open(error);
}

bool AppleAudioStream::isReady() const
{
    return d->decryptor.isValid() && d->decoder.isOpen() && d->device != 0;
}

void AppleAudioStream::process(const QList<QByteArray>& datagrams, bool muted)
{
    if (!isReady()) {
        return;
    }
    if (d->muted != muted) {
        d->muted = muted;
        SDL_ClearQueuedAudio(d->device);
        SDL_PauseAudioDevice(d->device, 1);
        d->playbackStarted = false;
        d->queueWasEmpty = false;
    }
    constexpr int bytesPerFrame = ChannelCount * sizeof(float);
    constexpr int maximumQueuedBytes = FramesPerPacket * 12 * bytesPerFrame;
    constexpr int startupQueuedBytes = FramesPerPacket * 3 * bytesPerFrame;
    const int queuedBefore = static_cast<int>(
            SDL_GetQueuedAudioSize(d->device));
    const bool queueEmpty = queuedBefore == 0;
    if (d->playbackStarted && queueEmpty && !d->queueWasEmpty) {
        ++d->statistics.playbackUnderflows;
    }
    d->queueWasEmpty = queueEmpty;
    for (const QByteArray& datagram : datagrams) {
        if (!isAudioRtp(datagram)) {
            continue;
        }
        AppleRtpPacket packet;
        if (!d->decryptor.decrypt(datagram, &packet, nullptr)) {
            ++d->statistics.droppedPackets;
            continue;
        }
        ++d->statistics.receivedPackets;
        QByteArray pcm;
        int frames = 0;
        QElapsedTimer decodeTimer;
        decodeTimer.start();
        ++d->statistics.decodeAttempts;
        const bool decoded = d->decoder.decode(
                packet.payload, &pcm, &frames, nullptr);
        d->statistics.decodeNanoseconds +=
                static_cast<quint64>(decodeTimer.nsecsElapsed());
        if (!decoded ||
                frames <= 0 || pcm.isEmpty()) {
            ++d->statistics.droppedPackets;
            continue;
        }
        ++d->statistics.decodedPackets;
        if (static_cast<int>(SDL_GetQueuedAudioSize(d->device)) + pcm.size() >
                maximumQueuedBytes) {
            ++d->statistics.droppedPackets;
            continue;
        }
        if (d->muted) {
            std::memset(pcm.data(), 0, pcm.size());
        }
        if (SDL_QueueAudio(d->device, pcm.constData(), pcm.size()) != 0) {
            ++d->statistics.droppedPackets;
            continue;
        }
        if (!d->playbackStarted &&
                static_cast<int>(SDL_GetQueuedAudioSize(d->device)) >=
                        startupQueuedBytes) {
            SDL_PauseAudioDevice(d->device, 0);
            d->playbackStarted = true;
        }
    }
    const quint32 queuedAfter = SDL_GetQueuedAudioSize(d->device);
    d->queueWasEmpty = d->playbackStarted && queuedAfter == 0;
    d->statistics.queuedMilliseconds = static_cast<int>(
            queuedAfter * 1000ULL /
            (SampleRate * bytesPerFrame));
}

AppleAudioStatistics AppleAudioStream::statistics() const
{
    return d->statistics;
}

void AppleAudioStream::close()
{
    if (!d) {
        return;
    }
    if (d->device != 0) {
        SDL_PauseAudioDevice(d->device, 1);
        SDL_ClearQueuedAudio(d->device);
        SDL_CloseAudioDevice(d->device);
        d->device = 0;
    }
    d->decoder.close();
    if (d->ownsAudioSubsystem && SDL_WasInit(SDL_INIT_AUDIO) != 0) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    d->ownsAudioSubsystem = false;
}
