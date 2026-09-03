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
#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr int AuthenticationTagLength = 10;
constexpr quint8 HevcAggregationType = 48;
constexpr quint8 HevcFragmentationType = 49;
// This is the EOD field negotiated in the High Performance FLS decoder
// details below. Apple appends the control field to the final VCL RBSP byte.
constexpr quint8 HevcEndOfDataBit = 1;

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
    const bool baseValid = audioViewer.size() == BlobLength &&
            audioServer.size() == BlobLength &&
            videoViewer.size() == BlobLength && videoServer.size() == BlobLength;
    const bool secondaryEmpty = secondaryVideoViewer.isEmpty() &&
            secondaryVideoServer.isEmpty();
    const bool secondaryValid = secondaryVideoViewer.size() == BlobLength &&
            secondaryVideoServer.size() == BlobLength;
    return baseValid && (secondaryEmpty || secondaryValid);
}

bool AppleMediaKeys::hasSecondaryVideo() const
{
    return secondaryVideoViewer.size() == BlobLength &&
            secondaryVideoServer.size() == BlobLength;
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
    return configuration(offers, nullptr, keys, callId, error);
}

QByteArray configuration(const AppleMediaOffers& offers,
                         const AppleMediaOffers* secondaryOffers,
                         const AppleMediaKeys& keys,
                         const QUuid& callId,
                         QString* error)
{
    const bool hasSecondaryOffer = secondaryOffers != nullptr &&
            !secondaryOffers->video.isEmpty();
    const int secondaryOfferSize = hasSecondaryOffer
            ? secondaryOffers->video.size() : 0;
    const int messageSize = offers.audio.size() + offers.video.size() +
            secondaryOfferSize + 0xd8 + (hasSecondaryOffer ? 92 : 0);
    if (!keys.isValid() || offers.audio.isEmpty() || offers.video.isEmpty() ||
            callId.isNull() || offers.audio.size() > 65535 ||
            offers.video.size() > 65535 || secondaryOfferSize > 65535 ||
            keys.hasSecondaryVideo() != hasSecondaryOffer ||
            messageSize > 65535) {
        setError(error, QCoreApplication::translate(
                "AppleMediaProtocol", "The media configuration is invalid or too large."));
        return {};
    }
    QByteArray message(messageSize + 4, '\0');
    message[0] = char(0x1c);
    writeUInt16(message, 2, static_cast<quint16>(messageSize));
    writeUInt16(message, 4, 3);
    writeUInt32(message, 6, hasSecondaryOffer ? 0x07 : 0x05);
    writeUInt16(message, 10, static_cast<quint16>(offers.audio.size()));
    writeUInt16(message, 12, static_cast<quint16>(offers.video.size()));
    writeUInt16(message, 14, static_cast<quint16>(secondaryOfferSize));
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
    if (hasSecondaryOffer) {
        const int secondaryOffset = videoOffset + 92 + offers.video.size();
        std::memcpy(message.data() + secondaryOffset,
                    keys.secondaryVideoViewer.constData(), 46);
        std::memcpy(message.data() + secondaryOffset + 46,
                    keys.secondaryVideoServer.constData(), 46);
        std::memcpy(message.data() + secondaryOffset + 92,
                    secondaryOffers->video.constData(), secondaryOfferSize);
    }
    return message;
}

QByteArray framebufferUpdateRequest()
{
    return QByteArray::fromHex("030000000000ffffffff");
}

QByteArray autoFramebufferUpdate(quint32 intervalMilliseconds)
{
    QByteArray message = QByteArray::fromHex(
            "090000010000000000000000ffffffff");
    writeUInt32(message, 4, intervalMilliseconds);
    return message;
}

QByteArray controlMode(bool observing)
{
    QByteArray message = QByteArray::fromHex("0a000000");
    writeUInt16(message, 2, observing ? 0 : 1);
    return message;
}

QByteArray selectCombinedDisplays()
{
    return QByteArray::fromHex("0d01000000000000");
}

QByteArray selectDisplay(quint32 displayId)
{
    QByteArray message = QByteArray::fromHex("0d00000000000000");
    writeUInt32(message, 4, displayId);
    return message;
}

AppleScrollWheelEvent scrollWheelDeltas(qint32 deltaX,
                                        qint32 deltaY,
                                        double preciseDeltaX,
                                        double preciseDeltaY,
                                        bool flipped,
                                        quint32 scrollCount,
                                        double speedMultiplier)
{
    const qint64 direction = flipped ? -1 : 1;
    const double speed = std::isfinite(speedMultiplier)
            ? std::clamp(speedMultiplier, 0.5, 3.0) : 1.0;
    const auto clampInt16 = [](qint64 value) {
        return static_cast<qint16>(std::clamp<qint64>(
                value, std::numeric_limits<qint16>::min(),
                std::numeric_limits<qint16>::max()));
    };
    const auto scaledInt32 = [](double value, double scale) {
        if (!std::isfinite(value)) {
            return qint32{0};
        }
        const long double scaled = static_cast<long double>(value) * scale;
        const long double bounded = std::clamp<long double>(
                scaled, std::numeric_limits<qint32>::min(),
                std::numeric_limits<qint32>::max());
        return static_cast<qint32>(std::llround(bounded));
    };
    const auto effectivePrecision = [](double precise, qint32 integral) {
        return std::isfinite(precise) && (precise != 0.0 || integral == 0)
                ? precise : static_cast<double>(integral);
    };

    const double normalizedPreciseX = effectivePrecision(
            preciseDeltaX, deltaX) * direction * speed;
    const double normalizedPreciseY = effectivePrecision(
            preciseDeltaY, deltaY) * direction * speed;
    AppleScrollWheelEvent event;
    event.deltaX = clampInt16(std::llround(deltaX * direction * speed));
    event.deltaY = clampInt16(std::llround(deltaY * direction * speed));
    event.fixedDeltaX = scaledInt32(normalizedPreciseX, 65536.0);
    event.fixedDeltaY = scaledInt32(normalizedPreciseY, 65536.0);
    event.pointDeltaX = scaledInt32(normalizedPreciseX, 10.0);
    event.pointDeltaY = scaledInt32(normalizedPreciseY, 10.0);
    event.scrollCount = scrollCount;
    return event;
}

QByteArray scrollWheelEvent(const AppleScrollWheelEvent& event,
                            quint16 x,
                            quint16 y)
{
    QByteArray message(58, '\0');
    message[0] = char(0x17);
    writeUInt16(message, 2, 54);
    writeUInt16(message, 4, 1);
    writeUInt16(message, 6, 11);
    writeUInt16(message, 8, static_cast<quint16>(event.deltaX));
    writeUInt16(message, 10, static_cast<quint16>(event.deltaY));
    writeUInt16(message, 12, static_cast<quint16>(event.deltaZ));
    writeUInt32(message, 14, static_cast<quint32>(event.fixedDeltaX));
    writeUInt32(message, 18, static_cast<quint32>(event.fixedDeltaY));
    writeUInt32(message, 22, static_cast<quint32>(event.fixedDeltaZ));
    writeUInt32(message, 26, static_cast<quint32>(event.pointDeltaX));
    writeUInt32(message, 30, static_cast<quint32>(event.pointDeltaY));
    writeUInt32(message, 34, static_cast<quint32>(event.pointDeltaZ));
    writeUInt32(message, 38, event.scrollPhase);
    writeUInt32(message, 42, event.momentumPhase);
    writeUInt32(message, 46, event.scrollCount);
    writeUInt32(message, 50, event.flags);
    writeUInt16(message, 54, x);
    writeUInt16(message, 56, y);
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
    candidate.videos.append(candidate.video);
    if (answer.size() > 0x18 && (byteAt(answer, 0x18) & 1) != 0) {
        const quint16 secondary = AppleWire::readUInt16(answer, 0x16);
        if (secondary == 0) {
            return false;
        }
        candidate.videos.append(secondary);
    }
    if (!candidate.isUsable()) {
        return false;
    }
    *ports = candidate;
    return true;
}

bool parseCanvas(const QByteArray& answer, AppleCanvas* canvas)
{
    if (canvas == nullptr) {
        return false;
    }
    const QList<AppleCanvas> canvases = parseCanvases(answer);
    if (canvases.isEmpty()) {
        return false;
    }
    *canvas = canvases.first();
    return true;
}

QList<AppleCanvas> parseCanvases(const QByteArray& answer)
{
    QList<AppleCanvas> canvases;
    if (answer.isEmpty() || byteAt(answer, 0) != 0) {
        return canvases;
    }
    int search = 0;
    while ((search = answer.indexOf("bplist", search)) >= 0) {
        bool found = false;
        for (int end = answer.size(); end >= search + 40; --end) {
            QByteArray compressed;
            BinaryPlistReader reader(answer.mid(search, end - search));
            if (!reader.dataForKey(QByteArrayLiteral(
                        "avcMediaStreamNegotiatorMediaBlob"), &compressed)) {
                continue;
            }
            const QByteArray blob = inflateMediaBlob(compressed);
            AppleCanvas canvas;
            if (!blob.isEmpty() && canvasFromProtobuf(blob, &canvas)) {
                canvases.append(canvas);
                found = true;
                break;
            }
        }
        search += 6;
        if (!found) {
            continue;
        }
    }
    return canvases;
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
                                     quint16 keyCode,
                                     quint8 subtype)
{
    AppleInputEncryptionRequest request;
    request.header.append(char(0x10));
    request.header.append(static_cast<char>(subtype));
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

QByteArray receiverReport(quint32 sender,
                          const AppleRtpReceptionReport& report)
{
    QByteArray packet = QByteArray::fromHex("81c90007");
    AppleWire::appendUInt32(packet, sender);
    AppleWire::appendUInt32(packet, report.source);
    packet.append(static_cast<char>(report.fractionLost));
    const qint32 clampedLoss = std::clamp<qint32>(
            report.cumulativePacketsLost, -0x800000, 0x7fffff);
    const quint32 lossBits = static_cast<quint32>(clampedLoss) & 0x00ffffff;
    packet.append(static_cast<char>(lossBits >> 16));
    packet.append(static_cast<char>(lossBits >> 8));
    packet.append(static_cast<char>(lossBits));
    AppleWire::appendUInt32(packet, report.extendedHighestSequence);
    AppleWire::appendUInt32(packet, report.interarrivalJitter);
    AppleWire::appendUInt32(packet, report.lastSenderReport);
    AppleWire::appendUInt32(packet, report.delaySinceLastSenderReport);
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

QByteArray frameLossFeedback(
        quint32 sender,
        const AppleVideoFrameLossFeedback& feedback)
{
    QByteArray packet = QByteArray::fromHex("8fce0005");
    AppleWire::appendUInt32(packet, sender);
    AppleWire::appendUInt32(packet, feedback.mediaSource);
    AppleWire::appendUInt32(packet, 6);
    AppleWire::appendUInt32(packet, feedback.rtpTimestamp);
    AppleWire::appendUInt16(packet, feedback.packedLoss());
    packet.append(static_cast<char>(feedback.expectedPacketCount));
    packet.append(static_cast<char>(feedback.lostPacketCount));
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
    AppleVideoRateControlInfo info;
    info.rtpTimestamp = rtpTimestamp;
    info.estimatedBandwidthKilobitsPerSecond =
            estimatedBandwidthKilobitsPerSecond;
    info.receivedPacketCount = receivedPacketCount;
    info.feedbackDelayMilliseconds = feedbackDelayMilliseconds;
    info.echoTimestamp = echoTimestamp;
    return rateControl(sender, info);
}

QByteArray rateControl(quint32 sender,
                       const AppleVideoRateControlInfo& info)
{
    QByteArray packet = QByteArray::fromHex("80cc0007");
    AppleWire::appendUInt32(packet, sender);
    packet.append("RCTL", 4);

    packet.append(QByteArray::fromHex("85000004"));
    AppleWire::appendUInt16(packet, static_cast<quint16>(
            info.rtpTimestamp >> 8));
    AppleWire::appendUInt16(packet, 0);
    AppleWire::appendUInt16(packet, 0);
    AppleWire::appendUInt16(packet, static_cast<quint16>(
            qMin(info.feedbackDelayMilliseconds,
                 static_cast<quint32>(0xffff))));
    AppleWire::appendUInt16(packet, info.echoTimestamp);
    quint16 packedDelay = 0;
    if (std::isfinite(info.oneWayReceiveDelaySeconds) &&
            info.oneWayReceiveDelaySeconds > 0.0) {
        packedDelay = static_cast<quint16>(std::clamp<double>(
                std::floor(info.oneWayReceiveDelaySeconds * 8192.0),
                0.0, 65535.0));
    }
    AppleWire::appendUInt16(packet, packedDelay);
    AppleWire::appendUInt16(packet, static_cast<quint16>(
            (qMin(info.burstyLoss, static_cast<quint16>(15)) << 12) |
            (info.receivedPacketCount & 0x0fff)));
    AppleWire::appendUInt16(packet, static_cast<quint16>(
            qMin(info.estimatedBandwidthKilobitsPerSecond,
                 static_cast<quint32>(0xffff))));
    return packet;
}

} // namespace AppleMediaWire

void AppleVideoRateControlEstimator::observe(
        quint32 rtpTimestamp,
        qint64 arrivalNanoseconds,
        int packetSize,
        AppleVideoBandwidthProbeActivity activity)
{
    m_DidReceiveVideo = true;
    ++m_TotalPacketsReceived;
    updateTimestampStatistics(rtpTimestamp, arrivalNanoseconds);
    if (activity == AppleVideoBandwidthProbeActivity::Active) {
        updateBandwidthEstimate(rtpTimestamp, arrivalNanoseconds, packetSize);
    }
    else {
        finalizeBandwidthProbe();
    }
}

std::optional<AppleVideoRateControlInfo>
AppleVideoRateControlEstimator::feedback(qint64 nowNanoseconds) const
{
    if (!m_DidReceiveVideo) {
        return std::nullopt;
    }
    AppleVideoRateControlInfo info;
    info.rtpTimestamp = m_LastAcceptedTimestamp;
    if (m_EstimatedBandwidth.has_value()) {
        quint32 kilobits = static_cast<quint32>(qMax(
                0.0, *m_EstimatedBandwidth / 1000.0));
        kilobits &= ~quint32{7};
        info.estimatedBandwidthKilobitsPerSecond =
                m_EstimateState == EstimateState::Stable
                        ? kilobits + 1 : kilobits;
    }
    info.receivedPacketCount = m_LastAcceptedPacketCount;
    if (m_LastAcceptedArrivalNanoseconds < 0) {
        info.feedbackDelayMilliseconds = std::numeric_limits<quint32>::max();
    }
    else {
        const qint64 ageNanoseconds = qMax<qint64>(
                0, nowNanoseconds - m_LastAcceptedArrivalNanoseconds);
        info.feedbackDelayMilliseconds = static_cast<quint32>(qMin<quint64>(
                static_cast<quint64>(ageNanoseconds) / 1000000ULL,
                std::numeric_limits<quint32>::max()));
    }
    if (nowNanoseconds > 0) {
        const quint64 nanoseconds = static_cast<quint64>(nowNanoseconds);
        const quint64 scaled = (nanoseconds / 1000000000ULL) * 1024ULL +
                (nanoseconds % 1000000000ULL) * 1024ULL / 1000000000ULL;
        info.echoTimestamp = static_cast<quint16>(
                scaled & 0xffff);
    }
    info.oneWayReceiveDelaySeconds = m_OneWayReceiveDelay;
    return info;
}

void AppleVideoRateControlEstimator::updateTimestampStatistics(
        quint32 rtpTimestamp,
        qint64 arrivalNanoseconds)
{
    if (!m_PreviousTimestamp.has_value()) {
        m_PreviousTimestamp = rtpTimestamp;
        return;
    }
    if (rtpTimestamp == *m_PreviousTimestamp) {
        return;
    }
    const quint32 delta = rtpTimestamp - *m_PreviousTimestamp;
    if (delta >= 0x7fffffffU) {
        return;
    }
    m_PreviousTimestamp = rtpTimestamp;
    m_LastAcceptedTimestamp = rtpTimestamp;
    m_LastAcceptedPacketCount = m_TotalPacketsReceived;
    m_LastAcceptedArrivalNanoseconds = arrivalNanoseconds;
    updateOneWayReceiveDelay(rtpTimestamp, arrivalNanoseconds);
}

void AppleVideoRateControlEstimator::updateBandwidthEstimate(
        quint32 rtpTimestamp,
        qint64 arrivalNanoseconds,
        int packetSize)
{
    if (!m_ProbeGroup.has_value()) {
        m_ProbeGroup = ProbeGroup{
            rtpTimestamp, arrivalNanoseconds, arrivalNanoseconds, 0, 0,
        };
        return;
    }
    if (m_ProbeGroup->timestamp != rtpTimestamp) {
        finalize(*m_ProbeGroup);
        m_ProbeGroup = ProbeGroup{
            rtpTimestamp, arrivalNanoseconds, arrivalNanoseconds, 0, 0,
        };
        return;
    }
    m_ProbeGroup->bytesAfterReference +=
            static_cast<quint64>(qMax(0, packetSize));
    ++m_ProbeGroup->packetsAfterReference;
    m_ProbeGroup->lastArrivalNanoseconds = qMax(
            m_ProbeGroup->lastArrivalNanoseconds, arrivalNanoseconds);
}

void AppleVideoRateControlEstimator::finalizeBandwidthProbe()
{
    if (!m_ProbeGroup.has_value()) {
        return;
    }
    finalize(*m_ProbeGroup);
    m_ProbeGroup.reset();
}

void AppleVideoRateControlEstimator::finalize(const ProbeGroup& group)
{
    if (group.bytesAfterReference <= 250 ||
            group.packetsAfterReference < 1) {
        return;
    }
    const qint64 durationNanoseconds = group.lastArrivalNanoseconds -
            group.referenceArrivalNanoseconds;
    if (durationNanoseconds <= 0) {
        return;
    }
    const double durationSeconds = durationNanoseconds / 1000000000.0;
    const double candidate = qMin(
            60000000.0,
            group.bytesAfterReference * 8.0 / durationSeconds);
    if (durationNanoseconds < 8000000) {
        ++m_ConsecutiveShortProbeCount;
        if (m_ConsecutiveShortProbeCount < 3) {
            m_EstimateState = EstimateState::InsufficientProbeWindow;
            return;
        }
    }
    else {
        m_ConsecutiveShortProbeCount = 0;
    }
    applyCandidate(candidate);
}

void AppleVideoRateControlEstimator::applyCandidate(
        double candidateBitsPerSecond)
{
    if (!m_EstimatedBandwidth.has_value()) {
        m_EstimatedBandwidth = qMax(candidateBitsPerSecond, 100000.0);
        m_EstimateState = EstimateState::Initial;
        return;
    }
    const double difference = candidateBitsPerSecond - *m_EstimatedBandwidth;
    const bool isDown = candidateBitsPerSecond <=
                    0.5 * *m_EstimatedBandwidth ||
            -difference > 200000.0;
    const bool isUp = candidateBitsPerSecond >=
                    1.5 * *m_EstimatedBandwidth ||
            difference > 200000.0;
    if (!isDown && !isUp) {
        m_EstimatedBandwidth = 0.1 * candidateBitsPerSecond +
                0.9 * *m_EstimatedBandwidth;
        clearPendingEstimate(EstimateState::Stable);
        return;
    }
    const int direction = isUp ? 1 : -1;
    if (direction != m_PendingDirection) {
        m_PendingDirection = direction;
        m_PendingBandwidth = candidateBitsPerSecond;
        m_PendingCount = 1;
    }
    else {
        m_PendingBandwidth += candidateBitsPerSecond;
        ++m_PendingCount;
    }
    m_EstimateState = direction > 0
            ? EstimateState::PendingUp : EstimateState::PendingDown;
    if (m_PendingCount < 3) {
        return;
    }
    m_EstimatedBandwidth = m_PendingBandwidth / m_PendingCount;
    clearPendingEstimate(EstimateState::Committed);
}

void AppleVideoRateControlEstimator::clearPendingEstimate(EstimateState state)
{
    m_PendingDirection = 0;
    m_PendingBandwidth = 0.0;
    m_PendingCount = 0;
    m_EstimateState = state;
}

void AppleVideoRateControlEstimator::updateOneWayReceiveDelay(
        quint32 timestamp,
        qint64 arrivalNanoseconds)
{
    if (m_DelayPreviousTimestamp.has_value() &&
            timestamp < *m_DelayPreviousTimestamp &&
            *m_DelayPreviousTimestamp - timestamp > 0x7fffffffU) {
        m_DelayWrapOffset += quint64{1} << 32;
    }
    m_DelayPreviousTimestamp = timestamp;
    const quint64 unwrappedTimestamp = m_DelayWrapOffset + timestamp;
    if (!m_FirstUnwrappedTimestamp.has_value()) {
        m_FirstUnwrappedTimestamp = unwrappedTimestamp;
        m_FirstDelayArrivalNanoseconds = arrivalNanoseconds;
        return;
    }
    const double sendRelative =
            (unwrappedTimestamp - *m_FirstUnwrappedTimestamp) / 24000.0;
    const double receiveRelative =
            (arrivalNanoseconds - m_FirstDelayArrivalNanoseconds) /
            1000000000.0;
    const double lag = receiveRelative - sendRelative;
    m_ShortDelay = 0.1 * lag + 0.9 * m_ShortDelay;
    m_LongDelay = 0.0001 * lag + 0.9999 * m_LongDelay;
    const double delay = m_ShortDelay - m_LongDelay;
    if (delay < 0.0) {
        m_LongDelay = m_ShortDelay;
        m_OneWayReceiveDelay = 0.0;
    }
    else {
        m_OneWayReceiveDelay = delay;
    }
}

std::optional<quint32> AppleFrameUpdatePauseState::setMiniaturized(
        bool miniaturized,
        int endpointWindowCount)
{
    if (miniaturized) {
        if (endpointWindowCount != 1 || m_IsPaused) {
            return std::nullopt;
        }
        m_IsPaused = true;
        return std::numeric_limits<quint32>::max();
    }
    if (!m_IsPaused) {
        return std::nullopt;
    }
    m_IsPaused = false;
    return 0;
}

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

AppleHevcAccessUnit::SubframeBoundary AppleHevcAssembler::subframeBoundary(
        const QList<QByteArray>& units)
{
    auto unit = std::find_if(units.crbegin(), units.crend(),
                             [](const QByteArray& value) {
        return !value.isEmpty() && hevcType(value) <= 31;
    });
    if (unit == units.crend() || unit->size() <= 2) {
        return AppleHevcAccessUnit::SubframeBoundary::Unknown;
    }

    QByteArray rbsp;
    rbsp.reserve(unit->size() - 2);
    int zeroCount = 0;
    for (int index = 2; index < unit->size(); ++index) {
        const quint8 byte = byteAt(*unit, index);
        if (zeroCount >= 2 && byte == 0x03) {
            zeroCount = 0;
            continue;
        }
        rbsp.append(static_cast<char>(byte));
        zeroCount = byte == 0 ? zeroCount + 1 : 0;
    }
    if (rbsp.isEmpty()) {
        return AppleHevcAccessUnit::SubframeBoundary::Unknown;
    }

    // FLS reserves the high bit as a suffix terminator. VCP clears it before
    // reading the negotiated control fields, so mirror that behavior here.
    const quint8 control = byteAt(rbsp, rbsp.size() - 1) & 0x7f;
    return (control & (1U << HevcEndOfDataBit)) != 0
            ? AppleHevcAccessUnit::SubframeBoundary::Last
            : AppleHevcAccessUnit::SubframeBoundary::NotLast;
}

void AppleHevcAssembler::observeSequence(const AppleRtpPacket& packet,
                                         qint64 nowMilliseconds,
                                         qint64 arrivalNanoseconds)
{
    updateReceptionState(packet, arrivalNanoseconds);
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

void AppleHevcAssembler::updateReceptionState(
        const AppleRtpPacket& packet,
        qint64 arrivalNanoseconds)
{
    const quint32 source = packet.synchronizationSource;
    const qint64 arrivalTimestamp = static_cast<qint64>(std::llround(
            arrivalNanoseconds * (24000.0 / 1000000000.0)));
    const qint64 transit = arrivalTimestamp - packet.timestamp;
    auto stateIterator = m_ReceptionStates.find(source);
    if (stateIterator == m_ReceptionStates.end()) {
        ReceptionState state;
        state.baseExtendedSequence = packet.sequenceNumber;
        state.maximumExtendedSequence = packet.sequenceNumber;
        state.previousTransit = transit;
        m_ReceptionStates.insert(source, state);
        return;
    }
    ReceptionState& state = stateIterator.value();
    const quint32 sequence = extendedSequence(
            packet.sequenceNumber, state.maximumExtendedSequence);
    state.maximumExtendedSequence = qMax(
            state.maximumExtendedSequence, sequence);
    ++state.receivedPackets;
    if (state.previousTransit.has_value()) {
        const double variation = std::abs(
                static_cast<double>(transit - *state.previousTransit));
        state.jitter += (variation - state.jitter) / 16.0;
    }
    state.previousTransit = transit;
}

void AppleHevcAssembler::observeRateControl(
        const AppleRtpPacket& packet,
        qint64 arrivalNanoseconds)
{
    AppleVideoBandwidthProbeActivity activity =
            AppleVideoBandwidthProbeActivity::Boundary;
    if (m_LastProbeEndNanoseconds >= 0 &&
            arrivalNanoseconds - m_LastProbeEndNanoseconds < 2000000000LL) {
        activity = AppleVideoBandwidthProbeActivity::Suppressed;
    }
    else {
        const bool hasPrivateProbeSignature = packet.payload.size() >= 4 &&
                byteAt(packet.payload, 0) == 0x92 &&
                byteAt(packet.payload, 1) == 0xe6 &&
                byteAt(packet.payload, 2) == 0xc0 &&
                byteAt(packet.payload, 3) == 0xa3;
        const bool startsProbe = packet.payload.size() >= 4 &&
                (hasPrivateProbeSignature ||
                 hevcType(packet.payload) == HevcFragmentationType);
        const bool active = startsProbe ||
                (m_ActiveProbeTimestamp.has_value() &&
                 *m_ActiveProbeTimestamp == packet.timestamp);
        if (startsProbe) {
            m_ActiveProbeTimestamp = packet.timestamp;
        }
        else if (!active) {
            m_ActiveProbeTimestamp.reset();
        }
        activity = active ? AppleVideoBandwidthProbeActivity::Active
                          : AppleVideoBandwidthProbeActivity::Boundary;
        if (packet.marker || !active) {
            m_LastProbeEndNanoseconds = arrivalNanoseconds;
        }
    }
    m_RateControlEstimator.observe(
            packet.timestamp, arrivalNanoseconds, packet.payload.size(),
            activity);
}

quint32 AppleHevcAssembler::extendedSequence(quint16 sequence,
                                              quint32 maximum)
{
    const quint32 cycle = maximum & 0xffff0000U;
    const quint16 maximumLow = static_cast<quint16>(maximum);
    const int difference = static_cast<int>(sequence) - maximumLow;
    if (difference < -0x8000) {
        return cycle + 0x10000U + sequence;
    }
    if (difference > 0x8000 && cycle >= 0x10000U) {
        return cycle - 0x10000U + sequence;
    }
    return cycle + sequence;
}

bool AppleHevcAssembler::process(const AppleRtpPacket& packet,
                                 qint64 nowMilliseconds,
                                 AppleHevcAccessUnit* accessUnit,
                                 qint64 arrivalNanoseconds)
{
    if (accessUnit == nullptr || packet.payload.isEmpty()) {
        return false;
    }
    if (arrivalNanoseconds < 0) {
        arrivalNanoseconds = nowMilliseconds * 1000000;
    }
    observeRateControl(packet, arrivalNanoseconds);
    correctCachedFrameLoss(packet);
    observeSequence(packet, nowMilliseconds, arrivalNanoseconds);
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
    cacheFrameLossIfNeeded(packet.synchronizationSource,
                           packet.timestamp, group);
    harvest(units);
    m_CompletedSources.insert(packet.synchronizationSource);
    accessUnit->synchronizationSource = packet.synchronizationSource;
    accessUnit->timestamp = packet.timestamp;
    accessUnit->decodingOrderNumber = group.decodingOrderNumber;
    accessUnit->frameSequenceNumber = group.frameSequenceNumber;
    accessUnit->totalPacketsPerFrame = group.totalPacketsPerFrame;
    accessUnit->nalUnits = units;
    accessUnit->subframeBoundary = subframeBoundary(units);
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
        const QHash<quint32, int>& baseline,
        const QSet<quint32>& excluded) const
{
    if (tileCount <= 0) {
        return {};
    }
    QList<quint32> sources = m_SourcePacketCounts.keys();
    sources.erase(std::remove_if(
            sources.begin(), sources.end(),
            [&excluded](quint32 source) {
                return excluded.contains(source);
            }), sources.end());
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

QList<quint32> AppleHevcAssembler::replacementSources(
        int tileCount,
        const QList<quint32>& currentSources,
        const QSet<quint32>& abandonedSources,
        int minimumPacketsPerSource) const
{
    QSet<quint32> excluded = abandonedSources;
    for (quint32 source : currentSources) {
        excluded.insert(source);
    }
    const QList<quint32> candidates = primarySources(
            tileCount, {}, excluded);
    if (candidates.size() != tileCount ||
            !std::all_of(candidates.cbegin(), candidates.cend(),
                         [this, minimumPacketsPerSource](quint32 source) {
                return m_SourcePacketCounts.value(source) >=
                        minimumPacketsPerSource;
            })) {
        return {};
    }
    return candidates;
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

std::optional<AppleRtpReceptionReport>
AppleHevcAssembler::receptionReport(quint32 source)
{
    auto iterator = m_ReceptionStates.find(source);
    if (iterator == m_ReceptionStates.end()) {
        return std::nullopt;
    }
    ReceptionState& state = iterator.value();
    const quint32 expected = state.maximumExtendedSequence -
            state.baseExtendedSequence + 1;
    const qint64 cumulativeLost = static_cast<qint64>(expected) -
            state.receivedPackets;
    const quint32 expectedInterval = expected -
            state.previousExpectedPackets;
    const quint32 receivedInterval = state.receivedPackets -
            state.previousReceivedPackets;
    const qint64 lostInterval = static_cast<qint64>(expectedInterval) -
            receivedInterval;
    quint8 fractionLost = 0;
    if (expectedInterval != 0 && lostInterval > 0) {
        fractionLost = static_cast<quint8>(qMin<qint64>(
                255, (lostInterval << 8) / expectedInterval));
    }
    state.previousExpectedPackets = expected;
    state.previousReceivedPackets = state.receivedPackets;

    AppleRtpReceptionReport report;
    report.source = source;
    report.fractionLost = fractionLost;
    report.cumulativePacketsLost = static_cast<qint32>(std::clamp<qint64>(
            cumulativeLost, std::numeric_limits<qint32>::min(),
            std::numeric_limits<qint32>::max()));
    report.extendedHighestSequence = state.maximumExtendedSequence;
    report.interarrivalJitter = static_cast<quint32>(qMax(
            0.0, std::round(state.jitter)));
    return report;
}

std::optional<AppleVideoRateControlInfo>
AppleHevcAssembler::rateControlFeedback(qint64 nowNanoseconds) const
{
    return m_RateControlEstimator.feedback(nowNanoseconds);
}

void AppleHevcAssembler::cacheFrameLossIfNeeded(
        quint32 source,
        quint32 timestamp,
        const PendingAccessUnit& group)
{
    if (!group.frameSequenceNumber.has_value() ||
            !group.totalPacketsPerFrame.has_value() ||
            *group.totalPacketsPerFrame <= group.packets.size()) {
        return;
    }
    CachedFrameLoss loss;
    loss.rtpTimestamp = timestamp;
    loss.frameSequenceNumber = *group.frameSequenceNumber;
    loss.expectedPacketCount = *group.totalPacketsPerFrame;
    for (const PendingPacket& packet : group.packets) {
        loss.receivedSequences.insert(packet.sequence);
    }
    m_CachedFrameLosses.insert(source, std::move(loss));
}

void AppleHevcAssembler::correctCachedFrameLoss(
        const AppleRtpPacket& packet)
{
    auto iterator = m_CachedFrameLosses.find(packet.synchronizationSource);
    const auto frameInfo = packet.framePacketInfo();
    if (iterator == m_CachedFrameLosses.end() || !frameInfo.has_value() ||
            iterator->rtpTimestamp != packet.timestamp ||
            iterator->frameSequenceNumber != frameInfo->frameSequenceNumber ||
            iterator->expectedPacketCount != frameInfo->totalPacketsPerFrame) {
        return;
    }
    iterator->receivedSequences.insert(packet.sequenceNumber);
    if (iterator->receivedSequences.size() >= iterator->expectedPacketCount) {
        m_CachedFrameLosses.erase(iterator);
    }
}

QList<AppleVideoFrameLossFeedback>
AppleHevcAssembler::frameLossFeedbackDue(qint64 nowMilliseconds) const
{
    QList<quint32> sources = m_CachedFrameLosses.keys();
    std::sort(sources.begin(), sources.end());
    QList<AppleVideoFrameLossFeedback> result;
    for (quint32 source : sources) {
        const CachedFrameLoss& loss = m_CachedFrameLosses.value(source);
        const int lost = qMax(0, static_cast<int>(loss.expectedPacketCount) -
                                    loss.receivedSequences.size());
        if (lost == 0 || (loss.lastSentAt >= 0 &&
                         nowMilliseconds - loss.lastSentAt < 100)) {
            continue;
        }
        result.append({source, loss.rtpTimestamp, loss.expectedPacketCount,
                       static_cast<quint16>(lost)});
    }
    return result;
}

void AppleHevcAssembler::markFrameLossFeedbackSent(
        const AppleVideoFrameLossFeedback& feedback,
        qint64 nowMilliseconds)
{
    auto iterator = m_CachedFrameLosses.find(feedback.mediaSource);
    if (iterator != m_CachedFrameLosses.end() &&
            iterator->rtpTimestamp == feedback.rtpTimestamp) {
        iterator->lastSentAt = nowMilliseconds;
    }
}
