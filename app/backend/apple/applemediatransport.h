#pragma once

#include "applemediaprotocol.h"

#include <QByteArray>
#include <QHostAddress>

#include <atomic>
#include <memory>

class AppleControlChannel;
class AppleTcpTransport;
class QUdpSocket;

class AppleMediaTransport
{
public:
    AppleMediaTransport();
    ~AppleMediaTransport();

    AppleMediaTransport(const AppleMediaTransport&) = delete;
    AppleMediaTransport& operator=(const AppleMediaTransport&) = delete;

    bool open(const QHostAddress& remoteAddress,
              quint16 basePort,
              QString* error = nullptr);
    void configureRemotePorts(const AppleMediaPorts& ports);
    void punchIfDue();
    void stopPunching();

    bool receiveVideo(QByteArray* datagram,
                      int timeoutMilliseconds,
                      std::atomic_bool* cancelled,
                      QString* error = nullptr);
    QList<QByteArray> drainControl();
    bool sendVideoControl(const QByteArray& datagram,
                          QString* error = nullptr);
    void close();

    bool isOpen() const;

private:
    bool bindSocket(QUdpSocket* socket, quint16 port, QString* error);

    std::unique_ptr<QUdpSocket> m_ControlSocket;
    std::unique_ptr<QUdpSocket> m_VideoSocket;
    QHostAddress m_RemoteAddress;
    quint16 m_ControlRemotePort = 0;
    quint16 m_VideoRemotePort = 0;
    qint64 m_LastPunchMilliseconds = -1;
    bool m_Punching = false;
};

struct AppleMediaNegotiationResult
{
    AppleMediaKeys keys;
    AppleMediaOffers offers;
    AppleCanvas canvas;
    QByteArray configuration;

    bool isUsable() const
    {
        return keys.isValid() && canvas.isUsable() && !configuration.isEmpty();
    }
};

class AppleMediaNegotiator
{
public:
    bool negotiate(AppleTcpTransport& tcp,
                   AppleControlChannel& control,
                   AppleMediaTransport& media,
                   quint16 basePort,
                   bool audioEnabled,
                   AppleMediaNegotiationResult* result,
                   std::atomic_bool* cancelled,
                   QString* error = nullptr) const;
};

