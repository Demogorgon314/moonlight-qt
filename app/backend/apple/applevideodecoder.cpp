#include "applevideodecoder.h"

#include <QCoreApplication>

#include <cstring>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace {

struct AppleDecodedFrameMetadata
{
    int tileIndex;
    quint32 rtpTimestamp;
    quint16 frameSequenceNumber;
    quint16 hasFrameSequenceNumber;
};

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
}

QString ffmpegError(int result)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(result, text, sizeof(text));
    return QString::fromUtf8(text);
}

void appendAnnexBNal(QByteArray& packet, const QByteArray& nal)
{
    if (!nal.isEmpty()) {
        packet.append(QByteArray::fromHex("00000001"));
        packet.append(nal);
    }
}

} // namespace

void AppleDecodedFrameBatcher::recordSubmission(
        const AppleHevcAccessUnit& accessUnit,
        int tileIndex)
{
    if (!accessUnit.frameSequenceNumber.has_value()) {
        return;
    }
    const quint16 sequence = *accessUnit.frameSequenceNumber;
    if (m_CurrentFrameSequenceNumber.has_value() &&
            *m_CurrentFrameSequenceNumber != sequence) {
        auto previous = m_Groups.find(*m_CurrentFrameSequenceNumber);
        if (previous != m_Groups.end()) {
            previous->closed = true;
        }
    }
    if (!m_Groups.contains(sequence)) {
        m_Groups.insert(sequence, Group{});
        m_GroupOrder.append(sequence);
    }
    m_CurrentFrameSequenceNumber = sequence;
    Group& group = m_Groups[sequence];
    if (!group.submittedTileSet.contains(tileIndex)) {
        group.submittedTileSet.insert(tileIndex);
        group.submittedTiles.append(tileIndex);
    }
}

void AppleDecodedFrameBatcher::recordDecodedFrames(
        QList<AppleDecodedTile> frames)
{
    for (AppleDecodedTile& frame : frames) {
        if (!frame.frameSequenceNumber.has_value()) {
            QList<AppleDecodedTile> immediate;
            immediate.append(std::move(frame));
            m_ImmediateBatches.append(std::move(immediate));
            continue;
        }
        auto group = m_Groups.find(*frame.frameSequenceNumber);
        if (group != m_Groups.end() &&
                group->submittedTileSet.contains(frame.tileIndex)) {
            group->decodedFrames.insert(frame.tileIndex, std::move(frame));
        }
    }
}

void AppleDecodedFrameBatcher::recordDecodeFailure(
        std::optional<quint16> frameSequenceNumber,
        int tileIndex)
{
    if (!frameSequenceNumber.has_value()) {
        return;
    }
    auto group = m_Groups.find(*frameSequenceNumber);
    if (group == m_Groups.end()) {
        return;
    }
    group->submittedTileSet.remove(tileIndex);
    group->submittedTiles.removeAll(tileIndex);
}

QList<QList<AppleDecodedTile>> AppleDecodedFrameBatcher::takeReadyBatches()
{
    QList<QList<AppleDecodedTile>> batches = std::move(m_ImmediateBatches);
    m_ImmediateBatches.clear();
    while (!m_GroupOrder.isEmpty()) {
        const quint16 sequence = m_GroupOrder.first();
        auto group = m_Groups.find(sequence);
        if (group == m_Groups.end()) {
            m_GroupOrder.removeFirst();
            continue;
        }
        if (!group->closed ||
                group->decodedFrames.size() != group->submittedTileSet.size()) {
            break;
        }
        QList<AppleDecodedTile> frames;
        frames.reserve(group->submittedTiles.size());
        for (int tileIndex : std::as_const(group->submittedTiles)) {
            auto frame = group->decodedFrames.find(tileIndex);
            if (frame != group->decodedFrames.end()) {
                frames.append(std::move(frame.value()));
            }
        }
        m_Groups.erase(group);
        m_GroupOrder.removeFirst();
        if (!frames.isEmpty()) {
            batches.append(std::move(frames));
        }
    }
    return batches;
}

void AppleDecodedFrameBatcher::reset()
{
    m_Groups.clear();
    m_GroupOrder.clear();
    m_CurrentFrameSequenceNumber.reset();
    m_ImmediateBatches.clear();
}

AppleHevcDecoder::AppleHevcDecoder(bool preferHardware)
    : m_PreferHardware(preferHardware)
{
}

AppleHevcDecoder::~AppleHevcDecoder()
{
    close();
}

bool AppleHevcDecoder::open(QString* error)
{
    if (isOpen()) {
        return true;
    }
#ifdef Q_OS_WIN
    if (m_PreferHardware && openBackend(true, nullptr)) {
        return true;
    }
#endif
    if (m_PreferHardware) {
        m_HardwareFallbackOccurred = true;
    }
    return openBackend(false, error);
}

bool AppleHevcDecoder::openBackend(bool hardware, QString* error)
{
    close();
    m_Codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (m_Codec == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleVideoDecoder", "This build does not contain an HEVC decoder."));
        return false;
    }
    m_Context = avcodec_alloc_context3(m_Codec);
    m_Packet = av_packet_alloc();
    m_Frame = av_frame_alloc();
    m_TransferFrame = av_frame_alloc();
    if (m_Context == nullptr || m_Packet == nullptr || m_Frame == nullptr ||
            m_TransferFrame == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleVideoDecoder", "Couldn’t allocate the HEVC software pipeline."));
        close();
        return false;
    }
    m_Context->thread_count = 0;
    m_Context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    // VideoToolbox associates each asynchronous output callback with the
    // submitted tile sample. FFmpeg may delay or reorder output, so preserve
    // the same association explicitly instead of labeling a returned frame
    // with whichever tile packet happened to be submitted most recently.
    m_Context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    m_Context->flags2 |= AV_CODEC_FLAG2_FAST;

#ifdef Q_OS_WIN
    if (hardware) {
        const int hardwareResult = av_hwdevice_ctx_create(
                &m_HardwareDevice, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
        if (hardwareResult < 0 || m_HardwareDevice == nullptr) {
            close();
            return false;
        }
        m_Context->hw_device_ctx = av_buffer_ref(m_HardwareDevice);
        m_Context->opaque = this;
        m_Context->get_format = reinterpret_cast<AVPixelFormat (*)(
                AVCodecContext*, const AVPixelFormat*)>(&AppleHevcDecoder::selectHardwareFormat);
    }
#else
    Q_UNUSED(hardware);
#endif

    const int result = avcodec_open2(m_Context, m_Codec, nullptr);
    if (result < 0) {
        if (!hardware) {
            setError(error, QCoreApplication::translate(
                    "AppleVideoDecoder", "Couldn’t open the HEVC decoder: %1")
                    .arg(ffmpegError(result)));
        }
        close();
        return false;
    }
    m_Backend = hardware ? Backend::D3D11va : Backend::Software;
    m_ParameterSetsSubmitted = false;
    return true;
}

int AppleHevcDecoder::selectHardwareFormat(AVCodecContext*, const int* rawFormats)
{
    const AVPixelFormat* formats = reinterpret_cast<const AVPixelFormat*>(rawFormats);
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
#ifdef Q_OS_WIN
        if (*format == AV_PIX_FMT_D3D11) {
            return *format;
        }
#endif
    }
    return formats[0];
}

QByteArray AppleHevcDecoder::annexB(
        const AppleHevcAccessUnit& accessUnit,
        const AppleHevcParameterSets& parameterSets) const
{
    QByteArray result;
    if (!m_ParameterSetsSubmitted) {
        appendAnnexBNal(result, parameterSets.video);
        appendAnnexBNal(result, parameterSets.sequence);
        for (const QByteArray& picture : parameterSets.pictures) {
            appendAnnexBNal(result, picture);
        }
    }
    for (const QByteArray& nal : accessUnit.nalUnits) {
        appendAnnexBNal(result, nal);
    }
    return result;
}

QList<AppleDecodedTile> AppleHevcDecoder::decode(
        const AppleHevcAccessUnit& accessUnit,
        const AppleHevcParameterSets& parameterSets,
        int tileIndex,
        QString* error)
{
    if ((!isOpen() && !open(error)) || !parameterSets.isComplete() ||
            !accessUnit.containsVideoSlice()) {
        return {};
    }
    const QByteArray packet = annexB(accessUnit, parameterSets);
    QString decodeError;
    QList<AppleDecodedTile> frames = decodePacket(
            packet, tileIndex, accessUnit.timestamp,
            accessUnit.frameSequenceNumber, &decodeError);
    if (decodeError.isEmpty() || m_Backend != Backend::D3D11va) {
        if (!decodeError.isEmpty()) {
            setError(error, decodeError);
        }
        m_ParameterSetsSubmitted = true;
        return frames;
    }

    // D3D11VA availability can change after creation (driver reset, remote
    // session, unsupported profile). Rebuild only this decoder as software and
    // resubmit the same access unit with parameter sets.
    m_HardwareFallbackOccurred = true;
    if (!openBackend(false, error)) {
        return {};
    }
    const QByteArray fallbackPacket = annexB(accessUnit, parameterSets);
    decodeError.clear();
    frames = decodePacket(fallbackPacket, tileIndex, accessUnit.timestamp,
                          accessUnit.frameSequenceNumber, &decodeError);
    if (!decodeError.isEmpty()) {
        setError(error, decodeError);
    }
    m_ParameterSetsSubmitted = true;
    return frames;
}

QList<AppleDecodedTile> AppleHevcDecoder::decodePacket(
        const QByteArray& annexBPacket,
        int tileIndex,
        quint32 timestamp,
        std::optional<quint16> frameSequenceNumber,
        QString* error)
{
    QList<AppleDecodedTile> result;
    av_packet_unref(m_Packet);
    const int allocation = av_new_packet(m_Packet, annexBPacket.size());
    if (allocation < 0) {
        setError(error, ffmpegError(allocation));
        return result;
    }
    std::memcpy(m_Packet->data, annexBPacket.constData(), annexBPacket.size());
    m_Packet->pts = timestamp;
    m_Packet->dts = timestamp;
    m_Packet->opaque_ref = av_buffer_alloc(sizeof(AppleDecodedFrameMetadata));
    if (m_Packet->opaque_ref == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleVideoDecoder", "Couldn’t preserve the HEVC tile metadata."));
        return result;
    }
    const AppleDecodedFrameMetadata metadata{
        tileIndex,
        timestamp,
        frameSequenceNumber.value_or(0),
        static_cast<quint16>(frameSequenceNumber.has_value()),
    };
    std::memcpy(m_Packet->opaque_ref->data, &metadata, sizeof(metadata));
    int status = avcodec_send_packet(m_Context, m_Packet);
    if (status < 0 && status != AVERROR(EAGAIN)) {
        setError(error, QCoreApplication::translate(
                "AppleVideoDecoder", "HEVC packet submission failed: %1")
                .arg(ffmpegError(status)));
        return result;
    }
    while (true) {
        av_frame_unref(m_Frame);
        status = avcodec_receive_frame(m_Context, m_Frame);
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
            break;
        }
        if (status < 0) {
            setError(error, QCoreApplication::translate(
                    "AppleVideoDecoder", "HEVC frame decoding failed: %1")
                    .arg(ffmpegError(status)));
            result.clear();
            break;
        }
        int outputTileIndex = tileIndex;
        quint32 outputTimestamp = timestamp;
        std::optional<quint16> outputFrameSequenceNumber = frameSequenceNumber;
        if (m_Frame->opaque_ref != nullptr &&
                m_Frame->opaque_ref->size >= sizeof(AppleDecodedFrameMetadata)) {
            AppleDecodedFrameMetadata outputMetadata;
            std::memcpy(&outputMetadata,
                        m_Frame->opaque_ref->data,
                        sizeof(outputMetadata));
            outputTileIndex = outputMetadata.tileIndex;
            outputTimestamp = outputMetadata.rtpTimestamp;
            if (outputMetadata.hasFrameSequenceNumber != 0) {
                outputFrameSequenceNumber = outputMetadata.frameSequenceNumber;
            }
        }
        AppleDecodedTile frame = convertFrame(
                m_Frame, outputTileIndex, outputTimestamp, error);
        frame.frameSequenceNumber = outputFrameSequenceNumber;
        if (frame.isValid()) {
            result.append(std::move(frame));
        }
    }
    return result;
}

AppleDecodedTile AppleHevcDecoder::convertFrame(
        AVFrame* frame,
        int tileIndex,
        quint32 timestamp,
        QString* error)
{
    AVFrame* source = frame;
#ifdef Q_OS_WIN
    if (frame->format == AV_PIX_FMT_D3D11) {
        av_frame_unref(m_TransferFrame);
        const int transfer = av_hwframe_transfer_data(m_TransferFrame, frame, 0);
        if (transfer < 0) {
            setError(error, QCoreApplication::translate(
                    "AppleVideoDecoder", "D3D11VA frame transfer failed: %1")
                    .arg(ffmpegError(transfer)));
            return {};
        }
        source = m_TransferFrame;
    }
#endif
    if (source->width <= 0 || source->height <= 0) {
        return {};
    }
    AppleDecodedTile output;
    output.tileIndex = tileIndex;
    output.width = source->width;
    output.height = source->height;
    output.stride = source->width;
    output.chromaStride = source->width;
    output.chromaOffset = output.stride * output.height;
    output.rtpTimestamp = timestamp;
    output.pixelFormat = AppleDecodedTile::PixelFormat::Nv12;
    output.pixels.resize(output.chromaOffset +
                         output.chromaStride * ((output.height + 1) / 2));

    // Preserve the decoder's bi-planar layout and reduce only the component
    // depth here. SDL's D3D11 renderer performs the expensive YUV-to-RGB step
    // on the GPU. Converting every 4K tile to BGRA on this media thread made
    // the synchronous readback path exceed a 60 fps frame budget.
    m_SwsContext = sws_getCachedContext(
            m_SwsContext,
            source->width,
            source->height,
            static_cast<AVPixelFormat>(source->format),
            source->width,
            source->height,
            AV_PIX_FMT_NV12,
            SWS_FAST_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
    if (m_SwsContext == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleVideoDecoder", "The decoded HEVC pixel format is unsupported."));
        return {};
    }
    uint8_t* destination[] = {
        reinterpret_cast<uint8_t*>(output.pixels.data()),
        reinterpret_cast<uint8_t*>(output.pixels.data() + output.chromaOffset),
        nullptr,
        nullptr,
    };
    int strides[] = {output.stride, output.chromaStride, 0, 0};
    const int scaled = sws_scale(m_SwsContext,
                                 source->data,
                                 source->linesize,
                                 0,
                                 source->height,
                                 destination,
                                 strides);
    if (scaled != source->height) {
        setError(error, QCoreApplication::translate(
                "AppleVideoDecoder", "The HEVC frame could not be converted for display."));
        return {};
    }
    return output;
}

void AppleHevcDecoder::close()
{
    if (m_SwsContext != nullptr) {
        sws_freeContext(m_SwsContext);
        m_SwsContext = nullptr;
    }
    av_frame_free(&m_TransferFrame);
    av_frame_free(&m_Frame);
    av_packet_free(&m_Packet);
    avcodec_free_context(&m_Context);
    av_buffer_unref(&m_HardwareDevice);
    m_Codec = nullptr;
    m_ParameterSetsSubmitted = false;
}
