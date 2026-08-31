#include "appleauthenticator.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QScopeGuard>
#include <QTcpSocket>
#include <QThread>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace {

constexpr int IoTimeoutMs = 8000;
constexpr int MaximumPublicKeyPacketLength = 64 * 1024;
constexpr int MaximumServerNameLength = 1024 * 1024;

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
}

bool cancelled(std::atomic_bool* value)
{
    return value != nullptr && value->load();
}

QByteArray encryptAesEcb(const QByteArray& plaintext,
                         const QByteArray& key,
                         QString* error)
{
    if (plaintext.size() % 16 != 0 || key.size() != 16) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "Invalid AES authentication input."));
        return {};
    }
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "Couldn’t create the authentication cipher."));
        return {};
    }
    const auto freeContext = qScopeGuard([context]() { EVP_CIPHER_CTX_free(context); });
    if (EVP_EncryptInit_ex(context, EVP_aes_128_ecb(), nullptr,
                           reinterpret_cast<const unsigned char*>(key.constData()), nullptr) != 1 ||
            EVP_CIPHER_CTX_set_padding(context, 0) != 1) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "Couldn’t initialize credential encryption."));
        return {};
    }
    QByteArray output(plaintext.size() + 16, Qt::Uninitialized);
    int produced = 0;
    int finalProduced = 0;
    if (EVP_EncryptUpdate(context,
                          reinterpret_cast<unsigned char*>(output.data()), &produced,
                          reinterpret_cast<const unsigned char*>(plaintext.constData()),
                          plaintext.size()) != 1 ||
            EVP_EncryptFinal_ex(context,
                                reinterpret_cast<unsigned char*>(output.data()) + produced,
                                &finalProduced) != 1) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "Credential encryption failed."));
        return {};
    }
    output.resize(produced + finalProduced);
    return output;
}

QByteArray encryptRsaPkcs1(const QByteArray& plaintext,
                           const QByteArray& subjectPublicKeyInfo,
                           QString* error)
{
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(
            subjectPublicKeyInfo.constData());
    EVP_PKEY* key = d2i_PUBKEY(nullptr, &cursor, subjectPublicKeyInfo.size());
    if (key == nullptr) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "The Mac returned an unsupported RSA public key."));
        return {};
    }
    const auto freeKey = qScopeGuard([key]() { EVP_PKEY_free(key); });
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new(key, nullptr);
    if (context == nullptr) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "Couldn’t create RSA encryption state."));
        return {};
    }
    const auto freeContext = qScopeGuard([context]() { EVP_PKEY_CTX_free(context); });
    if (EVP_PKEY_encrypt_init(context) <= 0 ||
            EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_PADDING) <= 0) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "The Mac’s RSA key cannot encrypt credentials."));
        return {};
    }
    size_t outputLength = 0;
    if (EVP_PKEY_encrypt(context, nullptr, &outputLength,
                         reinterpret_cast<const unsigned char*>(plaintext.constData()),
                         static_cast<size_t>(plaintext.size())) <= 0 ||
            outputLength != 256) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "The Mac did not provide the required 2048-bit RSA key."));
        return {};
    }
    QByteArray output(static_cast<int>(outputLength), Qt::Uninitialized);
    if (EVP_PKEY_encrypt(context,
                         reinterpret_cast<unsigned char*>(output.data()), &outputLength,
                         reinterpret_cast<const unsigned char*>(plaintext.constData()),
                         static_cast<size_t>(plaintext.size())) <= 0) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "RSA credential encryption failed."));
        return {};
    }
    output.resize(static_cast<int>(outputLength));
    return output;
}

QByteArray decryptAesEcb(const QByteArray& ciphertext,
                         const QByteArray& key,
                         QString* error)
{
    if (ciphertext.size() != 16 || key.size() != 16) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "The Mac returned invalid record key material."));
        return {};
    }
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        return {};
    }
    const auto freeContext = qScopeGuard([context]() { EVP_CIPHER_CTX_free(context); });
    if (EVP_DecryptInit_ex(context, EVP_aes_128_ecb(), nullptr,
                           reinterpret_cast<const unsigned char*>(key.constData()), nullptr) != 1 ||
            EVP_CIPHER_CTX_set_padding(context, 0) != 1) {
        return {};
    }
    QByteArray output(32, Qt::Uninitialized);
    int produced = 0;
    int finalProduced = 0;
    if (EVP_DecryptUpdate(context,
                          reinterpret_cast<unsigned char*>(output.data()), &produced,
                          reinterpret_cast<const unsigned char*>(ciphertext.constData()),
                          ciphertext.size()) != 1 ||
            EVP_DecryptFinal_ex(context,
                                reinterpret_cast<unsigned char*>(output.data()) + produced,
                                &finalProduced) != 1) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "The Mac’s record keys could not be decrypted."));
        return {};
    }
    output.resize(produced + finalProduced);
    return output;
}

} // namespace

AppleTcpTransport::AppleTcpTransport() = default;
AppleTcpTransport::~AppleTcpTransport() = default;

bool AppleTcpTransport::connectTo(const AppleConnectionEndpoint& endpoint,
                                  std::atomic_bool* cancelledFlag,
                                  QString* error)
{
    close();
    m_Socket = std::make_unique<QTcpSocket>();
    m_Socket->connectToHost(endpoint.host, endpoint.port);
    QElapsedTimer timer;
    timer.start();
    while (!cancelled(cancelledFlag) && timer.elapsed() < IoTimeoutMs) {
        if (m_Socket->waitForConnected(200)) {
            return true;
        }
        if (m_Socket->state() == QAbstractSocket::UnconnectedState &&
                m_Socket->error() != QAbstractSocket::SocketTimeoutError) {
            break;
        }
    }
    setError(error, cancelled(cancelledFlag)
            ? QCoreApplication::translate("AppleAuthenticator", "The Screen Sharing connection was cancelled.")
            : QCoreApplication::translate("AppleAuthenticator", "Couldn’t connect to the Mac: %1").arg(m_Socket->errorString()));
    close();
    return false;
}

bool AppleTcpTransport::writeAll(const QByteArray& data,
                                 std::atomic_bool* cancelledFlag,
                                 QString* error)
{
    if (!m_Socket || m_Socket->state() != QAbstractSocket::ConnectedState) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "The Screen Sharing socket is not connected."));
        return false;
    }
    qint64 offset = 0;
    QElapsedTimer timer;
    timer.start();
    while (offset < data.size() && !cancelled(cancelledFlag) && timer.elapsed() < IoTimeoutMs) {
        const qint64 written = m_Socket->write(data.constData() + offset, data.size() - offset);
        if (written < 0) {
            break;
        }
        offset += written;
        if (offset < data.size() && !m_Socket->waitForBytesWritten(200) &&
                m_Socket->state() != QAbstractSocket::ConnectedState) {
            break;
        }
    }
    if (offset == data.size()) {
        return true;
    }
    setError(error, cancelled(cancelledFlag)
            ? QCoreApplication::translate("AppleAuthenticator", "The Screen Sharing connection was cancelled.")
            : QCoreApplication::translate("AppleAuthenticator", "Couldn’t send Screen Sharing data: %1").arg(m_Socket->errorString()));
    return false;
}

bool AppleTcpTransport::readExactly(int length,
                                    QByteArray* data,
                                    std::atomic_bool* cancelledFlag,
                                    QString* error)
{
    if (!m_Socket || data == nullptr || length < 0) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "Invalid Screen Sharing read."));
        return false;
    }
    QByteArray result;
    result.reserve(length);
    QElapsedTimer timer;
    timer.start();
    while (result.size() < length && !cancelled(cancelledFlag) && timer.elapsed() < IoTimeoutMs) {
        if (m_Socket->bytesAvailable() == 0 && !m_Socket->waitForReadyRead(200)) {
            if (m_Socket->state() != QAbstractSocket::ConnectedState) {
                break;
            }
            continue;
        }
        const QByteArray chunk = m_Socket->read(length - result.size());
        if (chunk.isEmpty() && m_Socket->state() != QAbstractSocket::ConnectedState) {
            break;
        }
        result.append(chunk);
    }
    if (result.size() == length) {
        *data = result;
        return true;
    }
    setError(error, cancelled(cancelledFlag)
            ? QCoreApplication::translate("AppleAuthenticator", "The Screen Sharing connection was cancelled.")
            : QCoreApplication::translate("AppleAuthenticator", "The Mac closed or stalled the Screen Sharing connection."));
    return false;
}

void AppleTcpTransport::protocolDelay(int milliseconds, std::atomic_bool* cancelledFlag)
{
    int remaining = milliseconds;
    while (remaining > 0 && !cancelled(cancelledFlag)) {
        const int interval = qMin(remaining, 25);
        QThread::msleep(static_cast<unsigned long>(interval));
        remaining -= interval;
    }
}

void AppleTcpTransport::close()
{
    if (m_Socket) {
        m_Socket->abort();
        m_Socket.reset();
    }
}

bool AppleAuthenticator::probe(AppleByteTransport& transport,
                               const AppleConnectionEndpoint& endpoint,
                               AppleHostIdentity* identity,
                               std::atomic_bool* cancelledFlag,
                               QString* error) const
{
    if (!transport.connectTo(endpoint, cancelledFlag, error)) {
        return false;
    }
    return prepareHostIdentity(transport, identity, cancelledFlag, error);
}

bool AppleAuthenticator::authenticate(
        AppleByteTransport& transport,
        const AppleConnectionEndpoint& endpoint,
        const QString& expectedFingerprint,
        const CredentialLoader& loadCredentials,
        AppleAuthenticatedControl* result,
        std::atomic_bool* cancelledFlag,
        QString* error) const
{
    if (!transport.connectTo(endpoint, cancelledFlag, error)) {
        return false;
    }
    AppleHostIdentity identity;
    if (!prepareHostIdentity(transport, &identity, cancelledFlag, error)) {
        return false;
    }
    if (expectedFingerprint.isEmpty() ||
            identity.fingerprint.compare(expectedFingerprint, Qt::CaseInsensitive) != 0) {
        setError(error, QCoreApplication::translate(
                "AppleAuthenticator",
                "The Mac’s host identity does not match the trusted fingerprint. Credentials were not read or sent."));
        return false;
    }

    // This callback is intentionally invoked only after the live connection's host
    // identity matches the saved trust record. It is the sole secret-read seam.
    AppleCredentials credentials;
    if (!loadCredentials || !loadCredentials(&credentials, error) || !credentials.validate(error)) {
        return false;
    }

    QByteArray masterKey(16, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(masterKey.data()), masterKey.size()) != 1) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "Couldn’t generate the session master key."));
        return false;
    }
    const QByteArray plaintext = AppleWire::credentialPlaintext(
            credentials.username.trimmed(), credentials.password, error);
    if (plaintext.size() != 128) {
        return false;
    }
    const QByteArray encryptedCredentials = encryptAesEcb(plaintext, masterKey, error);
    const QByteArray encryptedMasterKey = encryptRsaPkcs1(
            masterKey, identity.subjectPublicKeyInfo, error);
    const QByteArray request = AppleWire::authenticationRequest(
            encryptedCredentials, encryptedMasterKey, error);
    if (request.isEmpty() || !transport.writeAll(request, cancelledFlag, error)) {
        return false;
    }

    QByteArray ignored;
    QByteArray securityResult;
    if (!transport.readExactly(4, &ignored, cancelledFlag, error) ||
            !transport.readExactly(4, &securityResult, cancelledFlag, error)) {
        return false;
    }
    bool resultOk = false;
    const quint32 securityCode = AppleWire::readUInt32(securityResult, 0, &resultOk);
    if (!resultOk || securityCode != 0) {
        setError(error, resultOk
                ? QCoreApplication::translate("AppleAuthenticator", "The account name or password was not accepted.")
                : QCoreApplication::translate("AppleAuthenticator", "The Mac returned an invalid authentication result."));
        return false;
    }

    if (!transport.writeAll(QByteArray(1, static_cast<char>(0xc1)), cancelledFlag, error)) {
        return false;
    }
    QByteArray serverHeader;
    if (!transport.readExactly(24, &serverHeader, cancelledFlag, error)) {
        return false;
    }
    bool widthOk = false;
    bool heightOk = false;
    bool nameLengthOk = false;
    const quint16 width = AppleWire::readUInt16(serverHeader, 0, &widthOk);
    const quint16 height = AppleWire::readUInt16(serverHeader, 2, &heightOk);
    const quint32 nameLength = AppleWire::readUInt32(serverHeader, 20, &nameLengthOk);
    if (!widthOk || !heightOk || !nameLengthOk || width == 0 || height == 0 ||
            nameLength > MaximumServerNameLength) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "The Mac returned an invalid display description."));
        return false;
    }
    QByteArray nameData;
    if (!transport.readExactly(static_cast<int>(nameLength), &nameData, cancelledFlag, error)) {
        return false;
    }
    QByteArray plainName = nameData;
    if (nameData.size() >= 2 && nameData.at(0) == 0 && nameData.at(1) == 0) {
        if (nameData.size() < 22) {
            setError(error, QCoreApplication::translate(
                    "AppleAuthenticator",
                    "The Mac returned an invalid display description."));
            return false;
        }
        plainName = nameData.mid(22);
    }
    const QString serverName = QString::fromUtf8(plainName);
    if (serverName.toUtf8() != plainName) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "The Mac returned an invalid display name."));
        return false;
    }
    if (result != nullptr) {
        result->masterKey = masterKey;
        result->serverName = serverName;
        result->width = width;
        result->height = height;
    }
    return true;
}

bool AppleAuthenticator::prepareHostIdentity(
        AppleByteTransport& transport,
        AppleHostIdentity* identity,
        std::atomic_bool* cancelledFlag,
        QString* error) const
{
    QByteArray banner;
    if (!transport.readExactly(12, &banner, cancelledFlag, error) ||
            !AppleWire::parseVersionBanner(banner)) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "The host did not return a valid Screen Sharing protocol banner."));
        return false;
    }
    if (!transport.writeAll(AppleWire::versionBanner(), cancelledFlag, error)) {
        return false;
    }
    QByteArray countData;
    if (!transport.readExactly(1, &countData, cancelledFlag, error)) {
        return false;
    }
    const int count = static_cast<unsigned char>(countData.at(0));
    if (count <= 0) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "The Mac rejected the connection before authentication."));
        return false;
    }
    QByteArray securityTypes;
    if (!transport.readExactly(count, &securityTypes, cancelledFlag, error) ||
            !securityTypes.contains(static_cast<char>(33))) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "This Mac does not offer Apple RSA authentication."));
        return false;
    }
    if (!transport.writeAll(AppleWire::publicKeyRequest(), cancelledFlag, error)) {
        return false;
    }
    QByteArray packetLengthData;
    if (!transport.readExactly(4, &packetLengthData, cancelledFlag, error)) {
        return false;
    }
    bool lengthOk = false;
    const quint32 packetLength = AppleWire::readUInt32(packetLengthData, 0, &lengthOk);
    if (!lengthOk || packetLength == 0 || packetLength > MaximumPublicKeyPacketLength) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "The Mac returned an invalid RSA key packet."));
        return false;
    }
    QByteArray packet;
    QByteArray spki;
    if (!transport.readExactly(static_cast<int>(packetLength), &packet, cancelledFlag, error) ||
            !AppleWire::parsePublicKeyResponse(packet, &spki, error)) {
        return false;
    }
    if (identity != nullptr) {
        identity->subjectPublicKeyInfo = spki;
        identity->fingerprint = QString::fromLatin1(
                QCryptographicHash::hash(spki, QCryptographicHash::Sha256).toHex());
    }
    return true;
}

bool AppleControlChannel::negotiate(AppleByteTransport& transport,
                                    const QByteArray& masterKey,
                                    std::atomic_bool* cancelledFlag,
                                    QString* error)
{
    if (masterKey.size() != 16 ||
            !transport.writeAll(AppleWire::viewerInfo() + AppleWire::setEncryption(),
                                cancelledFlag, error)) {
        return false;
    }
    // The native host changes control framing asynchronously after SetEncryption.
    // Keep the captured pacing, but delegate it to the transport so tests use a
    // deterministic no-wait implementation.
    transport.protocolDelay(100, cancelledFlag);
    if (cancelled(cancelledFlag) ||
            !transport.writeAll(AppleWire::setEncodings(), cancelledFlag, error)) {
        return false;
    }
    QByteArray encryptedKey;
    QByteArray encryptedIv;
    if (!readRekey(transport, &encryptedKey, &encryptedIv, cancelledFlag, error)) {
        return false;
    }
    const QByteArray key = decryptAesEcb(encryptedKey, masterKey, error);
    const QByteArray iv = decryptAesEcb(encryptedIv, masterKey, error);
    m_Records = AppleEncryptedRecordLayer(key, iv);
    if (!m_Records.isValid() ||
            !transport.writeAll(AppleWire::postEncryptionToggle(), cancelledFlag, error)) {
        return false;
    }
    transport.protocolDelay(200, cancelledFlag);
    return !cancelled(cancelledFlag) && sendEncrypted(
            transport,
            AppleWire::displayConfiguration(),
            cancelledFlag,
            error);
}

bool AppleControlChannel::sendEncrypted(AppleByteTransport& transport,
                                        const QByteArray& message,
                                        std::atomic_bool* cancelledFlag,
                                        QString* error)
{
    const QByteArray record = m_Records.encrypt(message, error);
    return !record.isEmpty() && transport.writeAll(record, cancelledFlag, error);
}

bool AppleControlChannel::receiveEncrypted(AppleByteTransport& transport,
                                           QByteArray* message,
                                           std::atomic_bool* cancelledFlag,
                                           QString* error)
{
    QByteArray header;
    if (!transport.readExactly(2, &header, cancelledFlag, error)) {
        return false;
    }
    bool ok = false;
    const quint16 length = AppleWire::readUInt16(header, 0, &ok);
    if (!ok || length == 0 || length % 16 != 0) {
        setError(error, QCoreApplication::translate("AppleAuthenticator", "The Mac returned an invalid encrypted record length."));
        return false;
    }
    QByteArray ciphertext;
    if (!transport.readExactly(length, &ciphertext, cancelledFlag, error)) {
        return false;
    }
    const QByteArray plaintext = m_Records.decrypt(ciphertext, error);
    if (plaintext.isEmpty() && !ciphertext.isEmpty()) {
        return false;
    }
    if (message != nullptr) {
        *message = plaintext;
    }
    return true;
}

bool AppleControlChannel::readRekey(AppleByteTransport& transport,
                                    QByteArray* encryptedKey,
                                    QByteArray* encryptedIv,
                                    std::atomic_bool* cancelledFlag,
                                    QString* error)
{
    while (!cancelled(cancelledFlag)) {
        QByteArray typeData;
        if (!transport.readExactly(1, &typeData, cancelledFlag, error)) {
            return false;
        }
        const quint8 type = static_cast<quint8>(typeData.at(0));
        if (type == 0x14) {
            QByteArray ignored;
            if (!transport.readExactly(7, &ignored, cancelledFlag, error)) {
                return false;
            }
            continue;
        }
        if (type != 0x00) {
            setError(error, QCoreApplication::translate("AppleAuthenticator", "The Mac returned an unexpected setup message."));
            return false;
        }
        QByteArray updateHeader;
        if (!transport.readExactly(3, &updateHeader, cancelledFlag, error)) {
            return false;
        }
        bool countOk = false;
        const quint16 rectangleCount = AppleWire::readUInt16(updateHeader, 1, &countOk);
        if (!countOk) {
            return false;
        }
        for (quint16 i = 0; i < rectangleCount; ++i) {
            QByteArray rectangle;
            if (!transport.readExactly(12, &rectangle, cancelledFlag, error)) {
                return false;
            }
            bool encodingOk = false;
            const qint32 encoding = AppleWire::readInt32(rectangle, 8, &encodingOk);
            if (!encodingOk) {
                return false;
            }
            if (encoding == 1103) {
                QByteArray payload;
                if (!transport.readExactly(36, &payload, cancelledFlag, error)) {
                    return false;
                }
                *encryptedKey = payload.mid(4, 16);
                *encryptedIv = payload.mid(20, 16);
                return true;
            }
            if (encoding == 1010 || encoding == 1011) {
                QByteArray lengthData;
                if (!transport.readExactly(2, &lengthData, cancelledFlag, error)) {
                    return false;
                }
                bool lengthOk = false;
                const quint16 length = AppleWire::readUInt16(lengthData, 0, &lengthOk);
                QByteArray ignored;
                if (!lengthOk || !transport.readExactly(length, &ignored, cancelledFlag, error)) {
                    return false;
                }
                continue;
            }
            setError(error, QCoreApplication::translate("AppleAuthenticator", "The Mac returned an unsupported setup encoding."));
            return false;
        }
    }
    setError(error, QCoreApplication::translate("AppleAuthenticator", "The Screen Sharing connection was cancelled."));
    return false;
}
