#pragma once

#include "appleconnectionstore.h"
#include "applecredentialstore.h"
#include "appleprotocol.h"

#include <QByteArray>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

class QTcpSocket;
class QHostAddress;

class AppleByteTransport
{
public:
    virtual ~AppleByteTransport() = default;

    virtual bool connectTo(const AppleConnectionEndpoint& endpoint,
                           std::atomic_bool* cancelled,
                           QString* error) = 0;
    virtual bool writeAll(const QByteArray& data,
                          std::atomic_bool* cancelled,
                          QString* error) = 0;
    virtual bool readExactly(int length,
                             QByteArray* data,
                             std::atomic_bool* cancelled,
                             QString* error) = 0;
    virtual void protocolDelay(int milliseconds, std::atomic_bool* cancelled) = 0;
    virtual void close() = 0;
};

class AppleTcpTransport final : public AppleByteTransport
{
public:
    AppleTcpTransport();
    ~AppleTcpTransport() override;

    bool connectTo(const AppleConnectionEndpoint& endpoint,
                   std::atomic_bool* cancelled,
                   QString* error) override;
    bool writeAll(const QByteArray& data,
                  std::atomic_bool* cancelled,
                  QString* error) override;
    bool readExactly(int length,
                     QByteArray* data,
                     std::atomic_bool* cancelled,
                     QString* error) override;
    void protocolDelay(int milliseconds, std::atomic_bool* cancelled) override;
    void close() override;

    void setWaitCallback(std::function<void()> callback);
    QHostAddress peerAddress() const;
    bool hasPendingData();
    bool isConnected() const;

private:
    std::unique_ptr<QTcpSocket> m_Socket;
    std::function<void()> m_WaitCallback;
};

struct AppleHostIdentity
{
    QByteArray subjectPublicKeyInfo;
    QString fingerprint;
};

struct AppleAuthenticatedControl
{
    QByteArray masterKey;
    QString serverName;
    quint16 width = 0;
    quint16 height = 0;
};

class AppleAuthenticator
{
public:
    using CredentialLoader = std::function<bool(AppleCredentials*, QString*)>;

    bool probe(AppleByteTransport& transport,
               const AppleConnectionEndpoint& endpoint,
               AppleHostIdentity* identity,
               std::atomic_bool* cancelled,
               QString* error) const;

    bool authenticate(AppleByteTransport& transport,
                      const AppleConnectionEndpoint& endpoint,
                      const QString& expectedFingerprint,
                      const CredentialLoader& loadCredentials,
                      AppleAuthenticatedControl* result,
                      std::atomic_bool* cancelled,
                      QString* error) const;

private:
    bool prepareHostIdentity(AppleByteTransport& transport,
                             AppleHostIdentity* identity,
                             std::atomic_bool* cancelled,
                             QString* error) const;
};

class AppleControlChannel
{
public:
    bool negotiate(AppleByteTransport& transport,
                   const QByteArray& masterKey,
                   std::atomic_bool* cancelled,
                   QString* error);
    bool negotiate(AppleByteTransport& transport,
                   const QByteArray& masterKey,
                   const QByteArray& displayConfiguration,
                   std::atomic_bool* cancelled,
                   QString* error);
    bool sendEncrypted(AppleByteTransport& transport,
                       const QByteArray& message,
                       std::atomic_bool* cancelled,
                       QString* error);
    bool receiveEncrypted(AppleByteTransport& transport,
                          QByteArray* message,
                          std::atomic_bool* cancelled,
                          QString* error);
    bool sendEncryptedInput(AppleByteTransport& transport,
                            const QByteArray& header,
                            const QByteArray& plaintextBlock,
                            std::atomic_bool* cancelled,
                            QString* error);

private:
    bool readRekey(AppleByteTransport& transport,
                   QByteArray* encryptedKey,
                   QByteArray* encryptedIv,
                   std::atomic_bool* cancelled,
                   QString* error);

    AppleEncryptedRecordLayer m_Records;
};
