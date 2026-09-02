#include "applefiletransfer.h"

#include "appleprotocol.h"

#include <zlib.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace {

constexpr int MaximumClipboardItems = 1024;
constexpr int MaximumClipboardFlavors = 4096;
constexpr int MaximumClipboardAliases = 8192;
constexpr int MaximumClipboardMetadataBytes = 1024 * 1024;
constexpr int MaximumClipboardMetadataLength = 4096;
const QByteArray FileUrlFlavor = QByteArrayLiteral("public.file-url");
const QByteArray DragImageFlavor =
        QByteArrayLiteral("com.apple.remotedesktop.dragimage-png");
const QByteArray OSTypeTagClass = QByteArrayLiteral("com.apple.ostype");
const QByteArray FileUrlTag = QByteArrayLiteral("furl");

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
}

bool fail(QString* error, const char* value)
{
    setError(error, QString::fromUtf8(value));
    return false;
}

quint8 byteAt(const QByteArray& data, int offset)
{
    return static_cast<quint8>(data.at(offset));
}

void appendUInt64(QByteArray& data, quint64 value)
{
    AppleWire::appendUInt32(data, static_cast<quint32>(value >> 32));
    AppleWire::appendUInt32(data, static_cast<quint32>(value));
}

void appendUInt32Le(QByteArray& data, quint32 value)
{
    data.append(static_cast<char>(value));
    data.append(static_cast<char>(value >> 8));
    data.append(static_cast<char>(value >> 16));
    data.append(static_cast<char>(value >> 24));
}

quint16 readUInt16Le(const QByteArray& data, int offset, bool* ok = nullptr)
{
    const bool valid = offset >= 0 && offset <= data.size() - 2;
    if (ok != nullptr) *ok = valid;
    if (!valid) return 0;
    return static_cast<quint16>(byteAt(data, offset)) |
            static_cast<quint16>(byteAt(data, offset + 1) << 8);
}

quint32 readUInt32Le(const QByteArray& data, int offset, bool* ok = nullptr)
{
    const bool valid = offset >= 0 && offset <= data.size() - 4;
    if (ok != nullptr) *ok = valid;
    if (!valid) return 0;
    return static_cast<quint32>(byteAt(data, offset)) |
            (static_cast<quint32>(byteAt(data, offset + 1)) << 8) |
            (static_cast<quint32>(byteAt(data, offset + 2)) << 16) |
            (static_cast<quint32>(byteAt(data, offset + 3)) << 24);
}

quint64 readUInt64(const QByteArray& data, int offset, bool* ok = nullptr)
{
    bool highOk = false;
    bool lowOk = false;
    const quint32 high = AppleWire::readUInt32(data, offset, &highOk);
    const quint32 low = AppleWire::readUInt32(data, offset + 4, &lowOk);
    if (ok != nullptr) *ok = highOk && lowOk;
    return highOk && lowOk
            ? (static_cast<quint64>(high) << 32) | low : 0;
}

qint32 readInt32Le(const QByteArray& data, int offset, bool* ok = nullptr)
{
    return static_cast<qint32>(readUInt32Le(data, offset, ok));
}

bool appendField(QByteArray& data, const QByteArray& value, QString* error)
{
    if (value.size() < 0 ||
            static_cast<quint64>(value.size()) >
                    std::numeric_limits<quint32>::max()) {
        return fail(error, "The file transfer field is too large.");
    }
    AppleWire::appendUInt32(data, static_cast<quint32>(value.size()));
    data.append(value);
    return true;
}

bool readBlob(const QByteArray& data, int* offset, QByteArray* value)
{
    if (offset == nullptr || *offset < 0 || *offset > data.size() - 4) {
        return false;
    }
    bool ok = false;
    const quint32 length = AppleWire::readUInt32(data, *offset, &ok);
    *offset += 4;
    if (!ok || length > static_cast<quint32>(data.size() - *offset)) {
        return false;
    }
    if (value != nullptr) {
        *value = data.mid(*offset, static_cast<int>(length));
    }
    *offset += static_cast<int>(length);
    return true;
}

bool readMetadata(const QByteArray& data,
                  int* offset,
                  int* totalBytes,
                  QByteArray* value)
{
    QByteArray candidate;
    if (totalBytes == nullptr || !readBlob(data, offset, &candidate) ||
            candidate.size() > MaximumClipboardMetadataLength ||
            *totalBytes > MaximumClipboardMetadataBytes - candidate.size()) {
        return false;
    }
    *totalBytes += candidate.size();
    if (value != nullptr) *value = std::move(candidate);
    return true;
}

QByteArray deflateSyncFlush(const QByteArray& input, int maximumSize)
{
    z_stream stream = {};
    if (deflateInit(&stream, Z_BEST_COMPRESSION) != Z_OK) {
        return {};
    }
    stream.next_in = reinterpret_cast<Bytef*>(
            const_cast<char*>(input.constData()));
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

QByteArray inflateSyncFlush(const QByteArray& compressed,
                            int expectedSize,
                            int maximumSize)
{
    if (compressed.isEmpty() || expectedSize <= 0 ||
            expectedSize > maximumSize) {
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

QByteArray fileCopyMessage(const QByteArray& body)
{
    QByteArray message;
    message.append(char(0x22));
    message.append(char(0));
    AppleWire::appendUInt32(message, static_cast<quint32>(body.size()));
    message.append(body);
    return message;
}

QByteArray senderBody(quint16 command, quint32 sessionId)
{
    QByteArray body;
    AppleWire::appendUInt16(body, 1);
    AppleWire::appendUInt16(body, command);
    AppleWire::appendUInt32(body, sessionId);
    return body;
}

void appendDate(QByteArray& data, const AppleFileCopyUtcDateTime& value)
{
    AppleWire::appendUInt16(data,
                            static_cast<quint16>(value.seconds >> 32));
    AppleWire::appendUInt32(data, static_cast<quint32>(value.seconds));
    AppleWire::appendUInt16(data, value.fraction);
}

bool readDate(const QByteArray& data,
              int offset,
              AppleFileCopyUtcDateTime* value)
{
    bool highOk = false;
    bool lowOk = false;
    bool fractionOk = false;
    const quint16 high = AppleWire::readUInt16(data, offset, &highOk);
    const quint32 low = AppleWire::readUInt32(data, offset + 2, &lowOk);
    const quint16 fraction = AppleWire::readUInt16(
            data, offset + 6, &fractionOk);
    if (!highOk || !lowOk || !fractionOk || value == nullptr) {
        return false;
    }
    value->seconds = (static_cast<quint64>(high) << 32) | low;
    value->fraction = fraction;
    return true;
}

struct FileCopyEnvelope
{
    quint16 command = 0;
    quint32 sessionId = 0;
    QByteArray body;
};

bool parseEnvelope(const QByteArray& message,
                   FileCopyEnvelope* envelope,
                   QString* error)
{
    if (envelope == nullptr || message.size() < 14 ||
            byteAt(message, 0) != 0x22 || byteAt(message, 1) != 0) {
        return fail(error, "The Mac returned an invalid file transfer message.");
    }
    bool lengthOk = false;
    const quint32 bodyLength = AppleWire::readUInt32(message, 2, &lengthOk);
    if (!lengthOk || bodyLength < 8 ||
            bodyLength > AppleFileTransferProtocol::MaximumFileCopyBodyLength ||
            bodyLength != static_cast<quint32>(message.size() - 6)) {
        return fail(error, "The Mac returned an invalid file transfer length.");
    }
    envelope->body = message.mid(6);
    bool versionOk = false;
    bool commandOk = false;
    bool sessionOk = false;
    const quint16 version = AppleWire::readUInt16(
            envelope->body, 0, &versionOk);
    envelope->command = AppleWire::readUInt16(
            envelope->body, 2, &commandOk);
    envelope->sessionId = AppleWire::readUInt32(
            envelope->body, 4, &sessionOk);
    if (!versionOk || !commandOk || !sessionOk || version != 1 ||
            envelope->sessionId == 0) {
        return fail(error, "The Mac returned an invalid file transfer envelope.");
    }
    return true;
}

QByteArray startFileCopy(quint16 command,
                         quint32 direction,
                         quint32 sessionId,
                         const QString& path,
                         int maximumPathLength,
                         QString* error)
{
    const QByteArray encoded = path.toUtf8();
    if (sessionId == 0 || encoded.isEmpty() || encoded.contains('\0') ||
            encoded.size() > maximumPathLength ||
            encoded.size() > std::numeric_limits<quint16>::max()) {
        setError(error, QStringLiteral("The file transfer path is invalid."));
        return {};
    }
    QByteArray body = senderBody(command, sessionId);
    AppleWire::appendUInt32(body, direction);
    AppleWire::appendUInt32(body, 0);
    AppleWire::appendUInt16(body, static_cast<quint16>(encoded.size()));
    body.append(encoded);
    body.append(char(0));
    return fileCopyMessage(body);
}

bool isStrictUtf8(const QByteArray& encoded, QString* decoded)
{
    if (encoded.contains('\0')) return false;
    const QString value = QString::fromUtf8(encoded);
    if (value.toUtf8() != encoded) return false;
    if (decoded != nullptr) *decoded = value;
    return true;
}

QByteArray encodeExtendedAttributes(
        const QList<AppleFileCopyExtendedAttribute>& attributes,
        QString* error)
{
    if (attributes.isEmpty()) return {};
    struct EncodedAttribute
    {
        QByteArray name;
        QByteArray value;
    };
    QList<EncodedAttribute> encoded;
    for (const AppleFileCopyExtendedAttribute& attribute : attributes) {
        const QByteArray name = attribute.name.toUtf8();
        if (name.isEmpty() || name.contains('\0')) {
            setError(error, QStringLiteral("A file attribute name is invalid."));
            return {};
        }
        encoded.append({name, attribute.value});
    }

    QByteArray blob;
    AppleWire::appendUInt32(blob, 0);
    AppleWire::appendUInt32(blob, static_cast<quint32>(encoded.size()));
    for (const EncodedAttribute& attribute : std::as_const(encoded)) {
        AppleWire::appendUInt32(
                blob, static_cast<quint32>(attribute.name.size() + 1));
        AppleWire::appendUInt32(
                blob, static_cast<quint32>(attribute.value.size()));
    }
    for (const EncodedAttribute& attribute : std::as_const(encoded)) {
        blob.append(attribute.name);
        blob.append(char(0));
        blob.append(attribute.value);
    }
    if (blob.size() > std::numeric_limits<quint16>::max() - 10) {
        return {};
    }
    QByteArray sizeBytes;
    AppleWire::appendUInt32(sizeBytes, static_cast<quint32>(blob.size()));
    blob.replace(0, 4, sizeBytes);
    QByteArray record = QByteArrayLiteral("ext1");
    AppleWire::appendUInt16(record, 0);
    AppleWire::appendUInt16(record, 1);
    AppleWire::appendUInt16(record,
                            static_cast<quint16>(blob.size() + 10));
    record.append(blob);
    return record;
}

bool parseExtendedAttributes(
        const QByteArray& record,
        QList<AppleFileCopyExtendedAttribute>* attributes)
{
    if (attributes == nullptr) return false;
    attributes->clear();
    if (record.isEmpty()) return true;
    if (record.size() < 18 || record.left(4) != QByteArrayLiteral("ext1")) {
        return false;
    }
    bool ok = false;
    const int recordLength = AppleWire::readUInt16(record, 8, &ok);
    if (!ok || recordLength < 18 || recordLength > record.size()) return false;
    const quint32 blobLengthRaw = AppleWire::readUInt32(record, 10, &ok);
    if (!ok || blobLengthRaw < 8 ||
            blobLengthRaw > static_cast<quint32>(recordLength - 10)) {
        return false;
    }
    const int blobLength = static_cast<int>(blobLengthRaw);
    const quint32 countRaw = AppleWire::readUInt32(record, 14, &ok);
    if (!ok || countRaw > 50'000 ||
            countRaw > static_cast<quint32>((blobLength - 8) / 8)) {
        return false;
    }
    struct Header { int nameLength; int valueLength; };
    QList<Header> headers;
    int headerOffset = 18;
    for (quint32 index = 0; index < countRaw; ++index) {
        const quint32 nameLength = AppleWire::readUInt32(
                record, headerOffset, &ok);
        if (!ok) return false;
        const quint32 valueLength = AppleWire::readUInt32(
                record, headerOffset + 4, &ok);
        if (!ok || nameLength == 0 ||
                nameLength > static_cast<quint32>(std::numeric_limits<int>::max()) ||
                valueLength > static_cast<quint32>(std::numeric_limits<int>::max())) {
            return false;
        }
        headers.append({static_cast<int>(nameLength),
                        static_cast<int>(valueLength)});
        headerOffset += 8;
    }
    const int blobEnd = 10 + blobLength;
    int dataOffset = headerOffset;
    for (const Header& header : std::as_const(headers)) {
        if (header.nameLength > blobEnd - dataOffset) return false;
        const int nameEnd = dataOffset + header.nameLength;
        if (byteAt(record, nameEnd - 1) != 0 ||
                header.valueLength > blobEnd - nameEnd) {
            return false;
        }
        QString name;
        if (!isStrictUtf8(record.mid(dataOffset, header.nameLength - 1),
                          &name) || name.isEmpty()) {
            return false;
        }
        attributes->append({name,
                            record.mid(nameEnd, header.valueLength)});
        dataOffset = nameEnd + header.valueLength;
    }
    return dataOffset <= blobEnd;
}

} // namespace

QByteArray AppleFileTransferProtocol::beginDrop(
        const QList<QUrl>& fileUrls,
        quint32 sessionId,
        QString* error)
{
    if (sessionId == 0 || fileUrls.isEmpty() ||
            fileUrls.size() > MaximumClipboardItems) {
        setError(error, QStringLiteral("The file drop is invalid."));
        return {};
    }
    QByteArray archive;
    for (const QUrl& input : fileUrls) {
        if (!input.isLocalFile()) {
            setError(error, QStringLiteral("Only local files can be transferred."));
            return {};
        }
        const QUrl url = input.adjusted(QUrl::NormalizePathSegments);
        const QByteArray value = url.toEncoded(QUrl::FullyEncoded);
        AppleWire::appendUInt32(archive, 1);
        if (!appendField(archive, FileUrlFlavor, error)) return {};
        AppleWire::appendUInt32(archive, 0);
        AppleWire::appendUInt32(archive, 1);
        if (!appendField(archive, OSTypeTagClass, error) ||
                !appendField(archive, FileUrlTag, error) ||
                !appendField(archive, value, error)) {
            return {};
        }
    }
    const QByteArray compressed = deflateSyncFlush(
            archive, MaximumDropPayloadLength);
    if (compressed.isEmpty() || archive.size() > MaximumDropPayloadLength) {
        setError(error, QStringLiteral("The file drop payload is too large."));
        return {};
    }
    QByteArray message(4, '\0');
    message[0] = char(0x20);
    AppleWire::appendUInt32(message, sessionId);
    AppleWire::appendUInt32(message, static_cast<quint32>(archive.size()));
    AppleWire::appendUInt32(message, static_cast<quint32>(compressed.size()));
    message.append(compressed);
    return message;
}

QByteArray AppleFileTransferProtocol::cancelDrop(
        quint32 sessionId, QString* error)
{
    if (sessionId == 0) {
        setError(error, QStringLiteral("The file drop session is invalid."));
        return {};
    }
    QByteArray message(4, '\0');
    message[0] = char(0x20);
    AppleWire::appendUInt32(message, sessionId);
    AppleWire::appendUInt32(message, 0);
    AppleWire::appendUInt32(message, 0);
    return message;
}

bool AppleFileTransferProtocol::parseFileRequest(
        const QByteArray& message,
        AppleFileTransferRequest* request,
        QString* error)
{
    if (request == nullptr || message.size() < 12 ||
            byteAt(message, 0) != 0x1e) {
        return fail(error, "The Mac returned an invalid file request.");
    }
    bool sessionOk = false;
    bool lengthOk = false;
    const quint32 sessionId = AppleWire::readUInt32(message, 4, &sessionOk);
    const quint32 pathLength = AppleWire::readUInt32(message, 8, &lengthOk);
    if (!sessionOk || !lengthOk || sessionId == 0 || pathLength == 0 ||
            pathLength > MaximumDestinationPathLength ||
            pathLength != static_cast<quint32>(message.size() - 12)) {
        return fail(error, "The Mac returned an invalid file request length.");
    }
    QString path;
    if (!isStrictUtf8(message.mid(12), &path) || path.isEmpty()) {
        return fail(error, "The Mac returned an invalid destination path.");
    }
    request->sessionId = sessionId;
    request->destinationPath = path;
    return true;
}

QByteArray AppleFileTransferProtocol::startFileReceive(
        quint32 sessionId, const QString& destinationPath, QString* error)
{
    return startFileCopy(2, 0, sessionId, destinationPath,
                         MaximumDestinationPathLength, error);
}

QByteArray AppleFileTransferProtocol::startFileSend(
        quint32 sessionId, const QString& sourcePath, QString* error)
{
    return startFileCopy(1, 1, sessionId, sourcePath,
                         MaximumRemoteSourcePathLength, error);
}

QByteArray AppleFileTransferProtocol::control(
        quint32 sessionId, AppleFileTransferControl action, QString* error)
{
    if (sessionId == 0) {
        setError(error, QStringLiteral("The file transfer session is invalid."));
        return {};
    }
    QByteArray body = senderBody(static_cast<quint16>(action), sessionId);
    return fileCopyMessage(body);
}

QByteArray AppleFileTransferProtocol::completion(
        quint32 sessionId,
        quint16 errorCode,
        const QString& name,
        QString* error)
{
    const QByteArray encoded = name.toUtf8();
    if (sessionId == 0 ||
            (errorCode != 0 && errorCode != 3 && errorCode != 5) ||
            encoded.contains('\0') || encoded.size() > MaximumCompletionNameLength) {
        setError(error, QStringLiteral("The file completion response is invalid."));
        return {};
    }
    QByteArray body = senderBody(200, sessionId);
    AppleWire::appendUInt16(body, errorCode);
    AppleWire::appendUInt16(body, static_cast<quint16>(encoded.size()));
    body.append(encoded);
    body.append(char(0));
    return fileCopyMessage(body);
}

QByteArray AppleFileTransferProtocol::progress(
        quint32 sessionId, double fraction, QString* error)
{
    if (sessionId == 0 || !std::isfinite(fraction) ||
            fraction < 0.0 || fraction > 1.0) {
        setError(error, QStringLiteral("The file transfer progress is invalid."));
        return {};
    }
    quint64 bits = 0;
    static_assert(sizeof(bits) == sizeof(fraction), "double must be 64-bit");
    std::memcpy(&bits, &fraction, sizeof(bits));
    QByteArray body = senderBody(300, sessionId);
    appendUInt64(body, bits);
    return fileCopyMessage(body);
}

QByteArray AppleFileTransferProtocol::senderSummary(
        quint32 sessionId,
        const AppleFileCopySummary& summary,
        QString* error)
{
    if (sessionId == 0 ||
            (summary.rootIsFile
                     ? (summary.fileCount != 1 || summary.folderCount != 0)
                     : summary.folderCount == 0)) {
        setError(error, QStringLiteral("The file transfer summary is invalid."));
        return {};
    }
    QByteArray body = senderBody(100, sessionId);
    AppleWire::appendUInt32(body, summary.rootIsFile ? 1 : 0);
    AppleWire::appendUInt32(body, 0);
    appendUInt64(body, summary.logicalBytes);
    appendUInt64(body, summary.physicalBytes);
    appendUInt64(body, summary.fileCount);
    appendUInt64(body, 0);
    appendUInt64(body, summary.folderCount);
    AppleWire::appendUInt32(body, 0);
    return fileCopyMessage(body);
}

QByteArray AppleFileTransferProtocol::senderItem(
        quint32 sessionId,
        const AppleFileCopyItemMetadata& item,
        QString* error)
{
    const QByteArray name = item.name.normalized(
            QString::NormalizationForm_C).toUtf8();
    const bool symbolicLink =
            item.type == AppleFileCopyItemType::SymbolicLink;
    if (sessionId == 0 || item.finderInfo.size() != 32 ||
            name.isEmpty() || name.contains('\0') ||
            name.size() > std::numeric_limits<quint16>::max() ||
            symbolicLink == item.symbolicLinkTarget.isEmpty() ||
            item.symbolicLinkTarget.size() >
                    std::numeric_limits<quint16>::max() ||
            (symbolicLink && name.size() + 1 +
                    item.symbolicLinkTarget.size() > 1279)) {
        setError(error, QStringLiteral("The file item metadata is invalid."));
        return {};
    }

    QByteArray body = senderBody(101, sessionId);
    body.append(static_cast<char>(item.type));
    body.append(static_cast<char>(item.userAccess));
    QByteArray finderInfo = item.finderInfo;
    const char finderFlagsHigh = finderInfo.at(24);
    finderInfo[24] = finderInfo.at(25);
    finderInfo[25] = finderFlagsHigh;
    body.append(finderInfo);
    appendUInt64(body, item.resourceForkSize);
    appendUInt64(body, item.dataForkSize);
    appendDate(body, item.creationDate);
    appendDate(body, item.contentModificationDate);
    appendDate(body, item.attributeModificationDate);
    appendDate(body, item.accessDate);
    appendDate(body, item.backupDate);
    AppleWire::appendUInt16(body, item.nodeFlags);
    AppleWire::appendUInt16(body, item.level);
    AppleWire::appendUInt16(body, item.mode);
    AppleWire::appendUInt32(body, item.textEncodingHint);
    AppleWire::appendUInt16(body, static_cast<quint16>(name.size()));
    AppleWire::appendUInt16(
            body,
            symbolicLink
                    ? static_cast<quint16>(item.symbolicLinkTarget.size() - 1)
                    : 0);
    body.append(name);
    body.append(char(0));
    body.append(item.symbolicLinkTarget);
    if (!item.extendedAttributes.isEmpty()) {
        const QByteArray attributes = encodeExtendedAttributes(
                item.extendedAttributes, error);
        if (attributes.isEmpty() && error != nullptr && !error->isEmpty()) {
            return {};
        }
        if (body.size() + attributes.size() <= 0xefff) {
            body.append(attributes);
        }
    }
    if (body.size() > MaximumFileCopyBodyLength) {
        setError(error, QStringLiteral("The file item metadata is too large."));
        return {};
    }
    return fileCopyMessage(body);
}

QByteArray AppleFileTransferProtocol::senderData(
        quint32 sessionId, const QByteArray& data, QString* error)
{
    if (sessionId == 0 || data.isEmpty() ||
            data.size() > MaximumDataBlockLength) {
        setError(error, QStringLiteral("The file data block is invalid."));
        return {};
    }
    QByteArray body = senderBody(102, sessionId);
    AppleWire::appendUInt32(body, static_cast<quint32>(data.size()));
    body.append(data);
    return fileCopyMessage(body);
}

QByteArray AppleFileTransferProtocol::senderCompressedData(
        quint32 sessionId,
        int uncompressedLength,
        const QByteArray& data,
        QString* error)
{
    if (sessionId == 0 || uncompressedLength <= 0 ||
            uncompressedLength > MaximumDataBlockLength || data.isEmpty() ||
            data.size() > MaximumFileCopyBodyLength - 18) {
        setError(error, QStringLiteral("The compressed file block is invalid."));
        return {};
    }
    QByteArray body = senderBody(103, sessionId);
    AppleWire::appendUInt16(body, 1);
    AppleWire::appendUInt32(body,
                            static_cast<quint32>(uncompressedLength));
    AppleWire::appendUInt32(body, static_cast<quint32>(data.size()));
    body.append(data);
    return fileCopyMessage(body);
}

QByteArray AppleFileTransferProtocol::senderDone(
        quint32 sessionId, qint32 status, QString* error)
{
    if (sessionId == 0) {
        setError(error, QStringLiteral("The file transfer session is invalid."));
        return {};
    }
    QByteArray body = senderBody(104, sessionId);
    appendUInt32Le(body, static_cast<quint32>(status));
    return fileCopyMessage(body);
}

bool AppleFileTransferProtocol::parseResponse(
        const QByteArray& message,
        AppleFileTransferResponse* response,
        QString* error)
{
    if (response == nullptr) {
        return fail(error, "The file transfer response target is missing.");
    }
    FileCopyEnvelope envelope;
    if (!parseEnvelope(message, &envelope, error)) return false;
    bool ok = false;
    if (envelope.command == 200) {
        if (envelope.body.size() < 13) {
            return fail(error, "The file completion response is truncated.");
        }
        const quint16 errorCode = AppleWire::readUInt16(
                envelope.body, 8, &ok);
        if (!ok) return false;
        const quint16 nameLength = AppleWire::readUInt16(
                envelope.body, 10, &ok);
        if (!ok || envelope.body.size() != 13 + nameLength ||
                byteAt(envelope.body, 12 + nameLength) != 0) {
            return fail(error, "The file completion response is invalid.");
        }
        QString name;
        if (!isStrictUtf8(envelope.body.mid(12, nameLength), &name)) {
            return fail(error, "The file completion name is invalid.");
        }
        response->kind = AppleFileTransferResponse::Kind::Completed;
        response->sessionId = envelope.sessionId;
        response->errorCode = errorCode;
        response->name = name;
        response->fraction = 0.0;
        return true;
    }
    if (envelope.command == 300) {
        if (envelope.body.size() != 16) {
            return fail(error, "The file progress response is invalid.");
        }
        const quint64 bits = readUInt64(envelope.body, 8, &ok);
        if (!ok) return false;
        double fraction = 0.0;
        std::memcpy(&fraction, &bits, sizeof(fraction));
        response->kind = AppleFileTransferResponse::Kind::Progress;
        response->sessionId = envelope.sessionId;
        response->errorCode = 0;
        response->name.clear();
        response->fraction = fraction;
        return true;
    }
    return fail(error, "The Mac returned an unsupported file transfer response.");
}

bool AppleFileTransferProtocol::parseReceiverFrame(
        const QByteArray& message,
        AppleFileCopyReceiverFrame* frame,
        QString* error)
{
    if (frame == nullptr) {
        return fail(error, "The file frame target is missing.");
    }
    FileCopyEnvelope envelope;
    if (!parseEnvelope(message, &envelope, error)) return false;
    frame->sessionId = envelope.sessionId;
    bool ok = false;
    switch (envelope.command) {
    case 100: {
        if (envelope.body.size() != 60) {
            return fail(error, "The file summary is invalid.");
        }
        const quint32 root = AppleWire::readUInt32(envelope.body, 8, &ok);
        if (!ok || root > 1 || AppleWire::readUInt32(envelope.body, 12, &ok) != 0 ||
                !ok) return fail(error, "The file summary is invalid.");
        const quint64 files = readUInt64(envelope.body, 32, &ok);
        if (!ok) return false;
        const quint64 forks = readUInt64(envelope.body, 40, &ok);
        if (!ok) return false;
        const quint64 folders = readUInt64(envelope.body, 48, &ok);
        if (!ok || forks != 0 ||
                AppleWire::readUInt32(envelope.body, 56, &ok) != 0 || !ok ||
                (root == 0 ? folders == 0 : (files != 1 || folders != 0))) {
            return fail(error, "The file summary is inconsistent.");
        }
        frame->kind = AppleFileCopyReceiverFrame::Kind::Summary;
        frame->summary.rootIsFile = root == 1;
        frame->summary.logicalBytes = readUInt64(envelope.body, 16);
        frame->summary.physicalBytes = readUInt64(envelope.body, 24);
        frame->summary.fileCount = files;
        frame->summary.folderCount = folders;
        return true;
    }
    case 101: {
        if (envelope.body.size() < 113) {
            return fail(error, "The file item is truncated.");
        }
        const quint8 rawType = byteAt(envelope.body, 8);
        if (rawType < 1 || rawType > 3) {
            return fail(error, "The file item type is invalid.");
        }
        const quint16 nameLength = AppleWire::readUInt16(
                envelope.body, 108, &ok);
        if (!ok) return false;
        const quint16 linkLengthMinusOne = AppleWire::readUInt16(
                envelope.body, 110, &ok);
        if (!ok || nameLength == 0 || nameLength > envelope.body.size() - 113) {
            return fail(error, "The file item name is invalid.");
        }
        const int nameEnd = 112 + nameLength;
        QString name;
        if (byteAt(envelope.body, nameEnd) != 0 ||
                !isStrictUtf8(envelope.body.mid(112, nameLength), &name) ||
                name.isEmpty()) {
            return fail(error, "The file item name is invalid.");
        }
        AppleFileCopyItemMetadata item;
        item.type = static_cast<AppleFileCopyItemType>(rawType);
        item.userAccess = byteAt(envelope.body, 9);
        item.finderInfo = envelope.body.mid(10, 32);
        item.finderInfo[24] = 0;
        item.finderInfo[25] = 0;
        item.resourceForkSize = readUInt64(envelope.body, 42, &ok);
        if (!ok) return false;
        item.dataForkSize = readUInt64(envelope.body, 50, &ok);
        if (!ok || !readDate(envelope.body, 58, &item.creationDate) ||
                !readDate(envelope.body, 66, &item.contentModificationDate) ||
                !readDate(envelope.body, 74, &item.attributeModificationDate) ||
                !readDate(envelope.body, 82, &item.accessDate) ||
                !readDate(envelope.body, 90, &item.backupDate)) {
            return fail(error, "The file item dates are invalid.");
        }
        item.nodeFlags = AppleWire::readUInt16(envelope.body, 98, &ok);
        if (!ok) return false;
        item.level = AppleWire::readUInt16(envelope.body, 100, &ok);
        if (!ok) return false;
        item.mode = AppleWire::readUInt16(envelope.body, 102, &ok);
        if (!ok) return false;
        item.textEncodingHint = AppleWire::readUInt32(
                envelope.body, 104, &ok);
        if (!ok) return false;
        item.name = name;
        const int trailingOffset = nameEnd + 1;
        if (item.type == AppleFileCopyItemType::SymbolicLink) {
            if (item.dataForkSize == 0 || item.dataForkSize > 65'536 ||
                    linkLengthMinusOne != item.dataForkSize - 1 ||
                    trailingOffset + static_cast<int>(item.dataForkSize) !=
                            envelope.body.size()) {
                return fail(error, "The symbolic link item is invalid.");
            }
            item.symbolicLinkTarget = envelope.body.mid(
                    trailingOffset, static_cast<int>(item.dataForkSize));
        }
        else {
            if (linkLengthMinusOne != 0 ||
                    !parseExtendedAttributes(
                            envelope.body.mid(trailingOffset),
                            &item.extendedAttributes)) {
                return fail(error, "The file item attributes are invalid.");
            }
        }
        frame->kind = AppleFileCopyReceiverFrame::Kind::Item;
        frame->item = std::move(item);
        return true;
    }
    case 102: {
        if (envelope.body.size() < 12) {
            return fail(error, "The file data block is truncated.");
        }
        const quint32 count = AppleWire::readUInt32(envelope.body, 8, &ok);
        if (!ok || count == 0 || count > MaximumDataBlockLength ||
                count != static_cast<quint32>(envelope.body.size() - 12)) {
            return fail(error, "The file data block is invalid.");
        }
        frame->kind = AppleFileCopyReceiverFrame::Kind::Data;
        frame->data = envelope.body.mid(12);
        frame->uncompressedLength = frame->data.size();
        return true;
    }
    case 103: {
        if (envelope.body.size() < 18 ||
                AppleWire::readUInt16(envelope.body, 8, &ok) != 1 || !ok) {
            return fail(error, "The compressed file block is invalid.");
        }
        const quint32 uncompressed = AppleWire::readUInt32(
                envelope.body, 10, &ok);
        if (!ok) return false;
        const quint32 compressed = AppleWire::readUInt32(
                envelope.body, 14, &ok);
        const quint32 maximumCompressed = uncompressed +
                uncompressed / 16'384 + 10'038;
        if (!ok || uncompressed == 0 || uncompressed > MaximumDataBlockLength ||
                compressed == 0 || compressed > maximumCompressed ||
                compressed != static_cast<quint32>(envelope.body.size() - 18)) {
            return fail(error, "The compressed file block is invalid.");
        }
        frame->kind = AppleFileCopyReceiverFrame::Kind::CompressedData;
        frame->uncompressedLength = static_cast<int>(uncompressed);
        frame->data = envelope.body.mid(18);
        return true;
    }
    case 104:
        if (envelope.body.size() != 12) {
            return fail(error, "The file transfer completion is invalid.");
        }
        frame->kind = AppleFileCopyReceiverFrame::Kind::Done;
        frame->status = readInt32Le(envelope.body, 8, &ok);
        return ok;
    default:
        return fail(error, "The Mac returned an unsupported file copy command.");
    }
}

bool AppleFileTransferProtocol::parseRemoteDrag(
        const QByteArray& message,
        std::optional<AppleRemoteFileDrag>* drag,
        QString* error)
{
    if (drag == nullptr || message.size() < 16 ||
            byteAt(message, 0) != 0x20) {
        return fail(error, "The Mac returned an invalid remote file drag.");
    }
    bool ok = false;
    const quint32 sessionId = AppleWire::readUInt32(message, 4, &ok);
    if (!ok) return false;
    const quint32 unpackedLength = AppleWire::readUInt32(message, 8, &ok);
    if (!ok) return false;
    const quint32 packedLength = AppleWire::readUInt32(message, 12, &ok);
    if (!ok || unpackedLength > MaximumRemoteDragPayloadLength ||
            packedLength > MaximumRemoteDragPayloadLength ||
            packedLength != static_cast<quint32>(message.size() - 16)) {
        return fail(error, "The remote file drag length is invalid.");
    }
    if (sessionId == 0 || (unpackedLength == 0 && packedLength == 0)) {
        drag->reset();
        return true;
    }
    if (unpackedLength == 0 || packedLength == 0) {
        return fail(error, "The remote file drag payload is invalid.");
    }
    const QByteArray archive = inflateSyncFlush(
            message.mid(16), static_cast<int>(unpackedLength),
            MaximumRemoteDragPayloadLength);
    if (archive.size() != static_cast<int>(unpackedLength)) {
        return fail(error, "The remote file drag could not be decompressed.");
    }

    AppleRemoteFileDrag value;
    value.sessionId = sessionId;
    int offset = 0;
    int items = 0;
    int flavors = 0;
    int aliases = 0;
    int metadataBytes = 0;
    while (offset < archive.size()) {
        if (++items > MaximumClipboardItems || offset > archive.size() - 4) {
            return fail(error, "The remote file drag archive is invalid.");
        }
        const quint32 flavorCount = AppleWire::readUInt32(
                archive, offset, &ok);
        offset += 4;
        if (!ok || flavorCount == 0 ||
                flavorCount > static_cast<quint32>(
                        MaximumClipboardFlavors - flavors)) {
            return fail(error, "The remote file drag flavors are invalid.");
        }
        flavors += static_cast<int>(flavorCount);
        for (quint32 flavorIndex = 0;
             flavorIndex < flavorCount;
             ++flavorIndex) {
            QByteArray type;
            if (!readMetadata(archive, &offset, &metadataBytes, &type) ||
                    offset > archive.size() - 8) {
                return fail(error, "The remote file drag flavor is truncated.");
            }
            offset += 4;
            const quint32 aliasCount = AppleWire::readUInt32(
                    archive, offset, &ok);
            offset += 4;
            if (!ok || aliasCount > static_cast<quint32>(
                        MaximumClipboardAliases - aliases) ||
                    (type.isEmpty() && aliasCount == 0)) {
                return fail(error, "The remote file drag aliases are invalid.");
            }
            aliases += static_cast<int>(aliasCount);
            bool fileUrlAlias = false;
            for (quint32 aliasIndex = 0;
                 aliasIndex < aliasCount;
                 ++aliasIndex) {
                QByteArray tagClass;
                QByteArray preferredTag;
                if (!readMetadata(archive, &offset, &metadataBytes, &tagClass) ||
                        !readMetadata(archive, &offset, &metadataBytes,
                                      &preferredTag)) {
                    return fail(error, "The remote file drag alias is truncated.");
                }
                fileUrlAlias = fileUrlAlias ||
                        (tagClass == OSTypeTagClass && preferredTag == FileUrlTag);
            }
            QByteArray payload;
            if (!readBlob(archive, &offset, &payload)) {
                return fail(error, "The remote file drag value is truncated.");
            }
            if (value.imagePng.isEmpty() && type == DragImageFlavor) {
                value.imagePng = payload;
            }
            if (type == FileUrlFlavor || fileUrlAlias) {
                const QUrl url = QUrl::fromEncoded(payload, QUrl::StrictMode);
                if (url.isLocalFile() && !url.toLocalFile().isEmpty()) {
                    value.sourcePaths.append(url.toLocalFile());
                }
            }
        }
    }
    if (value.sourcePaths.isEmpty()) {
        return fail(error, "The remote file drag contains no file paths.");
    }
    *drag = std::move(value);
    return true;
}

bool AppleFileTransferProtocol::expectedStreamMessageLength(
        const QByteArray& fragment,
        int* length,
        QString* error)
{
    if (length == nullptr) return false;
    *length = 0;
    if (fragment.isEmpty()) return true;
    bool ok = false;
    switch (byteAt(fragment, 0)) {
    case 0x1e:
        if (fragment.size() < 12) return true;
        {
            const quint32 pathLength = AppleWire::readUInt32(
                    fragment, 8, &ok);
            if (!ok || pathLength > MaximumDestinationPathLength) {
                return fail(error, "The file request is too large.");
            }
            *length = 12 + static_cast<int>(pathLength);
            return true;
        }
    case 0x20:
        if (fragment.size() < 16) return true;
        {
            const quint32 unpacked = AppleWire::readUInt32(fragment, 8, &ok);
            if (!ok) return false;
            const quint32 transmitted = AppleWire::readUInt32(
                    fragment, 12, &ok);
            if (!ok || unpacked > MaximumRemoteDragPayloadLength ||
                    transmitted > MaximumRemoteDragPayloadLength) {
                return fail(error, "The remote file drag is too large.");
            }
            *length = 16 + static_cast<int>(transmitted);
            return true;
        }
    case 0x22:
        if (fragment.size() < 6) return true;
        {
            const quint32 bodyLength = AppleWire::readUInt32(
                    fragment, 2, &ok);
            if (!ok || bodyLength < 8 ||
                    bodyLength > MaximumFileCopyBodyLength) {
                return fail(error, "The file copy message is too large.");
            }
            *length = 6 + static_cast<int>(bodyLength);
            return true;
        }
    default:
        return false;
    }
}

QList<QByteArray> AppleFileTransferProtocol::fragments(
        const QByteArray& message)
{
    QList<QByteArray> result;
    for (int offset = 0; offset < message.size();
         offset += MaximumRecordPlaintextLength) {
        result.append(message.mid(offset, MaximumRecordPlaintextLength));
    }
    return result;
}

bool AppleFileTransferReassembler::receive(
        const QByteArray& fragment,
        std::optional<QByteArray>* message,
        QString* error)
{
    if (message == nullptr || fragment.isEmpty()) return false;
    message->reset();
    if (m_Buffer.isEmpty()) {
        const quint8 type = byteAt(fragment, 0);
        if (type != 0x1e && type != 0x20 && type != 0x22) {
            return false;
        }
    }
    m_Buffer.append(fragment);
    if (m_ExpectedLength == 0 &&
            !AppleFileTransferProtocol::expectedStreamMessageLength(
                    m_Buffer, &m_ExpectedLength, error)) {
        reset();
        return true;
    }
    if (m_ExpectedLength == 0) return true;
    if (m_Buffer.size() > m_ExpectedLength) {
        reset();
        setError(error, QStringLiteral("The file transfer fragments are malformed."));
        return true;
    }
    if (m_Buffer.size() == m_ExpectedLength) {
        *message = std::move(m_Buffer);
        m_Buffer.clear();
        m_ExpectedLength = 0;
    }
    return true;
}

void AppleFileTransferReassembler::reset()
{
    m_Buffer.clear();
    m_ExpectedLength = 0;
}
