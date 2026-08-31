#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>

namespace AppleWire {

QByteArray versionBanner();
QByteArray publicKeyRequest();
QByteArray viewerInfo();
QByteArray setEncryption();
QByteArray setEncodings();
QByteArray postEncryptionToggle();
QByteArray displayConfiguration(int width = 1440, int height = 900);

bool parseVersionBanner(const QByteArray& data);
bool parsePublicKeyResponse(const QByteArray& packet,
                            QByteArray* subjectPublicKeyInfo,
                            QString* error = nullptr);
QByteArray credentialPlaintext(const QString& username,
                               const QString& password,
                               QString* error = nullptr);
QByteArray authenticationRequest(const QByteArray& encryptedCredentials,
                                 const QByteArray& encryptedMasterKey,
                                 QString* error = nullptr);

quint16 readUInt16(const QByteArray& data, int offset, bool* ok = nullptr);
quint32 readUInt32(const QByteArray& data, int offset, bool* ok = nullptr);
qint32 readInt32(const QByteArray& data, int offset, bool* ok = nullptr);
void appendUInt16(QByteArray& data, quint16 value);
void appendUInt32(QByteArray& data, quint32 value);

} // namespace AppleWire

class AppleEncryptedRecordLayer
{
public:
    AppleEncryptedRecordLayer() = default;
    AppleEncryptedRecordLayer(QByteArray key, QByteArray initializationVector);

    bool isValid() const;
    QByteArray encrypt(const QByteArray& message, QString* error = nullptr);
    QByteArray decrypt(const QByteArray& ciphertext, QString* error = nullptr);
    QByteArray encryptInput(const QByteArray& header,
                            const QByteArray& plaintextBlock,
                            QString* error = nullptr) const;

private:
    QByteArray m_Key;
    QByteArray m_EncryptionIv;
    QByteArray m_DecryptionIv;
    quint32 m_SendCounter = 0;
    quint32 m_ReceiveCounter = 0;
};
