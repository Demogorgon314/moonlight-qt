#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

struct AppleKeyboardInputSourceState
{
    quint16 field = 0;
    bool secureInput = false;
    QString identifier;
};

enum class AppleRemoteSystemCommand : quint32
{
    Launchpad = 0x1008fd00,
    ShowDesktop = 0x1008fd01,
    MissionControl = 0x1008fd02,
    ApplicationWindows = 0x1008fd03,
};

namespace AppleKeyboardInputSourceWire {

// Encodes the viewer-to-host input-source message. An empty identifier turns
// sharing off without relying on a separate capability or preference message.
QByteArray sharingMessage(const QString& identifier,
                          QString* error = nullptr);

} // namespace AppleKeyboardInputSourceWire

// Keeps input-source protocol decisions independent from the platform monitor.
// The caller owns persistence and sends a returned message in control-stream
// order. No message is produced until the host advertises support.
class AppleKeyboardInputSourceSharing
{
public:
    std::optional<QByteArray> setSharingEnabled(bool enabled);
    std::optional<QByteArray> setControlling(bool controlling);
    std::optional<QByteArray> setLocalIdentifier(const QString& identifier);
    std::optional<QByteArray> receiveRemoteState(
            const AppleKeyboardInputSourceState& state);
    void resetForReconnect();

    bool supported() const { return m_Supported; }
    bool sharingEnabled() const { return m_SharingEnabled; }
    bool controlling() const { return m_Controlling; }
    bool secureInput() const { return m_SecureInput; }
    QString localIdentifier() const { return m_LocalIdentifier; }
    QString remoteIdentifier() const { return m_RemoteIdentifier; }
    bool usesSameInputSource() const;
    quint8 keyEventSubtype() const;

private:
    std::optional<QByteArray> currentSharingMessage() const;

    QString m_LocalIdentifier;
    QString m_RemoteIdentifier;
    bool m_Supported = false;
    bool m_SharingEnabled = false;
    bool m_Controlling = true;
    bool m_SecureInput = false;
};
