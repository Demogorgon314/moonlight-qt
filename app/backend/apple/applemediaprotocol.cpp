#include "applemediaprotocol.h"

#include "appleprotocol.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QSysInfo>
#include <QtEndian>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace {

constexpr int AuthenticationTagLength = 10;
constexpr quint8 HevcAggregationType = 48;
constexpr quint8 HevcFragmentationType = 49;

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

void writeUInt16(QByteArray& data, int offset, quint16 value)
{
    data[offset] = static_cast<char>(value >> 8);
    data[offset + 1] = static_cast<char>(value);
}

void writeUInt32(QByteArray& data, int offset, quint32 value)
{
    data[offset] = static_cast<char>(value >> 24);
    data[offset + 1] = static_cast<char>(value >> 16);
    data[offset + 2] = static_cast<char>(value >> 8);
    data[offset + 3] = static_cast<char>(value);
}

void appendUInt64(QByteArray& data, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.append(static_cast<char>(value >> shift));
    }
}

quint64 readVariableUInt(const QByteArray& data, int offset, int size, bool* ok = nullptr)
{
    const bool valid = size > 0 && size <= 8 && offset >= 0 &&
                       offset <= data.size() - size;
    if (ok != nullptr) {
        *ok = valid;
    }
    if (!valid) {
        return 0;
    }
    quint64 value = 0;
    for (int index = 0; index < size; ++index) {
        value = value << 8 | byteAt(data, offset + index);
    }
    return value;
}

QByteArray randomBytes(int length, QString* error)
{
    QByteArray result(length, Qt::Uninitialized);
    if (length < 0 || (length > 0 && RAND_bytes(
            reinterpret_cast<unsigned char*>(result.data()), length) != 1)) {
        setError(error, QCoreApplication::translate(
                "AppleMediaProtocol", "Couldn’t generate secure media key material."));
        return {};
    }
    return result;
}

QByteArray aesEcbBlock(const QByteArray& block,
                       const QByteArray& key,
                       QString* error)
{
    if (block.size() != 16 || key.size() != 32) {
        setError(error, QCoreApplication::translate(
                "AppleMediaProtocol", "The media encryption key is invalid."));
        return {};
    }
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        return {};
    }
    QByteArray output(32, Qt::Uninitialized);
    int produced = 0;
    int finalProduced = 0;
    const bool succeeded =
            EVP_EncryptInit_ex(context, EVP_aes_256_ecb(), nullptr,
                               reinterpret_cast<const unsigned char*>(key.constData()),
                               nullptr) == 1 &&
            EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
            EVP_EncryptUpdate(context,
                              reinterpret_cast<unsigned char*>(output.data()), &produced,
                              reinterpret_cast<const unsigned char*>(block.constData()),
                              block.size()) == 1 &&
            EVP_EncryptFinal_ex(context,
                                reinterpret_cast<unsigned char*>(output.data()) + produced,
                                &finalProduced) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!succeeded) {
        setError(error, QCoreApplication::translate(
                "AppleMediaProtocol", "Media key derivation failed."));
        return {};
    }
    output.resize(produced + finalProduced);
    return output;
}

QByteArray aesCtr(const QByteArray& input,
                  const QByteArray& key,
                  const QByteArray& initializationVector,
                  QString* error)
{
    if (key.size() != 32 || initializationVector.size() != 16) {
        setError(error, QCoreApplication::translate(
                "AppleMediaProtocol", "The media cipher state is invalid."));
        return {};
    }
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        return {};
    }
    QByteArray output(input.size() + 16, Qt::Uninitialized);
    int produced = 0;
    int finalProduced = 0;
    const bool succeeded =
            EVP_EncryptInit_ex(context, EVP_aes_256_ctr(), nullptr,
                               reinterpret_cast<const unsigned char*>(key.constData()),
                               reinterpret_cast<const unsigned char*>(
                                       initializationVector.constData())) == 1 &&
            EVP_EncryptUpdate(context,
                              reinterpret_cast<unsigned char*>(output.data()), &produced,
                              reinterpret_cast<const unsigned char*>(input.constData()),
                              input.size()) == 1 &&
            EVP_EncryptFinal_ex(context,
                                reinterpret_cast<unsigned char*>(output.data()) + produced,
                                &finalProduced) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!succeeded) {
        setError(error, QCoreApplication::translate(
                "AppleMediaProtocol", "Media encryption failed."));
        return {};
    }
    output.resize(produced + finalProduced);
    return output;
}

QByteArray sha1Hmac(const QByteArray& data, const QByteArray& key)
{
    unsigned int length = EVP_MAX_MD_SIZE;
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    if (HMAC(EVP_sha1(), key.constData(), key.size(),
             reinterpret_cast<const unsigned char*>(data.constData()), data.size(),
             digest.data(), &length) == nullptr) {
        return {};
    }
    return QByteArray(reinterpret_cast<const char*>(digest.data()),
                      static_cast<int>(length));
}

void incrementBlock(QByteArray& block)
{
    for (int index = block.size() - 1; index >= 0; --index) {
        block[index] = static_cast<char>(byteAt(block, index) + 1);
        if (byteAt(block, index) != 0) {
            break;
        }
    }
}

QByteArray deriveMediaKey(const QByteArray& masterKey,
                          const QByteArray& masterSalt,
                          quint8 label,
                          int count,
                          QString* error)
{
    if (masterKey.size() != 32 || masterSalt.size() != 14 || count <= 0) {
        return {};
    }
    QByteArray block(16, '\0');
    std::memcpy(block.data(), masterSalt.constData(), 14);
    block[7] = static_cast<char>(byteAt(block, 7) ^ label);
    QByteArray output;
    while (output.size() < count) {
        const QByteArray encrypted = aesEcbBlock(block, masterKey, error);
        if (encrypted.size() != 16) {
            return {};
        }
        output.append(encrypted);
        incrementBlock(block);
    }
    output.resize(count);
    return output;
}

bool deriveSchedule(const QByteArray& blob,
                    quint8 cipherLabel,
                    quint8 authenticationLabel,
                    quint8 saltLabel,
                    QByteArray* cipherKey,
                    QByteArray* authenticationKey,
                    QByteArray* salt,
                    QString* error)
{
    if (blob.size() != AppleMediaKeys::BlobLength) {
        setError(error, QCoreApplication::translate(
                "AppleMediaProtocol", "The media encryption key has an invalid length."));
        return false;
    }
    const QByteArray masterKey = blob.left(32);
    const QByteArray masterSalt = blob.mid(32, 14);
    *cipherKey = deriveMediaKey(masterKey, masterSalt, cipherLabel, 32, error);
    *authenticationKey = deriveMediaKey(
            masterKey, masterSalt, authenticationLabel, 20, error);
    *salt = deriveMediaKey(masterKey, masterSalt, saltLabel, 14, error);
    return cipherKey->size() == 32 && authenticationKey->size() == 20 &&
           salt->size() == 14;
}

QByteArray protobufVarint(quint64 value)
{
    QByteArray result;
    do {
        quint8 byte = value & 0x7f;
        value >>= 7;
        if (value != 0) {
            byte |= 0x80;
        }
        result.append(static_cast<char>(byte));
    } while (value != 0);
    return result;
}

void appendProtobufVarint(QByteArray& data, int number, quint64 value)
{
    data.append(protobufVarint(static_cast<quint64>(number << 3)));
    data.append(protobufVarint(value));
}

void appendProtobufBytes(QByteArray& data, int number, const QByteArray& value)
{
    data.append(protobufVarint(static_cast<quint64>((number << 3) | 2)));
    data.append(protobufVarint(static_cast<quint64>(value.size())));
    data.append(value);
}

QByteArray remoteEndpointInfo(const QString& operatingSystemVersion)
{
    QByteArray value;
    appendProtobufVarint(value, 1, 0);
    appendProtobufVarint(value, 2, 1);
    appendProtobufBytes(value, 3, QByteArrayLiteral("Mac"));
    appendProtobufBytes(value, 4, QByteArrayLiteral("1.0.0"));
    appendProtobufBytes(value, 5,
                        operatingSystemVersion.left(127).toUtf8());
    return value;
}

QByteArray mediaBlob(int mode,
                     quint32 synchronizationSource,
                     quint64 timestampNanoseconds,
                     bool audioEnabled)
{
    QByteArray description;
    int descriptionField = 0;
    if (mode == 7) {
        QByteArray standardResolution;
        appendProtobufVarint(standardResolution, 1, 1);
        appendProtobufVarint(standardResolution, 2, 1);
        appendProtobufVarint(standardResolution, 3, 50115);
        appendProtobufVarint(standardResolution, 4, 0);

        QByteArray alternateResolution;
        appendProtobufVarint(alternateResolution, 1, 1);
        appendProtobufVarint(alternateResolution, 2, 2);
        appendProtobufVarint(alternateResolution, 3, 50115);
        appendProtobufVarint(alternateResolution, 4, 0);

        const QByteArray hevcParameters = QByteArrayLiteral(
                "FLS;MS:-1;LF:-1;LTR;CABAC;POS:0;EOD:1;HTS:2;RR:3;AR:16/9,5/8;XR:16/9,5/8;");
        const QByteArray decoderDetails = QByteArrayLiteral(
                "FLS;LF:-1;POS:5;EOD:1;HTS:2;RR:3;POSE:4;AR:16/9,5/8;XR:16/9,5/8;");

        QByteArray h264Bank;
        appendProtobufVarint(h264Bank, 1, 123);
        appendProtobufBytes(h264Bank, 2, standardResolution);
        appendProtobufBytes(h264Bank, 2, alternateResolution);
        appendProtobufBytes(h264Bank, 2, standardResolution);
        appendProtobufBytes(h264Bank, 2, alternateResolution);
        appendProtobufBytes(h264Bank, 3, hevcParameters);
        appendProtobufVarint(h264Bank, 4, 1);

        QByteArray hevcBank;
        appendProtobufVarint(hevcBank, 1, 100);
        appendProtobufBytes(hevcBank, 2, standardResolution);
        appendProtobufBytes(hevcBank, 2, alternateResolution);
        appendProtobufBytes(hevcBank, 3, decoderDetails);
        appendProtobufVarint(hevcBank, 4, 14);

        appendProtobufVarint(description, 1, synchronizationSource);
        appendProtobufVarint(description, 2, 1);
        appendProtobufBytes(description, 3, h264Bank);
        appendProtobufBytes(description, 3, hevcBank);
        appendProtobufVarint(description, 6, 4);
        appendProtobufVarint(description, 7, 1);
        appendProtobufVarint(description, 8, 63);
        appendProtobufVarint(description, 9, 1);
        appendProtobufVarint(description, 12, 1);
        descriptionField = 5;
    }
    else {
        appendProtobufVarint(description, 1, synchronizationSource);
        appendProtobufVarint(description, 2, 0);
        appendProtobufVarint(description, 3, 0);
        appendProtobufVarint(description, 4, audioEnabled ? 24191 : 1000);
        appendProtobufVarint(description, 5, 0);
        appendProtobufVarint(description, 6, 0);
        descriptionField = 3;
    }

    QByteArray result;
    appendProtobufVarint(result, 1, 1);
    appendProtobufVarint(result, 2, 1);
    appendProtobufBytes(result, descriptionField, description);
    appendProtobufBytes(result, 6, QByteArrayLiteral("Moonlight V+"));
    appendProtobufVarint(result, 8, 0);

    const std::array<std::array<quint64, 3>, 10> tiers = {{
        {{0, 40000000, 12288}}, {{0, 6000000, 131072}},
        {{4074, 0, 16384}}, {{16, 4100, std::numeric_limits<quint64>::max()}},
        {{0, 75000000, 524288}}, {{0, 20000000, 98304}},
        {{4, 6500, std::numeric_limits<quint64>::max()}},
        {{0, 60000000, 262144}}, {{1, 299, std::numeric_limits<quint64>::max()}},
        {{0, 100000000, 1048576}},
    }};
    for (const auto& tier : tiers) {
        QByteArray item;
        appendProtobufVarint(item, 1, tier[0]);
        appendProtobufVarint(item, 2, tier[1]);
        if (tier[2] != std::numeric_limits<quint64>::max()) {
            appendProtobufVarint(item, 3, tier[2]);
        }
        appendProtobufBytes(result, 9, item);
    }
    appendProtobufVarint(result, 13, timestampNanoseconds);
    appendProtobufVarint(result, 14, 2);
    appendProtobufVarint(result, 16, 0);
    appendProtobufVarint(result, 18, 1);
    return result;
}

QByteArray binaryPlistLength(int type, quint64 length)
{
    if (length < 15) {
        return QByteArray(1, static_cast<char>((type << 4) | length));
    }
    QByteArray result(1, static_cast<char>((type << 4) | 0x0f));
    if (length <= 0xff) {
        result.append(char(0x10));
        result.append(static_cast<char>(length));
    }
    else if (length <= 0xffff) {
        result.append(char(0x11));
        AppleWire::appendUInt16(result, static_cast<quint16>(length));
    }
    else {
        result.append(char(0x12));
        AppleWire::appendUInt32(result, static_cast<quint32>(length));
    }
    return result;
}

QByteArray binaryPlistAscii(const QByteArray& value)
{
    return binaryPlistLength(5, value.size()) + value;
}

QByteArray binaryPlistData(const QByteArray& value)
{
    return binaryPlistLength(4, value.size()) + value;
}

QByteArray binaryPlistInteger(quint64 value)
{
    if (value <= 0xff) {
        return QByteArray::fromRawData("\x10", 1) +
               QByteArray(1, static_cast<char>(value));
    }
    if (value <= 0xffff) {
        QByteArray result(1, char(0x11));
        AppleWire::appendUInt16(result, static_cast<quint16>(value));
        return result;
    }
    QByteArray result(1, char(0x12));
    AppleWire::appendUInt32(result, static_cast<quint32>(value));
    return result;
}

QByteArray mediaOfferPlist(int mode,
                           const QByteArray& endpointInfo,
                           const QByteArray& compressedBlob,
                           const QString& callId)
{
    QList<QByteArray> objects;
    // Object zero is the top dictionary. All references fit in one byte.
    QByteArray dictionary(1, char(0xd4));
    dictionary.append(QByteArray::fromHex("01020304"));
    dictionary.append(QByteArray::fromHex("05060708"));
    objects.append(dictionary);
    objects.append(binaryPlistAscii(QByteArrayLiteral(
            "avcMediaStreamOptionRemoteEndpointInfo")));
    objects.append(binaryPlistAscii(QByteArrayLiteral(
            "avcMediaStreamNegotiatorMode")));
    objects.append(binaryPlistAscii(QByteArrayLiteral(
            "avcMediaStreamNegotiatorMediaBlob")));
    objects.append(binaryPlistAscii(QByteArrayLiteral(
            "avcMediaStreamOptionCallID")));
    objects.append(binaryPlistData(endpointInfo));
    objects.append(binaryPlistInteger(static_cast<quint64>(mode)));
    objects.append(binaryPlistData(compressedBlob));
    objects.append(binaryPlistAscii(callId.toLatin1()));

    QByteArray result = QByteArrayLiteral("bplist00");
    QList<quint64> offsets;
    for (const QByteArray& object : objects) {
        offsets.append(static_cast<quint64>(result.size()));
        result.append(object);
    }
    const quint64 offsetTableOffset = result.size();
    int offsetSize = 1;
    if (result.size() > 0xff) {
        offsetSize = result.size() <= 0xffff ? 2 : 4;
    }
    for (quint64 offset : offsets) {
        for (int shift = (offsetSize - 1) * 8; shift >= 0; shift -= 8) {
            result.append(static_cast<char>(offset >> shift));
        }
    }
    result.append(QByteArray(6, '\0'));
    result.append(static_cast<char>(offsetSize));
    result.append(char(1));
    appendUInt64(result, static_cast<quint64>(objects.size()));
    appendUInt64(result, 0);
    appendUInt64(result, offsetTableOffset);
    return result;
}

class BinaryPlistReader
{
public:
    explicit BinaryPlistReader(QByteArray data)
        : m_Data(std::move(data))
    {
    }

    bool dataForKey(const QByteArray& wantedKey, QByteArray* value)
    {
        if (!prepare() || value == nullptr) {
            return false;
        }
        const quint64 rootOffset = objectOffset(m_TopObject);
        if (rootOffset >= static_cast<quint64>(m_Data.size()) ||
                (byteAt(m_Data, rootOffset) >> 4) != 0x0d) {
            return false;
        }
        int cursor = static_cast<int>(rootOffset) + 1;
        quint64 count = 0;
        if (!readLength(byteAt(m_Data, rootOffset) & 0x0f, &cursor, &count) ||
                count > 1024 || cursor > m_Data.size() - static_cast<int>(count * 2 * m_RefSize)) {
            return false;
        }
        for (quint64 index = 0; index < count; ++index) {
            bool keyOk = false;
            bool valueOk = false;
            const quint64 keyReference = readVariableUInt(
                    m_Data, cursor + static_cast<int>(index * m_RefSize),
                    m_RefSize, &keyOk);
            const quint64 valueReference = readVariableUInt(
                    m_Data,
                    cursor + static_cast<int>((count + index) * m_RefSize),
                    m_RefSize, &valueOk);
            if (!keyOk || !valueOk || stringObject(keyReference) != wantedKey) {
                continue;
            }
            return dataObject(valueReference, value);
        }
        return false;
    }

private:
    bool prepare()
    {
        if (!m_Data.startsWith("bplist00") || m_Data.size() < 40) {
            return false;
        }
        const int trailer = m_Data.size() - 32;
        m_OffsetSize = byteAt(m_Data, trailer + 6);
        m_RefSize = byteAt(m_Data, trailer + 7);
        bool countOk = false;
        bool topOk = false;
        bool tableOk = false;
        m_ObjectCount = readVariableUInt(m_Data, trailer + 8, 8, &countOk);
        m_TopObject = readVariableUInt(m_Data, trailer + 16, 8, &topOk);
        m_OffsetTable = readVariableUInt(m_Data, trailer + 24, 8, &tableOk);
        return countOk && topOk && tableOk && m_ObjectCount > 0 &&
               m_ObjectCount <= 65535 && m_TopObject < m_ObjectCount &&
               m_OffsetSize > 0 && m_OffsetSize <= 8 &&
               m_RefSize > 0 && m_RefSize <= 8 &&
               m_OffsetTable >= 8 &&
               m_OffsetTable + m_ObjectCount * m_OffsetSize <=
                       static_cast<quint64>(trailer);
    }

    quint64 objectOffset(quint64 reference) const
    {
        if (reference >= m_ObjectCount) {
            return std::numeric_limits<quint64>::max();
        }
        bool ok = false;
        const quint64 value = readVariableUInt(
                m_Data,
                static_cast<int>(m_OffsetTable + reference * m_OffsetSize),
                m_OffsetSize,
                &ok);
        return ok ? value : std::numeric_limits<quint64>::max();
    }

    bool readLength(quint8 nibble, int* cursor, quint64* length) const
    {
        if (nibble < 15) {
            *length = nibble;
            return true;
        }
        if (*cursor >= m_Data.size()) {
            return false;
        }
        const quint8 marker = byteAt(m_Data, (*cursor)++);
        if ((marker >> 4) != 1 || (marker & 0x0f) > 3) {
            return false;
        }
        const int size = 1 << (marker & 0x0f);
        bool ok = false;
        *length = readVariableUInt(m_Data, *cursor, size, &ok);
        *cursor += size;
        return ok;
    }

    QByteArray stringObject(quint64 reference) const
    {
        const quint64 rawOffset = objectOffset(reference);
        if (rawOffset >= static_cast<quint64>(m_Data.size())) {
            return {};
        }
        const int offset = static_cast<int>(rawOffset);
        const quint8 type = byteAt(m_Data, offset) >> 4;
        int cursor = offset + 1;
        quint64 length = 0;
        if ((type != 5 && type != 6) ||
                !readLength(byteAt(m_Data, offset) & 0x0f, &cursor, &length)) {
            return {};
        }
        if (type == 5 && length <= static_cast<quint64>(m_Data.size() - cursor)) {
            return m_Data.mid(cursor, static_cast<int>(length));
        }
        if (type == 6 && length <= static_cast<quint64>((m_Data.size() - cursor) / 2)) {
            QString string;
            string.reserve(static_cast<int>(length));
            for (quint64 index = 0; index < length; ++index) {
                string.append(QChar(static_cast<char16_t>(readVariableUInt(
                        m_Data, cursor + static_cast<int>(index * 2), 2))));
            }
            return string.toUtf8();
        }
        return {};
    }

    bool dataObject(quint64 reference, QByteArray* value) const
    {
        const quint64 rawOffset = objectOffset(reference);
        if (rawOffset >= static_cast<quint64>(m_Data.size())) {
            return false;
        }
        const int offset = static_cast<int>(rawOffset);
        if ((byteAt(m_Data, offset) >> 4) != 4) {
            return false;
        }
        int cursor = offset + 1;
        quint64 length = 0;
        if (!readLength(byteAt(m_Data, offset) & 0x0f, &cursor, &length) ||
                length > static_cast<quint64>(m_Data.size() - cursor)) {
            return false;
        }
        *value = m_Data.mid(cursor, static_cast<int>(length));
        return true;
    }

    QByteArray m_Data;
    int m_OffsetSize = 0;
    int m_RefSize = 0;
    quint64 m_ObjectCount = 0;
    quint64 m_TopObject = 0;
    quint64 m_OffsetTable = 0;
};

QByteArray inflateMediaBlob(const QByteArray& compressed)
{
    // qUncompress uses a four-byte expected-size prefix. The Apple wire sends
    // a normal zlib stream without that prefix, so grow the bounded output
    // allowance until Qt's zlib wrapper can complete it.
    for (quint32 size = 1024; size <= 4 * 1024 * 1024; size *= 2) {
        QByteArray framed;
        AppleWire::appendUInt32(framed, size);
        framed.append(compressed);
        const QByteArray output = qUncompress(framed);
        if (!output.isEmpty()) {
            return output;
        }
    }
    return {};
}

bool readProtobufVarint(const QByteArray& data, int* offset, quint64* value)
{
    quint64 result = 0;
    for (int shift = 0; shift <= 63; shift += 7) {
        if (*offset >= data.size()) {
            return false;
        }
        const quint8 byte = byteAt(data, (*offset)++);
        result |= static_cast<quint64>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            *value = result;
            return true;
        }
    }
    return false;
}

bool skipProtobufField(const QByteArray& data,
                       int wireType,
                       int* offset,
                       QByteArray* bytes,
                       quint64* varint)
{
    switch (wireType) {
    case 0:
        return readProtobufVarint(data, offset, varint);
    case 1:
        if (*offset > data.size() - 8) return false;
        *offset += 8;
        return true;
    case 2: {
        quint64 length = 0;
        if (!readProtobufVarint(data, offset, &length) ||
                length > static_cast<quint64>(data.size() - *offset)) {
            return false;
        }
        if (bytes != nullptr) {
            *bytes = data.mid(*offset, static_cast<int>(length));
        }
        *offset += static_cast<int>(length);
        return true;
    }
    case 5:
        if (*offset > data.size() - 4) return false;
        *offset += 4;
        return true;
    default:
        return false;
    }
}

bool canvasFromProtobuf(const QByteArray& data, AppleCanvas* canvas)
{
    int offset = 0;
    while (offset < data.size()) {
        quint64 tag = 0;
        if (!readProtobufVarint(data, &offset, &tag)) {
            return false;
        }
        QByteArray bytes;
        quint64 ignored = 0;
        if (!skipProtobufField(data, tag & 7, &offset, &bytes, &ignored)) {
            return false;
        }
        if ((tag >> 3) != 5 || (tag & 7) != 2) {
            continue;
        }
        AppleCanvas candidate;
        int nestedOffset = 0;
        while (nestedOffset < bytes.size()) {
            quint64 nestedTag = 0;
            quint64 value = 0;
            QByteArray ignoredBytes;
            if (!readProtobufVarint(bytes, &nestedOffset, &nestedTag) ||
                    !skipProtobufField(bytes, nestedTag & 7, &nestedOffset,
                                       &ignoredBytes, &value)) {
                break;
            }
            if ((nestedTag & 7) != 0) {
                continue;
            }
            if ((nestedTag >> 3) == 4) candidate.width = static_cast<int>(value);
            if ((nestedTag >> 3) == 5) candidate.height = static_cast<int>(value);
            if ((nestedTag >> 3) == 6) candidate.tileCount = static_cast<int>(value);
        }
        if (candidate.isUsable()) {
            *canvas = candidate;
            return true;
        }
    }
    return false;
}

quint8 hevcType(const QByteArray& data)
{
    return data.size() >= 2 ? (byteAt(data, 0) >> 1) & 0x3f : 0xff;
}

bool startsAccessUnit(const QByteArray& payload)
{
    const quint8 type = hevcType(payload);
    if (type == HevcFragmentationType) {
        return payload.size() >= 5 && (byteAt(payload, 2) & 0x80) != 0;
    }
    return type != 0xff && payload.size() >= 4;
}

} // namespace

QList<int> AppleMediaLayout::verticalTileBoundaries(
        const AppleCanvas& canvas,
        const QList<int>& tileHeights,
        int outputHeight)
{
    if (!canvas.isUsable() || outputHeight <= 0) {
        return {};
    }
    const int fallbackHeight =
            (canvas.height + canvas.tileCount - 1) / canvas.tileCount;
    QList<int> boundaries;
    boundaries.reserve(canvas.tileCount + 1);
    boundaries.append(0);
    int logicalTop = 0;
    for (int tile = 0; tile < canvas.tileCount; ++tile) {
        const int tileHeight = qMax(0, tileHeights.value(tile, fallbackHeight));
        const int validHeight = qMax(0, qMin(
                tileHeight, canvas.height - logicalTop));
        const int logicalBottom = logicalTop + validHeight;
        boundaries.append(qRound(
                static_cast<double>(logicalBottom) * outputHeight /
                canvas.height));
        logicalTop += tileHeight;
    }
    return boundaries;
}

bool AppleMediaKeys::isValid() const
{
    return audioViewer.size() == BlobLength && audioServer.size() == BlobLength &&
           videoViewer.size() == BlobLength && videoServer.size() == BlobLength;
}

namespace AppleMediaWire {

AppleMediaOffers createOffers(bool audioEnabled,
                              const QString& operatingSystemVersion,
                              QString* error)
{
    const QByteArray random = randomBytes(8, error);
    if (random.size() != 8) {
        return {};
    }
    const quint32 videoSource = qFromBigEndian<quint32>(
            reinterpret_cast<const uchar*>(random.constData()));
    const quint32 audioSource = qFromBigEndian<quint32>(
            reinterpret_cast<const uchar*>(random.constData() + 4));
    const quint64 timestamp = static_cast<quint64>(
            QDateTime::currentMSecsSinceEpoch()) * 1000000ULL;
    AppleMediaOffers offers;
    offers.videoSynchronizationSource = videoSource;
    offers.audioSynchronizationSource = audioSource;
    offers.video = createOffer(7, videoSource, timestamp, QUuid::createUuid(),
                               audioEnabled, operatingSystemVersion, error);
    offers.audio = createOffer(8, audioSource, timestamp, QUuid::createUuid(),
                               audioEnabled, operatingSystemVersion, error);
    if (offers.video.isEmpty() || offers.audio.isEmpty()) {
        return {};
    }
    return offers;
}

QByteArray createOffer(int mode,
                       quint32 synchronizationSource,
                       quint64 timestampNanoseconds,
                       const QUuid& callId,
                       bool audioEnabled,
                       const QString& operatingSystemVersion,
                       QString* error)
{
    if ((mode != 7 && mode != 8) || callId.isNull()) {
        setError(error, QCoreApplication::translate(
                "AppleMediaProtocol", "The media offer parameters are invalid."));
        return {};
    }
    const QByteArray blob = mediaBlob(mode, synchronizationSource,
                                      timestampNanoseconds, audioEnabled);
    const QByteArray compressedWithLength = qCompress(blob, 9);
    if (compressedWithLength.size() <= 4) {
        setError(error, QCoreApplication::translate(
                "AppleMediaProtocol", "The media offer could not be compressed."));
        return {};
    }
    QString uppercaseCallId = callId.toString(QUuid::WithoutBraces).toUpper();
    return mediaOfferPlist(mode,
                           remoteEndpointInfo(operatingSystemVersion),
                           compressedWithLength.mid(4),
                           uppercaseCallId);
}

QByteArray configuration(const AppleMediaOffers& offers,
                         const AppleMediaKeys& keys,
                         const QUuid& callId,
                         QString* error)
{
    const int messageSize = offers.audio.size() + offers.video.size() + 0xd8;
    if (!keys.isValid() || offers.audio.isEmpty() || offers.video.isEmpty() ||
            callId.isNull() || offers.audio.size() > 65535 ||
            offers.video.size() > 65535 || messageSize > 65535) {
        setError(error, QCoreApplication::translate(
                "AppleMediaProtocol", "The media configuration is invalid or too large."));
        return {};
    }
    QByteArray message(messageSize + 4, '\0');
    message[0] = char(0x1c);
    writeUInt16(message, 2, static_cast<quint16>(messageSize));
    writeUInt16(message, 4, 3);
    writeUInt32(message, 6, 0x05);
    writeUInt16(message, 10, static_cast<quint16>(offers.audio.size()));
    writeUInt16(message, 12, static_cast<quint16>(offers.video.size()));
    writeUInt16(message, 14, 0);
    const QByteArray uuid = callId.toRfc4122();
    std::memcpy(message.data() + 0x14, uuid.constData(), 16);
    std::memcpy(message.data() + 0x24, keys.audioViewer.constData(), 46);
    std::memcpy(message.data() + 0x52, keys.audioServer.constData(), 46);
    std::memcpy(message.data() + 0x80, offers.audio.constData(), offers.audio.size());
    const int videoOffset = 0x80 + offers.audio.size();
    std::memcpy(message.data() + videoOffset, keys.videoViewer.constData(), 46);
    std::memcpy(message.data() + videoOffset + 46, keys.videoServer.constData(), 46);
    std::memcpy(message.data() + videoOffset + 92,
                offers.video.constData(), offers.video.size());
    return message;
}

QByteArray framebufferUpdateRequest()
{
    return QByteArray::fromHex("030000000000ffffffff");
}

QByteArray autoFramebufferUpdate()
{
    return QByteArray::fromHex("090000010000000000000000ffffffff");
}

QByteArray controlMode(bool observing)
{
    QByteArray message = QByteArray::fromHex("0a000000");
    writeUInt16(message, 2, observing ? 0 : 1);
    return message;
}

bool parsePorts(const QByteArray& answer, AppleMediaPorts* ports)
{
    if (ports == nullptr || answer.size() < 0x14 ||
            AppleWire::readUInt16(answer, 2) != 1 ||
            AppleWire::readUInt16(answer, 4) == 0 ||
            AppleWire::readUInt16(answer, 0x12) != 1) {
        return false;
    }
    AppleMediaPorts candidate;
    candidate.audio = AppleWire::readUInt16(answer, 0x0a);
    candidate.video = AppleWire::readUInt16(answer, 0x10);
    if (!candidate.isUsable()) {
        return false;
    }
    *ports = candidate;
    return true;
}

bool parseCanvas(const QByteArray& answer, AppleCanvas* canvas)
{
    if (canvas == nullptr || answer.isEmpty() || byteAt(answer, 0) != 0) {
        return false;
    }
    int search = 0;
    while ((search = answer.indexOf("bplist", search)) >= 0) {
        for (int end = answer.size(); end >= search + 40; --end) {
            QByteArray compressed;
            BinaryPlistReader reader(answer.mid(search, end - search));
            if (!reader.dataForKey(QByteArrayLiteral(
                        "avcMediaStreamNegotiatorMediaBlob"), &compressed)) {
                continue;
            }
            const QByteArray blob = inflateMediaBlob(compressed);
            if (!blob.isEmpty() && canvasFromProtobuf(blob, canvas)) {
                return true;
            }
        }
        search += 6;
    }
    return false;
}

bool containsMediaAnswer(const QByteArray& answer)
{
    return answer.contains("avcMediaStreamNegotiatorMediaBlob");
}

AppleInputEncryptionRequest pointerEvent(quint8 buttons,
                                         quint16 x,
                                         quint16 y,
                                         int clickCount,
                                         quint32 timestampDelta)
{
    AppleInputEncryptionRequest request;
    request.header.append(char(0x10));
    request.header.append(static_cast<char>(1 | ((qMax(0, clickCount) & 7) << 1)));
    request.plaintextBlock = QByteArray(16, '\0');
    writeUInt32(request.plaintextBlock, 6, timestampDelta);
    request.plaintextBlock[10] = char(0xff);
    request.plaintextBlock[11] = static_cast<char>(buttons);
    writeUInt16(request.plaintextBlock, 12, x);
    writeUInt16(request.plaintextBlock, 14, y);
    return request;
}

AppleInputEncryptionRequest keyEvent(bool isDown,
                                     quint32 keySymbol,
                                     quint32 timestampDelta,
                                     quint16 keyboardType,
                                     quint16 keyCode)
{
    AppleInputEncryptionRequest request;
    request.header = QByteArray::fromHex("1001");
    request.plaintextBlock = QByteArray(16, '\0');
    request.plaintextBlock[0] = char(0xff);
    request.plaintextBlock[1] = isDown ? char(1) : char(0);
    writeUInt32(request.plaintextBlock, 2, keySymbol);
    writeUInt32(request.plaintextBlock, 6, timestampDelta);
    writeUInt16(request.plaintextBlock, 12, keyboardType);
    writeUInt16(request.plaintextBlock, 14, keyCode);
    return request;
}

QByteArray receiverReport(quint32 sender)
{
    QByteArray packet = QByteArray::fromHex("80c90001");
    AppleWire::appendUInt32(packet, sender);
    return packet;
}

QByteArray genericNack(quint32 sender,
                       quint32 mediaSource,
                       const QList<quint16>& lostSequences)
{
    QList<quint16> sequences = lostSequences;
    std::sort(sequences.begin(), sequences.end());
    sequences.erase(std::unique(sequences.begin(), sequences.end()), sequences.end());
    if (sequences.isEmpty()) {
        return {};
    }
    QByteArray feedback;
    int index = 0;
    while (index < sequences.size()) {
        const quint16 packetId = sequences.at(index);
        quint16 bitmask = 0;
        int next = index + 1;
        while (next < sequences.size()) {
            const quint16 difference = sequences.at(next) - packetId;
            if (difference < 1 || difference > 16) {
                break;
            }
            bitmask |= static_cast<quint16>(1U << (difference - 1));
            ++next;
        }
        AppleWire::appendUInt16(feedback, packetId);
        AppleWire::appendUInt16(feedback, bitmask);
        index = next;
    }
    QByteArray packet = QByteArray::fromHex("81cd");
    AppleWire::appendUInt16(packet, static_cast<quint16>(2 + feedback.size() / 4));
    AppleWire::appendUInt32(packet, sender);
    AppleWire::appendUInt32(packet, mediaSource);
    packet.append(feedback);
    return packet;
}

QByteArray fullIntraRequest(quint32 sender,
                            quint32 mediaSource,
                            quint8 sequence)
{
    QByteArray packet = QByteArray::fromHex("84ce0004");
    AppleWire::appendUInt32(packet, sender);
    AppleWire::appendUInt32(packet, 0);
    AppleWire::appendUInt32(packet, mediaSource);
    packet.append(static_cast<char>(sequence));
    packet.append(QByteArray(3, '\0'));
    return packet;
}

QList<QByteArray> fullIntraRequests(quint32 sender,
                                    const QList<quint32>& mediaSources,
                                    quint8 initialSequence)
{
    QList<QByteArray> packets;
    packets.reserve(mediaSources.size());
    quint8 sequence = initialSequence;
    for (quint32 source : mediaSources) {
        packets.append(fullIntraRequest(sender, source, sequence++));
    }
    return packets;
}

QByteArray rateControl(quint32 sender,
                       quint32 rtpTimestamp,
                       quint32 estimatedBandwidthKilobitsPerSecond,
                       quint32 receivedPacketCount,
                       quint32 feedbackDelayMilliseconds,
                       quint16 echoTimestamp)
{
    QByteArray packet = QByteArray::fromHex("80cc0007");
    AppleWire::appendUInt32(packet, sender);
    packet.append("RCTL", 4);

    packet.append(QByteArray::fromHex("85000004"));
    AppleWire::appendUInt16(packet, static_cast<quint16>(rtpTimestamp >> 8));
    AppleWire::appendUInt16(packet, 0);
    AppleWire::appendUInt16(packet, 0);
    AppleWire::appendUInt16(packet, static_cast<quint16>(
            qMin(feedbackDelayMilliseconds, static_cast<quint32>(0xffff))));
    AppleWire::appendUInt16(packet, echoTimestamp);
    AppleWire::appendUInt16(packet, 0);
    AppleWire::appendUInt16(packet, static_cast<quint16>(receivedPacketCount & 0x0fff));
    AppleWire::appendUInt16(packet, static_cast<quint16>(
            qMin(estimatedBandwidthKilobitsPerSecond,
                 static_cast<quint32>(0xffff))));
    return packet;
}

} // namespace AppleMediaWire

AppleSrtpDecryptor::AppleSrtpDecryptor(const QByteArray& keyBlob, QString* error)
{
    deriveSchedule(keyBlob, 0, 1, 2, &m_CipherKey,
                   &m_AuthenticationKey, &m_Salt, error);
}

bool AppleSrtpDecryptor::isValid() const
{
    return m_CipherKey.size() == 32 && m_AuthenticationKey.size() == 20 &&
           m_Salt.size() == 14;
}

QByteArray AppleSrtpDecryptor::initializationVector(quint32 source,
                                                    quint64 index) const
{
    QByteArray vector = m_Salt + QByteArray(2, '\0');
    for (int offset = 0; offset < 4; ++offset) {
        vector[4 + offset] = static_cast<char>(
                byteAt(vector, 4 + offset) ^
                static_cast<quint8>(source >> (24 - offset * 8)));
    }
    for (int offset = 0; offset < 6; ++offset) {
        vector[8 + offset] = static_cast<char>(
                byteAt(vector, 8 + offset) ^
                static_cast<quint8>(index >> (40 - offset * 8)));
    }
    return vector;
}

bool AppleSrtpDecryptor::authenticates(const QByteArray& datagram,
                                       int bodyLength,
                                       quint32 rollover) const
{
    QByteArray authenticated = datagram.left(bodyLength);
    AppleWire::appendUInt32(authenticated, rollover);
    const QByteArray expected = sha1Hmac(authenticated, m_AuthenticationKey).left(
            AuthenticationTagLength);
    const QByteArray actual = datagram.right(AuthenticationTagLength);
    return expected.size() == AuthenticationTagLength &&
           CRYPTO_memcmp(expected.constData(), actual.constData(),
                         AuthenticationTagLength) == 0;
}

bool AppleSrtpDecryptor::decrypt(const QByteArray& datagram,
                                 AppleRtpPacket* packet,
                                 QString* error)
{
    if (!isValid() || packet == nullptr || datagram.size() < 22) {
        setError(error, QCoreApplication::translate(
                "AppleMediaProtocol", "The Mac sent an invalid media packet."));
        return false;
    }
    bool sequenceOk = false;
    bool timestampOk = false;
    bool sourceOk = false;
    const quint16 sequence = AppleWire::readUInt16(datagram, 2, &sequenceOk);
    const quint32 timestamp = AppleWire::readUInt32(datagram, 4, &timestampOk);
    const quint32 source = AppleWire::readUInt32(datagram, 8, &sourceOk);
    if (!sequenceOk || !timestampOk || !sourceOk) {
        return false;
    }
    const int bodyLength = datagram.size() - AuthenticationTagLength;
    const SourceState state = m_States.value(source);
    quint32 guess = state.rolloverCounter;
    if (state.initialized) {
        const int difference = static_cast<int>(sequence) - state.maximumSequence;
        if (difference > 0x7fff) {
            guess = state.rolloverCounter == 0 ? 0 : state.rolloverCounter - 1;
        }
        else if (difference < -0x7fff) {
            guess = state.rolloverCounter + 1;
        }
    }
    QList<quint32> candidates = {
        guess, state.rolloverCounter, guess + 1, guess == 0 ? 0 : guess - 1,
    };
    for (int index = candidates.size() - 1; index >= 0; --index) {
        if (candidates.indexOf(candidates.at(index)) != index) {
            candidates.removeAt(index);
        }
    }
    for (quint32 rollover : candidates) {
        if (!authenticates(datagram, bodyLength, rollover)) {
            continue;
        }
        const quint8 first = byteAt(datagram, 0);
        if ((first >> 6) != 2) {
            break;
        }
        int headerLength = 12 + (first & 0x0f) * 4;
        if (headerLength > bodyLength) {
            break;
        }
        if ((first & 0x10) != 0) {
            bool wordsOk = false;
            const quint16 words = AppleWire::readUInt16(
                    datagram, headerLength + 2, &wordsOk);
            if (!wordsOk) {
                break;
            }
            headerLength += 4 + words * 4;
        }
        if (headerLength > bodyLength) {
            break;
        }
        const quint64 packetIndex = static_cast<quint64>(rollover) << 16 | sequence;
        const QByteArray payload = aesCtr(
                datagram.mid(headerLength, bodyLength - headerLength),
                m_CipherKey, initializationVector(source, packetIndex), error);
        if (payload.isNull()) {
            return false;
        }
        packet->header = datagram.left(headerLength);
        packet->payload = payload;
        packet->sequenceNumber = sequence;
        packet->timestamp = timestamp;
        packet->synchronizationSource = source;
        packet->payloadType = byteAt(datagram, 1) & 0x7f;
        packet->marker = (byteAt(datagram, 1) & 0x80) != 0;

        SourceState updated = state;
        const quint64 incoming = static_cast<quint64>(rollover) << 16 | sequence;
        const quint64 current = static_cast<quint64>(state.rolloverCounter) << 16 |
                                state.maximumSequence;
        if (!updated.initialized || incoming > current) {
            updated.rolloverCounter = rollover;
            updated.maximumSequence = sequence;
            updated.initialized = true;
            m_States.insert(source, updated);
        }
        return true;
    }
    setError(error, QCoreApplication::translate(
            "AppleMediaProtocol", "A media packet failed authentication."));
    return false;
}

std::optional<AppleRtpPacket::FramePacketInfo>
AppleRtpPacket::framePacketInfo() const
{
    if (header.size() < 12 || (byteAt(header, 0) & 0x10) == 0) {
        return std::nullopt;
    }
    const int extensionOffset = 12 + (byteAt(header, 0) & 0x0f) * 4;
    bool wordCountOk = false;
    const quint16 wordCount = AppleWire::readUInt16(
            header, extensionOffset + 2, &wordCountOk);
    if (!wordCountOk || extensionOffset + 4 > header.size() ||
            (byteAt(header, extensionOffset) >> 6) != 2) {
        return std::nullopt;
    }
    const int endOffset = extensionOffset + 4 + wordCount * 4;
    const quint8 controlByte = byteAt(header, extensionOffset + 1);
    const int infoOffset = extensionOffset + 4;
    if ((controlByte & 0x01) == 0 || endOffset > header.size() ||
            infoOffset + 4 > endOffset) {
        return std::nullopt;
    }
    bool packetCountOk = false;
    bool frameSequenceOk = false;
    const quint16 packetCount = AppleWire::readUInt16(
            header, infoOffset, &packetCountOk);
    const quint16 frameSequence = AppleWire::readUInt16(
            header, infoOffset + 2, &frameSequenceOk);
    if (!packetCountOk || !frameSequenceOk) {
        return std::nullopt;
    }
    return FramePacketInfo{packetCount, frameSequence};
}

AppleSrtcpEncryptor::AppleSrtcpEncryptor(const QByteArray& keyBlob, QString* error)
{
    deriveSchedule(keyBlob, 3, 4, 5, &m_CipherKey,
                   &m_AuthenticationKey, &m_Salt, error);
}

bool AppleSrtcpEncryptor::isValid() const
{
    return m_CipherKey.size() == 32 && m_AuthenticationKey.size() == 20 &&
           m_Salt.size() == 14;
}

QByteArray AppleSrtcpEncryptor::protect(const QByteArray& packet, QString* error)
{
    if (!isValid() || packet.size() < 8) {
        setError(error, QCoreApplication::translate(
                "AppleMediaProtocol", "The recovery feedback packet is invalid."));
        return {};
    }
    const quint32 source = AppleWire::readUInt32(packet, 4);
    const quint32 packetIndex = m_Index & 0x7fffffff;
    QByteArray vector = m_Salt + QByteArray(2, '\0');
    for (int offset = 0; offset < 4; ++offset) {
        vector[4 + offset] = static_cast<char>(
                byteAt(vector, 4 + offset) ^
                static_cast<quint8>(source >> (24 - offset * 8)));
        vector[10 + offset] = static_cast<char>(
                byteAt(vector, 10 + offset) ^
                static_cast<quint8>(packetIndex >> (24 - offset * 8)));
    }
    const QByteArray ciphertext = aesCtr(packet.mid(8), m_CipherKey, vector, error);
    if (ciphertext.isNull()) {
        return {};
    }
    QByteArray body = packet.left(8) + ciphertext;
    AppleWire::appendUInt32(body, 0x80000000U | packetIndex);
    const QByteArray digest = sha1Hmac(body, m_AuthenticationKey);
    if (digest.size() < AuthenticationTagLength) {
        return {};
    }
    body.append(digest.left(AuthenticationTagLength));
    m_Index = (packetIndex + 1) & 0x7fffffff;
    return body;
}

bool AppleHevcAccessUnit::containsVideoSlice() const
{
    return std::any_of(nalUnits.cbegin(), nalUnits.cend(), [](const QByteArray& unit) {
        return hevcType(unit) < 32;
    });
}

bool AppleHevcAccessUnit::containsRandomAccessPicture() const
{
    return std::any_of(nalUnits.cbegin(), nalUnits.cend(), [](const QByteArray& unit) {
        const quint8 type = hevcType(unit);
        return type >= 16 && type <= 23;
    });
}

QList<AppleHevcAccessUnit> AppleHevcDecodingOrderQueue::enqueue(
        const QList<AppleHevcAccessUnit>& accessUnits)
{
    QList<AppleHevcAccessUnit> ready;
    QList<AppleHevcAccessUnit> ordered;
    for (const AppleHevcAccessUnit& accessUnit : accessUnits) {
        if (accessUnit.decodingOrderNumber.has_value()) {
            ordered.append(accessUnit);
        }
        else {
            ready.append(accessUnit);
        }
    }
    if (!m_ExpectedDecodingOrderNumber.has_value()) {
        ordered = circularlyOrdered(ordered);
    }
    for (const AppleHevcAccessUnit& accessUnit : ordered) {
        const quint16 order = *accessUnit.decodingOrderNumber;
        if (m_ExpectedDecodingOrderNumber.has_value()) {
            const quint16 delta = order - *m_ExpectedDecodingOrderNumber;
            if (delta > 0x7ffe) {
                continue;
            }
        }
        ready.append(accessUnit);
        m_ExpectedDecodingOrderNumber = static_cast<quint16>(order + 1);
    }
    return ready;
}

void AppleHevcDecodingOrderQueue::reset()
{
    m_ExpectedDecodingOrderNumber.reset();
}

QList<AppleHevcAccessUnit> AppleHevcDecodingOrderQueue::circularlyOrdered(
        const QList<AppleHevcAccessUnit>& accessUnits)
{
    QList<AppleHevcAccessUnit> sorted = accessUnits;
    std::sort(sorted.begin(), sorted.end(), [](const AppleHevcAccessUnit& lhs,
                                                const AppleHevcAccessUnit& rhs) {
        return *lhs.decodingOrderNumber < *rhs.decodingOrderNumber;
    });
    if (sorted.size() <= 1) {
        return sorted;
    }
    int largestGapIndex = 0;
    quint16 largestGap = 0;
    for (int index = 0; index < sorted.size(); ++index) {
        const quint16 current = *sorted.at(index).decodingOrderNumber;
        const quint16 next = *sorted.at((index + 1) % sorted.size())
                                      .decodingOrderNumber;
        const quint16 gap = next - current;
        if (gap > largestGap) {
            largestGap = gap;
            largestGapIndex = index;
        }
    }
    const int start = (largestGapIndex + 1) % sorted.size();
    return sorted.mid(start) + sorted.mid(0, start);
}

std::optional<quint16> AppleHevcAssembler::firstDecodingOrderNumber(
        const QByteArray& payload)
{
    const quint8 type = hevcType(payload);
    const int offset = type == HevcFragmentationType ? 3 : 2;
    bool ok = false;
    const quint16 value = AppleWire::readUInt16(payload, offset, &ok);
    return ok ? std::optional<quint16>(value) : std::nullopt;
}

QList<AppleHevcAssembler::PendingPacket> AppleHevcAssembler::sequenceOrdered(
        const QList<PendingPacket>& packets)
{
    QList<PendingPacket> sorted = packets;
    std::sort(sorted.begin(), sorted.end(), [](const PendingPacket& lhs,
                                                const PendingPacket& rhs) {
        return lhs.sequence < rhs.sequence;
    });
    if (sorted.size() <= 1 ||
            static_cast<int>(sorted.last().sequence) - sorted.first().sequence <= 0x8000) {
        return sorted;
    }
    int gapIndex = 0;
    int largestGap = -1;
    for (int index = 0; index < sorted.size(); ++index) {
        const quint16 next = sorted.at((index + 1) % sorted.size()).sequence;
        const int gap = (static_cast<int>(next) - sorted.at(index).sequence) & 0xffff;
        if (gap > largestGap) {
            largestGap = gap;
            gapIndex = index;
        }
    }
    const int start = (gapIndex + 1) % sorted.size();
    return sorted.mid(start) + sorted.mid(0, start);
}

QList<QByteArray> AppleHevcAssembler::reassemble(const QList<PendingPacket>& packets)
{
    QList<QByteArray> units;
    QByteArray fragmented;
    bool fragmentActive = false;
    for (const PendingPacket& packet : packets) {
        const QByteArray& payload = packet.payload;
        const quint8 type = hevcType(payload);
        if (type == HevcAggregationType) {
            int offset = 4;
            QList<QByteArray> aggregated;
            bool valid = true;
            while (offset < payload.size()) {
                bool lengthOk = false;
                const int length = AppleWire::readUInt16(payload, offset, &lengthOk);
                offset += 2;
                if (!lengthOk || length <= 0 || length > payload.size() - offset) {
                    valid = false;
                    break;
                }
                aggregated.append(payload.mid(offset, length));
                offset += length;
            }
            if (valid && !aggregated.isEmpty()) {
                units.append(aggregated);
            }
        }
        else if (type == HevcFragmentationType) {
            if (payload.size() < 6) {
                continue;
            }
            const quint8 fragmentHeader = byteAt(payload, 2);
            const bool start = (fragmentHeader & 0x80) != 0;
            const bool end = (fragmentHeader & 0x40) != 0;
            if (start) {
                const quint8 innerType = fragmentHeader & 0x3f;
                fragmented.clear();
                fragmented.append(static_cast<char>(
                        (byteAt(payload, 0) & 0x81) | (innerType << 1)));
                fragmented.append(payload.at(1));
                fragmented.append(payload.mid(5));
                fragmentActive = true;
                if (end) {
                    units.append(fragmented);
                    fragmentActive = false;
                }
            }
            else if (fragmentActive) {
                fragmented.append(payload.mid(5));
                if (end) {
                    units.append(fragmented);
                    fragmentActive = false;
                }
            }
        }
        else if (type != 0xff && payload.size() >= 4) {
            units.append(payload.left(2) + payload.mid(4));
        }
    }
    return units;
}

void AppleHevcAssembler::observeSequence(const AppleRtpPacket& packet,
                                         qint64 nowMilliseconds)
{
    const quint32 source = packet.synchronizationSource;
    const quint16 sequence = packet.sequenceNumber;
    if (!m_MaximumSequence.contains(source)) {
        m_MaximumSequence.insert(source, sequence);
        return;
    }
    const quint16 previous = m_MaximumSequence.value(source);
    const quint16 difference = sequence - previous;
    if (difference == 0) {
        return;
    }
    if (difference > 0x8000) {
        m_MissingPackets[source].remove(sequence);
        return;
    }
    if (difference > 1) {
        const int count = qMin(static_cast<int>(difference) - 1, 17);
        const int first = static_cast<int>(difference) - count;
        for (int offset = first; offset < difference; ++offset) {
            const quint16 missing = previous + static_cast<quint16>(offset);
            if (!m_MissingPackets[source].contains(missing)) {
                m_MissingPackets[source].insert(missing,
                                                {nowMilliseconds, -1});
            }
        }
    }
    m_MaximumSequence[source] = sequence;
}

bool AppleHevcAssembler::process(const AppleRtpPacket& packet,
                                 qint64 nowMilliseconds,
                                 AppleHevcAccessUnit* accessUnit)
{
    if (accessUnit == nullptr || packet.payload.isEmpty()) {
        return false;
    }
    observeSequence(packet, nowMilliseconds);
    ++m_SourcePacketCounts[packet.synchronizationSource];
    const quint64 key = static_cast<quint64>(packet.synchronizationSource) << 32 |
                        packet.timestamp;
    PendingAccessUnit group = m_Groups.value(key);
    if (group.firstSeenAt == 0) {
        group.firstSeenAt = nowMilliseconds;
    }
    const std::optional<quint16> decodingOrder =
            firstDecodingOrderNumber(packet.payload);
    if (group.decodingOrderNumber.has_value() && decodingOrder.has_value() &&
            group.decodingOrderNumber != decodingOrder) {
        m_Groups.insert(key, group);
        expire(nowMilliseconds);
        return false;
    }
    if (!group.decodingOrderNumber.has_value()) {
        group.decodingOrderNumber = decodingOrder;
    }
    const std::optional<AppleRtpPacket::FramePacketInfo> framePacketInfo =
            packet.framePacketInfo();
    if (framePacketInfo.has_value()) {
        if (group.frameSequenceNumber.has_value() &&
                group.frameSequenceNumber != framePacketInfo->frameSequenceNumber) {
            m_Groups.insert(key, group);
            expire(nowMilliseconds);
            return false;
        }
        group.frameSequenceNumber = framePacketInfo->frameSequenceNumber;
        group.totalPacketsPerFrame = framePacketInfo->totalPacketsPerFrame;
    }
    const auto duplicate = std::find_if(
            group.packets.cbegin(), group.packets.cend(),
            [&packet](const PendingPacket& existing) {
                return existing.sequence == packet.sequenceNumber;
            });
    if (duplicate == group.packets.cend()) {
        group.packets.append({packet.sequenceNumber, packet.payload, packet.marker});
    }
    m_Groups.insert(key, group);
    if (!m_PlayoutTimestamp.has_value() ||
            (packet.timestamp - *m_PlayoutTimestamp) <= 0x7fffffffU) {
        m_PlayoutTimestamp = packet.timestamp;
    }
    expire(nowMilliseconds);
    if (std::none_of(group.packets.cbegin(), group.packets.cend(),
                     [](const PendingPacket& value) { return value.marker; })) {
        return false;
    }
    const QList<PendingPacket> ordered = sequenceOrdered(group.packets);
    if (ordered.isEmpty() || !ordered.last().marker ||
            !startsAccessUnit(ordered.first().payload)) {
        return false;
    }
    for (int index = 0; index + 1 < ordered.size(); ++index) {
        if (static_cast<quint16>(ordered.at(index).sequence + 1) !=
                ordered.at(index + 1).sequence) {
            return false;
        }
    }
    const QList<QByteArray> units = reassemble(ordered);
    if (units.isEmpty()) {
        return false;
    }
    m_Groups.remove(key);
    harvest(units);
    m_CompletedSources.insert(packet.synchronizationSource);
    accessUnit->synchronizationSource = packet.synchronizationSource;
    accessUnit->timestamp = packet.timestamp;
    accessUnit->decodingOrderNumber = group.decodingOrderNumber;
    accessUnit->frameSequenceNumber = group.frameSequenceNumber;
    accessUnit->totalPacketsPerFrame = group.totalPacketsPerFrame;
    accessUnit->nalUnits = units;
    return true;
}

void AppleHevcAssembler::harvest(const QList<QByteArray>& units)
{
    for (const QByteArray& unit : units) {
        switch (hevcType(unit)) {
        case 32:
            m_ParameterSets.video = unit;
            break;
        case 33:
            m_ParameterSets.sequence = unit;
            break;
        case 34:
            if (!m_ParameterSets.pictures.contains(unit)) {
                m_ParameterSets.pictures.append(unit);
            }
            break;
        default:
            break;
        }
    }
}

void AppleHevcAssembler::expire(qint64 nowMilliseconds)
{
    QList<quint64> expired;
    for (auto iterator = m_Groups.cbegin(); iterator != m_Groups.cend(); ++iterator) {
        const quint32 timestamp = static_cast<quint32>(iterator.key());
        const quint32 delta = m_PlayoutTimestamp.has_value()
                ? *m_PlayoutTimestamp - timestamp : 0;
        if (nowMilliseconds - iterator.value().firstSeenAt > 500 ||
                (delta <= 0x7fffffffU && delta > 12000)) {
            expired.append(iterator.key());
        }
    }
    for (quint64 key : expired) {
        m_Groups.remove(key);
    }
}

void AppleHevcAssembler::discardIncomplete()
{
    m_Groups.clear();
}

int AppleHevcAssembler::totalPacketCount() const
{
    int result = 0;
    for (int count : m_SourcePacketCounts) {
        result += count;
    }
    return result;
}

QList<quint32> AppleHevcAssembler::primarySources(
        int tileCount,
        const QHash<quint32, int>& baseline) const
{
    if (tileCount <= 0) {
        return {};
    }
    QList<quint32> sources = m_SourcePacketCounts.keys();
    std::sort(sources.begin(), sources.end());
    QList<QList<quint32>> groups;
    for (quint32 source : sources) {
        if (!groups.isEmpty() && groups.last().size() < tileCount &&
                source - groups.last().last() <= 1) {
            groups.last().append(source);
        }
        else {
            groups.append({source});
        }
    }
    QList<quint32> best;
    int bestRecent = -1;
    int bestTotal = -1;
    for (const QList<quint32>& group : groups) {
        if (group.size() != tileCount) {
            continue;
        }
        int recent = 0;
        int total = 0;
        for (quint32 source : group) {
            total += m_SourcePacketCounts.value(source);
            recent += qMax(0, m_SourcePacketCounts.value(source) - baseline.value(source));
        }
        if (recent > bestRecent || (recent == bestRecent && total > bestTotal)) {
            best = group;
            bestRecent = recent;
            bestTotal = total;
        }
    }
    return best;
}

QHash<quint32, QList<quint16>> AppleHevcAssembler::takeNacks(
        qint64 nowMilliseconds)
{
    QHash<quint32, QList<quint16>> result;
    for (auto sourceIterator = m_MissingPackets.begin();
         sourceIterator != m_MissingPackets.end();) {
        auto& packets = sourceIterator.value();
        for (auto packetIterator = packets.begin(); packetIterator != packets.end();) {
            MissingPacket& packet = packetIterator.value();
            if (nowMilliseconds - packet.firstDetectedAt > 1000) {
                packetIterator = packets.erase(packetIterator);
                continue;
            }
            if (packet.lastNackAt < 0 || nowMilliseconds - packet.lastNackAt >= 150) {
                result[sourceIterator.key()].append(packetIterator.key());
                packet.lastNackAt = nowMilliseconds;
            }
            ++packetIterator;
        }
        if (packets.isEmpty()) {
            sourceIterator = m_MissingPackets.erase(sourceIterator);
        }
        else {
            ++sourceIterator;
        }
    }
    return result;
}
