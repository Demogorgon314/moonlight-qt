#include "backend/apple/appleauthenticator.h"
#include "backend/apple/appleconnectionstore.h"
#include "backend/apple/applefeaturegate.h"
#include "backend/apple/applemediaprotocol.h"
#include "backend/apple/applemediatransport.h"
#include "backend/apple/appleprotocol.h"
#include "backend/apple/applevideodecoder.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>

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

void testHighPerformanceMediaOfferAndAnswer()
{
    QString error;
    const QUuid callId(QStringLiteral("00112233-4455-6677-8899-aabbccddeeff"));
    const QByteArray offer = AppleMediaWire::createOffer(
            7,
            0x11223344,
            123456789,
            callId,
            false,
            QStringLiteral("Windows test"),
            &error);
    require(error.isEmpty() && offer.startsWith("bplist00") &&
            offer.contains("avcMediaStreamNegotiatorMediaBlob") &&
            offer.contains("00112233-4455-6677-8899-AABBCCDDEEFF"),
            "video offer must be a deterministic binary plist with the Apple media blob");

    AppleMediaOffers offers;
    offers.audio = QByteArray::fromHex("a1a2a3");
    offers.video = QByteArray::fromHex("b1b2");
    AppleMediaKeys keys{
        QByteArray(46, char(0x11)),
        QByteArray(46, char(0x22)),
        QByteArray(46, char(0x33)),
        QByteArray(46, char(0x44)),
    };
    const QByteArray configuration = AppleMediaWire::configuration(
            offers, keys, callId, &error);
    require(configuration.size() == 0xd8 + 3 + 2 + 4 &&
            static_cast<quint8>(configuration.at(0)) == 0x1c &&
            AppleWire::readUInt16(configuration, 2) == configuration.size() - 4 &&
            AppleWire::readUInt32(configuration, 6) == 0x05 &&
            configuration.mid(0x24, 46) == keys.audioViewer &&
            configuration.mid(0x52, 46) == keys.audioServer &&
            configuration.mid(0x80, 3) == offers.audio &&
            configuration.mid(0x83, 46) == keys.videoViewer &&
            configuration.mid(0xb1, 46) == keys.videoServer &&
            configuration.endsWith(offers.video),
            "media configuration must preserve the captured key and offer layout");

    const QByteArray answer = QByteArray::fromBase64(
            "AHByZWZpeGJwbGlzdDAw0QECXxAhYXZjTWVkaWFTdHJlYW1OZWdvdGlhdG9yTWVkaWFCbG9iTxASeJzT4lBo4NfYwWHAAgAK9AH+CAsvAAAAAAAAAQEAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAER0YWls");
    AppleCanvas canvas;
    require(AppleMediaWire::parseCanvas(answer, &canvas) &&
            canvas == AppleCanvas{1920, 1080, 4},
            "embedded binary-plist media answer must expose the single-screen tile canvas");

    QByteArray portAnswer(0x19, '\0');
    writeUInt16(portAnswer, 2, 1);
    writeUInt16(portAnswer, 4, 3);
    writeUInt16(portAnswer, 0x0a, 5900);
    writeUInt16(portAnswer, 0x10, 6001);
    writeUInt16(portAnswer, 0x12, 1);
    AppleMediaPorts ports;
    require(AppleMediaWire::parsePorts(portAnswer, &ports) &&
            ports.audio == 5900 && ports.video == 6001,
            "explicit server media ports must replace the optimistic UDP destinations");
}

void testSrtpAndRecoveryFeedbackVectors()
{
    QByteArray keyBlob(46, Qt::Uninitialized);
    for (int index = 0; index < keyBlob.size(); ++index) {
        keyBlob[index] = static_cast<char>(index);
    }
    QString error;
    AppleSrtpDecryptor decryptor(keyBlob, &error);
    AppleRtpPacket packet;
    require(decryptor.isValid() && decryptor.decrypt(
                    QByteArray::fromHex(
                            "80e0000000000000112233448e12afed4336c640f79680aab77904"),
                    &packet,
                    &error) &&
            packet.payload == QByteArrayLiteral("hello") &&
            packet.sequenceNumber == 0 && packet.timestamp == 0 &&
            packet.synchronizationSource == 0x11223344 &&
            packet.payloadType == 96 && packet.marker,
            "SRTP AES-CTR/HMAC processing must match the independent reference vector");

    AppleRtpPacket extended;
    extended.header = QByteArray::fromHex(
            "916012340102030411223344aabbccdd9301000100251682");
    const auto framePacketInfo = extended.framePacketInfo();
    require(framePacketInfo.has_value() &&
            *framePacketInfo == AppleRtpPacket::FramePacketInfo{0x25, 0x1682},
            "Apple MCI Info ID 6 must expose packet count and sender frame sequence");

    AppleSrtcpEncryptor encryptor(keyBlob, &error);
    const QByteArray plain = QByteArray::fromHex(
            "80c900011122334481cd00041122334455667788000a0005001e0000");
    require(encryptor.protect(plain, &error) == QByteArray::fromHex(
                "80c9000111223344ea6540af06680fabb3ce005aa192a9c2d9138ea18000000005039c4d56c23bf9e2ba"),
            "SRTCP encryption and authentication must match the independent reference vector");

    const QByteArray nack = AppleMediaWire::genericNack(
            0x11223344, 0x55667788, {10, 11, 15, 30});
    require(nack == QByteArray::fromHex(
                "81cd00041122334455667788000a0011001e0000"),
            "generic NACK feedback must compact nearby sequence losses into the RTCP bitmask");

    const QByteArray fir = AppleMediaWire::fullIntraRequest(
            0x11223344, 0x55667788, 9);
    require(fir == QByteArray::fromHex(
                "84ce000411223344000000005566778809000000"),
            "key-frame recovery must use the native standalone FIR packet");
    require((AppleMediaWire::receiverReport(0x11223344) + nack) ==
                QByteArray::fromHex(
                    "80c900011122334481cd00041122334455667788000a0011001e0000"),
            "packet-loss recovery must compound the receiver report and generic NACK");

    const QList<QByteArray> tileRefresh = AppleMediaWire::fullIntraRequests(
            0x11223344, {0x10101010, 0x20202020, 0x30303030, 0x40404040}, 0xfe);
    require(tileRefresh.size() == 4 &&
            tileRefresh.at(0) == QByteArray::fromHex(
                    "84ce0004112233440000000010101010fe000000") &&
            tileRefresh.at(1) == QByteArray::fromHex(
                    "84ce0004112233440000000020202020ff000000") &&
            tileRefresh.at(2) == QByteArray::fromHex(
                    "84ce000411223344000000003030303000000000") &&
            tileRefresh.at(3) == QByteArray::fromHex(
                    "84ce000411223344000000004040404001000000"),
            "refresh recovery must request every tile source and wrap its FIR sequence");

    require(AppleMediaWire::rateControl(
                    0x11223344, 0x12345678, 60001, 0xabc, 0x1234, 0x5678) ==
                QByteArray::fromHex(
                    "80cc0007112233445243544c850000043456000000001234567800000abcea61"),
            "periodic RCTL feedback must match Apple's fixed-LAN wire format");
}

void testHevcAssemblyAndLossTracking()
{
    AppleHevcAssembler assembler;
    auto packet = [](quint32 source,
                     quint32 timestamp,
                     quint16 sequence,
                     bool marker,
                     const QByteArray& payload) {
        AppleRtpPacket value;
        value.synchronizationSource = source;
        value.timestamp = timestamp;
        value.sequenceNumber = sequence;
        value.marker = marker;
        value.payload = payload;
        return value;
    };
    AppleHevcAccessUnit unit;
    require(assembler.process(packet(100, 10, 1, true,
                                     QByteArray::fromHex("4001000142")), 10, &unit) &&
            unit.nalUnits == QList<QByteArray>{QByteArray::fromHex("400142")},
            "single-NAL RTP payloads must strip the private DONL field");
    require(assembler.process(packet(100, 20, 2, true,
                                     QByteArray::fromHex("4201000143")), 20, &unit) &&
            assembler.process(packet(100, 30, 3, true,
                                     QByteArray::fromHex("4401000144")), 30, &unit) &&
            assembler.parameterSets().isComplete(),
            "VPS, SPS, and PPS must be harvested before decoder creation");

    require(!assembler.process(packet(101, 40, 10, false,
                                      QByteArray::fromHex("6201810001aa")), 40, &unit) &&
            assembler.process(packet(101, 40, 11, true,
                                     QByteArray::fromHex("6201410001bb")), 41, &unit) &&
            unit.nalUnits == QList<QByteArray>{QByteArray::fromHex("0201aabb")},
            "HEVC fragmentation units must reassemble only after a contiguous marker-terminated frame");

    require(assembler.process(packet(101, 41, 12, true,
                                     QByteArray::fromHex("02010001aa80")), 42, &unit) &&
            unit.subframeBoundary ==
                    AppleHevcAccessUnit::SubframeBoundary::NotLast &&
            assembler.process(packet(101, 42, 13, true,
                                     QByteArray::fromHex("0201000100000382")), 43, &unit) &&
            unit.subframeBoundary ==
                    AppleHevcAccessUnit::SubframeBoundary::Last,
            "the negotiated FLS EOD bit must close the final tile after removing RBSP emulation prevention bytes");

    require(assembler.primarySources(2) == QList<quint32>({100, 101}) &&
            assembler.completedSources().contains(100) &&
            assembler.completedSources().contains(101),
            "single-screen tile sources must be selected as one contiguous SSRC group");

    assembler.process(packet(101, 50, 15, true,
                             QByteArray::fromHex("0201000145")), 50, &unit);
    const auto nacks = assembler.takeNacks(50);
    require(nacks.value(101).contains(14),
            "sequence gaps must produce bounded NACK recovery feedback");
}

void testHevcGlobalDecodingOrderAdmission()
{
    auto unit = [](quint32 source, std::optional<quint16> order) {
        AppleHevcAccessUnit value;
        value.synchronizationSource = source;
        value.decodingOrderNumber = order;
        return value;
    };

    AppleHevcDecodingOrderQueue admission;
    const QList<AppleHevcAccessUnit> initial = admission.enqueue({
        unit(400, 0), unit(100, 0xfffe), unit(200, 0xffff),
    });
    require(initial.size() == 3 &&
            initial.at(0).synchronizationSource == 100 &&
            initial.at(1).synchronizationSource == 200 &&
            initial.at(2).synchronizationSource == 400,
            "initial multi-tile DON admission must preserve circular decoding order");

    const QList<AppleHevcAccessUnit> continued = admission.enqueue({
        unit(100, 0xffff), unit(300, 1), unit(500, std::nullopt),
    });
    require(continued.size() == 2 &&
            continued.at(0).synchronizationSource == 500 &&
            continued.at(1).synchronizationSource == 300,
            "global DON admission must reject a late tile while accepting forward and unnumbered subframes");

    admission.reset();
    require(admission.enqueue({unit(100, 10)}).size() == 1 &&
            admission.enqueue({unit(200, 9)}).isEmpty() &&
            admission.enqueue({unit(300, 12)}).size() == 1,
            "DON admission must be shared across tile SSRCs and allow forward gaps");
}

void testEncryptedInputWireBoundary()
{
    const AppleInputEncryptionRequest pointer = AppleMediaWire::pointerEvent(
            0x03, 640, 480, 1, 0x01020304);
    require(pointer.isValid() && pointer.header == QByteArray::fromHex("1003") &&
            pointer.plaintextBlock == QByteArray::fromHex(
                    "00000000000001020304ff03028001e0"),
            "pointer input must preserve buttons, click count, timestamp, and canvas coordinates");

    const AppleInputEncryptionRequest key = AppleMediaWire::keyEvent(
            true, 0xff0d, 9, 0, 40);
    require(key.isValid() && key.header == QByteArray::fromHex("1001") &&
            key.plaintextBlock == QByteArray::fromHex(
                    "ff010000ff0d00000009000000000028"),
            "key input must preserve X11 keysym and native scan code fields");

    AppleEncryptedRecordLayer records(
            QByteArray::fromHex("00112233445566778899aabbccddeeff"),
            QByteArray::fromHex("ffeeddccbbaa99887766554433221100"));
    const QByteArray inputMessage = records.encryptInput(
            key.header, key.plaintextBlock);
    require(inputMessage.size() == 18 && inputMessage.startsWith(key.header) &&
            inputMessage.mid(2) != key.plaintextBlock,
            "the inner input block must use the record content key without consuming record order");
}

void testHevcDecoderBackendFallback()
{
    QString error;
    AppleHevcDecoder preferred(true);
    require(preferred.open(&error),
            "the preferred HEVC decoder must open with D3D11VA or software fallback");
    require(preferred.backend() == AppleHevcDecoder::Backend::D3D11va ||
            (preferred.backend() == AppleHevcDecoder::Backend::Software &&
             preferred.hardwareFallbackOccurred()),
            "hardware decoder failure must be explicit when software fallback is selected");
    std::fprintf(stderr, "preferred HEVC decoder backend: %s\n",
                 preferred.backend() == AppleHevcDecoder::Backend::D3D11va
                         ? "D3D11VA" : "software fallback");

    AppleHevcDecoder software(false);
    error.clear();
    require(software.open(&error) &&
            software.backend() == AppleHevcDecoder::Backend::Software &&
            !software.hardwareFallbackOccurred(),
            "the software HEVC decoder must remain independently usable");
}

void testScaledTileBoundariesRemainContiguous()
{
    const AppleCanvas canvas{3840, 2160, 4};
    const QList<int> boundaries = AppleMediaLayout::verticalTileBoundaries(
            canvas, {540, 540, 540, 540}, 1001);
    require(boundaries == QList<int>({0, 250, 501, 751, 1001}),
            "fractionally scaled tiles must share rounded boundaries without a one-pixel seam");
    int coveredPixels = 0;
    for (int tile = 0; tile < canvas.tileCount; ++tile) {
        require(boundaries.at(tile + 1) >= boundaries.at(tile),
                "scaled tile boundaries must remain monotonic");
        coveredPixels += boundaries.at(tile + 1) - boundaries.at(tile);
    }
    require(coveredPixels == 1001,
            "scaled tile rows must cover the exact presentation height");
}

void testDecodedNv12TileValidation()
{
    AppleDecodedTile tile;
    tile.tileIndex = 1;
    tile.width = 1920;
    tile.height = 540;
    tile.stride = 1920;
    tile.chromaStride = 1920;
    tile.chromaOffset = tile.stride * tile.height;
    tile.pixels.resize(tile.chromaOffset + tile.chromaStride * tile.height / 2);
    require(tile.isValid(),
            "a complete NV12 tile must be accepted by the presentation boundary");
    tile.pixels.chop(1);
    require(!tile.isValid(),
            "a truncated NV12 chroma plane must be rejected before SDL upload");
}

QList<QByteArray> splitAnnexB(const QByteArray& stream)
{
    QList<QByteArray> nalUnits;
    auto startCodeLength = [&stream](int offset) {
        if (offset + 3 <= stream.size() &&
                stream.mid(offset, 3) == QByteArray::fromHex("000001")) {
            return 3;
        }
        if (offset + 4 <= stream.size() &&
                stream.mid(offset, 4) == QByteArray::fromHex("00000001")) {
            return 4;
        }
        return 0;
    };
    int cursor = 0;
    while (cursor < stream.size()) {
        int prefixLength = 0;
        while (cursor < stream.size() &&
                (prefixLength = startCodeLength(cursor)) == 0) {
            ++cursor;
        }
        if (prefixLength == 0) {
            break;
        }
        const int nalStart = cursor + prefixLength;
        int next = nalStart;
        while (next < stream.size() && startCodeLength(next) == 0) {
            ++next;
        }
        if (next > nalStart) {
            nalUnits.append(stream.mid(nalStart, next - nalStart));
        }
        cursor = next;
    }
    return nalUnits;
}

void testAppleHevcDecoderPreservesLowLatency444Output()
{
    // HEVC RExt 8-bit 4:4:4 fixture: 64x64, no B-frames, SMPTE-170M,
    // limited range, one random-access picture.
    const QByteArray encoded = QByteArray::fromBase64(
            "AAAAAUABDAH//wQIAAADAJ4oAAADAAAeugJAAAAAAUIBAQQIAAADAJ4oAAADAAAekAQQILLdSSZXgLUCAgYEAAADAAQAAAMA8CAAAAABRAHBcoYMAiQAAAEoAa946wIBgtv/rYj/dW1RUec=");
    AppleHevcParameterSets parameterSets;
    AppleHevcAccessUnit accessUnit;
    accessUnit.timestamp = 90'000;
    accessUnit.frameSequenceNumber = 7;
    for (const QByteArray& nalUnit : splitAnnexB(encoded)) {
        require(nalUnit.size() >= 2, "the embedded HEVC fixture must contain valid NAL units");
        const int type = (static_cast<quint8>(nalUnit.at(0)) >> 1) & 0x3f;
        if (type == 32) {
            parameterSets.video = nalUnit;
        }
        else if (type == 33) {
            parameterSets.sequence = nalUnit;
        }
        else if (type == 34) {
            parameterSets.pictures.append(nalUnit);
        }
        else if (type < 32) {
            accessUnit.nalUnits.append(nalUnit);
        }
    }
    require(parameterSets.isComplete() && accessUnit.containsVideoSlice(),
            "the embedded HEVC fixture must expose complete parameters and one picture");

    AppleHevcDecoder decoder(false);
    QString error;
    require(decoder.open(&error),
            "the low-latency software HEVC decoder must open for the 4:4:4 fixture");
    const QList<AppleDecodedTile> frames = decoder.decode(
            accessUnit, parameterSets, 0, &error);
    require(frames.size() == 1,
            "one low-delay access unit must produce its frame without frame-thread buffering");
    const AppleDecodedTile& frame = frames.first();
    require(frame.pixelFormat == AppleDecodedTile::PixelFormat::Vuya &&
            frame.colorSpace == AppleDecodedTile::ColorSpace::Bt601 &&
            frame.colorRange == AppleDecodedTile::ColorRange::Limited &&
            frame.stride >= frame.width * 4 &&
            frame.pixels.size() >= frame.stride * frame.height,
            "HEVC RExt 4:4:4 chroma and its matrix/range must survive the display boundary");
}

void testDecodedTilesPublishAsAtomicSenderFrames()
{
    AppleDecodedFrameBatcher batcher;
    auto accessUnit = [](quint16 sequence) {
        AppleHevcAccessUnit unit;
        unit.frameSequenceNumber = sequence;
        return unit;
    };
    auto tile = [](int tileIndex, quint16 sequence) {
        AppleDecodedTile frame;
        frame.tileIndex = tileIndex;
        frame.frameSequenceNumber = sequence;
        return frame;
    };

    batcher.recordSubmission(accessUnit(100), 0);
    batcher.recordSubmission(accessUnit(100), 3);
    batcher.recordDecodedFrames({tile(3, 100), tile(0, 100)});
    require(batcher.takeReadyBatches().isEmpty(),
            "a sender frame must remain pending until its sequence is closed");

    batcher.recordSubmission(accessUnit(101), 1);
    const QList<QList<AppleDecodedTile>> first = batcher.takeReadyBatches();
    require(first.size() == 1 && first.first().size() == 2 &&
            first.first().at(0).tileIndex == 0 &&
            first.first().at(1).tileIndex == 3,
            "all decoded tiles in one sender frame must publish atomically in submission order");

    batcher.recordDecodedFrames({tile(1, 101)});
    batcher.recordSubmission(accessUnit(102), 2);
    const QList<QList<AppleDecodedTile>> second = batcher.takeReadyBatches();
    require(second.size() == 1 && second.first().size() == 1 &&
            second.first().first().frameSequenceNumber == 101,
            "sparse tile frames must publish without waiting for every canvas tile");
}

void testDecodedTilesPublishOnFlsEndOfDataWithoutTearing()
{
    AppleDecodedFrameBatcher batcher;
    auto accessUnit = [](int tileIndex, bool isLast) {
        AppleHevcAccessUnit unit;
        unit.synchronizationSource = static_cast<quint32>(tileIndex + 1);
        unit.frameSequenceNumber = 500;
        unit.subframeBoundary = isLast
                ? AppleHevcAccessUnit::SubframeBoundary::Last
                : AppleHevcAccessUnit::SubframeBoundary::NotLast;
        return unit;
    };
    auto tile = [](int tileIndex) {
        AppleDecodedTile frame;
        frame.tileIndex = tileIndex;
        frame.frameSequenceNumber = 500;
        return frame;
    };

    batcher.recordSubmission(accessUnit(0, false), 0);
    batcher.recordDecodedFrames({tile(0)});
    batcher.recordSubmission(accessUnit(1, true), 1);
    require(batcher.takeReadyBatches().isEmpty(),
            "FLS EOD must not expose a partially decoded sender frame");
    batcher.recordDecodedFrames({tile(1)});
    const QList<QList<AppleDecodedTile>> batches = batcher.takeReadyBatches();
    require(batches.size() == 1 && batches.first().size() == 2 &&
            batches.first().at(0).tileIndex == 0 &&
            batches.first().at(1).tileIndex == 1,
            "FLS EOD must publish every decoded tile atomically without waiting for the next sender frame");

    AppleDecodedFrameBatcher recoveringBatcher;
    recoveringBatcher.recordSubmission(accessUnit(0, false), 0);
    recoveringBatcher.recordDecodedFrames({tile(0)});
    recoveringBatcher.recordSubmission(accessUnit(1, true), 1);
    recoveringBatcher.recordDecodeFailure(500, 1);
    const QList<QList<AppleDecodedTile>> recovered =
            recoveringBatcher.takeReadyBatches();
    require(recovered.size() == 1 && recovered.first().size() == 1 &&
            recovered.first().first().tileIndex == 0,
            "a failed final tile must be removed immediately so it cannot head-of-line block later atomic frames");
}

quint16 findFourAvailableUdpPorts()
{
    for (quint16 basePort = 40000; basePort < 65000; basePort += 4) {
        QUdpSocket sockets[4];
        bool available = true;
        for (quint16 offset = 0; offset < 4; ++offset) {
            if (!sockets[offset].bind(QHostAddress::LocalHost,
                                      static_cast<quint16>(basePort + offset),
                                      QUdpSocket::DontShareAddress)) {
                available = false;
                break;
            }
        }
        if (available) {
            return basePort;
        }
    }
    return 0;
}

void testUdpPunchIgnoresClosedOptimisticPortReset()
{
#ifdef Q_OS_WIN
    const quint16 basePort = findFourAvailableUdpPorts();
    require(basePort != 0, "four consecutive UDP ports must be available for the reset test");

    AppleMediaTransport media;
    QString error;
    require(media.open(QHostAddress::LocalHost, basePort, &error),
            "media transport must bind its optimistic local UDP ports");

    QByteArray initialPunch;
    std::atomic_bool cancelled = false;
    require(media.receiveVideo(&initialPunch, 250, &cancelled, &error),
            "the initial loopback video punch must be drained before the reset test");
    media.drainControl();

    media.configureRemotePorts(AppleMediaPorts{
            static_cast<quint16>(basePort + 2),
            static_cast<quint16>(basePort + 3)});
    QThread::msleep(110);
    media.punchIfDue();

    for (int attempt = 0; attempt < 5 && error.isEmpty(); ++attempt) {
        QByteArray datagram;
        media.receiveVideo(&datagram, 100, &cancelled, &error);
    }
    require(error.isEmpty(),
            "an ICMP reset from an unopened optimistic UDP port must not abort media negotiation");
#endif
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
    std::fprintf(stderr, "testHighPerformanceMediaOfferAndAnswer\n");
    testHighPerformanceMediaOfferAndAnswer();
    std::fprintf(stderr, "testSrtpAndRecoveryFeedbackVectors\n");
    testSrtpAndRecoveryFeedbackVectors();
    std::fprintf(stderr, "testHevcAssemblyAndLossTracking\n");
    testHevcAssemblyAndLossTracking();
    std::fprintf(stderr, "testHevcGlobalDecodingOrderAdmission\n");
    testHevcGlobalDecodingOrderAdmission();
    std::fprintf(stderr, "testEncryptedInputWireBoundary\n");
    testEncryptedInputWireBoundary();
    std::fprintf(stderr, "testHevcDecoderBackendFallback\n");
    testHevcDecoderBackendFallback();
    std::fprintf(stderr, "testScaledTileBoundariesRemainContiguous\n");
    testScaledTileBoundariesRemainContiguous();
    std::fprintf(stderr, "testDecodedNv12TileValidation\n");
    testDecodedNv12TileValidation();
    std::fprintf(stderr, "testAppleHevcDecoderPreservesLowLatency444Output\n");
    testAppleHevcDecoderPreservesLowLatency444Output();
    std::fprintf(stderr, "testDecodedTilesPublishAsAtomicSenderFrames\n");
    testDecodedTilesPublishAsAtomicSenderFrames();
    std::fprintf(stderr, "testDecodedTilesPublishOnFlsEndOfDataWithoutTearing\n");
    testDecodedTilesPublishOnFlsEndOfDataWithoutTearing();
    std::fprintf(stderr, "testUdpPunchIgnoresClosedOptimisticPortReset\n");
    testUdpPunchIgnoresClosedOptimisticPortReset();
    std::fprintf(stderr, "Apple Screen Sharing stage 3 protocol tests passed\n");
    return 0;
}
