#pragma once

#include "applemediaprotocol.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <memory>

struct AVBufferRef;
struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

enum class AppleVideoDecoderBackend
{
    Software,
    D3D11va,
    VideoToolbox,
};

// Opaque lifetime token shared between a hardware decoder and its matching
// presentation adapter. Platform handles never need to be interpreted by the
// session or media transport.
struct AppleVideoBackendContext
{
    AppleVideoDecoderBackend backend = AppleVideoDecoderBackend::Software;
    std::shared_ptr<void> nativeDevice;
};

struct AppleDecodedTile
{
    enum class PixelFormat
    {
        Nv12,
        Nv24,
        Vuya,
        D3d11Ayuv,
        VideoToolboxNv24,
    };

    enum class ColorSpace
    {
        Unknown,
        Bt601,
        Bt709,
        Bt2020,
    };

    enum class ColorRange
    {
        Unknown,
        Limited,
        Full,
    };

    int tileIndex = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
    int chromaOffset = 0;
    int chromaStride = 0;
    quint32 rtpTimestamp = 0;
    quint64 decodeSubmittedAtNanoseconds = 0;
    std::optional<quint16> frameSequenceNumber;
    PixelFormat pixelFormat = PixelFormat::Nv12;
    ColorSpace colorSpace = ColorSpace::Unknown;
    ColorRange colorRange = ColorRange::Unknown;
    QByteArray pixels;
    std::shared_ptr<AVFrame> hardwareFrame;

    bool isValid() const
    {
        if (pixelFormat == PixelFormat::D3d11Ayuv ||
                pixelFormat == PixelFormat::VideoToolboxNv24) {
            return tileIndex >= 0 && width > 0 && height > 0 &&
                   hardwareFrame != nullptr;
        }
        if (pixelFormat == PixelFormat::Vuya) {
            return tileIndex >= 0 && width > 0 && height > 0 &&
                   stride >= width * 4 &&
                   pixels.size() >= stride * height;
        }
        const int chromaRows = (height + 1) / 2;
        if (pixelFormat == PixelFormat::Nv24) {
            return tileIndex >= 0 && width > 0 && height > 0 &&
                   stride >= width && chromaStride >= width * 2 &&
                   chromaOffset >= stride * height &&
                   pixels.size() >= chromaOffset + chromaStride * height;
        }
        return tileIndex >= 0 && width > 0 && height > 0 &&
               stride >= width && chromaStride >= width &&
               chromaOffset >= stride * height &&
               pixels.size() >= chromaOffset + chromaStride * chromaRows;
    }
};

// Mirrors Apple's tile-frame synchronizer: all submitted tiles carrying one
// sender frame sequence are published atomically after that frame closes and
// every corresponding decoder output has arrived.
class AppleDecodedFrameBatcher
{
public:
    void recordSubmission(const AppleHevcAccessUnit& accessUnit, int tileIndex);
    void recordDecodedFrames(QList<AppleDecodedTile> frames);
    void recordDecodeFailure(std::optional<quint16> frameSequenceNumber,
                             int tileIndex);
    QList<QList<AppleDecodedTile>> takeReadyBatches();
    void reset();

private:
    struct Group
    {
        QList<int> submittedTiles;
        QSet<int> submittedTileSet;
        QHash<int, AppleDecodedTile> decodedFrames;
        bool closed = false;
    };

    QHash<quint16, Group> m_Groups;
    QList<quint16> m_GroupOrder;
    std::optional<quint16> m_CurrentFrameSequenceNumber;
    QList<QList<AppleDecodedTile>> m_ImmediateBatches;
};

class AppleHevcDecoder
{
public:
    using Backend = AppleVideoDecoderBackend;

    explicit AppleHevcDecoder(bool preferHardware);
    ~AppleHevcDecoder();

    AppleHevcDecoder(const AppleHevcDecoder&) = delete;
    AppleHevcDecoder& operator=(const AppleHevcDecoder&) = delete;

    bool open(QString* error = nullptr);
    QList<AppleDecodedTile> decode(const AppleHevcAccessUnit& accessUnit,
                                   const AppleHevcParameterSets& parameterSets,
                                   int tileIndex,
                                   QString* error = nullptr);
    void flush();
    void close();

    bool isOpen() const { return m_Context != nullptr; }
    Backend backend() const { return m_Backend; }
    bool hardwareFallbackOccurred() const { return m_HardwareFallbackOccurred; }
    std::shared_ptr<AppleVideoBackendContext> presentationContext() const;

private:
    bool openBackend(bool hardware, QString* error);
    QList<AppleDecodedTile> decodePacket(const QByteArray& annexB,
                                         int tileIndex,
                                         quint32 timestamp,
                                         std::optional<quint16> frameSequenceNumber,
                                         QString* error);
    AppleDecodedTile convertFrame(AVFrame* frame,
                                  int tileIndex,
                                  quint32 timestamp,
                                  QString* error);
    QByteArray annexB(const AppleHevcAccessUnit& accessUnit,
                      const AppleHevcParameterSets& parameterSets) const;
    static int selectHardwareFormat(AVCodecContext* context,
                                    const int* formats);
    bool prepareHardwareFramesContext(AVCodecContext* context, int format);

    bool m_PreferHardware = false;
    bool m_ParameterSetsSubmitted = false;
    bool m_HardwareFallbackOccurred = false;
    Backend m_Backend = Backend::Software;
    const AVCodec* m_Codec = nullptr;
    AVCodecContext* m_Context = nullptr;
    AVPacket* m_Packet = nullptr;
    AVFrame* m_Frame = nullptr;
    AVFrame* m_TransferFrame = nullptr;
    AVBufferRef* m_HardwareDevice = nullptr;
    SwsContext* m_SwsContext = nullptr;
};
