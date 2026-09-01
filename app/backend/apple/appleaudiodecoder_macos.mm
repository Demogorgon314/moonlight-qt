#include "appleaudiostream.h"

#include <QCoreApplication>

#include <array>
#include <cstring>

#import <AudioToolbox/AudioToolbox.h>

namespace {

constexpr int MaximumOutputFrames = AppleAudioStream::FramesPerPacket;

// AudioToolbox expects the ESDS-wrapped magic cookie used by Apple's native
// Screen Sharing stack, rather than the bare AudioSpecificConfig consumed by
// the Windows FDK-AAC adapter.
constexpr std::array<UInt8, 45> AacEldMagicCookie = {
    0x03, 0x80, 0x80, 0x80, 0x26, 0x00, 0x00, 0x00,
    0x04, 0x80, 0x80, 0x80, 0x18, 0x40, 0x14, 0x00,
    0x18, 0x00, 0x00, 0x01, 0x38, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x05, 0x80, 0x80, 0x80, 0x06, 0xf8,
    0xe6, 0x51, 0x32, 0xe0, 0x00, 0x06, 0x80, 0x80,
    0x80, 0x01, 0x02,
};

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
}

QString audioStatus(OSStatus status)
{
    return QStringLiteral("AudioToolbox status %1").arg(status);
}

struct DecoderInput
{
    const QByteArray* bytes = nullptr;
    bool supplied = false;
    AudioStreamPacketDescription packet = {};
};

OSStatus provideCompressedPacket(AudioConverterRef,
                                 UInt32* packetCount,
                                 AudioBufferList* data,
                                 AudioStreamPacketDescription** description,
                                 void* context)
{
    auto* input = static_cast<DecoderInput*>(context);
    if (input == nullptr || input->bytes == nullptr || input->supplied ||
            input->bytes->isEmpty()) {
        *packetCount = 0;
        return noErr;
    }
    input->supplied = true;
    input->packet.mStartOffset = 0;
    input->packet.mVariableFramesInPacket = 0;
    input->packet.mDataByteSize = static_cast<UInt32>(input->bytes->size());
    data->mNumberBuffers = 1;
    data->mBuffers[0].mNumberChannels = AppleAudioStream::ChannelCount;
    data->mBuffers[0].mDataByteSize = input->packet.mDataByteSize;
    data->mBuffers[0].mData = const_cast<char*>(input->bytes->constData());
    if (description != nullptr) {
        *description = &input->packet;
    }
    *packetCount = 1;
    return noErr;
}

} // namespace

struct AppleAacEldDecoder::Private
{
    AudioConverterRef converter = nullptr;
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
    AudioStreamBasicDescription input = {};
    input.mSampleRate = AppleAudioStream::SampleRate;
    input.mFormatID = kAudioFormatMPEG4AAC_ELD_SBR;
    input.mFramesPerPacket = AppleAudioStream::FramesPerPacket;
    input.mChannelsPerFrame = AppleAudioStream::ChannelCount;

    AudioStreamBasicDescription output = {};
    output.mSampleRate = AppleAudioStream::SampleRate;
    output.mFormatID = kAudioFormatLinearPCM;
    output.mFormatFlags = kAudioFormatFlagIsFloat |
                          kAudioFormatFlagIsPacked |
                          kAudioFormatFlagsNativeEndian;
    output.mBytesPerPacket = sizeof(float) * AppleAudioStream::ChannelCount;
    output.mFramesPerPacket = 1;
    output.mBytesPerFrame = sizeof(float) * AppleAudioStream::ChannelCount;
    output.mChannelsPerFrame = AppleAudioStream::ChannelCount;
    output.mBitsPerChannel = sizeof(float) * 8;

    OSStatus status = AudioConverterNew(&input, &output, &d->converter);
    if (status != noErr || d->converter == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleAudioStream",
                "The AAC-ELD AudioToolbox decoder is unavailable: %1")
                               .arg(audioStatus(status)));
        close();
        return false;
    }
    status = AudioConverterSetProperty(
            d->converter,
            kAudioConverterDecompressionMagicCookie,
            static_cast<UInt32>(AacEldMagicCookie.size()),
            AacEldMagicCookie.data());
    if (status == noErr) {
        UInt32 primeMethod = kConverterPrimeMethod_None;
        AudioConverterSetProperty(d->converter,
                                  kAudioConverterPrimeMethod,
                                  sizeof(primeMethod),
                                  &primeMethod);
        return true;
    }
    setError(error, QCoreApplication::translate(
            "AppleAudioStream", "Couldn’t configure AAC-ELD decoding: %1")
                           .arg(audioStatus(status)));
    close();
    return false;
}

bool AppleAacEldDecoder::decode(const QByteArray& accessUnit,
                                QByteArray* interleavedFloatPcm,
                                int* frameCount,
                                QString* error)
{
    if (interleavedFloatPcm != nullptr) {
        interleavedFloatPcm->clear();
    }
    if (frameCount != nullptr) {
        *frameCount = 0;
    }
    if (!isOpen() || interleavedFloatPcm == nullptr ||
            accessUnit.isEmpty() || accessUnit.size() > 65535) {
        setError(error, QCoreApplication::translate(
                "AppleAudioStream", "The AAC-ELD access unit is invalid."));
        return false;
    }
    QByteArray output(MaximumOutputFrames * AppleAudioStream::ChannelCount *
                              static_cast<int>(sizeof(float)),
                      '\0');
    AudioBufferList buffers = {};
    buffers.mNumberBuffers = 1;
    buffers.mBuffers[0].mNumberChannels = AppleAudioStream::ChannelCount;
    buffers.mBuffers[0].mDataByteSize = static_cast<UInt32>(output.size());
    buffers.mBuffers[0].mData = output.data();
    UInt32 outputPackets = MaximumOutputFrames;
    DecoderInput input{&accessUnit};
    const OSStatus status = AudioConverterFillComplexBuffer(
            d->converter,
            provideCompressedPacket,
            &input,
            &outputPackets,
            &buffers,
            nullptr);
    if (status != noErr) {
        setError(error, QCoreApplication::translate(
                "AppleAudioStream", "AAC-ELD frame decoding failed: %1")
                               .arg(audioStatus(status)));
        return false;
    }
    const int bytesPerFrame = AppleAudioStream::ChannelCount *
                              static_cast<int>(sizeof(float));
    const int frames = qMin(
            static_cast<int>(outputPackets),
            static_cast<int>(buffers.mBuffers[0].mDataByteSize) /
                    bytesPerFrame);
    const int byteCount = frames * bytesPerFrame;
    output.resize(qBound(0, byteCount, output.size()));
    *interleavedFloatPcm = std::move(output);
    if (frameCount != nullptr) {
        *frameCount = frames;
    }
    return true;
}

void AppleAacEldDecoder::close()
{
    if (d->converter != nullptr) {
        AudioConverterDispose(d->converter);
        d->converter = nullptr;
    }
}

bool AppleAacEldDecoder::isOpen() const
{
    return d->converter != nullptr;
}
