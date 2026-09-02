#include "applevideodecoder.h"

#include <QCoreApplication>

#include <chrono>
#include <cstring>
#include <utility>

#ifdef Q_OS_WIN
#include <d3d10_1.h>
#include <d3d11.h>
#elif defined(Q_OS_DARWIN)
#include <CoreVideo/CoreVideo.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#ifdef Q_OS_WIN
#include <libavutil/hwcontext_d3d11va.h>
#endif
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace {

#ifdef Q_OS_DARWIN
constexpr OSType VideoToolboxNv24FullRange = 0x34343466; // '444f'
constexpr OSType VideoToolboxNv24VideoRange = 0x34343476; // '444v'
#endif

struct AppleDecodedFrameMetadata
{
    int tileIndex;
    quint32 rtpTimestamp;
    quint64 decodeSubmittedAtNanoseconds;
    quint16 frameSequenceNumber;
    quint16 hasFrameSequenceNumber;
};

quint64 steadyNanoseconds()
{
    return static_cast<quint64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
}

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

AppleDecodedTile::ColorSpace decodedColorSpace(const AVFrame* frame)
{
    switch (frame->colorspace) {
    case AVCOL_SPC_FCC:
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
        return AppleDecodedTile::ColorSpace::Bt601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return AppleDecodedTile::ColorSpace::Bt2020;
    case AVCOL_SPC_BT709:
    case AVCOL_SPC_UNSPECIFIED:
    default:
        // Apple's desktop stream is BT.709 when the bitstream omits VUI
        // colour description fields.
        return AppleDecodedTile::ColorSpace::Bt709;
    }
}

AppleDecodedTile::ColorRange decodedColorRange(const AVFrame* frame)
{
    return frame->color_range == AVCOL_RANGE_JPEG
            ? AppleDecodedTile::ColorRange::Full
            : AppleDecodedTile::ColorRange::Limited;
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
    if (accessUnit.subframeBoundary ==
            AppleHevcAccessUnit::SubframeBoundary::Last) {
        group.closed = true;
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
        const bool ready = group->closed &&
                group->decodedFrames.size() == group->submittedTileSet.size();
        if (!ready) {
            // VideoToolbox reports an explicit completion for every accepted
            // sample, including dropped frames. FFmpeg's synchronous API does
            // not: an accepted access unit can produce no frame and no error.
            // A later complete sender frame proves that keeping this older
            // group can only add latency and retain every following 4:4:4
            // surface, so discard it and resume at the next atomic boundary.
            bool laterGroupIsReady = false;
            for (qsizetype index = 1; index < m_GroupOrder.size(); ++index) {
                const auto later = m_Groups.constFind(m_GroupOrder.at(index));
                if (later != m_Groups.cend() && later->closed &&
                        later->decodedFrames.size() ==
                                later->submittedTileSet.size()) {
                    laterGroupIsReady = true;
                    break;
                }
            }
            if (!laterGroupIsReady) {
                break;
            }
            m_Groups.erase(group);
            m_GroupOrder.removeFirst();
            continue;
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

AppleHevcDecoder::AppleHevcDecoder(bool preferHardware, int tileCount)
    : m_PreferHardware(preferHardware),
      // One sender frame can expose every tile before the presentation thread
      // copies it. Keep room for that frame and the one currently entering the
      // decoder in addition to FFmpeg's reference-picture requirement.
      m_HardwareSurfaceSlack(qMax(3, qMax(1, tileCount) * 2))
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
    m_ConsecutiveHardwareFailures = 0;
    m_HardwareFallbackOccurred = false;
#if defined(Q_OS_WIN) || defined(Q_OS_DARWIN)
    if (m_PreferHardware && openBackend(true, nullptr)) {
        return true;
    }
#endif
    if (m_PreferHardware) {
        m_HardwareFallbackOccurred = true;
    }
    return openBackend(false, error);
}

bool AppleHevcDecoder::openBackend(bool hardware,
                                   QString* error,
                                   AVBufferRef* reusableHardwareDevice)
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
    // FFmpeg's automatic frame threading intentionally keeps several frames
    // in flight. That throughput-oriented policy adds hundreds of milliseconds
    // to an interactive desktop stream even when decode itself takes only a
    // few milliseconds. Native hardware backends already execute
    // asynchronously, and Apple's stream is low-delay, so submit one access
    // unit at a time.
    m_Context->thread_count = 1;
    m_Context->thread_type = 0;
    // VideoToolbox associates each asynchronous output callback with the
    // submitted tile sample. FFmpeg may delay or reorder output, so preserve
    // the same association explicitly instead of labeling a returned frame
    // with whichever tile packet happened to be submitted most recently.
    m_Context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    m_Context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_Context->flags2 |= AV_CODEC_FLAG2_FAST;

#ifdef Q_OS_WIN
    if (hardware) {
        const int hardwareResult = reusableHardwareDevice != nullptr
                ? ((m_HardwareDevice = av_buffer_ref(reusableHardwareDevice)) != nullptr
                           ? 0 : AVERROR(ENOMEM))
                : av_hwdevice_ctx_create(
                          &m_HardwareDevice, AV_HWDEVICE_TYPE_D3D11VA,
                          nullptr, nullptr, 0);
        if (hardwareResult < 0 || m_HardwareDevice == nullptr) {
            close();
            return false;
        }
        AVHWDeviceContext* deviceContext =
                reinterpret_cast<AVHWDeviceContext*>(m_HardwareDevice->data);
        AVD3D11VADeviceContext* d3d11Context =
                reinterpret_cast<AVD3D11VADeviceContext*>(deviceContext->hwctx);
        ID3D10Multithread* multithread = nullptr;
        if (d3d11Context != nullptr && d3d11Context->device_context != nullptr &&
                SUCCEEDED(d3d11Context->device_context->QueryInterface(
                        __uuidof(ID3D10Multithread),
                        reinterpret_cast<void**>(&multithread)))) {
            // Decoding runs on the media thread while the same immediate
            // context presents AYUV surfaces on Qt's UI thread. D3D11's
            // multithread guard preserves command ordering without a readback.
            multithread->SetMultithreadProtected(TRUE);
            multithread->Release();
        }
        m_Context->hw_device_ctx = av_buffer_ref(m_HardwareDevice);
        m_Context->opaque = this;
        m_Context->get_format = reinterpret_cast<AVPixelFormat (*)(
                AVCodecContext*, const AVPixelFormat*)>(&AppleHevcDecoder::selectHardwareFormat);
    }
#elif defined(Q_OS_DARWIN)
    if (hardware) {
        const int hardwareResult = reusableHardwareDevice != nullptr
                ? ((m_HardwareDevice = av_buffer_ref(reusableHardwareDevice)) != nullptr
                           ? 0 : AVERROR(ENOMEM))
                : av_hwdevice_ctx_create(
                          &m_HardwareDevice, AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                          nullptr, nullptr, 0);
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
    if (!hardware) {
        m_Backend = Backend::Software;
    }
#ifdef Q_OS_WIN
    else {
        m_Backend = Backend::D3D11va;
    }
#elif defined(Q_OS_DARWIN)
    else {
        m_Backend = Backend::VideoToolbox;
    }
#endif
    m_ParameterSetsSubmitted = false;
    ++m_Generation;
    return true;
}

int AppleHevcDecoder::selectHardwareFormat(AVCodecContext* context,
                                           const int* rawFormats)
{
    const AVPixelFormat* formats = reinterpret_cast<const AVPixelFormat*>(rawFormats);
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
#ifdef Q_OS_WIN
        if (*format == AV_PIX_FMT_D3D11) {
            AppleHevcDecoder* decoder =
                    static_cast<AppleHevcDecoder*>(context->opaque);
            if (decoder != nullptr &&
                    decoder->prepareHardwareFramesContext(context, *format)) {
                return *format;
            }
            if (decoder != nullptr) {
                decoder->m_HardwareFallbackOccurred = true;
                decoder->m_Backend = Backend::Software;
            }
        }
#elif defined(Q_OS_DARWIN)
        if (*format == AV_PIX_FMT_VIDEOTOOLBOX) {
            return *format;
        }
#endif
    }
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
#ifdef Q_OS_WIN
        if (*format == AV_PIX_FMT_D3D11) {
            continue;
        }
#elif defined(Q_OS_DARWIN)
        if (*format == AV_PIX_FMT_VIDEOTOOLBOX) {
            continue;
        }
#endif
        return *format;
    }
    return AV_PIX_FMT_NONE;
}

bool AppleHevcDecoder::prepareHardwareFramesContext(AVCodecContext* context,
                                                     int rawFormat)
{
#ifdef Q_OS_WIN
    av_buffer_unref(&context->hw_frames_ctx);
    const AVPixelFormat format = static_cast<AVPixelFormat>(rawFormat);
    int result = avcodec_get_hw_frames_parameters(
            context, context->hw_device_ctx, format, &context->hw_frames_ctx);
    if (result < 0 || context->hw_frames_ctx == nullptr) {
        return false;
    }
    AVHWFramesContext* framesContext =
            reinterpret_cast<AVHWFramesContext*>(context->hw_frames_ctx->data);
    AVD3D11VAFramesContext* d3d11FramesContext =
            reinterpret_cast<AVD3D11VAFramesContext*>(framesContext->hwctx);
    // FFmpeg's default decoder pool is decode-only. The AYUV surfaces must
    // also be shader resources so the presentation device can sample them
    // directly instead of transferring every tile back to system memory.
    d3d11FramesContext->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (framesContext->initial_pool_size > 0) {
        framesContext->initial_pool_size += m_HardwareSurfaceSlack;
    }
    result = av_hwframe_ctx_init(context->hw_frames_ctx);
    if (result < 0) {
        av_buffer_unref(&context->hw_frames_ctx);
        return false;
    }
    return true;
#else
    Q_UNUSED(context);
    Q_UNUSED(rawFormat);
    return false;
#endif
}

std::shared_ptr<AppleVideoBackendContext>
AppleHevcDecoder::presentationContext() const
{
    auto context = std::make_shared<AppleVideoBackendContext>();
    context->backend = m_Backend;
#ifdef Q_OS_WIN
    if (m_Backend != Backend::D3D11va || m_HardwareDevice == nullptr) {
        return context;
    }
    AVHWDeviceContext* deviceContext =
            reinterpret_cast<AVHWDeviceContext*>(m_HardwareDevice->data);
    AVD3D11VADeviceContext* d3d11Context =
            reinterpret_cast<AVD3D11VADeviceContext*>(deviceContext->hwctx);
    ID3D11Device* device = d3d11Context != nullptr
            ? d3d11Context->device : nullptr;
    if (device != nullptr) {
        device->AddRef();
        context->nativeDevice = std::shared_ptr<void>(
                device,
                [](void* value) {
                    static_cast<ID3D11Device*>(value)->Release();
                });
    }
#endif
    return context;
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
    if (decodeError.isEmpty() || m_Backend == Backend::Software) {
        if (decodeError.isEmpty() && !frames.isEmpty() &&
                m_Backend != Backend::Software) {
            m_ConsecutiveHardwareFailures = 0;
        }
        if (!decodeError.isEmpty()) {
            setError(error, decodeError);
        }
        m_ParameterSetsSubmitted = true;
        return frames;
    }

    // A transient D3D11/VideoToolbox resource shortage invalidates the codec
    // state but not necessarily the hardware device. Recreate the codec once
    // on the same device so the zero-copy renderer remains compatible. The
    // caller observes the generation change, clears its tile synchronizer, and
    // requests a random-access picture before submitting more inter frames.
    ++m_ConsecutiveHardwareFailures;
    AVBufferRef* reusableHardwareDevice = av_buffer_ref(m_HardwareDevice);
    const bool recoveredHardware = m_ConsecutiveHardwareFailures == 1 &&
            reusableHardwareDevice != nullptr &&
            openBackend(true, nullptr, reusableHardwareDevice);
    av_buffer_unref(&reusableHardwareDevice);
    if (recoveredHardware) {
        setError(error, QCoreApplication::translate(
                "AppleVideoDecoder",
                "The hardware HEVC decoder was restarted and needs a new random-access picture."));
        return {};
    }

    m_HardwareFallbackOccurred = true;
    if (!openBackend(false, error)) {
        return {};
    }
    setError(error, QCoreApplication::translate(
            "AppleVideoDecoder",
            "The hardware HEVC decoder failed repeatedly; software decoding is waiting for a new random-access picture."));
    return {};
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
        steadyNanoseconds(),
        frameSequenceNumber.value_or(0),
        static_cast<quint16>(frameSequenceNumber.has_value()),
    };
    std::memcpy(m_Packet->opaque_ref->data, &metadata, sizeof(metadata));
    auto receiveAvailableFrames = [&]() -> int {
        int status = 0;
        while (true) {
            av_frame_unref(m_Frame);
            status = avcodec_receive_frame(m_Context, m_Frame);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return status;
            }
            if (status < 0) {
                setError(error, QCoreApplication::translate(
                        "AppleVideoDecoder", "HEVC frame decoding failed: %1")
                        .arg(ffmpegError(status)));
                result.clear();
                return status;
            }
            int outputTileIndex = tileIndex;
            quint32 outputTimestamp = timestamp;
            quint64 outputDecodeSubmittedAtNanoseconds = 0;
            std::optional<quint16> outputFrameSequenceNumber = frameSequenceNumber;
            if (m_Frame->opaque_ref != nullptr &&
                    m_Frame->opaque_ref->size >= sizeof(AppleDecodedFrameMetadata)) {
                AppleDecodedFrameMetadata outputMetadata;
                std::memcpy(&outputMetadata,
                            m_Frame->opaque_ref->data,
                            sizeof(outputMetadata));
                outputTileIndex = outputMetadata.tileIndex;
                outputTimestamp = outputMetadata.rtpTimestamp;
                outputDecodeSubmittedAtNanoseconds =
                        outputMetadata.decodeSubmittedAtNanoseconds;
                if (outputMetadata.hasFrameSequenceNumber != 0) {
                    outputFrameSequenceNumber = outputMetadata.frameSequenceNumber;
                }
            }
            AppleDecodedTile frame = convertFrame(
                    m_Frame, outputTileIndex, outputTimestamp, error);
            frame.frameSequenceNumber = outputFrameSequenceNumber;
            frame.decodeSubmittedAtNanoseconds =
                    outputDecodeSubmittedAtNanoseconds;
            if (frame.isValid()) {
                result.append(std::move(frame));
            }
        }
    };

    int status = 0;
    while ((status = avcodec_send_packet(m_Context, m_Packet)) ==
           AVERROR(EAGAIN)) {
        const qsizetype frameCountBeforeDrain = result.size();
        const int receiveStatus = receiveAvailableFrames();
        if (receiveStatus < 0 && receiveStatus != AVERROR(EAGAIN) &&
                receiveStatus != AVERROR_EOF) {
            return {};
        }
        if (result.size() == frameCountBeforeDrain &&
                receiveStatus == AVERROR(EAGAIN)) {
            setError(error, QCoreApplication::translate(
                    "AppleVideoDecoder",
                    "The HEVC decoder made no progress while accepting a packet."));
            return {};
        }
    }
    if (status < 0) {
        setError(error, QCoreApplication::translate(
                "AppleVideoDecoder", "HEVC packet submission failed: %1")
                .arg(ffmpegError(status)));
        return {};
    }
    const int receiveStatus = receiveAvailableFrames();
    if (receiveStatus < 0 && receiveStatus != AVERROR(EAGAIN) &&
            receiveStatus != AVERROR_EOF) {
        return {};
    }
    return result;
}

void AppleHevcDecoder::flush()
{
    if (m_Context != nullptr) {
        avcodec_flush_buffers(m_Context);
    }
    m_ParameterSetsSubmitted = false;
}

AppleDecodedTile AppleHevcDecoder::convertFrame(
        AVFrame* frame,
        int tileIndex,
        quint32 timestamp,
        QString* error)
{
    AVFrame* source = frame;
#ifdef Q_OS_DARWIN
    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        CVPixelBufferRef pixelBuffer = reinterpret_cast<CVPixelBufferRef>(
                frame->data[3]);
        const OSType pixelFormat = pixelBuffer != nullptr
                ? CVPixelBufferGetPixelFormatType(pixelBuffer) : 0;
        if (pixelBuffer != nullptr && CVPixelBufferGetPlaneCount(pixelBuffer) >= 2 &&
                (pixelFormat == VideoToolboxNv24FullRange ||
                 pixelFormat == VideoToolboxNv24VideoRange)) {
            AVFrame* retainedFrame = av_frame_clone(frame);
            if (retainedFrame == nullptr) {
                setError(error, QCoreApplication::translate(
                        "AppleVideoDecoder",
                        "The VideoToolbox NV24 surface could not be retained."));
                return {};
            }
            AppleDecodedTile output;
            output.tileIndex = tileIndex;
            output.width = frame->width;
            output.height = frame->height;
            output.rtpTimestamp = timestamp;
            output.pixelFormat = AppleDecodedTile::PixelFormat::VideoToolboxNv24;
            output.colorSpace = decodedColorSpace(frame);
            output.colorRange = pixelFormat == VideoToolboxNv24FullRange
                    ? AppleDecodedTile::ColorRange::Full
                    : AppleDecodedTile::ColorRange::Limited;
            output.hardwareFrame = std::shared_ptr<AVFrame>(
                    retainedFrame,
                    [](AVFrame* value) { av_frame_free(&value); });
            return output;
        }

        // FFmpeg may select a different CVPixelBuffer type on hardware that
        // cannot expose native NV24. Preserve a usable 4:4:4 software path
        // instead of passing a 4:2:0 surface to the Metal NV24 shader.
        av_frame_unref(m_TransferFrame);
        const int transfer = av_hwframe_transfer_data(m_TransferFrame, frame, 0);
        if (transfer < 0) {
            setError(error, QCoreApplication::translate(
                    "AppleVideoDecoder",
                    "VideoToolbox frame transfer failed: %1")
                    .arg(ffmpegError(transfer)));
            return {};
        }
        av_frame_copy_props(m_TransferFrame, frame);
        source = m_TransferFrame;
    }
#endif
#ifdef Q_OS_WIN
    if (frame->format == AV_PIX_FMT_D3D11) {
        ID3D11Texture2D* texture =
                reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        D3D11_TEXTURE2D_DESC textureDescription = {};
        if (texture != nullptr) {
            texture->GetDesc(&textureDescription);
        }
        if (texture != nullptr &&
                textureDescription.Format == DXGI_FORMAT_AYUV &&
                (textureDescription.BindFlags &
                 D3D11_BIND_SHADER_RESOURCE) != 0) {
            AVFrame* retainedFrame = av_frame_clone(frame);
            if (retainedFrame == nullptr) {
                setError(error, QCoreApplication::translate(
                        "AppleVideoDecoder",
                        "The D3D11VA AYUV surface could not be retained."));
                return {};
            }
            AppleDecodedTile output;
            output.tileIndex = tileIndex;
            output.width = frame->width;
            output.height = frame->height;
            output.rtpTimestamp = timestamp;
            output.pixelFormat = AppleDecodedTile::PixelFormat::D3d11Ayuv;
            output.colorSpace = decodedColorSpace(frame);
            output.colorRange = decodedColorRange(frame);
            output.hardwareFrame = std::shared_ptr<AVFrame>(
                    retainedFrame,
                    [](AVFrame* value) { av_frame_free(&value); });
            return output;
        }
        av_frame_unref(m_TransferFrame);
        const int transfer = av_hwframe_transfer_data(m_TransferFrame, frame, 0);
        if (transfer < 0) {
            setError(error, QCoreApplication::translate(
                    "AppleVideoDecoder", "D3D11VA frame transfer failed: %1")
                    .arg(ffmpegError(transfer)));
            return {};
        }
        av_frame_copy_props(m_TransferFrame, frame);
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
    output.stride = source->width * 4;
    output.chromaStride = 0;
    output.chromaOffset = 0;
    output.rtpTimestamp = timestamp;
    output.pixelFormat = AppleDecodedTile::PixelFormat::Vuya;
    output.colorSpace = decodedColorSpace(source);
    output.colorRange = decodedColorRange(source);
    output.pixels.resize(output.stride * output.height);

    // Keep one luma and one chroma sample for every source pixel. VUYA is a
    // convenient packed 4:4:4 upload format for the native shaders and avoids
    // both the former NV12 chroma loss and a costly CPU YUV-to-RGB conversion.
    m_SwsContext = sws_getCachedContext(
            m_SwsContext,
            source->width,
            source->height,
            static_cast<AVPixelFormat>(source->format),
            source->width,
            source->height,
            AV_PIX_FMT_VUYA,
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
        nullptr,
        nullptr,
        nullptr,
    };
    int strides[] = {output.stride, 0, 0, 0};
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
