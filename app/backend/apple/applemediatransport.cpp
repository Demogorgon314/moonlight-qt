#include "applemediatransport.h"

#include "appleauthenticator.h"
#include "applecontrolfeatures.h"
#include "appleprotocol.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QSysInfo>
#include <QUdpSocket>
#include <QVariant>

#include <openssl/rand.h>

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <MSWSock.h>
#endif

namespace {

constexpr int NegotiationRetryLimit = 16;
constexpr int MaximumDatagramLength = 65535;

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
}

bool isCancelled(std::atomic_bool* cancelled)
{
    return cancelled != nullptr && cancelled->load();
}

bool configurePlatformUdpSocket(QUdpSocket* socket,
                                quint16 port,
                                QString* error)
{
#ifdef Q_OS_WIN
    // The Mac may not have opened the optimistic media port when punching
    // begins. Windows turns the resulting ICMP response into WSAECONNRESET on
    // receive unless this notification is disabled for the UDP socket.
    BOOL reportConnectionReset = FALSE;
    DWORD bytesReturned = 0;
    const SOCKET nativeSocket = static_cast<SOCKET>(socket->socketDescriptor());
    if (WSAIoctl(nativeSocket,
                 SIO_UDP_CONNRESET,
                 &reportConnectionReset,
                 sizeof(reportConnectionReset),
                 nullptr,
                 0,
                 &bytesReturned,
                 nullptr,
                 nullptr) == SOCKET_ERROR) {
        setError(error, QCoreApplication::translate(
                "AppleMediaTransport",
                "Couldn’t configure UDP port %1 for Screen Sharing media "
                "(Winsock error %2).")
                .arg(port).arg(WSAGetLastError()));
        return false;
    }
#else
    Q_UNUSED(socket);
    Q_UNUSED(port);
    Q_UNUSED(error);
#endif
    return true;
}

QByteArray randomBytes(int length, QString* error)
{
    QByteArray result(length, Qt::Uninitialized);
    if (length <= 0 || RAND_bytes(
            reinterpret_cast<unsigned char*>(result.data()), length) != 1) {
        setError(error, QCoreApplication::translate(
                "AppleMediaTransport", "Couldn’t generate secure media keys."));
        return {};
    }
    return result;
}

} // namespace

AppleMediaTransport::AppleMediaTransport() = default;

AppleMediaTransport::~AppleMediaTransport()
{
    close();
}

bool AppleMediaTransport::open(const QHostAddress& remoteAddress,
                               quint16 basePort,
                               int displayCount,
                               QString* error)
{
    close();
    if (remoteAddress.isNull() || basePort == 0 ||
            displayCount < 1 || displayCount > 2 ||
            basePort > 65535 - displayCount) {
        setError(error, QCoreApplication::translate(
                "AppleMediaTransport", "The Mac has an invalid media address."));
        return false;
    }
    m_RemoteAddress = remoteAddress;
    m_ControlRemotePort = basePort;
    m_VideoRemotePort = basePort + 1;
    m_SecondaryVideoRemotePort = displayCount > 1 ? basePort + 2 : 0;
    m_ControlSocket = std::make_unique<QUdpSocket>();
    m_VideoSocket = std::make_unique<QUdpSocket>();
    if (displayCount > 1) {
        m_SecondaryVideoSocket = std::make_unique<QUdpSocket>();
    }
    if (!bindSocket(m_ControlSocket.get(), basePort, error) ||
            !bindSocket(m_VideoSocket.get(), basePort + 1, error) ||
            (m_SecondaryVideoSocket &&
             !bindSocket(m_SecondaryVideoSocket.get(), basePort + 2, error))) {
        close();
        return false;
    }
    m_Punching = true;
    m_LastPunchMilliseconds = -1;
    punchIfDue();
    return true;
}

bool AppleMediaTransport::bindSocket(QUdpSocket* socket,
                                     quint16 port,
                                     QString* error)
{
    const QHostAddress local = m_RemoteAddress.protocol() == QAbstractSocket::IPv6Protocol
            ? QHostAddress::AnyIPv6 : QHostAddress::AnyIPv4;
    if (socket->bind(local, port,
                     QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        if (!configurePlatformUdpSocket(socket, port, error)) {
            socket->close();
            return false;
        }
        // A 4K60 multi-tile stream can queue several megabytes while a
        // hardware frame is transferred for presentation.  Keep enough
        // kernel-side headroom that synchronous decoder work doesn't turn
        // into avoidable RTP loss.
        socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,
                                16 * 1024 * 1024);
        qInfo().nospace()
                << "Apple Screen Sharing UDP port " << port
                << " receive buffer="
                << socket->socketOption(
                           QAbstractSocket::ReceiveBufferSizeSocketOption).toInt()
                << " bytes";
        return true;
    }
    setError(error, QCoreApplication::translate(
            "AppleMediaTransport",
            "UDP port %1 is unavailable for Screen Sharing media: %2")
            .arg(port).arg(socket->errorString()));
    return false;
}

bool AppleMediaTransport::configureRemotePorts(const AppleMediaPorts& ports,
                                               QString* error)
{
    const QList<quint16> videoPorts = ports.videoPorts();
    const int expectedVideoCount = m_SecondaryVideoSocket ? 2 : 1;
    if (!isOpen() || !ports.isUsable() ||
            videoPorts.size() != expectedVideoCount) {
        setError(error, QCoreApplication::translate(
                "AppleMediaTransport",
                "The Mac returned an invalid set of media ports."));
        return false;
    }
    m_ControlRemotePort = ports.audio;
    m_VideoRemotePort = videoPorts.at(0);
    if (m_SecondaryVideoSocket) {
        m_SecondaryVideoRemotePort = videoPorts.at(1);
    }
    return true;
}

void AppleMediaTransport::punchIfDue()
{
    if (!m_Punching || !isOpen()) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_LastPunchMilliseconds >= 0 && now - m_LastPunchMilliseconds < 100) {
        return;
    }
    const QByteArray byte(1, '\0');
    m_ControlSocket->writeDatagram(byte, m_RemoteAddress, m_ControlRemotePort);
    m_VideoSocket->writeDatagram(byte, m_RemoteAddress, m_VideoRemotePort);
    if (m_SecondaryVideoSocket) {
        m_SecondaryVideoSocket->writeDatagram(
                byte, m_RemoteAddress, m_SecondaryVideoRemotePort);
    }
    m_LastPunchMilliseconds = now;
}

void AppleMediaTransport::stopPunching()
{
    m_Punching = false;
}

bool AppleMediaTransport::receiveVideo(QByteArray* datagram,
                                       int timeoutMilliseconds,
                                       std::atomic_bool* cancelled,
                                       QString* error)
{
    return receiveVideo(0, datagram, timeoutMilliseconds,
                        cancelled, error);
}

bool AppleMediaTransport::receiveVideo(int mediaStreamIndex,
                                       QByteArray* datagram,
                                       int timeoutMilliseconds,
                                       std::atomic_bool* cancelled,
                                       QString* error)
{
    QUdpSocket* socket = mediaStreamIndex == 0 ? m_VideoSocket.get()
            : mediaStreamIndex == 1 ? m_SecondaryVideoSocket.get() : nullptr;
    if (!isOpen() || datagram == nullptr || socket == nullptr) {
        return false;
    }
    if (socket->state() != QAbstractSocket::BoundState) {
        setError(error, QCoreApplication::translate(
                "AppleMediaTransport", "The video UDP socket became unavailable: %1")
                .arg(socket->errorString()));
        return false;
    }
    punchIfDue();
    if (!socket->hasPendingDatagrams() &&
            !socket->waitForReadyRead(qMax(0, timeoutMilliseconds))) {
        if (socket->error() != QAbstractSocket::SocketTimeoutError &&
                !isCancelled(cancelled)) {
            setError(error, QCoreApplication::translate(
                    "AppleMediaTransport", "The video socket failed: %1")
                    .arg(socket->errorString()));
        }
        return false;
    }
    if (isCancelled(cancelled) || !socket->hasPendingDatagrams()) {
        return false;
    }
    QByteArray buffer(MaximumDatagramLength, Qt::Uninitialized);
    const qint64 received = socket->readDatagram(buffer.data(), buffer.size());
    if (received <= 0) {
        return false;
    }
    buffer.resize(static_cast<int>(received));
    *datagram = std::move(buffer);
    return true;
}

QList<QByteArray> AppleMediaTransport::drainControl()
{
    QList<QByteArray> result;
    if (!m_ControlSocket) {
        return result;
    }
    while (m_ControlSocket->hasPendingDatagrams()) {
        QByteArray datagram(MaximumDatagramLength, Qt::Uninitialized);
        const qint64 received = m_ControlSocket->readDatagram(
                datagram.data(), datagram.size());
        if (received <= 0) {
            break;
        }
        datagram.resize(static_cast<int>(received));
        result.append(std::move(datagram));
    }
    return result;
}

bool AppleMediaTransport::sendVideoControl(const QByteArray& datagram,
                                           QString* error)
{
    return sendVideoControl(0, datagram, error);
}

bool AppleMediaTransport::sendVideoControl(int mediaStreamIndex,
                                           const QByteArray& datagram,
                                           QString* error)
{
    QUdpSocket* socket = mediaStreamIndex == 0 ? m_VideoSocket.get()
            : mediaStreamIndex == 1 ? m_SecondaryVideoSocket.get() : nullptr;
    const quint16 port = mediaStreamIndex == 0 ? m_VideoRemotePort
            : mediaStreamIndex == 1 ? m_SecondaryVideoRemotePort : 0;
    if (socket == nullptr || port == 0 || datagram.isEmpty() ||
            socket->writeDatagram(datagram, m_RemoteAddress, port) !=
                    datagram.size()) {
        setError(error, QCoreApplication::translate(
                "AppleMediaTransport", "Screen Sharing recovery feedback could not be sent."));
        return false;
    }
    return true;
}

void AppleMediaTransport::close()
{
    m_Punching = false;
    if (m_ControlSocket) {
        m_ControlSocket->abort();
        m_ControlSocket.reset();
    }
    if (m_VideoSocket) {
        m_VideoSocket->abort();
        m_VideoSocket.reset();
    }
    if (m_SecondaryVideoSocket) {
        m_SecondaryVideoSocket->abort();
        m_SecondaryVideoSocket.reset();
    }
    m_RemoteAddress.clear();
    m_ControlRemotePort = 0;
    m_VideoRemotePort = 0;
    m_SecondaryVideoRemotePort = 0;
}

bool AppleMediaTransport::isOpen() const
{
    return m_ControlSocket != nullptr && m_VideoSocket != nullptr &&
           !m_RemoteAddress.isNull();
}

bool AppleMediaNegotiator::negotiate(
        AppleTcpTransport& tcp,
        AppleControlChannel& control,
        AppleMediaTransport& media,
        quint16 basePort,
        bool audioEnabled,
        int displayCount,
        AppleMediaNegotiationResult* result,
        std::atomic_bool* cancelled,
        QString* error) const
{
    displayCount = qBound(1, displayCount, 2);
    if (result == nullptr ||
            !media.open(tcp.peerAddress(), basePort, displayCount, error)) {
        return false;
    }
    tcp.setWaitCallback([&media]() { media.punchIfDue(); });

    AppleMediaNegotiationResult candidate;
    candidate.offers = AppleMediaWire::createOffers(
            audioEnabled, QSysInfo::prettyProductName(), error);
    AppleMediaOffers secondaryOffers;
    if (displayCount > 1) {
        secondaryOffers = AppleMediaWire::createOffers(
                audioEnabled, QSysInfo::prettyProductName(), error);
    }
    candidate.keys.audioViewer = randomBytes(AppleMediaKeys::BlobLength, error);
    candidate.keys.audioServer = randomBytes(AppleMediaKeys::BlobLength, error);
    candidate.keys.videoViewer = randomBytes(AppleMediaKeys::BlobLength, error);
    candidate.keys.videoServer = randomBytes(AppleMediaKeys::BlobLength, error);
    if (displayCount > 1) {
        candidate.keys.secondaryVideoViewer = randomBytes(
                AppleMediaKeys::BlobLength, error);
        candidate.keys.secondaryVideoServer = randomBytes(
                AppleMediaKeys::BlobLength, error);
    }
    candidate.configuration = AppleMediaWire::configuration(
            candidate.offers,
            displayCount > 1 ? &secondaryOffers : nullptr,
            candidate.keys, QUuid::createUuid(), error);
    if (!candidate.keys.isValid() || candidate.configuration.isEmpty()) {
        tcp.setWaitCallback({});
        return false;
    }

    const QList<QByteArray> startup = {
        AppleWire::setEncodings(),
        AppleMediaWire::framebufferUpdateRequest(),
        AppleMediaWire::autoFramebufferUpdate(),
    };
    for (const QByteArray& message : startup) {
        if (!control.sendEncrypted(tcp, message, cancelled, error)) {
            tcp.setWaitCallback({});
            return false;
        }
    }
    if (displayCount > 1) {
        bool receivedLayout = false;
        while (!isCancelled(cancelled) && !receivedLayout) {
            QByteArray response;
            if (!control.receiveEncrypted(tcp, &response, cancelled, error)) {
                tcp.setWaitCallback({});
                return false;
            }
            candidate.pendingMessages.append(response);
            const AppleControlEvents events =
                    AppleControlEventParser::parse(response);
            receivedLayout = std::any_of(
                    events.displayLayouts.cbegin(),
                    events.displayLayouts.cend(),
                    [displayCount](const AppleDisplayLayout& layout) {
                return layout.displays.size() >= displayCount;
            });
        }
    }
    if (!control.sendEncrypted(tcp, candidate.configuration, cancelled, error)) {
        tcp.setWaitCallback({});
        return false;
    }

    int retryCount = 0;
    bool configuredPorts = false;
    QList<AppleCanvas> negotiatedCanvases;
    while (!isCancelled(cancelled) && retryCount <= NegotiationRetryLimit) {
        QByteArray response;
        if (!control.receiveEncrypted(tcp, &response, cancelled, error)) {
            tcp.setWaitCallback({});
            return false;
        }
        if (!configuredPorts) {
            AppleMediaPorts ports;
            if (AppleMediaWire::parsePorts(response, &ports) &&
                    ports.videoPorts().size() == displayCount) {
                if (!media.configureRemotePorts(ports, error)) {
                    tcp.setWaitCallback({});
                    return false;
                }
                configuredPorts = true;
            }
        }
        negotiatedCanvases.append(AppleMediaWire::parseCanvases(response));
        if (negotiatedCanvases.size() >= displayCount) {
            candidate.canvas = negotiatedCanvases.at(0);
            candidate.videos = {
                AppleVideoNegotiation{
                    candidate.keys.videoViewer,
                    candidate.keys.videoServer,
                    candidate.offers.videoSynchronizationSource,
                    negotiatedCanvases.at(0),
                    0,
                    0,
                },
            };
            if (displayCount > 1) {
                candidate.videos.append(AppleVideoNegotiation{
                    candidate.keys.secondaryVideoViewer,
                    candidate.keys.secondaryVideoServer,
                    secondaryOffers.videoSynchronizationSource,
                    negotiatedCanvases.at(1),
                    1,
                    1,
                });
            }
            media.stopPunching();
            tcp.setWaitCallback({});
            *result = std::move(candidate);
            return true;
        }
        if (!AppleMediaWire::containsMediaAnswer(response)) {
            continue;
        }
        if (retryCount++ >= NegotiationRetryLimit) {
            break;
        }
        tcp.protocolDelay(200, cancelled);
        if (!control.sendEncrypted(tcp, candidate.configuration, cancelled, error)) {
            tcp.setWaitCallback({});
            return false;
        }
    }
    tcp.setWaitCallback({});
    setError(error, isCancelled(cancelled)
            ? QCoreApplication::translate(
                    "AppleMediaTransport", "The Screen Sharing connection was cancelled.")
            : QCoreApplication::translate(
                    "AppleMediaTransport", "The Mac did not provide a usable video canvas."));
    return false;
}
