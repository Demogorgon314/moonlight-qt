#include "applemediatransport.h"

#include "appleauthenticator.h"
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
                               QString* error)
{
    close();
    if (remoteAddress.isNull() || basePort == 0 || basePort == 65535) {
        setError(error, QCoreApplication::translate(
                "AppleMediaTransport", "The Mac has an invalid media address."));
        return false;
    }
    m_RemoteAddress = remoteAddress;
    m_ControlRemotePort = basePort;
    m_VideoRemotePort = basePort + 1;
    m_ControlSocket = std::make_unique<QUdpSocket>();
    m_VideoSocket = std::make_unique<QUdpSocket>();
    if (!bindSocket(m_ControlSocket.get(), basePort, error) ||
            !bindSocket(m_VideoSocket.get(), basePort + 1, error)) {
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

void AppleMediaTransport::configureRemotePorts(const AppleMediaPorts& ports)
{
    if (ports.isUsable()) {
        m_ControlRemotePort = ports.audio;
        m_VideoRemotePort = ports.video;
    }
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
    if (!isOpen() || datagram == nullptr) {
        return false;
    }
    if (m_VideoSocket->state() != QAbstractSocket::BoundState) {
        setError(error, QCoreApplication::translate(
                "AppleMediaTransport", "The video UDP socket became unavailable: %1")
                .arg(m_VideoSocket->errorString()));
        return false;
    }
    punchIfDue();
    if (!m_VideoSocket->hasPendingDatagrams() &&
            !m_VideoSocket->waitForReadyRead(qMax(0, timeoutMilliseconds))) {
        if (m_VideoSocket->error() != QAbstractSocket::SocketTimeoutError &&
                !isCancelled(cancelled)) {
            setError(error, QCoreApplication::translate(
                    "AppleMediaTransport", "The video socket failed: %1")
                    .arg(m_VideoSocket->errorString()));
        }
        return false;
    }
    if (isCancelled(cancelled) || !m_VideoSocket->hasPendingDatagrams()) {
        return false;
    }
    QByteArray buffer(MaximumDatagramLength, Qt::Uninitialized);
    const qint64 received = m_VideoSocket->readDatagram(buffer.data(), buffer.size());
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
    if (!m_VideoSocket || datagram.isEmpty() ||
            m_VideoSocket->writeDatagram(datagram, m_RemoteAddress,
                                         m_VideoRemotePort) != datagram.size()) {
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
    m_RemoteAddress.clear();
    m_ControlRemotePort = 0;
    m_VideoRemotePort = 0;
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
        AppleMediaNegotiationResult* result,
        std::atomic_bool* cancelled,
        QString* error) const
{
    if (result == nullptr || !media.open(tcp.peerAddress(), basePort, error)) {
        return false;
    }
    tcp.setWaitCallback([&media]() { media.punchIfDue(); });

    AppleMediaNegotiationResult candidate;
    candidate.offers = AppleMediaWire::createOffers(
            audioEnabled, QSysInfo::prettyProductName(), error);
    candidate.keys.audioViewer = randomBytes(AppleMediaKeys::BlobLength, error);
    candidate.keys.audioServer = randomBytes(AppleMediaKeys::BlobLength, error);
    candidate.keys.videoViewer = randomBytes(AppleMediaKeys::BlobLength, error);
    candidate.keys.videoServer = randomBytes(AppleMediaKeys::BlobLength, error);
    candidate.configuration = AppleMediaWire::configuration(
            candidate.offers, candidate.keys, QUuid::createUuid(), error);
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
    if (!control.sendEncrypted(tcp, candidate.configuration, cancelled, error)) {
        tcp.setWaitCallback({});
        return false;
    }

    int retryCount = 0;
    bool configuredPorts = false;
    while (!isCancelled(cancelled) && retryCount <= NegotiationRetryLimit) {
        QByteArray response;
        if (!control.receiveEncrypted(tcp, &response, cancelled, error)) {
            tcp.setWaitCallback({});
            return false;
        }
        if (!configuredPorts) {
            AppleMediaPorts ports;
            if (AppleMediaWire::parsePorts(response, &ports)) {
                media.configureRemotePorts(ports);
                configuredPorts = true;
            }
        }
        if (AppleMediaWire::parseCanvas(response, &candidate.canvas)) {
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
