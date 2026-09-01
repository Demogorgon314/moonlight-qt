#include "appleprotocol.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QScopeGuard>
#include <QtMath>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace {

constexpr int AesBlockSize = 16;
constexpr int AuthenticationCodeLength = 20;

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
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

void writeUInt64(QByteArray& data, int offset, quint64 value)
{
    for (int i = 0; i < 8; ++i) {
        data[offset + i] = static_cast<char>(value >> (56 - i * 8));
    }
}

QByteArray randomBytes(int length, QString* error)
{
    QByteArray output(length, Qt::Uninitialized);
    if (length > 0 && RAND_bytes(reinterpret_cast<unsigned char*>(output.data()), length) != 1) {
        setError(error, QCoreApplication::translate(
                "AppleProtocol",
                "Couldn’t generate cryptographically secure authentication data."));
        return {};
    }
    return output;
}

QByteArray cryptEcb(const QByteArray& input,
                    const QByteArray& key,
                    bool encrypt,
                    QString* error)
{
    if (key.size() != AesBlockSize || input.size() % AesBlockSize != 0) {
        setError(error, QCoreApplication::translate("AppleProtocol", "Invalid AES input."));
        return {};
    }
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        setError(error, QCoreApplication::translate("AppleProtocol", "Couldn’t create an AES context."));
        return {};
    }
    const auto freeContext = qScopeGuard([context]() { EVP_CIPHER_CTX_free(context); });
    const EVP_CIPHER* cipher = EVP_aes_128_ecb();
    const int initialized = encrypt
            ? EVP_EncryptInit_ex(context, cipher, nullptr,
                                 reinterpret_cast<const unsigned char*>(key.constData()), nullptr)
            : EVP_DecryptInit_ex(context, cipher, nullptr,
                                 reinterpret_cast<const unsigned char*>(key.constData()), nullptr);
    if (initialized != 1 || EVP_CIPHER_CTX_set_padding(context, 0) != 1) {
        setError(error, QCoreApplication::translate("AppleProtocol", "Couldn’t initialize AES."));
        return {};
    }
    QByteArray output(input.size() + AesBlockSize, Qt::Uninitialized);
    int produced = 0;
    int finalProduced = 0;
    const int updated = encrypt
            ? EVP_EncryptUpdate(context,
                                reinterpret_cast<unsigned char*>(output.data()), &produced,
                                reinterpret_cast<const unsigned char*>(input.constData()), input.size())
            : EVP_DecryptUpdate(context,
                                reinterpret_cast<unsigned char*>(output.data()), &produced,
                                reinterpret_cast<const unsigned char*>(input.constData()), input.size());
    const int finalized = encrypt
            ? EVP_EncryptFinal_ex(context,
                                  reinterpret_cast<unsigned char*>(output.data()) + produced,
                                  &finalProduced)
            : EVP_DecryptFinal_ex(context,
                                  reinterpret_cast<unsigned char*>(output.data()) + produced,
                                  &finalProduced);
    if (updated != 1 || finalized != 1) {
        setError(error, QCoreApplication::translate("AppleProtocol", "AES processing failed."));
        return {};
    }
    output.resize(produced + finalProduced);
    return output;
}

QByteArray cryptCbc(const QByteArray& input,
                    const QByteArray& key,
                    const QByteArray& iv,
                    bool encrypt,
                    QString* error)
{
    if (key.size() != AesBlockSize || iv.size() != AesBlockSize ||
            input.isEmpty() || input.size() % AesBlockSize != 0) {
        setError(error, QCoreApplication::translate("AppleProtocol", "Invalid encrypted record."));
        return {};
    }
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        setError(error, QCoreApplication::translate("AppleProtocol", "Couldn’t create an AES context."));
        return {};
    }
    const auto freeContext = qScopeGuard([context]() { EVP_CIPHER_CTX_free(context); });
    const int initialized = encrypt
            ? EVP_EncryptInit_ex(context, EVP_aes_128_cbc(), nullptr,
                                 reinterpret_cast<const unsigned char*>(key.constData()),
                                 reinterpret_cast<const unsigned char*>(iv.constData()))
            : EVP_DecryptInit_ex(context, EVP_aes_128_cbc(), nullptr,
                                 reinterpret_cast<const unsigned char*>(key.constData()),
                                 reinterpret_cast<const unsigned char*>(iv.constData()));
    if (initialized != 1 || EVP_CIPHER_CTX_set_padding(context, 0) != 1) {
        setError(error, QCoreApplication::translate("AppleProtocol", "Couldn’t initialize record encryption."));
        return {};
    }
    QByteArray output(input.size() + AesBlockSize, Qt::Uninitialized);
    int produced = 0;
    int finalProduced = 0;
    const int updated = encrypt
            ? EVP_EncryptUpdate(context,
                                reinterpret_cast<unsigned char*>(output.data()), &produced,
                                reinterpret_cast<const unsigned char*>(input.constData()), input.size())
            : EVP_DecryptUpdate(context,
                                reinterpret_cast<unsigned char*>(output.data()), &produced,
                                reinterpret_cast<const unsigned char*>(input.constData()), input.size());
    const int finalized = encrypt
            ? EVP_EncryptFinal_ex(context,
                                  reinterpret_cast<unsigned char*>(output.data()) + produced,
                                  &finalProduced)
            : EVP_DecryptFinal_ex(context,
                                  reinterpret_cast<unsigned char*>(output.data()) + produced,
                                  &finalProduced);
    if (updated != 1 || finalized != 1) {
        setError(error, QCoreApplication::translate("AppleProtocol", "Encrypted record processing failed."));
        return {};
    }
    output.resize(produced + finalProduced);
    return output;
}

bool constantTimeEqual(const QByteArray& lhs, const QByteArray& rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (int i = 0; i < lhs.size(); ++i) {
        difference |= static_cast<unsigned char>(lhs.at(i)) ^
                      static_cast<unsigned char>(rhs.at(i));
    }
    return difference == 0;
}

QByteArray credentialField(const QString& value, QString* error)
{
    QByteArray field = value.toUtf8();
    if (field.size() >= 64) {
        setError(error, QCoreApplication::translate(
                "AppleProtocol",
                "The account name or password is too long."));
        return {};
    }
    field.append('\0');
    const QByteArray padding = randomBytes(64 - field.size(), error);
    if (padding.size() != 64 - field.size()) {
        return {};
    }
    field.append(padding);
    return field;
}

} // namespace

namespace AppleWire {

QByteArray versionBanner()
{
    return QByteArrayLiteral("RFB 003.889\n");
}

QByteArray publicKeyRequest()
{
    return QByteArray::fromHex("210000000a01005253413100000000");
}

QByteArray viewerInfo()
{
    return QByteArray::fromHex(
            "2100003e0001"
            "00000002"
            "000000050000000300000000"
            "0000000f0000000700000007"
            "b0000803900000000000400000000000"
            "00000000000000000000000000000000");
}

QByteArray setEncryption()
{
    return QByteArray::fromHex("120000010001000100000001");
}

QByteArray setEncodings()
{
    const std::array<qint32, 11> encodings = {
        1010, 1011, 1002, 1104, 1100, -223, 1101, 1105, 1107, 1109, 1110,
    };
    QByteArray message;
    message.append(char(0x02));
    message.append(char(0x00));
    appendUInt16(message, static_cast<quint16>(encodings.size()));
    for (qint32 encoding : encodings) {
        appendUInt32(message, static_cast<quint32>(encoding));
    }
    return message;
}

QByteArray postEncryptionToggle()
{
    return QByteArray::fromHex("1200000200010000");
}

QByteArray displayConfiguration(int width, int height)
{
    return displayConfiguration({QSize(width, height)});
}

QByteArray displayConfiguration(const QList<QSize>& displaySizes,
                                double backingScale,
                                bool hdr)
{
    if (displaySizes.isEmpty() || displaySizes.size() > 2 ||
            !std::isfinite(backingScale) || backingScale <= 0.0 ||
            std::any_of(displaySizes.cbegin(), displaySizes.cend(),
                        [backingScale](const QSize& size) {
        return size.width() <= 0 || size.height() <= 0 ||
                size.width() * backingScale >
                        std::numeric_limits<quint32>::max() ||
                size.height() * backingScale >
                        std::numeric_limits<quint32>::max();
    })) {
        return {};
    }
    constexpr int DescriptorHeaderSize = 0x9c;
    constexpr int ModeSize = 28;
    constexpr int ModeCount = 5;
    constexpr int DescriptorSize = DescriptorHeaderSize + ModeSize * ModeCount;
    const std::array<std::array<int, 4>, ModeCount> modes = {{
        {{3840, 2160, 1920, 1080}},
        {{2880, 1800, 1440, 900}},
        {{3840, 2160, 1920, 1080}},
        {{2880, 1620, 1440, 810}},
        {{2624, 1696, 1312, 848}},
    }};

    QList<QByteArray> descriptors;
    descriptors.reserve(displaySizes.size());
    for (const QSize& size : displaySizes) {
        QByteArray descriptor(DescriptorSize, '\0');
        writeUInt16(descriptor, 0, DescriptorSize);
        const QByteArray displayName = QByteArrayLiteral("Moonlight V+ Virtual Display");
        std::memcpy(descriptor.data() + 2,
                    displayName.constData(),
                    static_cast<size_t>(qMin(displayName.size(), 119)));
        writeUInt32(descriptor, 0x7a, 1);
        writeUInt32(descriptor, 0x7e, 4);
        float physicalWidth = 369.4545593261719f;
        float physicalHeight = 207.81817626953125f;
        quint32 physicalWidthBits = 0;
        quint32 physicalHeightBits = 0;
        std::memcpy(&physicalWidthBits, &physicalWidth, sizeof(physicalWidthBits));
        std::memcpy(&physicalHeightBits, &physicalHeight, sizeof(physicalHeightBits));
        writeUInt32(descriptor, 0x82, physicalWidthBits);
        writeUInt32(descriptor, 0x86, physicalHeightBits);
        writeUInt32(descriptor, 0x8a, 3840);
        writeUInt32(descriptor, 0x8e, 2160);
        writeUInt32(descriptor, 0x96, 7);
        writeUInt16(descriptor, 0x9a, ModeCount);

        for (int index = 0; index < ModeCount; ++index) {
            const double horizontalScale = static_cast<double>(size.width()) / 1920.0;
            const double verticalScale = static_cast<double>(size.height()) / 1080.0;
            const quint32 pointWidth = static_cast<quint32>(
                    qRound(modes[index][2] * horizontalScale));
            const quint32 pointHeight = static_cast<quint32>(
                    qRound(modes[index][3] * verticalScale));
            const int offset = DescriptorHeaderSize + ModeSize * index;
            writeUInt32(descriptor, offset, static_cast<quint32>(
                    qRound(pointWidth * backingScale)));
            writeUInt32(descriptor, offset + 4, static_cast<quint32>(
                    qRound(pointHeight * backingScale)));
            writeUInt32(descriptor, offset + 8, pointWidth);
            writeUInt32(descriptor, offset + 12, pointHeight);
            const double refreshRate = 60.0;
            quint64 refreshBits = 0;
            std::memcpy(&refreshBits, &refreshRate, sizeof(refreshBits));
            writeUInt64(descriptor, offset + 16, refreshBits);
            writeUInt32(descriptor, offset + 24, hdr ? 1 : 0);
        }
        descriptors.append(std::move(descriptor));
    }

    QByteArray message;
    message.append(char(0x1d));
    message.append(char(0));
    appendUInt16(message, static_cast<quint16>(
            8 + DescriptorSize * descriptors.size()));
    appendUInt16(message, 1);
    appendUInt16(message, static_cast<quint16>(descriptors.size()));
    appendUInt32(message, 0);
    for (const QByteArray& descriptor : std::as_const(descriptors)) {
        message.append(descriptor);
    }
    return message;
}

bool parseVersionBanner(const QByteArray& data)
{
    if (data.size() != 12 || !data.startsWith("RFB ") || !data.endsWith('\n')) {
        return false;
    }
    for (int i : {4, 5, 6, 8, 9, 10}) {
        if (data.at(i) < '0' || data.at(i) > '9') {
            return false;
        }
    }
    return data.at(7) == '.';
}

bool parsePublicKeyResponse(const QByteArray& packet,
                            QByteArray* subjectPublicKeyInfo,
                            QString* error)
{
    bool ok = false;
    const quint32 keyLength = readUInt32(packet, 2, &ok);
    if (!ok || keyLength == 0 || keyLength > static_cast<quint32>(packet.size() - 6)) {
        setError(error, QCoreApplication::translate(
                "AppleProtocol",
                "The Mac returned an invalid RSA public key."));
        return false;
    }
    if (subjectPublicKeyInfo != nullptr) {
        *subjectPublicKeyInfo = packet.mid(6, static_cast<int>(keyLength));
    }
    return true;
}

QByteArray credentialPlaintext(const QString& username,
                               const QString& password,
                               QString* error)
{
    const QByteArray usernameField = credentialField(username, error);
    if (usernameField.size() != 64) {
        return {};
    }
    const QByteArray passwordField = credentialField(password, error);
    if (passwordField.size() != 64) {
        return {};
    }
    return usernameField + passwordField;
}

QByteArray authenticationRequest(const QByteArray& encryptedCredentials,
                                 const QByteArray& encryptedMasterKey,
                                 QString* error)
{
    if (encryptedCredentials.size() != 128 || encryptedMasterKey.size() != 256) {
        setError(error, QCoreApplication::translate(
                "AppleProtocol",
                "Couldn’t prepare the encrypted Screen Sharing credentials."));
        return {};
    }
    QByteArray body = QByteArray::fromHex("0100525341310001");
    body.append(encryptedCredentials);
    body.append(QByteArray::fromHex("0001"));
    body.append(encryptedMasterKey);
    QByteArray framed;
    appendUInt32(framed, static_cast<quint32>(body.size()));
    framed.append(body);
    return framed;
}

quint16 readUInt16(const QByteArray& data, int offset, bool* ok)
{
    const bool valid = offset >= 0 && data.size() >= offset + 2;
    if (ok != nullptr) {
        *ok = valid;
    }
    if (!valid) {
        return 0;
    }
    return static_cast<quint16>(static_cast<unsigned char>(data.at(offset))) << 8 |
           static_cast<quint16>(static_cast<unsigned char>(data.at(offset + 1)));
}

quint32 readUInt32(const QByteArray& data, int offset, bool* ok)
{
    const bool valid = offset >= 0 && data.size() >= offset + 4;
    if (ok != nullptr) {
        *ok = valid;
    }
    if (!valid) {
        return 0;
    }
    return static_cast<quint32>(static_cast<unsigned char>(data.at(offset))) << 24 |
           static_cast<quint32>(static_cast<unsigned char>(data.at(offset + 1))) << 16 |
           static_cast<quint32>(static_cast<unsigned char>(data.at(offset + 2))) << 8 |
           static_cast<quint32>(static_cast<unsigned char>(data.at(offset + 3)));
}

qint32 readInt32(const QByteArray& data, int offset, bool* ok)
{
    return static_cast<qint32>(readUInt32(data, offset, ok));
}

void appendUInt16(QByteArray& data, quint16 value)
{
    data.append(static_cast<char>(value >> 8));
    data.append(static_cast<char>(value));
}

void appendUInt32(QByteArray& data, quint32 value)
{
    data.append(static_cast<char>(value >> 24));
    data.append(static_cast<char>(value >> 16));
    data.append(static_cast<char>(value >> 8));
    data.append(static_cast<char>(value));
}

} // namespace AppleWire

AppleEncryptedRecordLayer::AppleEncryptedRecordLayer(
        QByteArray key,
        QByteArray initializationVector)
    : m_Key(std::move(key)),
      m_EncryptionIv(initializationVector),
      m_DecryptionIv(std::move(initializationVector))
{
}

bool AppleEncryptedRecordLayer::isValid() const
{
    return m_Key.size() == AesBlockSize &&
           m_EncryptionIv.size() == AesBlockSize &&
           m_DecryptionIv.size() == AesBlockSize;
}

QByteArray AppleEncryptedRecordLayer::encrypt(const QByteArray& message, QString* error)
{
    if (!isValid() || message.size() > 65535) {
        setError(error, QCoreApplication::translate("AppleProtocol", "The encrypted message is too large."));
        return {};
    }
    const int paddingLength =
            (AesBlockSize - ((2 + message.size() + AuthenticationCodeLength) % AesBlockSize)) %
            AesBlockSize;
    QByteArray body;
    AppleWire::appendUInt16(body, static_cast<quint16>(message.size()));
    body.append(message);
    body.append(QByteArray(paddingLength, '\0'));
    QByteArray digestInput;
    AppleWire::appendUInt32(digestInput, m_SendCounter);
    digestInput.append(body);
    body.append(QCryptographicHash::hash(digestInput, QCryptographicHash::Sha1));

    QByteArray ciphertext = cryptCbc(body, m_Key, m_EncryptionIv, true, error);
    if (ciphertext.isEmpty() || ciphertext.size() > 65535) {
        return {};
    }
    m_EncryptionIv = ciphertext.right(AesBlockSize);
    ++m_SendCounter;
    QByteArray record;
    AppleWire::appendUInt16(record, static_cast<quint16>(ciphertext.size()));
    record.append(ciphertext);
    return record;
}

QByteArray AppleEncryptedRecordLayer::decrypt(const QByteArray& ciphertext, QString* error)
{
    if (!isValid() || ciphertext.size() <= AuthenticationCodeLength ||
            ciphertext.size() % AesBlockSize != 0) {
        setError(error, QCoreApplication::translate("AppleProtocol", "The Mac returned an invalid encrypted message."));
        return {};
    }
    const QByteArray nextIv = ciphertext.right(AesBlockSize);
    const QByteArray plaintext = cryptCbc(ciphertext, m_Key, m_DecryptionIv, false, error);
    if (plaintext.isEmpty()) {
        return {};
    }
    // Apple treats every complete ciphertext record as part of the CBC chain,
    // even when its authentication code is rejected. Advancing here lets the
    // next ordered record recover, matching the native viewer behavior.
    m_DecryptionIv = nextIv;
    const QByteArray body = plaintext.left(plaintext.size() - AuthenticationCodeLength);
    const QByteArray receivedDigest = plaintext.right(AuthenticationCodeLength);
    const quint32 startCounter = m_ReceiveCounter++;
    const quint32 lower = startCounter == 0 ? 0 : startCounter - 1;
    const quint32 upper = startCounter > std::numeric_limits<quint32>::max() - 5
            ? std::numeric_limits<quint32>::max()
            : startCounter + 5;
    for (quint32 candidate = lower;; ++candidate) {
        QByteArray digestInput;
        AppleWire::appendUInt32(digestInput, candidate);
        digestInput.append(body);
        if (constantTimeEqual(QCryptographicHash::hash(digestInput, QCryptographicHash::Sha1),
                              receivedDigest)) {
            bool ok = false;
            const quint16 messageLength = AppleWire::readUInt16(body, 0, &ok);
            if (!ok || messageLength > body.size() - 2) {
                setError(error, QCoreApplication::translate("AppleProtocol", "The Mac returned an invalid encrypted message body."));
                return {};
            }
            m_ReceiveCounter = candidate + 1;
            return body.mid(2, messageLength);
        }
        if (candidate == upper) {
            break;
        }
    }
    setError(error, QCoreApplication::translate("AppleProtocol", "The encrypted Screen Sharing message failed authentication."));
    return {};
}

QByteArray AppleEncryptedRecordLayer::encryptInput(
        const QByteArray& header,
        const QByteArray& plaintextBlock,
        QString* error) const
{
    if (!isValid() || header.size() != 2 || plaintextBlock.size() != AesBlockSize) {
        setError(error, QCoreApplication::translate(
                "AppleProtocol", "The remote input event is invalid."));
        return {};
    }
    const QByteArray encrypted = cryptEcb(plaintextBlock, m_Key, true, error);
    return encrypted.size() == AesBlockSize ? header + encrypted : QByteArray();
}
