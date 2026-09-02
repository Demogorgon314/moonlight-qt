#include "appleaudiostream.h"

#include <QCoreApplication>

#include <array>
#include <cstring>

#import <AVFAudio/AVFAudio.h>

namespace {

constexpr int MaximumOutputFrames = AppleAudioStream::FramesPerPacket;

// AudioToolbox expects the ESDS-wrapped magic cookie used by Apple's native
// Screen Sharing stack, rather than the bare AudioSpecificConfig consumed by
// the Windows FDK-AAC adapter.
constexpr std::array<UInt8, 43> AacEldMagicCookie = {
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

QString audioError(NSError* error)
{
    if (error == nil) {
        return QStringLiteral("unknown AVAudioConverter error");
    }
    return QStringLiteral("AVAudioConverter error %1: %2")
            .arg(error.code)
            .arg(QString::fromNSString(error.localizedDescription));
}

} // namespace

struct AppleAacEldDecoder::Private
{
    AVAudioFormat* inputFormat = nil;
    AVAudioFormat* outputFormat = nil;
    AVAudioConverter* converter = nil;
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
    @autoreleasepool {
        close();
        AudioStreamBasicDescription input = {};
        input.mSampleRate = AppleAudioStream::SampleRate;
        input.mFormatID = kAudioFormatMPEG4AAC_ELD_SBR;
        input.mFramesPerPacket = AppleAudioStream::FramesPerPacket;
        input.mChannelsPerFrame = AppleAudioStream::ChannelCount;

        d->inputFormat = [[AVAudioFormat alloc]
                initWithStreamDescription:&input];
        d->outputFormat = [[AVAudioFormat alloc]
                initWithCommonFormat:AVAudioPCMFormatFloat32
                         sampleRate:AppleAudioStream::SampleRate
                           channels:AppleAudioStream::ChannelCount
                        interleaved:YES];
        if (d->inputFormat == nil || d->outputFormat == nil) {
            setError(error, QCoreApplication::translate(
                    "AppleAudioStream",
                    "The AAC-ELD audio formats are unavailable."));
            close();
            return false;
        }
        d->converter = [[AVAudioConverter alloc]
                initFromFormat:d->inputFormat
                      toFormat:d->outputFormat];
        if (d->converter == nil) {
            setError(error, QCoreApplication::translate(
                    "AppleAudioStream",
                    "The AAC-ELD AVAudioConverter decoder is unavailable."));
            close();
            return false;
        }
        d->converter.magicCookie = [NSData
                dataWithBytes:AacEldMagicCookie.data()
                        length:AacEldMagicCookie.size()];
        d->converter.primeMethod = AVAudioConverterPrimeMethod_None;
        return true;
    }
}

bool AppleAacEldDecoder::decode(const QByteArray& accessUnit,
                                QByteArray* interleavedFloatPcm,
                                int* frameCount,
                                QString* error)
{
    @autoreleasepool {
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

        AVAudioCompressedBuffer* input = [[[AVAudioCompressedBuffer alloc]
                initWithFormat:d->inputFormat
                packetCapacity:1
                maximumPacketSize:accessUnit.size()] autorelease];
        AVAudioPCMBuffer* output = [[[AVAudioPCMBuffer alloc]
                initWithPCMFormat:d->outputFormat
                frameCapacity:MaximumOutputFrames] autorelease];
        if (input == nil || output == nil) {
            setError(error, QCoreApplication::translate(
                    "AppleAudioStream",
                    "Couldn’t allocate AAC-ELD conversion buffers."));
            return false;
        }
        std::memcpy(input.data, accessUnit.constData(), accessUnit.size());
        input.byteLength = static_cast<uint32_t>(accessUnit.size());
        input.packetCount = 1;
        if (input.packetDescriptions != nullptr) {
            input.packetDescriptions[0] = AudioStreamPacketDescription{
                0,
                0,
                static_cast<UInt32>(accessUnit.size()),
            };
        }

        __block bool suppliedInput = false;
        NSError* conversionError = nil;
        // The raw AudioConverter callback has no equivalent of NoDataNow:
        // returning zero packets ends the stream after its first access unit.
        // AVAudioConverter preserves the live decoder between RTP packets,
        // matching Apple's Swift Screen Sharing client.
        const AVAudioConverterOutputStatus status = [d->converter
                convertToBuffer:output
                          error:&conversionError
             withInputFromBlock:^AVAudioBuffer*(
                     AVAudioPacketCount,
                     AVAudioConverterInputStatus* inputStatus) {
                 if (suppliedInput) {
                     *inputStatus = AVAudioConverterInputStatus_NoDataNow;
                     return nil;
                 }
                 suppliedInput = true;
                 *inputStatus = AVAudioConverterInputStatus_HaveData;
                 return input;
             }];
        if (conversionError != nil ||
                status == AVAudioConverterOutputStatus_Error) {
            setError(error, QCoreApplication::translate(
                    "AppleAudioStream", "AAC-ELD frame decoding failed: %1")
                                   .arg(audioError(conversionError)));
            return false;
        }

        const int frames = static_cast<int>(output.frameLength);
        const int bytesPerFrame = AppleAudioStream::ChannelCount *
                                  static_cast<int>(sizeof(float));
        const int byteCount = frames * bytesPerFrame;
        const AudioBufferList* buffers = output.audioBufferList;
        if (frames < 0 || frames > MaximumOutputFrames ||
                buffers == nullptr || buffers->mNumberBuffers != 1 ||
                buffers->mBuffers[0].mData == nullptr ||
                byteCount > static_cast<int>(
                                    buffers->mBuffers[0].mDataByteSize)) {
            setError(error, QCoreApplication::translate(
                    "AppleAudioStream",
                    "The AAC-ELD decoder returned an unsupported PCM buffer."));
            return false;
        }
        *interleavedFloatPcm = QByteArray(
                static_cast<const char*>(buffers->mBuffers[0].mData),
                byteCount);
        if (frameCount != nullptr) {
            *frameCount = frames;
        }
        return true;
    }
}

void AppleAacEldDecoder::close()
{
    @autoreleasepool {
        [d->converter release];
        d->converter = nil;
        [d->outputFormat release];
        d->outputFormat = nil;
        [d->inputFormat release];
        d->inputFormat = nil;
    }
}

bool AppleAacEldDecoder::isOpen() const
{
    return d->converter != nil;
}
