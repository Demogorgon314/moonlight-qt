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
              int displayCount,
              QString* error = nullptr);
    bool open(const QHostAddress& remoteAddress,
              quint16 basePort,
              QString* error = nullptr)
    {
        return open(remoteAddress, basePort, 1, error);
    }
    bool configureRemotePorts(const AppleMediaPorts& ports,
                              QString* error = nullptr);
    void punchIfDue();
    void stopPunching();

    bool receiveVideo(QByteArray* datagram,
                      int timeoutMilliseconds,
                      std::atomic_bool* cancelled,
                      QString* error = nullptr);
    bool receiveVideo(int mediaStreamIndex,
                      QByteArray* datagram,
                      int timeoutMilliseconds,
                      std::atomic_bool* cancelled,
                      QString* error = nullptr);
    QList<QByteArray> drainControl();
    bool sendVideoControl(const QByteArray& datagram,
                          QString* error = nullptr);
    bool sendVideoControl(int mediaStreamIndex,
                          const QByteArray& datagram,
                          QString* error = nullptr);
    void close();

    bool isOpen() const;

private:
    bool bindSocket(QUdpSocket* socket, quint16 port, QString* error);

    std::unique_ptr<QUdpSocket> m_ControlSocket;
    std::unique_ptr<QUdpSocket> m_VideoSocket;
    std::unique_ptr<QUdpSocket> m_SecondaryVideoSocket;
    QHostAddress m_RemoteAddress;
    quint16 m_ControlRemotePort = 0;
    quint16 m_VideoRemotePort = 0;
    quint16 m_SecondaryVideoRemotePort = 0;
    qint64 m_LastPunchMilliseconds = -1;
    bool m_Punching = false;
};

struct AppleVideoNegotiation
{
    QByteArray viewerKey;
    QByteArray serverKey;
    quint32 synchronizationSource = 0;
    AppleCanvas canvas;
    int mediaStreamIndex = 0;
    int displayIndex = 0;

    bool isUsable() const
    {
        return viewerKey.size() == AppleMediaKeys::BlobLength &&
                serverKey.size() == AppleMediaKeys::BlobLength &&
                synchronizationSource != 0 && canvas.isUsable() &&
                mediaStreamIndex >= 0 && mediaStreamIndex < 2;
    }
};

struct AppleMediaNegotiationResult
{
    AppleMediaKeys keys;
    AppleMediaOffers offers;
    AppleCanvas canvas;
    QByteArray configuration;
    QList<AppleVideoNegotiation> videos;
    QList<QByteArray> pendingMessages;

    bool isUsable() const
    {
        return keys.isValid() && canvas.isUsable() && !configuration.isEmpty() &&
                !videos.isEmpty() &&
                std::all_of(videos.cbegin(), videos.cend(),
                            [](const AppleVideoNegotiation& video) {
            return video.isUsable();
        });
    }
};

class AppleMediaNegotiator
{
public:
    static QList<QByteArray> framebufferStartupMessages();

    bool negotiate(AppleTcpTransport& tcp,
                   AppleControlChannel& control,
                   AppleMediaTransport& media,
                   quint16 basePort,
                   bool audioEnabled,
                   int displayCount,
                   AppleMediaNegotiationResult* result,
                   std::atomic_bool* cancelled,
                   QString* error = nullptr) const;
    bool negotiate(AppleTcpTransport& tcp,
                   AppleControlChannel& control,
                   AppleMediaTransport& media,
                   quint16 basePort,
                   bool audioEnabled,
                   AppleMediaNegotiationResult* result,
                   std::atomic_bool* cancelled,
                   QString* error = nullptr) const
    {
        return negotiate(tcp, control, media, basePort, audioEnabled, 1,
                         result, cancelled, error);
    }
};
