#include "backend/apple/appleauthenticator.h"
#include "backend/apple/appleconnectionstore.h"
#include "backend/apple/applefeaturegate.h"
#include "backend/apple/appleprotocol.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        qCritical("%s", message);
        std::exit(1);
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

class TranscriptTransport final : public AppleByteTransport
{
public:
    QByteArray incoming;
    QList<QByteArray> writes;
    int offset = 0;
    bool connected = false;

    bool connectTo(const AppleConnectionEndpoint& endpoint,
                   std::atomic_bool*, QString* error) override
    {
        connected = endpoint.isValid();
        if (!connected && error != nullptr) {
            *error = QStringLiteral("invalid endpoint");
        }
        return connected;
    }

    bool writeAll(const QByteArray& data, std::atomic_bool*, QString*) override
    {
        writes.append(data);
        return connected;
    }

    bool readExactly(int length,
                     QByteArray* data,
                     std::atomic_bool*,
                     QString* error) override
    {
        if (!connected || data == nullptr || length < 0 || offset + length > incoming.size()) {
            if (error != nullptr) {
                *error = QStringLiteral("transcript exhausted");
            }
            return false;
        }
        *data = incoming.mid(offset, length);
        offset += length;
        return true;
    }

    void protocolDelay(int, std::atomic_bool*) override {}
    void close() override { connected = false; }
};

QByteArray publicKeyPacket(const QByteArray& spki)
{
    QByteArray packet(2, '\0');
    AppleWire::appendUInt32(packet, static_cast<quint32>(spki.size()));
    packet.append(spki);
    return packet;
}

void appendHostIdentityTranscript(QByteArray& transcript, const QByteArray& spki)
{
    transcript.append(QByteArrayLiteral("RFB 003.889\n"));
    transcript.append(char(1));
    transcript.append(char(33));
    const QByteArray packet = publicKeyPacket(spki);
    AppleWire::appendUInt32(transcript, static_cast<quint32>(packet.size()));
    transcript.append(packet);
}

QByteArray generateRsaSpki()
{
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    require(context != nullptr, "RSA context must be created");
    const auto freeContext = qScopeGuard([context]() { EVP_PKEY_CTX_free(context); });
    require(EVP_PKEY_keygen_init(context) > 0,
            "RSA generation must initialize");
    require(EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048) > 0,
            "RSA key size must be 2048 bits");
    EVP_PKEY* key = nullptr;
    require(EVP_PKEY_keygen(context, &key) > 0 && key != nullptr,
            "RSA key generation must succeed");
    const auto freeKey = qScopeGuard([key]() { EVP_PKEY_free(key); });
    const int length = i2d_PUBKEY(key, nullptr);
    require(length > 0, "RSA public key must serialize");
    QByteArray spki(length, Qt::Uninitialized);
    unsigned char* cursor = reinterpret_cast<unsigned char*>(spki.data());
    require(i2d_PUBKEY(key, &cursor) == length,
            "RSA public key serialization must be complete");
    return spki;
}

QByteArray encryptEcb(const QByteArray& plaintext, const QByteArray& key)
{
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    require(context != nullptr, "AES context must be created");
    const auto freeContext = qScopeGuard([context]() { EVP_CIPHER_CTX_free(context); });
    require(EVP_EncryptInit_ex(context, EVP_aes_128_ecb(), nullptr,
                               reinterpret_cast<const unsigned char*>(key.constData()),
                               nullptr) == 1,
            "AES ECB must initialize");
    require(EVP_CIPHER_CTX_set_padding(context, 0) == 1,
            "AES ECB padding must be disabled");
    QByteArray output(plaintext.size() + 16, Qt::Uninitialized);
    int produced = 0;
    int finalProduced = 0;
    require(EVP_EncryptUpdate(context,
                              reinterpret_cast<unsigned char*>(output.data()), &produced,
                              reinterpret_cast<const unsigned char*>(plaintext.constData()),
                              plaintext.size()) == 1 &&
            EVP_EncryptFinal_ex(context,
                                reinterpret_cast<unsigned char*>(output.data()) + produced,
                                &finalProduced) == 1,
            "AES ECB encryption must succeed");
    output.resize(produced + finalProduced);
    return output;
}

void testFeatureEnabledBuildDiscoversByDefault()
{
    QTemporaryDir directory;
    require(directory.isValid(), "temporary feature-gate settings directory must exist");

    const bool hadRuntimeOverride = qEnvironmentVariableIsSet(
            "MOONLIGHT_APPLE_SCREEN_SHARING_RUNTIME");
    const QByteArray runtimeOverride = qgetenv("MOONLIGHT_APPLE_SCREEN_SHARING_RUNTIME");
    const auto restoreRuntimeOverride = qScopeGuard([hadRuntimeOverride, runtimeOverride]() {
        if (hadRuntimeOverride) {
            qputenv("MOONLIGHT_APPLE_SCREEN_SHARING_RUNTIME", runtimeOverride);
        }
        else {
            qunsetenv("MOONLIGHT_APPLE_SCREEN_SHARING_RUNTIME");
        }
    });

    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setOrganizationName(QStringLiteral("MoonlightFeatureGateTest"));
    QCoreApplication::setApplicationName(QStringLiteral("MoonlightFeatureGateTest"));
    qunsetenv("MOONLIGHT_APPLE_SCREEN_SHARING_RUNTIME");

    QSettings settings;
    settings.clear();
    settings.sync();

    qputenv("MOONLIGHT_APPLE_SCREEN_SHARING_RUNTIME", QByteArrayLiteral("1"));
    require(AppleFeatureGate::isRuntimeEnabled(),
            "the runtime-on environment override must take precedence");
    qputenv("MOONLIGHT_APPLE_SCREEN_SHARING_RUNTIME", QByteArrayLiteral("0"));
    require(!AppleFeatureGate::isRuntimeEnabled(),
            "the runtime-off environment override must take precedence");
    qunsetenv("MOONLIGHT_APPLE_SCREEN_SHARING_RUNTIME");

    require(AppleFeatureGate::isRuntimeEnabled(),
            "a feature-enabled build must register discovery by default");

    settings.setValue(QStringLiteral("appleScreenSharing/runtimeEnabled"), false);
    settings.sync();
    require(!AppleFeatureGate::isRuntimeEnabled(),
            "an explicit persisted runtime-off setting must disable discovery");
}

void testSavedConnectionIdentityAndSecretBoundary()
{
    QTemporaryDir directory;
    require(directory.isValid(), "temporary settings directory must exist");
    const QString path = directory.filePath(QStringLiteral("connections.ini"));

    QString firstId;
    AppleConnectionEndpoint updated;
    {
        AppleConnectionStore store(path);
        AppleConnectionEndpoint endpoint;
        endpoint.host = QStringLiteral("mac-one.local");
        endpoint.port = 5900;
        endpoint.serviceName = QStringLiteral("Office Mac");
        endpoint.serviceType = QStringLiteral("_rfb._tcp");
        endpoint.serviceDomain = QStringLiteral("local");
        const QString serviceKey = endpoint.serviceKey();
        const AppleSavedConnection saved = store.saveDiscovered(endpoint, QStringLiteral("Office Mac"));
        firstId = saved.id;
        require(!firstId.isEmpty(), "saving discovery must assign a UUID identity");
        require(store.rename(firstId, QStringLiteral("Studio Mac")),
                "display name must be mutable without replacing identity");
        require(store.setTrust(firstId, QStringLiteral("fingerprint-one")),
                "host trust must be persisted");
        require(store.setCredentialBinding(firstId,
                                           QStringLiteral("opaque-credential-reference"),
                                           QStringLiteral("alice")),
                "only an opaque credential binding may be persisted");

        updated = endpoint;
        updated.host = QStringLiteral("192.0.2.44");
        updated.port = 5901;
        require(store.updateDiscoveredEndpoint(serviceKey, updated),
                "discovered endpoint changes must update the saved object");
        const AppleSavedConnection afterEndpoint = store.connection(firstId);
        require(afterEndpoint.id == firstId &&
                afterEndpoint.displayName == QStringLiteral("Studio Mac") &&
                afterEndpoint.endpoint.host == updated.host &&
                afterEndpoint.endpoint.port == updated.port &&
                afterEndpoint.hasCredentialBinding(),
                "address, port, and display-name changes must preserve saved identity and binding");

        require(store.setTrust(firstId, QStringLiteral("fingerprint-two")),
                "explicit replacement trust must be accepted");
        const AppleSavedConnection afterTrustChange = store.connection(firstId);
        require(!afterTrustChange.hasCredentialBinding() &&
                afterTrustChange.preferredUsername.isEmpty(),
                "changed host identity must invalidate the previous credential binding");

        require(store.remove(firstId), "saved connection must be removable");
        const AppleSavedConnection readded = store.saveDiscovered(updated, QStringLiteral("Studio Mac"));
        require(readded.id != firstId,
                "delete and re-add must create a new saved identity");
    }

    QFile settingsFile(path);
    require(settingsFile.open(QIODevice::ReadOnly), "settings file must be readable");
    const QByteArray settingsData = settingsFile.readAll().toLower();
    require(!settingsData.contains("password") &&
            !settingsData.contains("credentialblob"),
            "connection settings must not contain password material");

    AppleConnectionStore reloaded(path);
    require(reloaded.connections().size() == 1 &&
            reloaded.connections().first().id != firstId,
            "saved identity must survive process restart and removed identity must stay absent");
}

void testMalformedWireInputs()
{
    require(AppleWire::parseVersionBanner(QByteArrayLiteral("RFB 003.889\n")),
            "Apple RFB banner must be accepted");
    require(!AppleWire::parseVersionBanner(QByteArrayLiteral("RFB 003.88x\n")),
            "non-numeric RFB banner must be rejected");

    QByteArray spki;
    QString error;
    require(!AppleWire::parsePublicKeyResponse(QByteArray::fromHex("000000000100"),
                                               &spki,
                                               &error),
            "truncated public-key packet must be rejected");
    require(!AppleWire::credentialPlaintext(QString(64, QLatin1Char('a')),
                                            QStringLiteral("password"),
                                            &error).size(),
            "overlong UTF-8 credential fields must be rejected");
    require(AppleWire::authenticationRequest(QByteArray(127, '\0'),
                                             QByteArray(256, '\0'),
                                             &error).isEmpty(),
            "authentication frame must reject invalid encrypted field sizes");
    require(AppleWire::displayConfiguration(0, 900).isEmpty(),
            "invalid display dimensions must not produce a wire message");

    AppleCredentials embeddedNull{
        QStringLiteral("alice"),
        QStringLiteral("secret") + QChar('\0') + QStringLiteral("suffix"),
    };
    require(!embeddedNull.validate(&error),
            "credential fields must reject embedded wire terminators");
    const QString expectedReference = AppleCredentialStore::referenceForConnection(
            QStringLiteral("saved-id"));
    require(AppleCredentialStore::isReferenceForConnection(
                expectedReference, QStringLiteral("saved-id")) &&
            !AppleCredentialStore::isReferenceForConnection(
                QStringLiteral("arbitrary-target"), QStringLiteral("saved-id")),
            "opaque credential bindings must be scoped to the saved connection UUID");
}

void testTrustPrecedesCredentialRead()
{
    const QByteArray fakeSpki = QByteArray::fromHex("3003010100");
    TranscriptTransport transport;
    appendHostIdentityTranscript(transport.incoming, fakeSpki);
    AppleConnectionEndpoint endpoint;
    endpoint.host = QStringLiteral("mac.example");
    bool credentialLoaderCalled = false;
    std::atomic_bool cancelled{false};
    QString error;
    AppleAuthenticatedControl result;
    const bool authenticated = AppleAuthenticator().authenticate(
            transport,
            endpoint,
            QStringLiteral("not-the-live-fingerprint"),
            [&](AppleCredentials*, QString*) {
                credentialLoaderCalled = true;
                return true;
            },
            &result,
            &cancelled,
            &error);
    require(!authenticated && !credentialLoaderCalled,
            "fingerprint mismatch must fail before the credential loader is invoked");
    require(transport.writes.size() == 2 &&
            transport.writes.at(0) == AppleWire::versionBanner() &&
            transport.writes.at(1) == AppleWire::publicKeyRequest(),
            "fingerprint mismatch must not send an authentication request");
    require(error.contains(QStringLiteral("not read or sent")),
            "trust failure must explicitly report the secret-safety boundary");
}

void testFormalAuthenticationTranscript()
{
    const QByteArray spki = generateRsaSpki();
    TranscriptTransport transport;
    appendHostIdentityTranscript(transport.incoming, spki);
    transport.incoming.append(QByteArray(4, '\0'));
    transport.incoming.append(QByteArray(4, '\0'));
    const QByteArray serverName = QByteArrayLiteral("Test Mac");
    QByteArray serverHeader(24, '\0');
    writeUInt16(serverHeader, 0, 1440);
    writeUInt16(serverHeader, 2, 900);
    writeUInt32(serverHeader, 20, static_cast<quint32>(serverName.size()));
    transport.incoming.append(serverHeader);
    transport.incoming.append(serverName);

    const QString fingerprint = QString::fromLatin1(
            QCryptographicHash::hash(spki, QCryptographicHash::Sha256).toHex());
    AppleConnectionEndpoint endpoint;
    endpoint.host = QStringLiteral("mac.example");
    int credentialLoads = 0;
    std::atomic_bool cancelled{false};
    QString error;
    AppleAuthenticatedControl result;
    const bool authenticated = AppleAuthenticator().authenticate(
            transport,
            endpoint,
            fingerprint,
            [&](AppleCredentials* credentials, QString*) {
                ++credentialLoads;
                credentials->username = QStringLiteral("alice");
                credentials->password = QStringLiteral("correct horse");
                return true;
            },
            &result,
            &cancelled,
            &error);
    require(authenticated && error.isEmpty(),
            "valid transcript must complete formal Apple authentication");
    require(credentialLoads == 1,
            "formal authentication must read credentials exactly once after trust");
    require(result.masterKey.size() == 16 && result.width == 1440 &&
            result.height == 900 && result.serverName == QStringLiteral("Test Mac"),
            "authenticated control state must retain negotiated server data");
    require(transport.writes.size() == 4 &&
            transport.writes.at(2).size() == 398 &&
            AppleWire::readUInt32(transport.writes.at(2), 0) == 394 &&
            transport.writes.at(3) == QByteArray(1, static_cast<char>(0xc1)),
            "formal authentication must emit the framed RSA request and client init in order");
}

void testAuthenticatedRecordRecoveryAndOrdering()
{
    const QByteArray key = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    const QByteArray iv = QByteArray::fromHex("ffeeddccbbaa99887766554433221100");
    AppleEncryptedRecordLayer sender(key, iv);
    AppleEncryptedRecordLayer receiver(key, iv);

    AppleEncryptedRecordLayer vectorSender(
            QByteArray::fromHex("000102030405060708090a0b0c0d0e0f"),
            QByteArray::fromHex("101112131415161718191a1b1c1d1e1f"));
    require(vectorSender.encrypt(QByteArrayLiteral("hello")) == QByteArray::fromHex(
                "0020c2c84a51efbfeb9a84e3b9e83dd11c2686fb5d7dca66028cf06c37df39959249"),
            "first encrypted record must match the independent reference vector");

    const QList<QByteArray> messages = {
        QByteArrayLiteral("first"),
        QByteArrayLiteral("second message"),
    };
    for (const QByteArray& message : messages) {
        const QByteArray record = sender.encrypt(message);
        require(record.size() > 2 &&
                AppleWire::readUInt16(record, 0) == record.size() - 2,
                "encrypted record must carry its exact ciphertext length");
        require(receiver.decrypt(record.mid(2)) == message,
                "ordered chained records must decrypt to the original message");
    }

    const QByteArray damagedRecord = sender.encrypt(QByteArrayLiteral("damaged"));
    const QByteArray nextRecord = sender.encrypt(QByteArrayLiteral("recoverable"));
    QByteArray corruptedCiphertext = damagedRecord.mid(2);
    corruptedCiphertext[0] ^= char(0x40);
    QString error;
    require(receiver.decrypt(corruptedCiphertext, &error).isEmpty() && !error.isEmpty(),
            "corrupted record must fail authentication");
    error.clear();
    require(receiver.decrypt(nextRecord.mid(2), &error) == QByteArrayLiteral("recoverable"),
            "a rejected complete record must advance the Apple chain so the next record recovers");
    require(sender.encrypt(QByteArray(65536, 'x'), &error).isEmpty(),
            "record layer must reject messages that exceed the wire length field");
}

void testControlNegotiationAndEncryptedWrite()
{
    const QByteArray masterKey = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    const QByteArray recordKey = QByteArray::fromHex("102132435465768798a9bacbdcedfe0f");
    const QByteArray recordIv = QByteArray::fromHex("0ffedccbbbaa99887766554433221100");

    QByteArray rectangle(12, '\0');
    writeUInt32(rectangle, 8, 1103);
    QByteArray rekeyPayload(4, '\0');
    rekeyPayload.append(encryptEcb(recordKey, masterKey));
    rekeyPayload.append(encryptEcb(recordIv, masterKey));

    TranscriptTransport transport;
    transport.incoming.append(char(0));
    transport.incoming.append(QByteArray::fromHex("000001"));
    transport.incoming.append(rectangle);
    transport.incoming.append(rekeyPayload);

    AppleConnectionEndpoint endpoint;
    endpoint.host = QStringLiteral("mac.example");
    QString error;
    require(transport.connectTo(endpoint, nullptr, &error),
            "control transcript must connect");
    AppleControlChannel control;
    std::atomic_bool cancelled{false};
    require(control.negotiate(transport, masterKey, &cancelled, &error),
            "control channel negotiation must accept a valid rekey update");
    require(transport.writes.size() == 4 &&
            transport.writes.at(0) == AppleWire::viewerInfo() + AppleWire::setEncryption() &&
            transport.writes.at(1) == AppleWire::setEncodings() &&
            transport.writes.at(2) == AppleWire::postEncryptionToggle(),
            "control setup writes must preserve protocol ordering");

    AppleEncryptedRecordLayer receiver(recordKey, recordIv);
    const QByteArray displayRecord = transport.writes.at(3);
    require(receiver.decrypt(displayRecord.mid(2)) == AppleWire::displayConfiguration(),
            "display configuration must be the first encrypted control message");
    const QByteArray observingMessage = QByteArray::fromHex("0a000000");
    require(control.sendEncrypted(transport, observingMessage, &cancelled, &error),
            "post-negotiation encrypted control write must succeed");
    require(transport.writes.size() == 5 &&
            receiver.decrypt(transport.writes.at(4).mid(2)) == observingMessage,
            "encrypted control writes must remain ordered on the negotiated record stream");
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    std::fprintf(stderr, "testFeatureEnabledBuildDiscoversByDefault\n");
    testFeatureEnabledBuildDiscoversByDefault();
    std::fprintf(stderr, "testSavedConnectionIdentityAndSecretBoundary\n");
    testSavedConnectionIdentityAndSecretBoundary();
    std::fprintf(stderr, "testMalformedWireInputs\n");
    testMalformedWireInputs();
    std::fprintf(stderr, "testTrustPrecedesCredentialRead\n");
    testTrustPrecedesCredentialRead();
    std::fprintf(stderr, "testFormalAuthenticationTranscript\n");
    testFormalAuthenticationTranscript();
    std::fprintf(stderr, "testAuthenticatedRecordRecoveryAndOrdering\n");
    testAuthenticatedRecordRecoveryAndOrdering();
    std::fprintf(stderr, "testControlNegotiationAndEncryptedWrite\n");
    testControlNegotiationAndEncryptedWrite();
    std::fprintf(stderr, "Apple Screen Sharing stage 2 protocol tests passed\n");
    return 0;
}
