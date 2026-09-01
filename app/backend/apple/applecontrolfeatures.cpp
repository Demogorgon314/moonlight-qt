#include "applecontrolfeatures.h"

#include "appleprotocol.h"

#include <QCoreApplication>
#include <QImage>

#include <QtZlib/zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr qint32 CursorEncoding = 0x450;
constexpr qint32 DisplayLayoutEncoding = 0x451;
constexpr int MaximumCursorDimension = 1024;
constexpr int MaximumClipboardItems = 1024;
constexpr int MaximumClipboardFlavors = 4096;
constexpr int MaximumClipboardAliases = 8192;
constexpr int MaximumClipboardMetadataBytes = 1024 * 1024;
constexpr int MaximumClipboardMetadataLength = 4096;
const QByteArray TextFlavor = QByteArrayLiteral("public.utf8-plain-text");

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
}

quint8 byteAt(const QByteArray& data, int offset)
{
    return static_cast<quint8>(data.at(offset));
}

bool isLengthPrefixedEncoding(qint32 encoding)
{
    switch (encoding) {
    case 1010:
    case 1011:
    case 1107:
    case 1109:
    case 1110:
        return true;
    default:
        return false;
    }
}

QByteArray inflateSyncFlush(const QByteArray& compressed,
                            int expectedSize,
                            int maximumSize)
{
    if (compressed.isEmpty() || expectedSize < 0 || expectedSize > maximumSize) {
        return {};
    }
    QByteArray output(expectedSize, Qt::Uninitialized);
    z_stream stream = {};
    stream.next_in = reinterpret_cast<Bytef*>(
            const_cast<char*>(compressed.constData()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    if (inflateInit(&stream) != Z_OK) {
        return {};
    }
    int status = Z_OK;
    while (status == Z_OK && stream.avail_out > 0) {
        status = inflate(&stream, Z_SYNC_FLUSH);
    }
    const bool valid = stream.total_out == static_cast<uLong>(expectedSize) &&
            (status == Z_OK || status == Z_STREAM_END || status == Z_BUF_ERROR);
    inflateEnd(&stream);
    return valid ? output : QByteArray();
}

QByteArray deflateSyncFlush(const QByteArray& input, int maximumSize)
{
    if (input.size() > AppleTextClipboardExchange::MaximumArchiveBytes) {
        return {};
    }
    z_stream stream = {};
    if (deflateInit(&stream, Z_BEST_COMPRESSION) != Z_OK) {
        return {};
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.constData()));
    stream.avail_in = static_cast<uInt>(input.size());
    QByteArray output;
    QByteArray chunk(64 * 1024, Qt::Uninitialized);
    int status = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
        stream.avail_out = static_cast<uInt>(chunk.size());
        status = deflate(&stream, Z_SYNC_FLUSH);
        const int produced = chunk.size() - static_cast<int>(stream.avail_out);
        if (produced > 0) {
            if (output.size() > maximumSize - produced) {
                deflateEnd(&stream);
                return {};
            }
            output.append(chunk.constData(), produced);
        }
    } while (status == Z_OK && stream.avail_out == 0);
    const bool valid = status == Z_OK && stream.avail_in == 0;
    deflateEnd(&stream);
    return valid ? output : QByteArray();
}

bool readBlob(const QByteArray& data, int* offset, QByteArray* value)
{
    if (offset == nullptr || *offset < 0 || *offset > data.size() - 4) {
        return false;
    }
    bool ok = false;
    const quint32 rawLength = AppleWire::readUInt32(data, *offset, &ok);
    *offset += 4;
    if (!ok || rawLength > static_cast<quint32>(data.size() - *offset)) {
        return false;
    }
    if (value != nullptr) {
        *value = data.mid(*offset, static_cast<int>(rawLength));
    }
    *offset += static_cast<int>(rawLength);
    return true;
}

bool readMetadata(const QByteArray& data,
                  int* offset,
                  int* metadataBytes,
                  QByteArray* value)
{
    QByteArray candidate;
    if (!readBlob(data, offset, &candidate) ||
            candidate.size() > MaximumClipboardMetadataLength ||
            *metadataBytes > MaximumClipboardMetadataBytes - candidate.size()) {
        return false;
    }
    *metadataBytes += candidate.size();
    if (value != nullptr) {
        *value = std::move(candidate);
    }
    return true;
}

AppleDisplayLayout parseDisplayLayout(const QByteArray& payload)
{
    AppleDisplayLayout layout;
    if (payload.size() < 20) {
        return layout;
    }
    bool ok = false;
    layout.scaledWidth = AppleWire::readUInt16(payload, 2, &ok);
    if (!ok) return {};
    layout.scaledHeight = AppleWire::readUInt16(payload, 4, &ok);
    if (!ok) return {};
    layout.backingWidth = AppleWire::readUInt16(payload, 6, &ok);
    if (!ok) return {};
    layout.backingHeight = AppleWire::readUInt16(payload, 8, &ok);
    if (!ok) return {};
    const int count = AppleWire::readUInt16(payload, 18, &ok);
    if (!ok || count <= 0 || count > 2 || payload.size() < 20 + count * 56) {
        return {};
    }
    for (int index = 0; index < count; ++index) {
        const int offset = 20 + index * 56;
        AppleDisplayRect display;
        display.id = AppleWire::readUInt32(payload, offset + 16, &ok);
        if (!ok) return {};
        const int logicalY0 = AppleWire::readUInt16(payload, offset + 20, &ok);
        if (!ok) return {};
        const int logicalX0 = AppleWire::readUInt16(payload, offset + 22, &ok);
        if (!ok) return {};
        const int logicalY1 = AppleWire::readUInt16(payload, offset + 24, &ok);
        if (!ok) return {};
        const int logicalX1 = AppleWire::readUInt16(payload, offset + 26, &ok);
        if (!ok) return {};
        const int y0 = AppleWire::readUInt16(payload, offset + 28, &ok);
        if (!ok) return {};
        const int x0 = AppleWire::readUInt16(payload, offset + 30, &ok);
        if (!ok) return {};
        const int y1 = AppleWire::readUInt16(payload, offset + 32, &ok);
        if (!ok) return {};
        const int x1 = AppleWire::readUInt16(payload, offset + 34, &ok);
        if (!ok || x1 <= x0 || y1 <= y0) {
            continue;
        }
        display.x = x0;
        display.y = y0;
        display.width = x1 - x0;
        display.height = y1 - y0;
        display.logicalX = logicalX1 > logicalX0 ? logicalX0 : x0;
        display.logicalY = logicalY1 > logicalY0 ? logicalY0 : y0;
        display.logicalWidth = logicalX1 > logicalX0 ? logicalX1 - logicalX0
                                                     : display.width;
        display.logicalHeight = logicalY1 > logicalY0 ? logicalY1 - logicalY0
                                                       : display.height;
        layout.displays.append(display);
    }
    return layout.isUsable() ? layout : AppleDisplayLayout();
}

} // namespace

QSize AppleDynamicResolution::normalizedSize(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return {};
    }
    const double maximumScale = std::min({
        1.0,
        1920.0 / width,
        1080.0 / height,
    });
    const double boundedWidth = width * maximumScale;
    const double boundedHeight = height * maximumScale;
    const double desiredMinimumScale = std::max({
        1.0,
        320.0 / boundedWidth,
        200.0 / boundedHeight,
    });
    const double availableScale = std::min(
            1920.0 / boundedWidth, 1080.0 / boundedHeight);
    const double finalScale = std::min(desiredMinimumScale, availableScale);
    const int normalizedWidth = qMax(
            2, static_cast<int>(std::floor(boundedWidth * finalScale)) & ~1);
    const int normalizedHeight = qMax(
            2, static_cast<int>(std::floor(boundedHeight * finalScale)) & ~1);
    return QSize(normalizedWidth, normalizedHeight);
}

bool AppleCursorImage::isUsable() const
{
    return width > 0 && height > 0 && width <= MaximumCursorDimension &&
            height <= MaximumCursorDimension && hotspotX >= 0 && hotspotY >= 0 &&
            hotspotX < width && hotspotY < height &&
            rgba.size() == width * height * 4;
}

AppleCursorImage AppleCursorImage::scaledForDpi(double scale) const
{
    if (!isUsable() || !std::isfinite(scale) || scale <= 1.0) {
        return *this;
    }

    const double availableScale = std::min(
            static_cast<double>(MaximumCursorDimension) / width,
            static_cast<double>(MaximumCursorDimension) / height);
    const double effectiveScale = std::min(scale, availableScale);
    if (effectiveScale <= 1.0) {
        return *this;
    }

    AppleCursorImage result;
    result.width = qBound(
            width, qRound(width * effectiveScale), MaximumCursorDimension);
    result.height = qBound(
            height, qRound(height * effectiveScale), MaximumCursorDimension);
    const QImage source(
            reinterpret_cast<const uchar*>(rgba.constData()),
            width, height, width * 4, QImage::Format_RGBA8888);
    const QImage scaled = source.scaled(
            result.width, result.height,
            Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (scaled.isNull()) {
        return *this;
    }
    result.rgba.resize(result.width * result.height * 4);
    for (int row = 0; row < result.height; ++row) {
        std::memcpy(result.rgba.data() + row * result.width * 4,
                    scaled.constScanLine(row),
                    static_cast<size_t>(result.width * 4));
    }
    result.hotspotX = qBound(
            0, qRound(hotspotX * effectiveScale), result.width - 1);
    result.hotspotY = qBound(
            0, qRound(hotspotY * effectiveScale), result.height - 1);
    return result;
}

bool AppleDisplayLayout::isUsable() const
{
    return scaledWidth > 0 && scaledHeight > 0 && backingWidth > 0 &&
            backingHeight > 0 && !displays.isEmpty() && displays.size() <= 2;
}

AppleControlEvents AppleControlEventParser::parse(const QByteArray& message)
{
    AppleControlEvents events;
    if (message.size() < 4 || byteAt(message, 0) != 0) {
        return events;
    }
    bool ok = false;
    const int rectangleCount = AppleWire::readUInt16(message, 2, &ok);
    if (!ok) {
        return events;
    }
    int offset = 4;
    for (int rectangle = 0; rectangle < rectangleCount; ++rectangle) {
        if (offset > message.size() - 12) {
            break;
        }
        const int hotspotX = AppleWire::readUInt16(message, offset, &ok);
        if (!ok) break;
        const int hotspotY = AppleWire::readUInt16(message, offset + 2, &ok);
        if (!ok) break;
        const int width = AppleWire::readUInt16(message, offset + 4, &ok);
        if (!ok) break;
        const int height = AppleWire::readUInt16(message, offset + 6, &ok);
        if (!ok) break;
        const qint32 encoding = AppleWire::readInt32(message, offset + 8, &ok);
        if (!ok) break;
        offset += 12;

        if (encoding == CursorEncoding) {
            if (offset > message.size() - 8) {
                break;
            }
            const quint32 id = AppleWire::readUInt32(message, offset, &ok);
            if (!ok) break;
            const quint32 rawLength = AppleWire::readUInt32(message, offset + 4, &ok);
            if (!ok || rawLength > static_cast<quint32>(message.size() - offset - 8)) {
                break;
            }
            const int length = static_cast<int>(rawLength);
            const int payloadOffset = offset + 8;
            if (length == 0) {
                events.cursorUpdates.append(
                        {AppleCursorUpdate::Kind::Select, id, {}});
            }
            else if (width > 0 && height > 0 &&
                     width <= MaximumCursorDimension &&
                     height <= MaximumCursorDimension &&
                     hotspotX < width && hotspotY < height &&
                     width <= std::numeric_limits<int>::max() / height / 5) {
                const int pixels = width * height;
                const QByteArray raw = inflateSyncFlush(
                        message.mid(payloadOffset, length), pixels * 5,
                        MaximumCursorDimension * MaximumCursorDimension * 5);
                if (raw.size() == pixels * 5) {
                    AppleCursorImage image;
                    image.width = width;
                    image.height = height;
                    image.hotspotX = hotspotX;
                    image.hotspotY = hotspotY;
                    image.rgba.resize(pixels * 4);
                    for (int pixel = 0; pixel < pixels; ++pixel) {
                        const int source = pixel * 4;
                        image.rgba[source] = raw.at(source + 2);
                        image.rgba[source + 1] = raw.at(source + 1);
                        image.rgba[source + 2] = raw.at(source);
                        image.rgba[source + 3] = raw.at(pixels * 4 + pixel);
                    }
                    events.cursorUpdates.append(
                            {AppleCursorUpdate::Kind::Store, id, image});
                }
            }
            offset = payloadOffset + length;
            continue;
        }

        if (encoding != DisplayLayoutEncoding &&
                !isLengthPrefixedEncoding(encoding)) {
            break;
        }
        if (offset > message.size() - 2) {
            break;
        }
        const int length = AppleWire::readUInt16(message, offset, &ok);
        if (!ok || length > message.size() - offset - 2) {
            break;
        }
        const int payloadOffset = offset + 2;
        if (encoding == DisplayLayoutEncoding) {
            const AppleDisplayLayout layout = parseDisplayLayout(
                    message.mid(payloadOffset, length));
            if (layout.isUsable()) {
                events.displayLayouts.append(layout);
            }
        }
        offset = payloadOffset + length;
    }
    return events;
}

QList<QByteArray> AppleTextClipboardExchange::setEligible(bool eligible)
{
    if (m_Eligible == eligible) {
        return {};
    }
    m_Eligible = eligible;
    if (!eligible) {
        m_Reassembly.clear();
        m_ExpectedLength = 0;
        m_RequestState = RequestState::Idle;
        m_RequestSessionId = 0;
    }
    return {eligible
                    ? QByteArray::fromHex("1500000100000000")
                    : QByteArray::fromHex("1500000200000000")};
}

QList<QByteArray> AppleTextClipboardExchange::advertiseLocalText(
        const QString& text,
        QString* error)
{
    m_LocalText = text;
    m_HasLocalText = true;
    if (!m_Eligible) {
        return {};
    }
    return encodeText(text, true, 0, error);
}

AppleTextClipboardResult AppleTextClipboardExchange::receive(
        const QByteArray& message,
        QString* error)
{
    AppleTextClipboardResult result;
    if (message.isEmpty()) {
        return result;
    }

    if (!m_Reassembly.isEmpty() || byteAt(message, 0) == 0x1f) {
        result.consumed = true;
        if (m_Reassembly.size() > MaximumCompressedBytes + 16 - message.size()) {
            m_Reassembly.clear();
            m_ExpectedLength = 0;
            m_RequestState = RequestState::Idle;
            setError(error, QCoreApplication::translate(
                    "AppleTextClipboardExchange",
                    "The remote clipboard payload is too large."));
            return result;
        }
        m_Reassembly.append(message);
        if (m_ExpectedLength == 0 && m_Reassembly.size() >= 16) {
            if (byteAt(m_Reassembly, 0) != 0x1f) {
                m_Reassembly.clear();
                return result;
            }
            bool ok = false;
            const quint32 compressedSize = AppleWire::readUInt32(
                    m_Reassembly, 12, &ok);
            if (!ok || compressedSize > MaximumCompressedBytes) {
                m_Reassembly.clear();
                setError(error, QCoreApplication::translate(
                        "AppleTextClipboardExchange",
                        "The remote clipboard payload is too large."));
                return result;
            }
            m_ExpectedLength = 16 + static_cast<int>(compressedSize);
        }
        if (m_ExpectedLength == 0) {
            return result;
        }
        if (m_Reassembly.size() > m_ExpectedLength) {
            m_Reassembly.clear();
            m_ExpectedLength = 0;
            m_RequestState = RequestState::Idle;
            setError(error, QCoreApplication::translate(
                    "AppleTextClipboardExchange",
                    "The remote clipboard fragments are malformed."));
            return result;
        }
        if (m_Reassembly.size() < m_ExpectedLength) {
            return result;
        }
        const QByteArray complete = std::move(m_Reassembly);
        m_Reassembly.clear();
        m_ExpectedLength = 0;
        quint32 sessionId = 0;
        bool promises = false;
        std::optional<QString> text;
        if (!decodeEnvelope(complete, &sessionId, &promises, &text, error)) {
            m_RequestState = RequestState::Idle;
            return result;
        }
        if (m_RequestState == RequestState::AwaitingPromises && promises &&
                sessionId == m_RequestSessionId) {
            result.outboundMessages.append(request(false, 0));
            m_RequestState = RequestState::AwaitingData;
        }
        else if (m_RequestState == RequestState::AwaitingData && !promises &&
                 sessionId == 0) {
            if (m_Eligible && text.has_value()) {
                result.receivedText = std::move(text);
            }
            m_RequestState = RequestState::Idle;
            m_RequestSessionId = 0;
        }
        return result;
    }

    if (byteAt(message, 0) == 0x0b && message.size() >= 8) {
        result.consumed = true;
        if (!m_Eligible || !m_HasLocalText) {
            return result;
        }
        const bool promises = byteAt(message, 1) != 0;
        bool ok = false;
        const quint32 sessionId = AppleWire::readUInt32(message, 4, &ok);
        if (ok) {
            result.outboundMessages = encodeText(
                    m_LocalText, promises, sessionId, error);
        }
        return result;
    }

    if (byteAt(message, 0) == 0x14 && message.size() >= 8) {
        bool ok = false;
        const quint16 subtype = AppleWire::readUInt16(message, 6, &ok);
        if (!ok || (subtype != 2 && subtype != 3)) {
            return result;
        }
        result.consumed = true;
        if (!m_Eligible) {
            return result;
        }
        if (subtype == 2 && m_RequestState == RequestState::Idle) {
            m_RequestSessionId = nextSessionId();
            m_RequestState = RequestState::AwaitingPromises;
            result.outboundMessages.append(request(true, m_RequestSessionId));
        }
        else if (subtype == 3 && m_HasLocalText) {
            result.outboundMessages = encodeText(m_LocalText, false, 0, error);
        }
    }
    return result;
}

void AppleTextClipboardExchange::resetForReconnect()
{
    m_Reassembly.clear();
    m_ExpectedLength = 0;
    m_RequestSessionId = 0;
    m_RequestState = RequestState::Idle;
    m_Eligible = false;
}

QByteArray AppleTextClipboardExchange::request(bool promises, quint32 sessionId)
{
    QByteArray message(4, '\0');
    message[0] = char(0x0b);
    message[1] = promises ? char(1) : char(0);
    AppleWire::appendUInt32(message, sessionId);
    return message;
}

QList<QByteArray> AppleTextClipboardExchange::encodeText(
        const QString& text,
        bool promises,
        quint32 sessionId,
        QString* error)
{
    const QByteArray value = text.toUtf8();
    QByteArray archive;
    AppleWire::appendUInt32(archive, 1);
    AppleWire::appendUInt32(archive, static_cast<quint32>(TextFlavor.size()));
    archive.append(TextFlavor);
    AppleWire::appendUInt32(archive, 0);
    AppleWire::appendUInt32(archive, 0);
    AppleWire::appendUInt32(archive, promises ? 0 : static_cast<quint32>(value.size()));
    if (!promises) {
        archive.append(value);
    }
    if (archive.size() > MaximumArchiveBytes) {
        setError(error, QCoreApplication::translate(
                "AppleTextClipboardExchange", "The local clipboard text is too large."));
        return {};
    }
    const QByteArray compressed = deflateSyncFlush(archive, MaximumCompressedBytes);
    if (compressed.isEmpty()) {
        setError(error, QCoreApplication::translate(
                "AppleTextClipboardExchange", "The local clipboard text could not be compressed."));
        return {};
    }
    QByteArray message(4, '\0');
    message[0] = char(0x1f);
    message[2] = promises ? char(1) : char(0);
    AppleWire::appendUInt32(message, sessionId);
    AppleWire::appendUInt32(message, static_cast<quint32>(archive.size()));
    AppleWire::appendUInt32(message, static_cast<quint32>(compressed.size()));
    message.append(compressed);
    return fragments(message);
}

bool AppleTextClipboardExchange::decodeEnvelope(
        const QByteArray& message,
        quint32* sessionId,
        bool* containsPromises,
        std::optional<QString>* text,
        QString* error)
{
    if (message.size() < 16 || byteAt(message, 0) != 0x1f) {
        return false;
    }
    bool ok = false;
    const quint32 rawSessionId = AppleWire::readUInt32(message, 4, &ok);
    if (!ok) return false;
    const quint32 rawArchiveSize = AppleWire::readUInt32(message, 8, &ok);
    if (!ok || rawArchiveSize > MaximumArchiveBytes) return false;
    const quint32 rawCompressedSize = AppleWire::readUInt32(message, 12, &ok);
    if (!ok || rawCompressedSize > MaximumCompressedBytes ||
            rawCompressedSize > static_cast<quint32>(message.size() - 16)) {
        return false;
    }
    const QByteArray archive = inflateSyncFlush(
            message.mid(16, static_cast<int>(rawCompressedSize)),
            static_cast<int>(rawArchiveSize), MaximumArchiveBytes);
    if (archive.size() != static_cast<int>(rawArchiveSize)) {
        setError(error, QCoreApplication::translate(
                "AppleTextClipboardExchange",
                "The remote clipboard payload could not be decompressed."));
        return false;
    }

    int offset = 0;
    int itemCount = 0;
    int flavorTotal = 0;
    int aliasTotal = 0;
    int metadataBytes = 0;
    std::optional<QString> decodedText;
    while (offset < archive.size()) {
        if (++itemCount > MaximumClipboardItems || offset > archive.size() - 4) {
            return false;
        }
        const quint32 flavorCount = AppleWire::readUInt32(archive, offset, &ok);
        offset += 4;
        if (!ok) return false;
        if (flavorCount == 0) break;
        if (flavorCount > static_cast<quint32>(MaximumClipboardFlavors - flavorTotal)) {
            return false;
        }
        flavorTotal += static_cast<int>(flavorCount);
        for (quint32 flavor = 0; flavor < flavorCount; ++flavor) {
            QByteArray type;
            if (!readMetadata(archive, &offset, &metadataBytes, &type) ||
                    offset > archive.size() - 8) {
                return false;
            }
            offset += 4; // reserved
            const quint32 aliasCount = AppleWire::readUInt32(archive, offset, &ok);
            offset += 4;
            if (!ok || aliasCount > static_cast<quint32>(
                        MaximumClipboardAliases - aliasTotal) ||
                    (type.isEmpty() && aliasCount == 0)) {
                return false;
            }
            aliasTotal += static_cast<int>(aliasCount);
            for (quint32 alias = 0; alias < aliasCount; ++alias) {
                if (!readMetadata(archive, &offset, &metadataBytes, nullptr) ||
                        !readMetadata(archive, &offset, &metadataBytes, nullptr)) {
                    return false;
                }
            }
            QByteArray value;
            if (!readBlob(archive, &offset, &value)) {
                return false;
            }
            if (!decodedText.has_value() && type == TextFlavor) {
                const QString candidate = QString::fromUtf8(value.constData(), value.size());
                if (candidate.toUtf8() != value || value.contains('\0')) {
                    return false;
                }
                decodedText = candidate;
            }
        }
    }
    if (sessionId != nullptr) *sessionId = rawSessionId;
    if (containsPromises != nullptr) *containsPromises = byteAt(message, 2) != 0;
    if (text != nullptr) *text = std::move(decodedText);
    return true;
}

QList<QByteArray> AppleTextClipboardExchange::fragments(const QByteArray& message)
{
    QList<QByteArray> result;
    for (int offset = 0; offset < message.size(); offset += FragmentBytes) {
        result.append(message.mid(offset, FragmentBytes));
    }
    return result;
}

quint32 AppleTextClipboardExchange::nextSessionId()
{
    quint32 result = m_NextSessionId++ & 0x7fffffffU;
    if (result == 0) {
        result = 1;
        m_NextSessionId = 2;
    }
    return result;
}
