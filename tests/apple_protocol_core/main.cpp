#include "backend/apple/appleauthenticator.h"
#include "backend/apple/appleaudiostream.h"
#include "backend/apple/appleconnectionstore.h"
#include "backend/apple/applecontrolfeatures.h"
#include "backend/apple/applefeaturegate.h"
#include "backend/apple/applefilecopy.h"
#include "backend/apple/applefiledrag.h"
#ifdef Q_OS_WIN
#include "backend/apple/applefiledrag_win.h"
#include <qt_windows.h>
#endif
#include "backend/apple/applefiletransfer.h"
#include "backend/apple/applefiletransferservice.h"
#include "backend/apple/applekeyboardmapper.h"
#ifdef Q_OS_DARWIN
#include "backend/apple/applefiledrag_mac.h"
#include "backend/apple/applemacinputbridge.h"
#include <ApplicationServices/ApplicationServices.h>

bool testAppleMacZoomButtonUsesNativeFullscreen();
bool testAppleMacInputBridgeRoutesRemoteDragBeforePointer();
bool testAppleMacInputBridgeRoutesLocalFileDrag();
bool testAppleMacInputBridgeReleasesModifiersOnFocusLoss();
#endif
#include "backend/apple/applemediaprotocol.h"
#include "backend/apple/applemediatransport.h"
#include "backend/apple/appleprotocol.h"
#include "backend/apple/applevideodecoder.h"
#include "backend/apple/applevideorenderer.h"
#include "backend/apple/applewindowplacement.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QMimeData>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>
#include <QUrl>

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

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

QByteArray cursorRectangle(quint16 hotspotX,
                           quint16 hotspotY,
                           quint16 width,
                           quint16 height,
                           quint32 id,
                           const QByteArray& compressed)
{
    QByteArray rectangle(12, '\0');
    writeUInt16(rectangle, 0, hotspotX);
    writeUInt16(rectangle, 2, hotspotY);
    writeUInt16(rectangle, 4, width);
    writeUInt16(rectangle, 6, height);
    writeUInt32(rectangle, 8, 0x450);
    AppleWire::appendUInt32(rectangle, id);
    AppleWire::appendUInt32(rectangle, static_cast<quint32>(compressed.size()));
    rectangle.append(compressed);
    return rectangle;
}

class TranscriptTransport final : public AppleByteTransport
{
public:
    QByteArray incoming;
    QList<QByteArray> writes;
    QList<int> protocolDelays;
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

    void protocolDelay(int milliseconds, std::atomic_bool*) override
    {
        protocolDelays.append(milliseconds);
    }
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
        require(store.setVirtualDisplayCount(firstId, 2) &&
                store.connection(firstId).virtualDisplayCount == 2,
                "the virtual display count must be stored per saved Mac");

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
        require(store.setVirtualDisplayCount(readded.id, 2),
                "the replacement connection display count must persist independently");
    }

    QFile settingsFile(path);
    require(settingsFile.open(QIODevice::ReadOnly), "settings file must be readable");
    const QByteArray settingsData = settingsFile.readAll().toLower();
    require(!settingsData.contains("password") &&
            !settingsData.contains("credentialblob"),
            "connection settings must not contain password material");

    AppleConnectionStore reloaded(path);
    require(reloaded.connections().size() == 1 &&
            reloaded.connections().first().id != firstId &&
            reloaded.connections().first().virtualDisplayCount == 2,
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
    require(AppleWire::displayConfiguration(
                    {QSize(800, 600), QSize(800, 600), QSize(800, 600)}).isEmpty(),
            "more than two Apple displays must be rejected");

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
    require(!AppleCredentialStore::displayName().isEmpty(),
            "the active platform credential adapter must expose a user-facing name");
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
    QByteArray serverCommands(16, '\0');
    serverCommands[2] = 0x01;
    QByteArray serverNameData = QByteArray::fromHex("000001020304") +
            serverCommands + serverName;
    QByteArray serverHeader(24, '\0');
    writeUInt16(serverHeader, 0, 1440);
    writeUInt16(serverHeader, 2, 900);
    writeUInt32(serverHeader, 20, static_cast<quint32>(serverNameData.size()));
    transport.incoming.append(serverHeader);
    transport.incoming.append(serverNameData);

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
    require(result.hasEnhancedServerInfo && result.serverFlags == 0x01020304 &&
            result.serverCommandBitmap == serverCommands &&
            result.supportsServerCommand(23) &&
            !result.supportsServerCommand(22),
            "enhanced ServerInit must retain the advertised server command bitmap");
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

void testHighPerformanceEncodingCapabilitiesMatchNativeOrder()
{
    require(AppleWire::setEncodings() == QByteArray::fromHex(
                    "0200000e"
                    "000003f2000003f3000003ea"
                    "0000000600000010ffffff11"
                    "000004500000044cffffff21"
                    "0000044d0000045100000453"
                    "0000045500000456"),
            "high-performance encodings must match the native capability order");
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
    require(transport.protocolDelays.isEmpty(),
            "control encryption negotiation must be driven by the rekey instead of fixed delays");
    require(transport.writes.size() == 5 &&
            transport.writes.at(0) == AppleWire::viewerInfo() &&
            transport.writes.at(1) == AppleWire::setEncryption() &&
            transport.writes.at(2) == AppleWire::postEncryptionToggle(),
            "control setup writes must preserve the native rekey ordering");

    AppleEncryptedRecordLayer receiver(recordKey, recordIv);
    const QByteArray displayRecord = transport.writes.at(3);
    require(receiver.decrypt(displayRecord.mid(2)) == AppleWire::displayConfiguration(),
            "display configuration must be the first encrypted control message");
    require(receiver.decrypt(transport.writes.at(4).mid(2)) ==
                    AppleWire::setEncodings(),
            "encoding capabilities must be sent only after encryption is enabled");
    const QByteArray observingMessage = QByteArray::fromHex("0a000000");
    require(control.sendEncrypted(transport, observingMessage, &cancelled, &error),
            "post-negotiation encrypted control write must succeed");
    require(transport.writes.size() == 6 &&
            receiver.decrypt(transport.writes.at(5).mid(2)) == observingMessage,
            "encrypted control writes must remain ordered on the negotiated record stream");
}

void testHighPerformanceMediaOfferAndAnswer()
{
    QString error;
    require(AppleMediaNegotiator::framebufferStartupMessages() ==
                    QList<QByteArray>{
                            AppleMediaWire::framebufferUpdateRequest(),
                            AppleMediaWire::autoFramebufferUpdate()},
            "media startup must not repeat the encrypted encoding capabilities");
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

    AppleMediaOffers secondaryOffers;
    secondaryOffers.video = QByteArray::fromHex("c1c2c3c4");
    AppleMediaKeys dualKeys = keys;
    dualKeys.secondaryVideoViewer = QByteArray(46, char(0x55));
    dualKeys.secondaryVideoServer = QByteArray(46, char(0x66));
    const QByteArray dualConfiguration = AppleMediaWire::configuration(
            offers, &secondaryOffers, dualKeys, callId, &error);
    const int secondaryOffset = 0x80 + offers.audio.size() + 92 +
            offers.video.size();
    require(error.isEmpty() &&
            AppleWire::readUInt32(dualConfiguration, 6) == 0x07 &&
            AppleWire::readUInt16(dualConfiguration, 14) ==
                    secondaryOffers.video.size() &&
            dualConfiguration.mid(secondaryOffset, 46) ==
                    dualKeys.secondaryVideoViewer &&
            dualConfiguration.mid(secondaryOffset + 46, 46) ==
                    dualKeys.secondaryVideoServer &&
            dualConfiguration.mid(secondaryOffset + 92) ==
                    secondaryOffers.video,
            "dual-display media configuration must carry an independent second offer and SRTP key pair");

    portAnswer.resize(0x19);
    writeUInt16(portAnswer, 0x16, 6002);
    portAnswer[0x18] = 1;
    require(AppleMediaWire::parsePorts(portAnswer, &ports) &&
            ports.videoPorts() == QList<quint16>{6001, 6002},
            "dual-display media answer must expose both independent video ports");

    const QList<AppleCanvas> dualCanvases =
            AppleMediaWire::parseCanvases(answer + answer);
    require(dualCanvases.size() == 2 &&
            dualCanvases.at(0) == AppleCanvas{1920, 1080, 4} &&
            dualCanvases.at(1) == AppleCanvas{1920, 1080, 4},
            "dual-display media answers must expose one canvas per video stream");
}

void testStageFourDisplayConfigurationAndDynamicResolution()
{
    constexpr int descriptorSize = 0x9c + 28 * 5;
    const QByteArray displays = AppleWire::displayConfiguration(
            {QSize(1920, 1080), QSize(1280, 720)});
    require(displays.size() == 12 + descriptorSize * 2 &&
            static_cast<quint8>(displays.at(0)) == 0x1d &&
            AppleWire::readUInt16(displays, 2) == displays.size() - 4 &&
            AppleWire::readUInt16(displays, 6) == 2,
            "display configuration must encode exactly two independent virtual displays");
    const int firstMode = 12 + 0x9c;
    const int secondMode = 12 + descriptorSize + 0x9c;
    require(AppleWire::readUInt32(displays, firstMode) == 3840 &&
            AppleWire::readUInt32(displays, firstMode + 4) == 2160 &&
            AppleWire::readUInt32(displays, secondMode) == 2560 &&
            AppleWire::readUInt32(displays, secondMode + 4) == 1440,
            "each display descriptor must scale its backing pixels independently");

    require(AppleDynamicResolution::normalizedSize(2560, 1440) ==
                    QSize(1920, 1080) &&
            AppleDynamicResolution::normalizedSize(1000, 901) ==
                    QSize(1000, 900) &&
            AppleDynamicResolution::normalizedSize(100, 100) ==
                    QSize(320, 320) &&
            !AppleDynamicResolution::normalizedSize(0, 1080).isValid(),
            "dynamic resolution must preserve aspect ratio within the Mac client's even 1920x1080 bounds");

    require(AppleDynamicResolution::initialDisplaySize(
                    QSize(1280, 720)) ==
                    QSize(1280, 720),
            "initial dynamic resolution must restore the last stream viewport instead of the launcher window size");
    require(AppleDynamicResolution::initialDisplaySize(std::nullopt) ==
                    QSize(1440, 900) &&
            AppleDynamicResolution::normalizedSizeForDpi(
                    3577, 1746, 2.0) == QSize(1788, 872),
            "dynamic resolution must use Swift's default and logical content size on HiDPI displays");

    require(AppleMediaWire::selectCombinedDisplays() ==
                    QByteArray::fromHex("0d01000000000000") &&
            AppleMediaWire::selectDisplay(0x12345678) ==
                    QByteArray::fromHex("0d00000012345678"),
            "dual-display input selection must match the native Apple wire messages");
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

    const AppleRtpReceptionReport report{
        0x55667788, 0x55, 1, 0x00010002, 0x1234, 0, 0,
    };
    require(AppleMediaWire::receiverReport(0x11223344, report) ==
                    QByteArray::fromHex(
                            "81c9000711223344556677885500000100010002000012340000000000000000"),
            "receiver reports must include native loss, sequence, and jitter statistics");

    const AppleVideoFrameLossFeedback frameLoss{
        0x55667788, 0x12345678, 3, 1,
    };
    require(AppleMediaWire::frameLossFeedback(0x11223344, frameLoss) ==
                    QByteArray::fromHex(
                            "8fce00051122334455667788000000061234567800c10301"),
            "Apple frame-loss feedback must preserve per-frame packet loss metadata");
}

void testAdaptiveRateControlFeedback()
{
    AppleVideoRateControlEstimator estimator;
    for (int packet = 0; packet < 6; ++packet) {
        estimator.observe(24'000, packet * 2'000'000LL, 1'200,
                          AppleVideoBandwidthProbeActivity::Active);
    }
    estimator.observe(24'400, 20'000'000LL, 1'200,
                      AppleVideoBandwidthProbeActivity::Boundary);

    const auto feedback = estimator.feedback(30'000'000LL);
    require(feedback.has_value() &&
                    feedback->rtpTimestamp == 24'400 &&
                    feedback->receivedPacketCount == 7 &&
                    feedback->estimatedBandwidthKilobitsPerSecond == 4'800 &&
                    feedback->feedbackDelayMilliseconds == 10,
            "RCTL must report the measured probe capacity and current playout age");
    require(estimator.feedback(10'000'000LL)
                    ->feedbackDelayMilliseconds == 0,
            "RCTL feedback age must clamp a clock adjustment to zero");
    require(AppleMediaWire::rateControl(0x11223344, *feedback) ==
                    QByteArray::fromHex(
                            "80cc0007112233445243544c85000004005f00000000000a001e0000000712c0"),
            "adaptive RCTL serialization must retain native timestamp, delay, echo, and capacity fields");

    AppleVideoRateControlEstimator changing;
    const auto feedGroup = [&changing](int frame,
                                       qint64 spacingNanoseconds,
                                       int packetCount = 6) {
        const quint32 timestamp = 90'000 + 1'500 * frame;
        const qint64 arrival = 1'000'000'000'000LL +
                frame * 20'000'000LL;
        for (int packet = 0; packet < packetCount; ++packet) {
            changing.observe(
                    timestamp, arrival + packet * spacingNanoseconds,
                    1'200, AppleVideoBandwidthProbeActivity::Active);
        }
    };
    feedGroup(0, 300'000, 33);
    feedGroup(1, 300'000, 33);
    feedGroup(2, 2'000'000);
    require(changing.feedback(1'001'000'000'000LL)
                    ->estimatedBandwidthKilobitsPerSecond == 32'001,
            "stable native probe samples must publish their measured capacity marker");
    feedGroup(3, 2'000'000);
    feedGroup(4, 2'000'000);
    feedGroup(5, 2'000'000);
    require(changing.feedback(1'001'000'000'000LL)
                    ->estimatedBandwidthKilobitsPerSecond == 4'800,
            "a sudden bandwidth change must require three consistent probe windows before commit");

    AppleVideoRateControlEstimator wrapping;
    wrapping.observe(0xffff'ff9bU, 1'000'000'000LL, 1'200,
                     AppleVideoBandwidthProbeActivity::Boundary);
    wrapping.observe(100, 1'010'000'000LL, 1'200,
                     AppleVideoBandwidthProbeActivity::Boundary);
    const auto wrapped = wrapping.feedback(1'020'000'000LL);
    require(wrapped.has_value() && wrapped->rtpTimestamp == 100 &&
                    wrapped->receivedPacketCount == 2,
            "RCTL playout statistics must advance across RTP timestamp wraparound");
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

    AppleHevcAssembler reception;
    reception.process(packet(200, 24'000, 100, true,
                             QByteArray::fromHex("0201000145")),
                      1'000, &unit, 1'000'000'000LL);
    reception.process(packet(200, 24'400, 102, true,
                             QByteArray::fromHex("0201000146")),
                      1'020, &unit, 1'020'000'000LL);
    const auto report = reception.receptionReport(200);
    require(report.has_value() && report->fractionLost == 85 &&
                    report->cumulativePacketsLost == 1 &&
                    report->extendedHighestSequence == 102,
            "RTP reception tracking must expose interval and cumulative loss for full RR blocks");
    require(reception.receptionReport(200)->fractionLost == 0,
            "receiver-report interval loss must reset after each report");

    auto framePacket = packet(300, 48'000, 10, false,
                              QByteArray::fromHex("0201000145"));
    framePacket.header = QByteArray::fromHex(
            "9060000a0000bb80300000008101000100030007");
    reception.process(framePacket, 2'000, &unit, 2'000'000'000LL);
    framePacket.sequenceNumber = 11;
    framePacket.marker = true;
    framePacket.payload = QByteArray::fromHex("0201000146");
    reception.process(framePacket, 2'001, &unit, 2'001'000'000LL);
    const auto frameLoss = reception.frameLossFeedbackDue(2'001);
    require(frameLoss.size() == 1 &&
                    frameLoss.first().mediaSource == 300 &&
                    frameLoss.first().expectedPacketCount == 3 &&
                    frameLoss.first().lostPacketCount == 1,
            "completed frames with missing MCI packets must emit Apple frame-loss feedback");
    reception.markFrameLossFeedbackSent(frameLoss.first(), 2'001);
    require(reception.frameLossFeedbackDue(2'099).isEmpty() &&
                    reception.frameLossFeedbackDue(2'101).size() == 1,
            "frame-loss feedback must retry at the native 100 ms cadence until repaired");
    framePacket.sequenceNumber = 12;
    framePacket.marker = false;
    reception.process(framePacket, 2'102, &unit, 2'102'000'000LL);
    require(reception.frameLossFeedbackDue(2'202).isEmpty(),
            "a late packet completing the frame must cancel repeated frame-loss feedback");

    AppleHevcAssembler replacement;
    for (quint32 source : {100U, 101U, 200U, 201U}) {
        const int count = source < 200 ? 8 : 5;
        for (int index = 0; index < count; ++index) {
            replacement.process(packet(source, 60'000 + index,
                                       static_cast<quint16>(index), true,
                                       QByteArray::fromHex("0201000145")),
                                index, &unit);
        }
    }
    require(replacement.replacementSources(
                    2, {100, 101}, {}, 5) ==
                    QList<quint32>({200, 201}),
            "stalled decoding must be able to select a fresh SSRC group while excluding abandoned sources");
    require(replacement.replacementSources(
                    2, {100, 101}, {}, 6).isEmpty() &&
            replacement.replacementSources(
                    2, {100, 101}, {200, 201}, 5).isEmpty(),
            "SSRC replacement must reject unproven and previously abandoned source groups");
}

void testMinimizedFrameUpdatePolicy()
{
    require(AppleMediaWire::autoFramebufferUpdate(0xffffffffU) ==
                    QByteArray::fromHex(
                            "09000001ffffffff00000000ffffffff") &&
            AppleMediaWire::autoFramebufferUpdate(0) ==
                    QByteArray::fromHex(
                            "090000010000000000000000ffffffff"),
            "automatic frame updates must support native pause and resume intervals");

    AppleFrameUpdatePauseState state;
    require(state.setMiniaturized(true, 1) ==
                    std::optional<quint32>(0xffffffffU) &&
            !state.setMiniaturized(true, 1).has_value() &&
            state.setMiniaturized(false, 1) == std::optional<quint32>(0) &&
            !state.setMiniaturized(true, 2).has_value(),
            "single-window minimization must pause once, resume once, and leave multi-window sessions active");
    require(state.setMiniaturized(true, 1).has_value() &&
                    state.setMiniaturized(false, 2) ==
                            std::optional<quint32>(0),
            "restoring frame updates must not depend on the current window count");
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

    const AppleInputEncryptionRequest fileDragDown =
            AppleMediaWire::pointerEvent(1, 640, 480, 1, 1);
    const AppleInputEncryptionRequest fileDragUp =
            AppleMediaWire::pointerEvent(0, 640, 480, 1, 2);
    require(fileDragDown.header == QByteArray::fromHex("1003") &&
                    fileDragUp.header == QByteArray::fromHex("1003"),
            "Finder file-drag down and up frames must retain click count one");

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

void testAppleKeyboardMappingAndFocusRelease()
{
    AppleKeyboardMapper printableMapper(false, 40);
    const auto firstDown = printableMapper.update(
            true, SDLK_a, SDL_SCANCODE_A, false);
    const auto heldRepeat = printableMapper.update(
            true, SDLK_a, SDL_SCANCODE_A, false);
    const auto firstUp = printableMapper.update(
            false, SDLK_a, SDL_SCANCODE_A, false);
    const auto secondDown = printableMapper.update(
            true, SDLK_a, SDL_SCANCODE_A, false);
    const auto secondUp = printableMapper.update(
            false, SDLK_a, SDL_SCANCODE_A, false);
    require(firstDown.has_value() && firstDown->isDown &&
                    firstDown->keyboardType == 40 &&
                    !heldRepeat.has_value() &&
                    firstUp.has_value() && !firstUp->isDown &&
                    firstUp->keyboardType == 40 &&
                    secondDown.has_value() && secondDown->isDown &&
                    secondUp.has_value() && !secondUp->isDown &&
                    printableMapper.pressedKeyCount() == 0,
            "ordinary keys must preserve physical down/up duration and remain repeatable");

    AppleKeyboardMapper swallowedWinMapper(false, 40);
    const QList<AppleRemoteKeyEvent> recoveredShortcut =
            swallowedWinMapper.updateWithModifiers(
                    true, SDLK_c, SDL_SCANCODE_C,
                    KMOD_LGUI, KMOD_NONE, true);
    require(recoveredShortcut.size() == 2 &&
                    recoveredShortcut.at(0).isDown &&
                    recoveredShortcut.at(0).symbol == 0xffeb &&
                    recoveredShortcut.at(0).keyCode == 55 &&
                    recoveredShortcut.at(1).isDown &&
                    recoveredShortcut.at(1).symbol == 'c' &&
                    recoveredShortcut.at(1).keyCode == 8,
            "a GUI modifier reported only on C must reconstruct native Command-down before C-down");
    const QList<AppleRemoteKeyEvent> recoveredCUp =
            swallowedWinMapper.updateWithModifiers(
                    false, SDLK_c, SDL_SCANCODE_C,
                    KMOD_LGUI, KMOD_NONE, true);
    const QList<AppleRemoteKeyEvent> recoveredCommandRelease =
            swallowedWinMapper.updateWithModifiers(
                    true, SDLK_v, SDL_SCANCODE_V,
                    KMOD_NONE, KMOD_NONE, true);
    require(recoveredCUp.size() == 1 &&
                    !recoveredCUp.at(0).isDown &&
                    recoveredCUp.at(0).symbol == 'c' &&
                    recoveredCommandRelease.size() == 2 &&
                    !recoveredCommandRelease.at(0).isDown &&
                    recoveredCommandRelease.at(0).symbol == 0xffeb &&
                    recoveredCommandRelease.at(1).isDown &&
                    recoveredCommandRelease.at(1).symbol == 'v',
            "a reconstructed Command must remain held across C-up and release before the next unmodified key");
    const QList<AppleRemoteKeyEvent> recoveredFocusRelease =
            swallowedWinMapper.releaseAll();
    require(recoveredFocusRelease.size() == 1 &&
                    recoveredFocusRelease.at(0).symbol == 'v' &&
                    !recoveredFocusRelease.at(0).isDown,
            "focus loss must not leave a reconstructed Command pressed remotely");

    AppleKeyboardMapper localSystemKeyMapper(false, 40);
    const QList<AppleRemoteKeyEvent> localSystemShortcut =
            localSystemKeyMapper.updateWithModifiers(
                    true, SDLK_c, SDL_SCANCODE_C,
                    KMOD_LGUI, KMOD_NONE, false);
    require(localSystemShortcut.size() == 1 &&
                    localSystemShortcut.at(0).symbol == 'c',
            "GUI state must not synthesize a remote Command while system-key capture is disabled");

    AppleKeyboardMapper nativeWinFallbackMapper(false, 40);
    const QList<AppleRemoteKeyEvent> nativeWinShortcut =
            nativeWinFallbackMapper.updateWithModifiers(
                    true, SDLK_c, SDL_SCANCODE_C,
                    KMOD_NUM, KMOD_LGUI, true);
    require(nativeWinShortcut.size() == 2 &&
                    nativeWinShortcut.at(0).isDown &&
                    nativeWinShortcut.at(0).symbol == 0xffeb &&
                    nativeWinShortcut.at(1).isDown &&
                    nativeWinShortcut.at(1).symbol == 'c',
            "native Left Win state must recover Command+C when SDL reports only NumLock");
    AppleKeyboardMapper nativeRightWinFallbackMapper(false, 40);
    const QList<AppleRemoteKeyEvent> nativeRightWinShortcut =
            nativeRightWinFallbackMapper.updateWithModifiers(
                    true, SDLK_v, SDL_SCANCODE_V,
                    KMOD_NUM, KMOD_RGUI, true);
    require(nativeRightWinShortcut.size() == 2 &&
                    nativeRightWinShortcut.at(0).isDown &&
                    nativeRightWinShortcut.at(0).symbol == 0xffec &&
                    nativeRightWinShortcut.at(0).keyCode == 54 &&
                    nativeRightWinShortcut.at(1).symbol == 'v',
            "native Right Win state must preserve right Command semantics");

    AppleKeyboardMapper semanticMapper(false, 40);
    require(!semanticMapper.update(
                    true, SDLK_LGUI, SDL_SCANCODE_LGUI, false).has_value(),
            "the local Win/Command key must stay local when system-key capture is disabled");

    const auto commandDown = semanticMapper.update(
            true, SDLK_LGUI, SDL_SCANCODE_LGUI, true);
    const auto cDown = semanticMapper.update(
            true, SDLK_c, SDL_SCANCODE_C, true);
    require(commandDown.has_value() && commandDown->isDown &&
                    commandDown->symbol == 0xffeb &&
                    commandDown->keyCode == 55 &&
                    commandDown->keyboardType == 40 &&
                    cDown.has_value() && cDown->symbol == 'c' &&
                    cDown->keyCode == 8,
            "semantic mapping must send left Win as native left Command and preserve the Apple C key code");

    const QList<AppleRemoteKeyEvent> focusReleases =
            semanticMapper.releaseAll();
    require(focusReleases.size() == 2 &&
                    !focusReleases.at(0).isDown &&
                    focusReleases.at(0).symbol == 'c' &&
                    focusReleases.at(1).symbol == 0xffeb &&
                    semanticMapper.pressedKeyCount() == 0 &&
                    !semanticMapper.update(
                            false, SDLK_LGUI, SDL_SCANCODE_LGUI,
                            false).has_value(),
            "focus loss must release ordinary keys before modifiers and suppress a later duplicate key-up");

    const auto optionDown = semanticMapper.update(
            true, SDLK_RALT, SDL_SCANCODE_RALT, true);
    const auto optionUp = semanticMapper.update(
            false, SDLK_RALT, SDL_SCANCODE_RALT, true);
    require(optionDown.has_value() && optionDown->symbol == 0xffea &&
                    optionDown->keyCode == 61 &&
                    optionUp.has_value() && !optionUp->isDown &&
                    optionUp->symbol == optionDown->symbol &&
                    optionUp->keyCode == optionDown->keyCode,
            "default mapping must preserve right Alt as native right Option across down and up");

    AppleKeyboardMapper positionalMapper(true);
    const auto altAsCommand = positionalMapper.update(
            true, SDLK_LALT, SDL_SCANCODE_LALT, true);
    const auto winAsOption = positionalMapper.update(
            true, SDLK_LGUI, SDL_SCANCODE_LGUI, true);
    require(altAsCommand.has_value() &&
                    altAsCommand->symbol == 0xffeb &&
                    altAsCommand->keyCode == 55 &&
                    winAsOption.has_value() &&
                    winAsOption->symbol == 0xffe9 &&
                    winAsOption->keyCode == 58,
            "the shared Alt/Win swap setting must swap both Apple modifier symbols and virtual key codes");

    AppleKeyboardMapper functionMapper(false);
    const auto f13 = functionMapper.update(
            true, SDLK_F13, SDL_SCANCODE_F13, true);
    const auto f24 = functionMapper.update(
            true, SDLK_F24, SDL_SCANCODE_F24, true);
    require(f13.has_value() && f13->symbol == 0xffca &&
                    f13->keyCode == 105 &&
                    f24.has_value() && f24->symbol == 0xffd5,
            "function-key mapping must follow X11 order across SDL's non-contiguous F12/F13 scancode boundary");
}

void testNativePrecisionScrollWireAndDeltas()
{
    AppleScrollWheelEvent event;
    event.deltaX = -1;
    event.deltaY = 2;
    event.deltaZ = -3;
    event.fixedDeltaX = 0x01020304;
    event.fixedDeltaY = -2;
    event.pointDeltaX = 0x11223344;
    event.pointDeltaY = -1;
    event.pointDeltaZ = 4;
    event.scrollPhase = 1;
    event.momentumPhase = 2;
    event.scrollCount = 3;
    event.flags = 0x12345678;
    require(AppleMediaWire::scrollWheelEvent(event, 0x9abc, 0xdef0) ==
                    QByteArray::fromHex(
                            "170000360001000bffff0002fffd"
                            "01020304fffffffe00000000"
                            "11223344ffffffff00000004"
                            "00000001000000020000000312345678"
                            "9abcdef0"),
            "native precision scrolling must match the Swift Apple wire vector");

    const AppleScrollWheelEvent mapped = AppleMediaWire::scrollWheelDeltas(
            -3, 5, -2.5, 4.25, false, 7, 1.0);
    require(mapped.deltaX == -3 && mapped.deltaY == 5 &&
            mapped.fixedDeltaX == -163840 && mapped.fixedDeltaY == 278528 &&
            mapped.pointDeltaX == -25 && mapped.pointDeltaY == 43 &&
            mapped.scrollCount == 7,
            "SDL precision scrolling must preserve both tick magnitude and fractional deltas");

    const AppleScrollWheelEvent flipped = AppleMediaWire::scrollWheelDeltas(
            -3, 5, -2.5, 4.25, true, 8, 1.0);
    require(flipped.deltaX == 3 && flipped.deltaY == -5 &&
            flipped.fixedDeltaX == 163840 && flipped.fixedDeltaY == -278528 &&
            flipped.pointDeltaX == 25 && flipped.pointDeltaY == -43,
            "SDL flipped scrolling must reverse every Apple delta representation");

    const AppleScrollWheelEvent accelerated = AppleMediaWire::scrollWheelDeltas(
            -4, 2, -1.5, 2.0, false, 9, 1.25);
    require(accelerated.deltaX == -5 && accelerated.deltaY == 3 &&
            accelerated.fixedDeltaX == -122880 && accelerated.fixedDeltaY == 163840 &&
            accelerated.pointDeltaX == -19 && accelerated.pointDeltaY == 25 &&
            accelerated.scrollCount == 9,
            "Apple scroll speed must scale every delta representation consistently");
}

#ifdef Q_OS_DARWIN
void testMacNativeScrollPreservesCgEventFields()
{
    CGEventRef cgEvent = CGEventCreateScrollWheelEvent(
            nullptr, kCGScrollEventUnitPixel, 3, 2, -1, -3);
    require(cgEvent != nullptr, "a synthetic macOS scroll event must be created");
    const auto releaseEvent = qScopeGuard([cgEvent]() { CFRelease(cgEvent); });
    CGEventSetIntegerValueField(
            cgEvent, kCGScrollWheelEventDeltaAxis1, 2);
    CGEventSetIntegerValueField(
            cgEvent, kCGScrollWheelEventDeltaAxis2, -1);
    CGEventSetIntegerValueField(
            cgEvent, kCGScrollWheelEventDeltaAxis3, -3);
    CGEventSetIntegerValueField(
            cgEvent, kCGScrollWheelEventFixedPtDeltaAxis1, -2);
    CGEventSetIntegerValueField(
            cgEvent, kCGScrollWheelEventFixedPtDeltaAxis2, 0x01020304);
    CGEventSetIntegerValueField(
            cgEvent, kCGScrollWheelEventFixedPtDeltaAxis3, 0);
    CGEventSetIntegerValueField(
            cgEvent, kCGScrollWheelEventPointDeltaAxis1, -1);
    CGEventSetIntegerValueField(
            cgEvent, kCGScrollWheelEventPointDeltaAxis2, 0x11223344);
    CGEventSetIntegerValueField(
            cgEvent, kCGScrollWheelEventPointDeltaAxis3, 4);
    CGEventSetIntegerValueField(
            cgEvent, kCGScrollWheelEventScrollPhase, 1);
    CGEventSetIntegerValueField(
            cgEvent, kCGScrollWheelEventMomentumPhase, 2);
    CGEventSetIntegerValueField(
            cgEvent, kCGScrollWheelEventScrollCount, 3);
    CGEventSetFlags(cgEvent, static_cast<CGEventFlags>(0x12345678));

    const AppleScrollWheelEvent event =
            appleMacScrollWheelEventFromCGEvent(cgEvent);
    require(AppleMediaWire::scrollWheelEvent(event, 0x9abc, 0xdef0) ==
                    QByteArray::fromHex(
                            "170000360001000bffff0002fffd"
                            "01020304fffffffe00000000"
                            "11223344ffffffff00000004"
                            "00000001000000020000000312345678"
                            "9abcdef0"),
            "macOS scrolling must preserve the complete Swift CGEvent wire fields");
}
#endif

void testAppleStreamWindowPlacementPersistence()
{
    QTemporaryDir directory;
    require(directory.isValid(),
            "temporary Apple window placement directory must be available");
    const QString path = directory.filePath(QStringLiteral("placement.ini"));

    AppleWindowPlacementStore store(path);
    require(!store.load(AppleWindowRole::Primary).has_value(),
            "Apple window placement must start empty");
    require(store.save(AppleWindowRole::Primary, QRect(2100, 120, 1400, 900)),
            "primary Apple window geometry must persist");
    require(store.save(AppleWindowRole::Secondary, QRect(100, 80, 960, 640)),
            "secondary Apple window geometry must persist independently");
    require(store.saveViewport(QStringLiteral("connection-a"), 0,
                               QSize(1280, 720)) &&
            store.saveViewport(QStringLiteral("connection-a"), 1,
                               QSize(900, 700)),
            "logical stream viewports must persist per connection and display");

    AppleWindowPlacementStore reopened(path);
    require(reopened.load(AppleWindowRole::Primary) ==
                    std::optional<QRect>(QRect(2100, 120, 1400, 900)) &&
            reopened.load(AppleWindowRole::Secondary) ==
                    std::optional<QRect>(QRect(100, 80, 960, 640)),
            "saved Apple window geometries must survive store recreation");
    require(reopened.loadViewport(QStringLiteral("connection-a"), 0) ==
                    std::optional<QSize>(QSize(1280, 720)) &&
            reopened.loadViewport(QStringLiteral("connection-a"), 1) ==
                    std::optional<QSize>(QSize(900, 700)) &&
            !reopened.loadViewport(QStringLiteral("connection-b"), 0)
                     .has_value(),
            "restored stream viewports must not leak across connections or displays");

    const QList<QRect> displays{
        QRect(0, 0, 1920, 1040),
        QRect(1920, 0, 2560, 1400),
    };
    require(AppleWindowPlacement::constrainToVisibleDisplays(
                    QRect(4000, 1200, 1600, 1000), displays) ==
                    QRect(2880, 400, 1600, 1000),
            "a partially visible saved Apple window must remain on its display");
    require(AppleWindowPlacement::constrainToVisibleDisplays(
                    QRect(7000, 3000, 1600, 900), displays) ==
                    QRect(320, 140, 1600, 900),
            "an off-screen saved Apple window must return to a visible display");
    require(AppleWindowPlacement::constrainToVisibleDisplays(
                    QRect(200, 100, 100, 80), displays) ==
                    QRect(200, 100, 320, 240),
            "restored Apple windows must retain a usable minimum size");
}

void testHevcDecoderBackendFallback()
{
    QString error;
    AppleHevcDecoder preferred(true);
    require(preferred.open(&error),
            "the preferred HEVC decoder must open with a native backend or software fallback");
#ifdef Q_OS_WIN
    require(preferred.backend() == AppleHevcDecoder::Backend::D3D11va ||
            (preferred.backend() == AppleHevcDecoder::Backend::Software &&
             preferred.hardwareFallbackOccurred()),
            "hardware decoder failure must be explicit when software fallback is selected");
#elif defined(Q_OS_DARWIN)
    require(preferred.backend() == AppleHevcDecoder::Backend::VideoToolbox ||
            (preferred.backend() == AppleHevcDecoder::Backend::Software &&
             preferred.hardwareFallbackOccurred()),
            "VideoToolbox failure must be explicit when software fallback is selected");
#endif
    const std::shared_ptr<AppleVideoBackendContext> preferredContext =
            preferred.presentationContext();
    require(preferredContext != nullptr &&
            preferredContext->backend == preferred.backend() &&
            (preferred.backend() != AppleHevcDecoder::Backend::D3D11va ||
             preferredContext->nativeDevice != nullptr),
            "the decoder must expose its platform-neutral presentation lifetime token");
    const char* preferredBackend =
            preferred.backend() == AppleHevcDecoder::Backend::D3D11va
                    ? "D3D11VA"
                    : preferred.backend() == AppleHevcDecoder::Backend::VideoToolbox
                            ? "VideoToolbox" : "software fallback";
    std::fprintf(stderr, "preferred HEVC decoder backend: %s\n",
                 preferredBackend);

    AppleHevcDecoder software(false);
    error.clear();
    require(software.open(&error) &&
            software.backend() == AppleHevcDecoder::Backend::Software &&
            !software.hardwareFallbackOccurred(),
            "the software HEVC decoder must remain independently usable");
    const std::shared_ptr<AppleVideoBackendContext> softwareContext =
            software.presentationContext();
    require(softwareContext != nullptr &&
            softwareContext->backend == AppleVideoDecoderBackend::Software &&
            softwareContext->nativeDevice == nullptr,
            "software decoding must not leak a native presentation device");
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

void testDecodedTilesDoNotRetainFramesBehindMissingDecoderOutput()
{
    AppleDecodedFrameBatcher batcher;
    auto accessUnit = [](quint16 sequence, int tileIndex) {
        AppleHevcAccessUnit unit;
        unit.synchronizationSource = static_cast<quint32>(tileIndex + 1);
        unit.frameSequenceNumber = sequence;
        unit.subframeBoundary = AppleHevcAccessUnit::SubframeBoundary::Last;
        return unit;
    };
    auto tile = [](int tileIndex, quint16 sequence) {
        AppleDecodedTile frame;
        frame.tileIndex = tileIndex;
        frame.frameSequenceNumber = sequence;
        return frame;
    };

    // FFmpeg can accept an access unit without returning either a frame or a
    // per-packet failure callback. Once a later sender frame is complete, the
    // missing output can no longer be allowed to retain every newer 4:4:4
    // surface behind it.
    batcher.recordSubmission(accessUnit(700, 0), 0);
    batcher.recordSubmission(accessUnit(701, 1), 1);
    batcher.recordDecodedFrames({tile(1, 701)});

    const QList<QList<AppleDecodedTile>> recovered = batcher.takeReadyBatches();
    require(recovered.size() == 1 && recovered.first().size() == 1 &&
                    recovered.first().first().frameSequenceNumber == 701,
            "a missing decoder output must not head-of-line block and retain later complete frames");
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

    require(media.configureRemotePorts(AppleMediaPorts{
            static_cast<quint16>(basePort + 2),
            static_cast<quint16>(basePort + 3)}),
            "a one-display transport must accept exactly one remote video port");
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

void testStageFourCursorAndDisplayLayoutEvents()
{
    // Cursor payload is four-byte BGRX pixels followed by one alpha byte per
    // pixel. qCompress produces a complete zlib stream, which exercises the
    // same decoder path as the host's sync-flushed stream.
    const QByteArray raw = QByteArray::fromHex("030201ff070605ff090a");
    const QByteArray compressed = qCompress(raw, 9).mid(4);
    QByteArray message = QByteArray::fromHex("00000003");
    message.append(cursorRectangle(1, 0, 2, 1, 42, compressed));
    message.append(cursorRectangle(0, 0, 0, 0, 42, {}));

    QByteArray layoutPayload(76, '\0');
    writeUInt16(layoutPayload, 0, 1);
    writeUInt16(layoutPayload, 2, 1440);
    writeUInt16(layoutPayload, 4, 900);
    writeUInt16(layoutPayload, 6, 2880);
    writeUInt16(layoutPayload, 8, 1800);
    writeUInt16(layoutPayload, 18, 1);
    writeUInt32(layoutPayload, 36, 77);
    writeUInt16(layoutPayload, 52, 1800);
    writeUInt16(layoutPayload, 54, 2880);
    QByteArray layoutRectangle(12, '\0');
    writeUInt32(layoutRectangle, 8, 0x451);
    AppleWire::appendUInt16(layoutRectangle,
                            static_cast<quint16>(layoutPayload.size()));
    layoutRectangle.append(layoutPayload);
    message.append(layoutRectangle);

    const AppleControlEvents events = AppleControlEventParser::parse(message);
    require(events.cursorUpdates.size() == 2,
            "cursor store and cache-selection updates must both be parsed");
    require(events.cursorUpdates.at(0).kind == AppleCursorUpdate::Kind::Store &&
            events.cursorUpdates.at(0).id == 42 &&
            events.cursorUpdates.at(0).image.hotspotX == 1 &&
            events.cursorUpdates.at(0).image.rgba ==
                    QByteArray::fromHex("010203090506070a"),
            "cursor BGR and alpha planes must become exact RGBA pixels");
    require(events.cursorUpdates.at(1).kind == AppleCursorUpdate::Kind::Select &&
            events.cursorUpdates.at(1).id == 42,
            "zero-length cursor updates must select a cached cursor");
    require(events.displayLayouts.size() == 1 &&
            events.displayLayouts.first().backingWidth == 2880 &&
            events.displayLayouts.first().displays.size() == 1 &&
            events.displayLayouts.first().displays.first().id == 77,
            "display-layout rectangles must preserve backing geometry and identity");

    QByteArray malformed = message.left(message.size() - 4);
    const AppleControlEvents prefix = AppleControlEventParser::parse(malformed);
    require(prefix.cursorUpdates.size() == 2,
            "a malformed trailing rectangle must not discard earlier cursor events");
}

void testRemoteCursorScalesForClientDpi()
{
    AppleCursorImage cursor;
    cursor.width = 2;
    cursor.height = 2;
    cursor.hotspotX = 1;
    cursor.hotspotY = 1;
    cursor.rgba = QByteArray::fromHex(
            "ff0000ff00ff00ff0000ffffffffffff");

    const AppleCursorImage scaled = cursor.scaledForDpi(2.0);
    require(scaled.isUsable() && scaled.width == 4 && scaled.height == 4 &&
            scaled.hotspotX == 2 && scaled.hotspotY == 2 &&
            scaled.rgba.size() == 4 * 4 * 4,
            "a custom remote cursor and its hotspot must follow the client window DPI");

    const AppleCursorImage unscaled = cursor.scaledForDpi(0.75);
    require(unscaled.width == cursor.width &&
            unscaled.height == cursor.height &&
            unscaled.hotspotX == cursor.hotspotX &&
            unscaled.hotspotY == cursor.hotspotY &&
            unscaled.rgba == cursor.rgba,
            "low client DPI must not shrink a remote cursor below its sender size");

    AppleCursorImage edge;
    edge.width = 2;
    edge.height = 1;
    edge.hotspotX = 0;
    edge.hotspotY = 0;
    edge.rgba = QByteArray::fromHex("ffffffff00000000");
    const AppleCursorImage scaledEdge = edge.scaledForDpi(2.0);
    bool foundFractionalAlpha = false;
    for (int pixel = 0; pixel < scaledEdge.width * scaledEdge.height; ++pixel) {
        const int offset = pixel * 4;
        const quint8 alpha = static_cast<quint8>(scaledEdge.rgba.at(offset + 3));
        if (alpha > 0 && alpha < 255) {
            foundFractionalAlpha = true;
            require(static_cast<quint8>(scaledEdge.rgba.at(offset)) >= 250 &&
                    static_cast<quint8>(scaledEdge.rgba.at(offset + 1)) >= 250 &&
                    static_cast<quint8>(scaledEdge.rgba.at(offset + 2)) >= 250,
                    "cursor scaling must not blend transparent black into visible edges");
        }
    }
    require(foundFractionalAlpha,
            "smooth cursor scaling fixture must exercise a fractional-alpha edge");
}

void testRemoteCursorCacheMatchesSwiftFallbacks()
{
    auto image = [](quint8 value) {
        AppleCursorImage cursor;
        cursor.width = 1;
        cursor.height = 1;
        cursor.rgba = QByteArray(4, Qt::Uninitialized);
        cursor.rgba[0] = static_cast<char>(value);
        cursor.rgba[1] = static_cast<char>(value);
        cursor.rgba[2] = static_cast<char>(value);
        cursor.rgba[3] = static_cast<char>(255);
        return cursor;
    };

    AppleCursorStore store;
    const AppleCursorImage first = image(7);
    require(store.apply({AppleCursorUpdate::Kind::Store, 42, first})
                    .value_or(AppleCursorImage()).rgba == first.rgba,
            "a stored cursor must become selected immediately");
    require(!store.apply({AppleCursorUpdate::Kind::Select, 99, {}}).has_value(),
            "an unknown cursor selection must fall back instead of retaining a stale shape");
    require(store.apply({AppleCursorUpdate::Kind::Select, 42, {}})
                    .value_or(AppleCursorImage()).rgba == first.rgba,
            "a cached cursor selection must restore its exact source image");

    store.clear();
    for (int id = 0; id <= AppleCursorStore::MaximumEntries; ++id) {
        store.apply({AppleCursorUpdate::Kind::Store,
                     static_cast<quint32>(id), image(static_cast<quint8>(id))});
    }
    require(!store.apply({AppleCursorUpdate::Kind::Select, 0, {}}).has_value(),
            "selecting an evicted cursor must use the same fallback as Swift");
    require(store.apply({AppleCursorUpdate::Kind::Select,
                         AppleCursorStore::MaximumEntries, {}})
                    .value_or(AppleCursorImage()).rgba ==
                    image(AppleCursorStore::MaximumEntries).rgba,
            "the newest cached cursor must remain selectable");
}

void testStageFourTextOnlyClipboardExchange()
{
    AppleTextClipboardExchange exchange;
    const QList<QByteArray> enable = exchange.setEligible(true);
    require(enable == QList<QByteArray>{QByteArray::fromHex("1500000100000000")},
            "controlling mode must explicitly enable the shared pasteboard");

    QString error;
    const QList<QByteArray> encoded = AppleTextClipboardExchange::encodeText(
            QStringLiteral("Hello, 世界 👋"), false, 0, &error);
    require(error.isEmpty() && !encoded.isEmpty(),
            "Unicode clipboard text must encode without loss");
    QByteArray complete;
    for (const QByteArray& fragment : encoded) complete.append(fragment);
    require(complete.size() >= 20 && complete.left(1) == QByteArray::fromHex("1f") &&
            complete.right(4) == QByteArray::fromHex("0000ffff"),
            "clipboard payloads must use the native zlib sync-flush framing");

    QByteArray changed = QByteArray::fromHex("1400000000000002");
    AppleTextClipboardResult result = exchange.receive(changed, &error);
    require(result.consumed && result.outboundMessages.size() == 1 &&
            result.outboundMessages.first() ==
                    AppleTextClipboardExchange::request(true, 1),
            "remote clipboard change must begin the native promise exchange");

    const QList<QByteArray> promises = AppleTextClipboardExchange::encodeText(
            QStringLiteral("Hello, 世界 👋"), true, 1, &error);
    for (const QByteArray& fragment : promises) {
        result = exchange.receive(fragment, &error);
    }
    require(result.outboundMessages ==
                    QList<QByteArray>{AppleTextClipboardExchange::request(false, 0)},
            "a matching promise must be resolved with native automatic session zero");

    for (const QByteArray& fragment : encoded) {
        result = exchange.receive(fragment, &error);
    }
    require(result.receivedText == std::optional<QString>(
                    QStringLiteral("Hello, 世界 👋")),
            "only the exact UTF-8 plain-text flavor must cross the clipboard seam");

    const QList<QByteArray> advertised = exchange.advertiseLocalText(
            QStringLiteral("local text"), &error);
    require(!advertised.isEmpty(),
            "eligible local text changes must advertise a pasteboard promise");
    result = exchange.receive(
            AppleTextClipboardExchange::request(false, 0x01020304), &error);
    require(result.consumed && !result.outboundMessages.isEmpty(),
            "a host request must resolve the most recently advertised local text");

    const QList<QByteArray> disable = exchange.setEligible(false);
    require(disable == QList<QByteArray>{QByteArray::fromHex("1500000200000000")},
            "observing mode must explicitly disable shared-pasteboard exchange");
    result = exchange.receive(changed, &error);
    require(result.consumed && result.outboundMessages.isEmpty(),
            "remote clipboard changes must not fetch while observing");
}

void testAppleFileTransferNativeWireContract()
{
    QString error;
    const QByteArray drop = AppleFileTransferProtocol::beginDrop(
            {QUrl(QStringLiteral("file:///tmp/type32-oracle-file.txt"))},
            0x1234,
            &error);
    require(error.isEmpty(), qPrintable(error));
    require(drop == QByteArray::fromHex(
                    "20000000000012340000006100000056"
                    "78da62606060646060e02f284dcac94cd64bcbcc49d52d2dca6180009094407"
                    "27eae5e6241414eaa5e7e714965412a508c250da24609a4de4a5f5fbf24b7401"
                    "f24676ca49b5f94980c340424a357525102000000ffff"),
            "type 32 file-drag archive differs from the native vector");
    require(AppleFileTransferProtocol::cancelDrop(0x01020304) ==
                    QByteArray::fromHex(
                            "20000000010203040000000000000000"),
            "type 32 cancel differs from the native vector");

    AppleFileTransferRequest request;
    require(AppleFileTransferProtocol::parseFileRequest(
                    QByteArray::fromHex(
                            "1e00000000001234000000042f746d70"),
                    &request,
                    &error) &&
                    request.sessionId == 0x1234 &&
                    request.destinationPath == QStringLiteral("/tmp"),
            "type 30 destination request did not parse");
    require(AppleFileTransferProtocol::startFileReceive(
                    0x1234, QStringLiteral("/tmp")) ==
                    QByteArray::fromHex(
                            "2200000000170001000200001234000000000000000000042f746d7000"),
            "start-receive differs from the native vector");
    require(AppleFileTransferProtocol::startFileSend(
                    0x1234, QStringLiteral("/tmp")) ==
                    QByteArray::fromHex(
                            "2200000000170001000100001234000000010000000000042f746d7000"),
            "start-send differs from the native vector");
    require(AppleFileTransferProtocol::control(
                    0x1234, AppleFileTransferControl::Pause) ==
                    QByteArray::fromHex("2200000000080001000300001234") &&
                    AppleFileTransferProtocol::control(
                            0x1234, AppleFileTransferControl::Resume) ==
                    QByteArray::fromHex("2200000000080001000400001234") &&
                    AppleFileTransferProtocol::control(
                            0x1234, AppleFileTransferControl::Stop) ==
                    QByteArray::fromHex("2200000000080001000500001234"),
            "native file-copy control vectors changed");
    require(AppleFileTransferProtocol::completion(
                    0x1234, 0, QStringLiteral("b.bin")) ==
                    QByteArray::fromHex(
                            "220000000012000100c80000123400000005622e62696e00") &&
                    AppleFileTransferProtocol::progress(0x1234, 0.5) ==
                    QByteArray::fromHex(
                            "2200000000100001012c000012343fe0000000000000"),
            "native file-copy response vectors changed");

    AppleFileCopySummary summary;
    summary.rootIsFile = true;
    summary.logicalBytes = 1;
    summary.physicalBytes = 4096;
    summary.fileCount = 1;
    require(AppleFileTransferProtocol::senderSummary(0x1234, summary) ==
                    QByteArray::fromHex(
                            "22000000003c000100640000123400000001000000000000000000000001000000000000100000000000000000010000000000000000000000000000000000000000"),
            "native sender summary vector changed");

    AppleFileCopyItemMetadata item;
    item.dataForkSize = 1;
    item.mode = 0100644;
    item.textEncodingHint = 0x7e;
    item.name = QStringLiteral("a");
    require(AppleFileTransferProtocol::senderItem(0x1234, item) ==
                    QByteArray::fromHex(
                            "22000000007200010065000012340100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001000000000000000000000000000000000000000000000000000000000000000000000000000000000000000081a40000007e000100006100"),
            "native sender item vector changed");
    require(AppleFileTransferProtocol::senderData(
                    0x1234, QByteArrayLiteral("X")) ==
                    QByteArray::fromHex(
                            "22000000000d00010066000012340000000158") &&
                    AppleFileTransferProtocol::senderDone(0x1234, -5000) ==
                    QByteArray::fromHex(
                            "22000000000c000100680000123478ecffff"),
            "native sender data/done vectors changed");

    AppleFileTransferResponse response;
    require(AppleFileTransferProtocol::parseResponse(
                    AppleFileTransferProtocol::progress(0x1234, 0.5),
                    &response,
                    &error) &&
                    response.kind == AppleFileTransferResponse::Kind::Progress &&
                    response.sessionId == 0x1234 && response.fraction == 0.5,
            "native progress response did not parse");

    item.finderInfo[0] = char(0x54);
    item.finderInfo[24] = char(0x12);
    item.finderInfo[25] = char(0x34);
    item.resourceForkSize = 3;
    item.dataForkSize = 4;
    item.creationDate = {10, 11};
    item.contentModificationDate = {12, 13};
    item.attributeModificationDate = {14, 15};
    item.accessDate = {16, 17};
    item.backupDate = {18, 19};
    item.nodeFlags = 1;
    item.level = 2;
    item.mode = 0100640;
    item.name = QStringLiteral("file.bin");
    item.extendedAttributes = {
        {QStringLiteral("a"), QByteArray::fromHex("0102")},
        {QStringLiteral("bc"), QByteArray::fromHex("03")},
    };
    AppleFileCopyReceiverFrame frame;
    require(AppleFileTransferProtocol::parseReceiverFrame(
                    AppleFileTransferProtocol::senderItem(0x1234, item),
                    &frame,
                    &error),
            qPrintable(error));
    require(frame.kind == AppleFileCopyReceiverFrame::Kind::Item &&
                    frame.sessionId == 0x1234 &&
                    frame.item.name == item.name &&
                    frame.item.level == 2 &&
                    frame.item.dataForkSize == 4 &&
                    frame.item.resourceForkSize == 3 &&
                    frame.item.creationDate == item.creationDate &&
                    frame.item.extendedAttributes == item.extendedAttributes &&
                    static_cast<quint8>(frame.item.finderInfo.at(0)) == 0x54 &&
                    frame.item.finderInfo.at(24) == 0 &&
                    frame.item.finderInfo.at(25) == 0,
            "file item metadata did not round-trip through the receiver parser");

    const QByteArray largeMessage = AppleFileTransferProtocol::senderData(
            0x1234, QByteArray(AppleFileTransferProtocol::MaximumDataBlockLength,
                              'X'));
    const QList<QByteArray> fragments =
            AppleFileTransferProtocol::fragments(largeMessage);
    require(fragments.size() == 2 && fragments.at(0).size() == 60000 &&
                    fragments.at(1).size() == largeMessage.size() - 60000,
            "encrypted-record fragmentation exceeded the native limit");
    AppleFileTransferReassembler reassembler;
    std::optional<QByteArray> reassembled;
    require(reassembler.receive(fragments.at(0), &reassembled, &error) &&
                    !reassembled.has_value() &&
                    reassembler.receive(fragments.at(1), &reassembled, &error) &&
                    reassembled == largeMessage,
            "file-copy encrypted records were not reassembled");

    std::optional<AppleRemoteFileDrag> remoteDrag;
    require(AppleFileTransferProtocol::parseRemoteDrag(
                    drop, &remoteDrag, &error) && remoteDrag.has_value() &&
                    remoteDrag->sessionId == 0x1234 &&
                    remoteDrag->sourcePaths ==
                            QStringList{QStringLiteral("/tmp/type32-oracle-file.txt")},
            "remote file-drag archive did not round-trip");
    require(!AppleFileTransferProtocol::progress(
                    0x1234, std::numeric_limits<double>::quiet_NaN()).size() &&
                    AppleFileTransferProtocol::startFileSend(
                            0, QStringLiteral("/tmp")).isEmpty(),
            "invalid file-transfer inputs were accepted");
}

void testAppleFileCopyDirectoryRoundTrip()
{
    QTemporaryDir temporary;
    require(temporary.isValid(), "file-copy temporary directory was unavailable");
    const QString source = QDir(temporary.path()).filePath(
            QStringLiteral("source"));
    require(QDir().mkpath(QDir(source).filePath(QStringLiteral("nested"))),
            "file-copy source tree could not be created");
    const auto writeFile = [](const QString& path, const QByteArray& data) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                file.write(data) == data.size();
    };
    const QByteArray compressible(180000, 'A');
    const QByteArray alreadyCompressed = QByteArray::fromHex(
            "504b0304140000000800a55a245b00000000000000000000000000000000");
    require(writeFile(QDir(source).filePath(QStringLiteral("large.txt")),
                      compressible) &&
                    writeFile(QDir(source).filePath(
                                      QStringLiteral("nested/archive.zip")),
                              alreadyCompressed) &&
                    writeFile(QDir(source).filePath(
                                      QStringLiteral("nested/empty.bin")),
                              {}),
            "file-copy source files could not be created");

    QList<QByteArray> messages;
    quint64 emittedBytes = 0;
    std::atomic_bool cancelled{false};
    QString error;
    int metricsReadCount = 0;
    AppleFileCopySender sender([&metricsReadCount]() {
        return metricsReadCount >= 2 ? quint32(9) : quint32(0);
    });
    require(sender.run(
                    source,
                    0x2345,
                    [&messages, &emittedBytes](const QByteArray& message,
                                               quint64 bytes,
                                               QString*) {
                        messages.append(message);
                        emittedBytes += bytes;
                        return true;
                    },
                    [&metricsReadCount]() {
                        ++metricsReadCount;
                        AppleFileCopySender::OutputMetrics metrics;
                        if (metricsReadCount >= 2) {
                            metrics.pendingBytes = 10 * 1024 * 1024;
                            metrics.totalBytesEnqueued = 1;
                        }
                        return metrics;
                    },
                    &cancelled,
                    &error),
            qPrintable(error));
    require(!messages.isEmpty() && emittedBytes ==
                    static_cast<quint64>(compressible.size() +
                                         alreadyCompressed.size()),
            "file-copy sender reported an incorrect payload size");
    bool sawCompressed = false;
    bool sawRaw = false;
    for (const QByteArray& message : std::as_const(messages)) {
        AppleFileCopyReceiverFrame frame;
        require(AppleFileTransferProtocol::parseReceiverFrame(
                        message, &frame, &error),
                qPrintable(error));
        sawCompressed = sawCompressed ||
                frame.kind == AppleFileCopyReceiverFrame::Kind::CompressedData;
        sawRaw = sawRaw || frame.kind == AppleFileCopyReceiverFrame::Kind::Data;
    }
    require(sawCompressed && sawRaw,
            "file-copy compression policy did not preserve raw archive data");

    const QString destination = QDir(temporary.path()).filePath(
            QStringLiteral("destination"));
    AppleFileCopyReceiver receiver(
            destination, QStringLiteral("received"), &error);
    require(receiver.isValid(), qPrintable(error));
    QString completedPath;
    double progress = 0.0;
    for (const QByteArray& message : std::as_const(messages)) {
        AppleFileCopyReceiverFrame frame;
        require(AppleFileTransferProtocol::parseReceiverFrame(
                        message, &frame, &error),
                qPrintable(error));
        AppleFileCopyReceiver::Update update;
        require(receiver.receive(frame, &update, &error), qPrintable(error));
        if (update.progress.has_value()) progress = *update.progress;
        if (!update.completedPath.isEmpty()) completedPath = update.completedPath;
    }
    require(progress == 1.0 &&
                    completedPath == QDir(destination).filePath(
                            QStringLiteral("received")),
            "file-copy receiver did not complete at the requested root");
    QFile receivedText(QDir(completedPath).filePath(QStringLiteral("large.txt")));
    QFile receivedArchive(QDir(completedPath).filePath(
            QStringLiteral("nested/archive.zip")));
    require(receivedText.open(QIODevice::ReadOnly) &&
                    receivedText.readAll() == compressible &&
                    receivedArchive.open(QIODevice::ReadOnly) &&
                    receivedArchive.readAll() == alreadyCompressed &&
                    QFileInfo(QDir(completedPath).filePath(
                                      QStringLiteral("nested/empty.bin"))).size() == 0,
            "file-copy directory contents changed during round-trip");

    AppleFileCopyReceiver duplicate(
            destination, QStringLiteral("received"), &error);
    require(duplicate.isValid(), qPrintable(error));
    QString duplicatePath;
    for (const QByteArray& message : std::as_const(messages)) {
        AppleFileCopyReceiverFrame frame;
        AppleFileCopyReceiver::Update update;
        require(AppleFileTransferProtocol::parseReceiverFrame(
                        message, &frame, &error) &&
                        duplicate.receive(frame, &update, &error),
                qPrintable(error));
        if (!update.completedPath.isEmpty()) duplicatePath = update.completedPath;
    }
    require(QFileInfo(duplicatePath).fileName() == QStringLiteral("received 2"),
            "file-copy receiver overwrote an existing destination");
}

void testAppleFileCopySenderBeginsWithNativeUncompressedData()
{
    QTemporaryDir temporary;
    require(temporary.isValid(),
            "file-copy native sender temporary directory failed");
    const QString source = temporary.filePath(QStringLiteral("payload.dmg"));
    QFile sourceFile(source);
    constexpr int payloadSize = 128 * 1024 + 1;
    require(sourceFile.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                    sourceFile.write(QByteArray(payloadSize, 'A')) == payloadSize,
            "file-copy native sender fixture could not be created");
    sourceFile.close();

    QList<QByteArray> messages;
    std::atomic_bool cancelled{false};
    QString error;
    AppleFileCopySender sender;
    require(sender.run(
                    source,
                    0x3456,
                    [&messages](const QByteArray& message,
                                quint64,
                                QString*) {
                        messages.append(message);
                        return true;
                    },
                    {},
                    &cancelled,
                    &error),
            qPrintable(error));

    require(messages.size() >= 4,
            "file-copy sender omitted native file frames");
    AppleFileCopyReceiverFrame summary;
    require(AppleFileTransferProtocol::parseReceiverFrame(
                    messages.first(), &summary, &error),
            qPrintable(error));
    require(summary.kind == AppleFileCopyReceiverFrame::Kind::Summary &&
                    summary.summary.logicalBytes == payloadSize &&
                    summary.summary.physicalBytes >
                            summary.summary.logicalBytes,
            "file-copy sender did not report the native allocated file size");
    AppleFileCopyReceiverFrame firstPayload;
    AppleFileCopyReceiverFrame metadata;
    require(AppleFileTransferProtocol::parseReceiverFrame(
                    messages.at(1), &metadata, &error),
            qPrintable(error));
    require(metadata.kind == AppleFileCopyReceiverFrame::Kind::Item &&
                    metadata.item.type == AppleFileCopyItemType::File &&
                    metadata.item.mode == 0100644 &&
                    metadata.item.nodeFlags == 0 &&
                    metadata.item.textEncodingHint == 0x7e,
            qPrintable(QStringLiteral(
                    "Windows file-copy metadata diverged from the native "
                    "regular-file contract (mode=%1 flags=%2 encoding=%3)")
                               .arg(metadata.item.mode, 0, 8)
                               .arg(metadata.item.nodeFlags)
                               .arg(metadata.item.textEncodingHint)));
    require(AppleFileTransferProtocol::parseReceiverFrame(
                    messages.at(2), &firstPayload, &error),
            qPrintable(error));
    require(firstPayload.kind == AppleFileCopyReceiverFrame::Kind::Data,
            "file-copy sender compressed its first payload before native backlog negotiation");
}

void testAppleFileTransferKeepsLogicalMessageFragmentsAtomic()
{
    QTemporaryDir temporary;
    require(temporary.isValid(),
            "file-transfer atomic-fragment temporary directory failed");
    const QString source = QDir(temporary.path()).filePath(
            QStringLiteral("payload.zip"));
    QFile sourceFile(source);
    const QByteArray contents(140000, 'R');
    require(sourceFile.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                    sourceFile.write(contents) == contents.size(),
            "file-transfer atomic-fragment source could not be created");
    sourceFile.close();

    AppleFileTransferService service;
    service.setAvailable(true);
    QList<QByteArray> dropMessages;
    QString error;
    require(service.beginLocalDrop({source}, &dropMessages, &error) &&
                    dropMessages.size() == 1,
            qPrintable(error));
    bool ok = false;
    const quint32 dropSessionId = AppleWire::readUInt32(
            dropMessages.first(), 4, &ok);
    require(ok && dropSessionId != 0,
            "local file drop omitted its session ID");

    const QByteArray destination = QByteArrayLiteral("/Users/test/Desktop");
    QByteArray request(4, '\0');
    request[0] = static_cast<char>(0x1e);
    AppleWire::appendUInt32(request, dropSessionId);
    AppleWire::appendUInt32(
            request, static_cast<quint32>(destination.size()));
    request.append(destination);
    require(service.receive(request, &error), qPrintable(error));

    QElapsedTimer timeout;
    timeout.start();
    bool sawFragmentedMessage = false;
    while (!sawFragmentedMessage && timeout.elapsed() < 5000) {
        const QList<QByteArray> batch = service.takeOutbound(1);
        if (batch.isEmpty()) {
            QThread::yieldCurrentThread();
            continue;
        }
        const QByteArray& first = batch.first();
        if (first.size() < 6 || static_cast<quint8>(first.at(0)) != 0x22) {
            continue;
        }
        const quint32 bodyLength = AppleWire::readUInt32(first, 2, &ok);
        require(ok, "queued file-copy message had a truncated length");
        const int logicalLength = static_cast<int>(bodyLength) + 6;
        if (logicalLength <= first.size()) continue;

        QByteArray reassembled;
        for (const QByteArray& fragment : batch) {
            reassembled.append(fragment);
        }
        require(reassembled.size() == logicalLength,
                "one logical file-copy message was exposed as separate queue batches");
        sawFragmentedMessage = true;
    }
    require(sawFragmentedMessage,
            "file-transfer sender did not produce a fragmented data message");
    service.close();
}

void testAppleFileTransferProgressNeverMovesBackward()
{
    QTemporaryDir temporary;
    require(temporary.isValid(),
            "file-transfer progress temporary directory failed");
    const QString source = QDir(temporary.path()).filePath(
            QStringLiteral("progress.bin"));
    QFile sourceFile(source);
    const QByteArray contents(4 * 1024 * 1024, 'P');
    require(sourceFile.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                    sourceFile.write(contents) == contents.size(),
            "file-transfer progress source could not be created");
    sourceFile.close();

    AppleFileTransferService service;
    service.setAvailable(true);
    QList<QByteArray> dropMessages;
    QString error;
    require(service.beginLocalDrop({source}, &dropMessages, &error) &&
                    dropMessages.size() == 1,
            qPrintable(error));
    bool ok = false;
    const quint32 dropSessionId = AppleWire::readUInt32(
            dropMessages.first(), 4, &ok);
    const QByteArray destination = QByteArrayLiteral("/Users/test/Desktop");
    QByteArray request(4, '\0');
    request[0] = static_cast<char>(0x1e);
    AppleWire::appendUInt32(request, dropSessionId);
    AppleWire::appendUInt32(
            request, static_cast<quint32>(destination.size()));
    request.append(destination);
    require(ok && service.receive(request, &error), qPrintable(error));

    quint32 transferSessionId = 0;
    double highestProgress = 0.0;
    QElapsedTimer timeout;
    timeout.start();
    while ((transferSessionId == 0 || highestProgress < 0.25) &&
           timeout.elapsed() < 5000) {
        service.takeOutbound(16);
        for (const AppleFileTransferEvent& event : service.takeEvents()) {
            if (event.direction != AppleFileTransferEvent::Direction::ToRemote) {
                continue;
            }
            if (event.kind == AppleFileTransferEvent::Kind::Started) {
                transferSessionId = event.sessionId;
            }
            if (event.kind == AppleFileTransferEvent::Kind::Progress) {
                highestProgress = qMax(highestProgress, event.progress);
            }
        }
        QThread::yieldCurrentThread();
    }
    require(transferSessionId != 0 && highestProgress >= 0.25,
            "file-transfer sender did not publish measurable progress");

    const double staleRemoteProgress = highestProgress / 4.0;
    require(service.receive(AppleFileTransferProtocol::progress(
                                    transferSessionId, staleRemoteProgress),
                            &error),
            qPrintable(error));
    double previous = highestProgress;
    for (const AppleFileTransferEvent& event : service.takeEvents()) {
        if (event.sessionId != transferSessionId ||
                event.kind != AppleFileTransferEvent::Kind::Progress) {
            continue;
        }
        require(event.progress >= previous,
                "outgoing file-transfer progress moved backward");
        previous = event.progress;
    }

    bool senderFinished = false;
    timeout.restart();
    while (!senderFinished && timeout.elapsed() < 5000) {
        service.takeOutbound(32);
        for (const AppleFileTransferEvent& event : service.takeEvents()) {
            senderFinished = senderFinished ||
                    (event.sessionId == transferSessionId &&
                     event.kind == AppleFileTransferEvent::Kind::Completing);
        }
        QThread::yieldCurrentThread();
    }
    require(senderFinished,
            "file-transfer sender did not reach its remote-completion boundary");
    require(service.receive(AppleFileTransferProtocol::completion(
                                    transferSessionId, 5,
                                    QStringLiteral("progress.bin")),
                            &error),
            qPrintable(error));
    bool sawRemoteFailure = false;
    for (const AppleFileTransferEvent& event : service.takeEvents()) {
        if (event.sessionId == transferSessionId &&
                event.kind == AppleFileTransferEvent::Kind::Failed) {
            sawRemoteFailure = event.errorText.contains(
                    QStringLiteral("error code 5"));
        }
    }
    require(sawRemoteFailure,
            "the remote completion error was hidden behind a generic failure");
    service.close();
}

void testAppleFileTransferStopsImmediatelyAfterRemoteRejection()
{
    QTemporaryDir temporary;
    require(temporary.isValid(),
            "file-transfer rejection temporary directory failed");
    const QString source = temporary.filePath(QStringLiteral("rejected.dmg"));
    QFile sourceFile(source);
    const QByteArray chunk(1024 * 1024, 'R');
    require(sourceFile.open(QIODevice::WriteOnly | QIODevice::NewOnly),
            "file-transfer rejection source could not be created");
    for (int index = 0; index < 32; ++index) {
        require(sourceFile.write(chunk) == chunk.size(),
                "file-transfer rejection source was truncated");
    }
    sourceFile.close();

    AppleFileTransferService service;
    service.setAvailable(true);
    QList<QByteArray> dropMessages;
    QString error;
    require(service.beginLocalDrop({source}, &dropMessages, &error),
            qPrintable(error));
    bool ok = false;
    const quint32 dropSessionId = AppleWire::readUInt32(
            dropMessages.first(), 4, &ok);
    const QByteArray destination = QByteArrayLiteral("/Volumes/ExternalSSD");
    QByteArray request(4, '\0');
    request[0] = static_cast<char>(0x1e);
    AppleWire::appendUInt32(request, dropSessionId);
    AppleWire::appendUInt32(
            request, static_cast<quint32>(destination.size()));
    request.append(destination);
    require(ok && service.receive(request, &error), qPrintable(error));

    const QList<QByteArray> start = service.takeOutbound(1);
    require(start.size() == 1,
            "file-transfer rejection test did not consume the start command");
    quint32 transferSessionId = 0;
    bool producerHasBacklog = false;
    QElapsedTimer timeout;
    timeout.start();
    while ((!producerHasBacklog || transferSessionId == 0) &&
           timeout.elapsed() < 5000) {
        for (const AppleFileTransferEvent& event : service.takeEvents()) {
            if (event.direction != AppleFileTransferEvent::Direction::ToRemote) {
                continue;
            }
            if (event.kind == AppleFileTransferEvent::Kind::Started) {
                transferSessionId = event.sessionId;
            }
            producerHasBacklog = producerHasBacklog ||
                    (event.kind == AppleFileTransferEvent::Kind::Progress &&
                     event.progress >= 0.25);
        }
        QThread::yieldCurrentThread();
    }
    require(transferSessionId != 0 && producerHasBacklog,
            "file-transfer rejection test could not create producer backlog");

    require(service.receive(AppleFileTransferProtocol::completion(
                                    transferSessionId,
                                    5,
                                    QStringLiteral("rejected 2.dmg")),
                            &error),
            qPrintable(error));
    bool failedImmediately = false;
    bool explainedExternalVolumeFailure = false;
    for (const AppleFileTransferEvent& event : service.takeEvents()) {
        if (event.sessionId == transferSessionId &&
                event.kind == AppleFileTransferEvent::Kind::Failed) {
            failedImmediately = true;
            explainedExternalVolumeFailure =
                    event.errorText.contains(QStringLiteral("external volume")) &&
                    event.errorText.contains(QStringLiteral("internal disk")) &&
                    event.errorText.contains(QStringLiteral("error code 5"));
        }
    }
    require(failedImmediately,
            "an early Mac rejection allowed the producer to continue to 100 percent");
    require(explainedExternalVolumeFailure,
            "an external-volume rejection did not explain the macOS workaround");
    require(service.takeOutbound(1).isEmpty(),
            "queued file payload survived an early Mac rejection");
    service.close();
}

void testAppleRemoteFileDragWaitsUntilPointerLeavesStream()
{
    AppleRemoteFileDrag drag;
    drag.sessionId = 0x3300;
    drag.sourcePaths = {QStringLiteral("/Users/test/report.pdf")};

    AppleRemoteFileDragGate gate;
    gate.update(drag);
    require(!gate.takeIfEligible(true, true).has_value(),
            "remote file drag activated while the pointer was still in the stream");
    require(gate.hasPending(),
            "remote file drag was discarded before it could leave the stream");
    require(!gate.takeIfEligible(false, false).has_value(),
            "remote file drag activated without the left button held");
    const std::optional<AppleRemoteFileDrag> activated =
            gate.takeIfEligible(true, false);
    require(activated.has_value() && activated->sessionId == drag.sessionId,
            "remote file drag did not activate after leaving the stream");
    require(!gate.hasPending(),
            "activated remote file drag remained pending");

    // Native type-32 session IDs are informational and may be reused. Swift
    // replaces its published drag on every non-empty notification, so the
    // Windows gate must not permanently suppress a later drag with the same
    // ID after the preceding native drag session has ended.
    gate.update(drag);
    require(gate.hasPending(),
            "a later remote drag reusing its session ID was suppressed");
    const std::optional<AppleRemoteFileDrag> repeated =
            gate.takeIfEligible(true, false);
    require(repeated.has_value() && repeated->sessionId == drag.sessionId,
            "a later remote drag with the same session ID could not activate");
}

void testAppleRemoteFileDragEndDoesNotDropAgain()
{
    AppleRemoteFileDragInputState input;
    AppleRemoteFileDragInputTransition transition = input.nativeDragBegan(7);
    require(transition.buttons == 7 && !transition.forwardToRemote,
            "starting a local promised-file drag released the remote file "
            "before the native drag finished");

    transition = input.nativeDragEnded(transition.buttons);
    require(transition.buttons == 0 && !transition.forwardToRemote,
            "ending a local promised-file drag completed a duplicate remote drop");

    transition = input.localLeftButtonChanged(false, transition.buttons);
    require(transition.buttons == 0 && !transition.forwardToRemote,
            "the stale SDL mouse-up escaped native drag ownership");

    transition = input.localLeftButtonChanged(true, transition.buttons);
    require(transition.buttons == 1 && transition.forwardToRemote,
            "a new physical drag remained suppressed after the prior drag ended");
    transition = input.localLeftButtonChanged(false, transition.buttons);
    require(transition.buttons == 0 && transition.forwardToRemote,
            "a new physical drag did not regain normal remote input ownership");

    transition = input.nativeDragBegan(1);
    transition = input.nativeDragStartFailed(transition.buttons);
    require(transition.buttons == 1 && !transition.forwardToRemote,
            "a native drag startup failure lost the held remote button state");
}

void testAppleFileTransferCancelsOnlyTheActiveRemoteDragOnce()
{
    AppleFileTransferService service;
    service.setAvailable(true);
    QString error;
    const QByteArray remoteDrag = AppleFileTransferProtocol::beginDrop(
            {QUrl(QStringLiteral("file:///Users/test/remote-file.txt"))},
            0x1234,
            &error);
    require(!remoteDrag.isEmpty() && service.receive(remoteDrag, &error),
            qPrintable(error));

    QList<QByteArray> messages;
    require(!service.cancelRemoteDrag(0x4321, &messages) &&
                    messages.isEmpty(),
            "a stale native drag cancelled the current remote session");
    require(service.cancelRemoteDrag(0x1234, &messages) &&
                    messages == QList<QByteArray>{
                            AppleFileTransferProtocol::cancelDrop(0x1234)},
            "the active remote drag did not send its native cancellation");
    require(!service.cancelRemoteDrag(0x1234, &messages) &&
                    messages.size() == 1,
            "the same remote drag was cancelled more than once");
    service.close();
}

#ifdef Q_OS_WIN
void testAppleWindowsPromisedFileExposesDescriptorAndContents()
{
    QTemporaryDir temporary;
    require(temporary.isValid(),
            "promised-file data-object temporary directory failed");
    QByteArray expected("remote promised file contents");
    expected.append('\0');
    expected.append("with binary");
    const QString stagedPath = temporary.filePath(
            QStringLiteral("unrelated-staging-name.tmp"));
    QFile stagedFile(stagedPath);
    require(stagedFile.open(QIODevice::WriteOnly),
            "promised-file test payload could not be created");
    require(stagedFile.write(expected) == expected.size(),
            "promised-file test payload was truncated");
    stagedFile.close();

    QString descriptorName;
    QByteArray actual;
    QString error;
    require(testAppleWindowsPromisedFileDataObject(
                    QStringLiteral("/Users/test/Report.pkg"),
                    stagedPath,
                    &descriptorName,
                    &actual,
                    &error),
            qPrintable(QStringLiteral(
                    "Windows promised-file formats were unavailable: %1")
                    .arg(error)));
    require(descriptorName == QStringLiteral("Report.pkg"),
            "the promised-file descriptor exposed the staging filename");
    require(actual == expected,
            "the promised-file content stream did not match the remote file");
}

void testAppleWindowsPromisedFileMetadataDoesNotStartTransfer()
{
    QString error;
    require(testAppleWindowsPromisedFileMetadataIsLazy(&error),
            qPrintable(error));
}

void testAppleWindowsPromisedFileAsyncCompletionCanRepeat()
{
    QString error;
    require(testAppleWindowsPromisedFileAsyncCompletionIsReusable(&error),
            qPrintable(error));
}

void testAppleWindowsPromisedFilesReachTwoShellFolders()
{
    QTemporaryDir temporary;
    require(temporary.isValid(),
            "repeated Shell drop temporary directory failed");
    const QString firstStaged = temporary.filePath(
            QStringLiteral("first-staged.bin"));
    const QString secondStaged = temporary.filePath(
            QStringLiteral("second-staged.bin"));
    const QString firstDestination = temporary.filePath(
            QStringLiteral("first-destination"));
    const QString secondDestination = temporary.filePath(
            QStringLiteral("second-destination"));
    require(QDir().mkpath(firstDestination) &&
                    QDir().mkpath(secondDestination),
            "repeated Shell drop destinations could not be created");
    QFile first(firstStaged);
    QFile second(secondStaged);
    require(first.open(QIODevice::WriteOnly) &&
                    first.write("first") == 5 &&
                    second.open(QIODevice::WriteOnly) &&
                    second.write("second") == 6,
            "repeated Shell drop fixtures could not be created");
    first.close();
    second.close();

    QString error;
    require(testAppleWindowsPromisedFilesDropIntoShellFolders(
                    QStringLiteral("/Users/test/First.pkg"),
                    firstStaged,
                    firstDestination,
                    QStringLiteral("/Users/test/Second.pkg"),
                    secondStaged,
                    secondDestination,
                    &error),
            qPrintable(error));
    QFile firstResult(QDir(firstDestination).filePath(
            QStringLiteral("First.pkg")));
    QFile secondResult(QDir(secondDestination).filePath(
            QStringLiteral("Second.pkg")));
    require(firstResult.open(QIODevice::ReadOnly) &&
                    firstResult.readAll() == QByteArrayLiteral("first") &&
                    secondResult.open(QIODevice::ReadOnly) &&
                    secondResult.readAll() == QByteArrayLiteral("second"),
            "a repeated promised-file drop did not reach its selected Shell folder");
}
#endif

void testAppleLocalFileDragTracksTheHoveredRemoteTarget()
{
    QStringList actions;
    AppleLocalFileDragLifecycle lifecycle(
            [](const AppleFileDragPoint&) { return true; },
            [&actions](const QStringList& paths) {
                actions.append(QStringLiteral("announce:%1").arg(paths.first()));
                return true;
            },
            [&actions](const AppleFileDragPoint& point,
                       AppleLocalFileDragPointerAction action) {
                actions.append(QStringLiteral("pointer:%1,%2:%3")
                                       .arg(point.x)
                                       .arg(point.y)
                                       .arg(action ==
                                                    AppleLocalFileDragPointerAction::Release
                                            ? QStringLiteral("up")
                                            : QStringLiteral("down")));
            },
            [&actions]() {
                actions.append(QStringLiteral("cancel"));
            });
    require(lifecycle.enter(1, {QStringLiteral("C:/test/report.pdf")},
                            {100, 120, 0}),
            "local file drag did not enter the remote stream");
    lifecycle.move({180, 210, 0});
    lifecycle.drop({240, 260, 0});
    require(actions == QStringList{
                        QStringLiteral("announce:C:/test/report.pdf"),
                        QStringLiteral("pointer:100,120:down"),
                        QStringLiteral("pointer:180,210:down"),
                        QStringLiteral("pointer:240,260:up")},
            "local file drag did not preserve enter, hover, and drop coordinates");
}

void testAppleLocalFileDragSurvivesAWindowBoundary()
{
    QStringList actions;
    AppleLocalFileDragLifecycle lifecycle(
            [](const AppleFileDragPoint&) { return true; },
            [&actions](const QStringList& paths) {
                actions.append(QStringLiteral("announce:%1").arg(paths.first()));
                return true;
            },
            [&actions](const AppleFileDragPoint& point,
                       AppleLocalFileDragPointerAction action) {
                actions.append(QStringLiteral("pointer:%1,%2:%3")
                                       .arg(point.x)
                                       .arg(point.y)
                                       .arg(action ==
                                                    AppleLocalFileDragPointerAction::Release
                                            ? QStringLiteral("up")
                                            : QStringLiteral("down")));
            },
            [&actions]() {
                actions.append(QStringLiteral("cancel"));
            });
    const QStringList paths{QStringLiteral("C:/test/report.pdf")};
    require(lifecycle.enter(1, paths, {100, 120, 0}),
            "local file drag did not enter the remote stream");
    lifecycle.leave();
    require(lifecycle.enter(1, paths, {180, 210, 0}),
            "a system drag could not re-enter after Windows changed drop targets");
    lifecycle.drop({240, 260, 0});
    require(actions == QStringList{
                        QStringLiteral("announce:C:/test/report.pdf"),
                        QStringLiteral("pointer:100,120:down"),
                        QStringLiteral("pointer:180,210:down"),
                        QStringLiteral("pointer:240,260:up")},
            "crossing a native window boundary restarted the remote drag");
}

void testAppleLocalFileDragSeparatesRepeatedDragsOfTheSameFile()
{
    QStringList actions;
    AppleLocalFileDragLifecycle lifecycle(
            [](const AppleFileDragPoint&) { return true; },
            [&actions](const QStringList&) {
                actions.append(QStringLiteral("announce"));
                return true;
            },
            [&actions](const AppleFileDragPoint&,
                       AppleLocalFileDragPointerAction action) {
                actions.append(action ==
                                       AppleLocalFileDragPointerAction::Release
                               ? QStringLiteral("up")
                               : QStringLiteral("down"));
            },
            [&actions]() { actions.append(QStringLiteral("cancel")); });
    const QStringList paths{QStringLiteral("C:/test/report.pdf")};
    require(lifecycle.enter(1, paths, {100, 120, 0}),
            "the first system drag did not start");
    lifecycle.leave();
    require(lifecycle.enter(2, paths, {180, 210, 0}),
            "a later system drag of the same path did not start");
    lifecycle.drop({180, 210, 0});
    require(actions == QStringList{
                        QStringLiteral("announce"),
                        QStringLiteral("down"),
                        QStringLiteral("cancel"),
                        QStringLiteral("up"),
                        QStringLiteral("announce"),
                        QStringLiteral("down"),
                        QStringLiteral("up")},
            "two physical drags of the same path shared one remote offer");
}

void testAppleLocalFileDragRejectsNonVideoCoordinatesBeforeAnnouncing()
{
    int announcements = 0;
    int pointers = 0;
    AppleLocalFileDragLifecycle lifecycle(
            [](const AppleFileDragPoint& point) { return point.x >= 0; },
            [&announcements](const QStringList&) {
                ++announcements;
                return true;
            },
            [&pointers](const AppleFileDragPoint&,
                        AppleLocalFileDragPointerAction) { ++pointers; },
            []() {});
    require(!lifecycle.enter(
                    1,
                    {QStringLiteral("C:/test/report.pdf")},
                    {-1, 120, 0}),
            "a file drag started outside the remote video viewport");
    require(announcements == 0 && pointers == 0 && !lifecycle.isActive(),
            "an invalid drop point emitted partial remote drag state");
}

void testWindowsPromisedFileDragRequiresTheWindowThread()
{
#ifdef Q_OS_WIN
    HWND window = CreateWindowExW(
            0, L"STATIC", L"Apple drag affinity test",
            WS_OVERLAPPED, 0, 0, 64, 64,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(window != nullptr,
            "the promised-file affinity test window could not be created");
    AppleWindowsRemoteFileDragSource source(window);
    require(source.isValid(),
            "the promised-file source rejected a valid test window");

    bool began = true;
    QString error;
    std::thread wrongThread([&]() {
        began = source.begin(
                {},
                [](const std::atomic_bool&, QStringList*, QString*) {
                    return false;
                },
                [](AppleWindowsRemoteFileDragResult, const QString&) {},
                &error);
    });
    wrongThread.join();
    DestroyWindow(window);
    require(!began && error.contains(QStringLiteral("window thread")),
            "a promised-file drag was allowed to run outside its HWND thread");
#endif
}

void testAppleFileTransferServiceReceivesRemoteFile()
{
    QTemporaryDir temporary;
    require(temporary.isValid(), "file-transfer service temporary directory failed");
    const QByteArray contents(140000, 'R');
    const QString source = QDir(temporary.path()).filePath(
            QStringLiteral("sender-source.bin"));
    QFile sourceFile(source);
    require(sourceFile.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                    sourceFile.write(contents) == contents.size(),
            "file-transfer service source could not be created");
    sourceFile.close();

    AppleFileTransferService service;
    service.setAvailable(true);
    const QString destination = QDir(temporary.path()).filePath(
            QStringLiteral("downloads"));
    const QString promisedDestination = QDir(destination).filePath(
            QStringLiteral("Finder Chosen.bin"));
    std::atomic_bool cancelled{false};
    QString materializedPath;
    QString materializeError;
    bool materialized = false;
    std::thread materializer([&]() {
        materialized = service.materializeRemoteFile(
                QStringLiteral("/Users/test/remote-source.bin"),
                promisedDestination,
                cancelled,
                &materializedPath,
                &materializeError);
    });

    QList<QByteArray> commands;
    QElapsedTimer commandTimeout;
    commandTimeout.start();
    while (commands.isEmpty() && commandTimeout.elapsed() < 5000) {
        commands = service.takeOutbound(8);
        QThread::yieldCurrentThread();
    }
    require(commands.size() == 1 &&
                    static_cast<quint8>(commands.first().at(0)) == 0x22,
            "remote receive did not queue its native start command");
    bool ok = false;
    const quint32 sessionId = AppleWire::readUInt32(
            commands.first(), 10, &ok);
    require(ok && sessionId != 0,
            "remote receive start command omitted its session ID");

    QString error;
    AppleFileCopySender sender;
    require(sender.run(
                    source,
                    sessionId,
                    [&service](const QByteArray& message,
                               quint64,
                               QString* callbackError) {
                        for (const QByteArray& fragment :
                             AppleFileTransferProtocol::fragments(message)) {
                            if (!service.receive(fragment, callbackError)) {
                                return false;
                            }
                        }
                        return true;
                    },
                    {},
                    &cancelled,
                    &error),
            qPrintable(error));
    materializer.join();
    require(materialized && materializedPath == promisedDestination,
            qPrintable(materializeError));

    const QString received = promisedDestination;
    QFile receivedFile(received);
    require(receivedFile.open(QIODevice::ReadOnly) &&
                    receivedFile.readAll() == contents,
            "file-transfer service changed received remote file contents");
    bool completed = false;
    for (const AppleFileTransferEvent& event : service.takeEvents()) {
        completed = completed ||
                (event.kind == AppleFileTransferEvent::Kind::Completed &&
                 event.direction ==
                         AppleFileTransferEvent::Direction::FromRemote &&
                 event.sessionId == sessionId && event.path == received);
    }
    require(completed,
            "file-transfer service did not publish remote completion");
    service.close();
}

void testAppleFileTransferServiceLimitsConcurrentCopies()
{
    QTemporaryDir temporary;
    require(temporary.isValid(),
            "file-transfer concurrency temporary directory failed");
    AppleFileTransferService service;
    service.setAvailable(true);
    AppleRemoteFileDrag drag;
    drag.sessionId = 0x7000;
    for (int index = 0; index < 5; ++index) {
        drag.sourcePaths.append(
                QStringLiteral("/Users/test/remote-%1.bin").arg(index));
    }
    QString error;
    require(service.acceptRemoteDrag(drag, temporary.path(), &error),
            qPrintable(error));
    const QList<QByteArray> initial = service.takeOutbound(8);
    require(initial.size() == 4,
            "file-transfer service started more than four concurrent copies");

    bool ok = false;
    const quint32 sessionId = AppleWire::readUInt32(initial.first(), 10, &ok);
    require(ok && sessionId != 0,
            "file-transfer concurrency start omitted its session ID");
    AppleFileCopySummary summary;
    summary.rootIsFile = true;
    summary.logicalBytes = 1;
    summary.physicalBytes = 1;
    summary.fileCount = 1;
    AppleFileCopyItemMetadata item;
    item.type = AppleFileCopyItemType::File;
    item.dataForkSize = 1;
    item.mode = 0100600;
    item.name = QStringLiteral("remote-0.bin");
    const QList<QByteArray> frames = {
        AppleFileTransferProtocol::senderSummary(sessionId, summary, &error),
        AppleFileTransferProtocol::senderItem(sessionId, item, &error),
        AppleFileTransferProtocol::senderData(
                sessionId, QByteArrayLiteral("X"), &error),
        AppleFileTransferProtocol::senderDone(sessionId, 0, &error),
    };
    for (const QByteArray& frame : frames) {
        require(!frame.isEmpty() && service.receive(frame, &error),
                qPrintable(error));
    }
    const QList<QByteArray> replacement = service.takeOutbound(8);
    require(replacement.size() == 1 &&
                    AppleWire::readUInt16(replacement.first(), 8, &ok) == 1 &&
                    ok,
            "file-transfer service did not start the next queued copy");
    service.close();
}

void testLocalClipboardRefreshesWhenStreamWindowRegainsFocus()
{
    AppleLocalClipboardTracker tracker;
    QMimeData initial;
    initial.setText(QStringLiteral("initial"));
    require(tracker.dataChanged(&initial) ==
                    std::optional<QString>(QStringLiteral("initial")),
            "a local clipboard notification must publish its text");

    // Native clipboard ownership can change while the SDL stream window is
    // inactive without Qt delivering QClipboard::dataChanged. Swift samples
    // NSPasteboard when the session window becomes key, so the Windows seam
    // must recover the latest value at the equivalent focus boundary.
    QMimeData changedWithoutNotification;
    changedWithoutNotification.setText(QStringLiteral("copied while unfocused"));
    require(tracker.windowFocusGained(&changedWithoutNotification) ==
                    std::optional<QString>(
                            QStringLiteral("copied while unfocused")),
            "refocusing the stream window must recover a missed local clipboard change");

    tracker.expectRemoteText(QStringLiteral("remote"));
    QMimeData remoteWrite;
    remoteWrite.setText(QStringLiteral("remote"));
    require(!tracker.dataChanged(&remoteWrite).has_value(),
            "a remote clipboard write must not be advertised back to the Mac");

    QMimeData localFile;
    localFile.setText(QStringLiteral("C:/private.txt"));
    localFile.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/private.txt"))});
    require(!tracker.windowFocusGained(&localFile).has_value(),
            "refocusing the stream window must not turn a file clipboard into text");
}

void testApplePerformanceOverlayFollowsSharedSettingsAndPlacement()
{
    const ApplePerformanceOverlayPolicy hidden =
            ApplePerformanceOverlayPolicy::fromSettings(false, 0);
    require(!hidden.visible,
            "the shared performance-overlay switch must hide Apple stream metrics");

    const ApplePerformanceOverlayPolicy moonlight =
            ApplePerformanceOverlayPolicy::fromSettings(true, 0);
    require(moonlight.visible &&
                    moonlight.style == ApplePerformanceOverlayStyle::Moonlight,
            "Apple metrics must expose the Moonlight-compatible compact style");
    require(moonlight.topLeft(QSize(1920, 1080), QSize(420, 180)) ==
                    QPoint(750, 0),
            "Apple metrics must use Moonlight's Windows top-center placement");

    const ApplePerformanceOverlayPolicy detailed =
            ApplePerformanceOverlayPolicy::fromSettings(true, 1);
    require(detailed.visible &&
                    detailed.style == ApplePerformanceOverlayStyle::Detailed,
            "Apple metrics must expose the detailed diagnostic style");

    ApplePerformanceOverlayMetrics metrics;
    metrics.canvasSize = QSize(3840, 2160);
    metrics.receivedFramesPerSecond = 59.8;
    metrics.decodedFramesPerSecond = 59.7;
    metrics.presentedFramesPerSecond = 59.6;
    metrics.networkMegabitsPerSecond = 42.3;
    metrics.decodeMilliseconds = 0.42;
    metrics.renderMilliseconds = 0.18;
    metrics.decoderBackend = QStringLiteral("D3D11VA");
    metrics.hasMediaSample = true;
    metrics.hasPresentationSample = true;
    const QStringList compactLines = appleMoonlightPerformanceLines(metrics);
    require(compactLines.size() == 1 &&
                    compactLines.at(0).contains(
                            QStringLiteral(
                                    "3840x2160@60 HEVC 4:4:4/D3D11VA  FPS 59.8 Rx · 59.7 De · 59.6 Rd")) &&
                    compactLines.at(0).contains(
                            QStringLiteral(
                                    "Network Video UDP 42.3 Mb/s")) &&
                    compactLines.at(0).contains(
                            QStringLiteral(
                                    "Render 0.18 ms · Decode 0.42 ms")),
            "Moonlight-style Apple metrics must remain on Moonlight's single rendered row");
    const QList<ApplePerformanceOverlayTextRun> compactRuns =
            appleMoonlightPerformanceRuns(metrics);
    require(std::any_of(compactRuns.cbegin(), compactRuns.cend(),
                        [](const ApplePerformanceOverlayTextRun& run) {
                            return run.text == QStringLiteral("Rx") &&
                                    run.pixelSize == 14 && !run.bold;
                        }) &&
                    std::any_of(compactRuns.cbegin(), compactRuns.cend(),
                                [](const ApplePerformanceOverlayTextRun& run) {
                                    return run.text == QStringLiteral("0.18") &&
                                            run.pixelSize == 18 && run.bold;
                                }),
            "Moonlight-style Apple metrics must preserve Moonlight's label sizing and value emphasis");
}

void testStageFourAacEldAudioContract()
{
    QByteArray audio(12, '\0');
    audio[0] = static_cast<char>(0x80);
    audio[1] = static_cast<char>(101);
    require(AppleAudioStream::isAudioRtp(audio),
            "Apple audio RTP payload 101 was not recognized");
    audio[1] = static_cast<char>(72);
    require(!AppleAudioStream::isAudioRtp(audio),
            "RTCP payload was mistaken for Apple audio RTP");
    audio[0] = 0;
    audio[1] = static_cast<char>(101);
    require(!AppleAudioStream::isAudioRtp(audio),
            "invalid RTP version was accepted as Apple audio");

    QString error;
    const bool supported = AppleAudioStream::decoderIsSupported(&error);
    if (!supported) {
        std::fprintf(stderr, "AAC-ELD probe: %s\n", qPrintable(error));
    }
    require(supported,
            qPrintable(QStringLiteral("AAC-ELD decoder probe failed: %1").arg(error)));
}

void testNativePresentationFactoryUsesLowLatencyAdapter()
{
    require(SDL_InitSubSystem(SDL_INIT_VIDEO) == 0,
            qPrintable(QStringLiteral("SDL video initialization failed: %1")
                               .arg(QString::fromUtf8(SDL_GetError()))));
    const auto quitVideo = qScopeGuard([]() { SDL_QuitSubSystem(SDL_INIT_VIDEO); });
    Uint32 windowFlags = SDL_WINDOW_HIDDEN;
#ifdef Q_OS_DARWIN
    windowFlags |= SDL_WINDOW_METAL;
#endif
    SDL_Window* window = SDL_CreateWindow(
            "Apple low-latency presentation test",
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            640,
            360,
            windowFlags);
    require(window != nullptr,
            qPrintable(QStringLiteral("SDL test window creation failed: %1")
                               .arg(QString::fromUtf8(SDL_GetError()))));
    const auto destroyWindow = qScopeGuard([window]() { SDL_DestroyWindow(window); });

    auto decoderContext = std::make_shared<AppleVideoBackendContext>();
    decoderContext->backend = AppleVideoDecoderBackend::Software;
    QString error;
    std::unique_ptr<AppleVideoRenderer> renderer = createAppleVideoRenderer(
            window, decoderContext, &error);
    require(renderer != nullptr,
            qPrintable(QStringLiteral("native renderer initialization failed: %1")
                               .arg(error)));
    require(!renderer->name().isEmpty() &&
            renderer->usesLowLatencyPresentation(),
            "the selected native adapter must expose latency-1 synchronized presentation");
#ifdef Q_OS_DARWIN
    require(renderer->startDisplayLink([]() { }),
            "the macOS Metal adapter must create a display-linked presentation clock");
    renderer->setDisplayLinkPaused(true);
    renderer->setDisplayLinkPaused(false);
    renderer->stopDisplayLink();
#endif
    error.clear();
    require(renderer->render(AppleCanvas{}, {}, &error) ==
                    AppleVideoRenderer::RenderResult::Failed &&
            !error.isEmpty(),
            "the native adapter must reject an invalid canvas at its public boundary");
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
#ifdef Q_OS_WIN
    if (application.arguments().contains(
                QStringLiteral("--windows-promised-file-test"))) {
        std::fprintf(stderr,
                     "testAppleWindowsPromisedFileExposesDescriptorAndContents\n");
        testAppleWindowsPromisedFileExposesDescriptorAndContents();
        std::fprintf(stderr,
                     "testAppleWindowsPromisedFileMetadataDoesNotStartTransfer\n");
        testAppleWindowsPromisedFileMetadataDoesNotStartTransfer();
        std::fprintf(stderr,
                     "testAppleWindowsPromisedFileAsyncCompletionCanRepeat\n");
        testAppleWindowsPromisedFileAsyncCompletionCanRepeat();
        std::fprintf(stderr,
                     "testAppleWindowsPromisedFilesReachTwoShellFolders\n");
        testAppleWindowsPromisedFilesReachTwoShellFolders();
        return 0;
    }
#endif
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
    std::fprintf(stderr, "testHighPerformanceEncodingCapabilitiesMatchNativeOrder\n");
    testHighPerformanceEncodingCapabilitiesMatchNativeOrder();
    std::fprintf(stderr, "testControlNegotiationAndEncryptedWrite\n");
    testControlNegotiationAndEncryptedWrite();
    std::fprintf(stderr, "testHighPerformanceMediaOfferAndAnswer\n");
    testHighPerformanceMediaOfferAndAnswer();
    std::fprintf(stderr, "testStageFourDisplayConfigurationAndDynamicResolution\n");
    testStageFourDisplayConfigurationAndDynamicResolution();
    std::fprintf(stderr, "testSrtpAndRecoveryFeedbackVectors\n");
    testSrtpAndRecoveryFeedbackVectors();
    std::fprintf(stderr, "testAdaptiveRateControlFeedback\n");
    testAdaptiveRateControlFeedback();
    std::fprintf(stderr, "testHevcAssemblyAndLossTracking\n");
    testHevcAssemblyAndLossTracking();
    std::fprintf(stderr, "testMinimizedFrameUpdatePolicy\n");
    testMinimizedFrameUpdatePolicy();
    std::fprintf(stderr, "testHevcGlobalDecodingOrderAdmission\n");
    testHevcGlobalDecodingOrderAdmission();
    std::fprintf(stderr, "testEncryptedInputWireBoundary\n");
    testEncryptedInputWireBoundary();
    std::fprintf(stderr, "testAppleKeyboardMappingAndFocusRelease\n");
    testAppleKeyboardMappingAndFocusRelease();
    std::fprintf(stderr, "testNativePrecisionScrollWireAndDeltas\n");
    testNativePrecisionScrollWireAndDeltas();
#ifdef Q_OS_DARWIN
    std::fprintf(stderr, "testMacNativeScrollPreservesCgEventFields\n");
    testMacNativeScrollPreservesCgEventFields();
    std::fprintf(stderr, "testAppleMacZoomButtonUsesNativeFullscreen\n");
    require(testAppleMacZoomButtonUsesNativeFullscreen(),
            "the macOS zoom button must use native fullscreen and restore its original action");
    std::fprintf(stderr, "testAppleMacInputBridgeRoutesRemoteDragBeforePointer\n");
    require(testAppleMacInputBridgeRoutesRemoteDragBeforePointer(),
            "the macOS input bridge forwarded an outgoing file drag as remote pointer motion");
    std::fprintf(stderr, "testAppleMacInputBridgeRoutesLocalFileDrag\n");
    require(testAppleMacInputBridgeRoutesLocalFileDrag(),
            "the macOS input bridge did not route an inbound file drag");
    std::fprintf(stderr, "testAppleMacInputBridgeReleasesModifiersOnFocusLoss\n");
    require(testAppleMacInputBridgeReleasesModifiersOnFocusLoss(),
            "the macOS input bridge left Shift pressed after losing focus");
#endif
    std::fprintf(stderr, "testAppleStreamWindowPlacementPersistence\n");
    testAppleStreamWindowPlacementPersistence();
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
    std::fprintf(stderr, "testDecodedTilesDoNotRetainFramesBehindMissingDecoderOutput\n");
    testDecodedTilesDoNotRetainFramesBehindMissingDecoderOutput();
    std::fprintf(stderr, "testUdpPunchIgnoresClosedOptimisticPortReset\n");
    testUdpPunchIgnoresClosedOptimisticPortReset();
    std::fprintf(stderr, "testStageFourCursorAndDisplayLayoutEvents\n");
    testStageFourCursorAndDisplayLayoutEvents();
    std::fprintf(stderr, "testRemoteCursorScalesForClientDpi\n");
    testRemoteCursorScalesForClientDpi();
    std::fprintf(stderr, "testRemoteCursorCacheMatchesSwiftFallbacks\n");
    testRemoteCursorCacheMatchesSwiftFallbacks();
    std::fprintf(stderr, "testStageFourTextOnlyClipboardExchange\n");
    testStageFourTextOnlyClipboardExchange();
    std::fprintf(stderr, "testAppleFileTransferNativeWireContract\n");
    testAppleFileTransferNativeWireContract();
    std::fprintf(stderr, "testAppleFileCopyDirectoryRoundTrip\n");
    testAppleFileCopyDirectoryRoundTrip();
    std::fprintf(stderr, "testAppleFileCopySenderBeginsWithNativeUncompressedData\n");
    testAppleFileCopySenderBeginsWithNativeUncompressedData();
    std::fprintf(stderr, "testAppleFileTransferKeepsLogicalMessageFragmentsAtomic\n");
    testAppleFileTransferKeepsLogicalMessageFragmentsAtomic();
    std::fprintf(stderr, "testAppleFileTransferProgressNeverMovesBackward\n");
    testAppleFileTransferProgressNeverMovesBackward();
    std::fprintf(stderr, "testAppleFileTransferStopsImmediatelyAfterRemoteRejection\n");
    testAppleFileTransferStopsImmediatelyAfterRemoteRejection();
    std::fprintf(stderr, "testAppleRemoteFileDragWaitsUntilPointerLeavesStream\n");
    testAppleRemoteFileDragWaitsUntilPointerLeavesStream();
    std::fprintf(stderr, "testAppleRemoteFileDragEndDoesNotDropAgain\n");
    testAppleRemoteFileDragEndDoesNotDropAgain();
    std::fprintf(stderr, "testAppleFileTransferCancelsOnlyTheActiveRemoteDragOnce\n");
    testAppleFileTransferCancelsOnlyTheActiveRemoteDragOnce();
#ifdef Q_OS_WIN
    std::fprintf(stderr, "testAppleWindowsPromisedFileExposesDescriptorAndContents\n");
    testAppleWindowsPromisedFileExposesDescriptorAndContents();
    std::fprintf(stderr, "testAppleWindowsPromisedFileMetadataDoesNotStartTransfer\n");
    testAppleWindowsPromisedFileMetadataDoesNotStartTransfer();
    std::fprintf(stderr, "testAppleWindowsPromisedFileAsyncCompletionCanRepeat\n");
    testAppleWindowsPromisedFileAsyncCompletionCanRepeat();
    std::fprintf(stderr, "testAppleWindowsPromisedFilesReachTwoShellFolders\n");
    testAppleWindowsPromisedFilesReachTwoShellFolders();
#endif
#ifdef Q_OS_DARWIN
    std::fprintf(stderr, "testAppleMacPromisedFileAdapter\n");
    QString macPromiseError;
    require(testAppleMacPromisedFileAdapter(&macPromiseError),
            qPrintable(QStringLiteral(
                    "macOS promised-file adapter failed: %1")
                               .arg(macPromiseError)));
#endif
    std::fprintf(stderr, "testAppleLocalFileDragTracksTheHoveredRemoteTarget\n");
    testAppleLocalFileDragTracksTheHoveredRemoteTarget();
    std::fprintf(stderr, "testAppleLocalFileDragSurvivesAWindowBoundary\n");
    testAppleLocalFileDragSurvivesAWindowBoundary();
    std::fprintf(stderr, "testAppleLocalFileDragSeparatesRepeatedDragsOfTheSameFile\n");
    testAppleLocalFileDragSeparatesRepeatedDragsOfTheSameFile();
    std::fprintf(stderr, "testAppleLocalFileDragRejectsNonVideoCoordinatesBeforeAnnouncing\n");
    testAppleLocalFileDragRejectsNonVideoCoordinatesBeforeAnnouncing();
    std::fprintf(stderr, "testWindowsPromisedFileDragRequiresTheWindowThread\n");
    testWindowsPromisedFileDragRequiresTheWindowThread();
    std::fprintf(stderr, "testAppleFileTransferServiceReceivesRemoteFile\n");
    testAppleFileTransferServiceReceivesRemoteFile();
    std::fprintf(stderr, "testAppleFileTransferServiceLimitsConcurrentCopies\n");
    testAppleFileTransferServiceLimitsConcurrentCopies();
    std::fprintf(stderr, "testLocalClipboardRefreshesWhenStreamWindowRegainsFocus\n");
    testLocalClipboardRefreshesWhenStreamWindowRegainsFocus();
    std::fprintf(stderr, "testApplePerformanceOverlayFollowsSharedSettingsAndPlacement\n");
    testApplePerformanceOverlayFollowsSharedSettingsAndPlacement();
    std::fprintf(stderr, "testStageFourAacEldAudioContract\n");
    testStageFourAacEldAudioContract();
    std::fprintf(stderr, "testNativePresentationFactoryUsesLowLatencyAdapter\n");
    testNativePresentationFactoryUsesLowLatencyAdapter();
    std::fprintf(stderr, "Apple Screen Sharing stage 3 and 4 protocol tests passed\n");
    return 0;
}
