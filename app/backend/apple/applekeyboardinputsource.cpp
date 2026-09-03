#include "applekeyboardinputsource.h"

#include "appleprotocol.h"

#include <limits>

namespace {

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
}

} // namespace

namespace AppleKeyboardInputSourceWire {

QByteArray sharingMessage(const QString& identifier, QString* error)
{
    const QByteArray utf8Identifier = identifier.toUtf8();
    constexpr int HeaderPayloadBytes = 4;
    const int maximumIdentifierBytes =
            std::numeric_limits<quint16>::max() - HeaderPayloadBytes;
    if (utf8Identifier.size() > maximumIdentifierBytes) {
        setError(error, QStringLiteral("Keyboard input source identifier is too long"));
        return {};
    }

    QByteArray message;
    message.reserve(8 + utf8Identifier.size());
    message.append(char(0x1a));
    message.append(char(0));
    AppleWire::appendUInt16(
            message,
            static_cast<quint16>(HeaderPayloadBytes + utf8Identifier.size()));
    AppleWire::appendUInt16(message, 1);
    AppleWire::appendUInt16(
            message, static_cast<quint16>(utf8Identifier.size()));
    message.append(utf8Identifier);
    return message;
}

} // namespace AppleKeyboardInputSourceWire

std::optional<QByteArray> AppleKeyboardInputSourceSharing::setSharingEnabled(
        bool enabled)
{
    if (m_SharingEnabled == enabled) {
        return std::nullopt;
    }
    m_SharingEnabled = enabled;
    if (!m_Supported || !m_Controlling) {
        return std::nullopt;
    }
    return enabled ? currentSharingMessage()
                   : std::optional<QByteArray>(
                           AppleKeyboardInputSourceWire::sharingMessage({}));
}

std::optional<QByteArray> AppleKeyboardInputSourceSharing::setControlling(
        bool controlling)
{
    if (m_Controlling == controlling) {
        return std::nullopt;
    }
    m_Controlling = controlling;
    return currentSharingMessage();
}

std::optional<QByteArray> AppleKeyboardInputSourceSharing::setLocalIdentifier(
        const QString& identifier)
{
    if (m_LocalIdentifier == identifier) {
        return std::nullopt;
    }
    m_LocalIdentifier = identifier;
    return currentSharingMessage();
}

std::optional<QByteArray> AppleKeyboardInputSourceSharing::receiveRemoteState(
        const AppleKeyboardInputSourceState& state)
{
    m_Supported = true;
    m_SecureInput = state.secureInput;
    m_RemoteIdentifier = state.identifier;
    return currentSharingMessage();
}

void AppleKeyboardInputSourceSharing::resetForReconnect()
{
    m_Supported = false;
    m_RemoteIdentifier.clear();
    m_SecureInput = false;
}

bool AppleKeyboardInputSourceSharing::usesSameInputSource() const
{
    return m_Supported &&
            ((m_SharingEnabled && !m_LocalIdentifier.isEmpty()) ||
             (!m_LocalIdentifier.isEmpty() &&
              m_LocalIdentifier == m_RemoteIdentifier));
}

quint8 AppleKeyboardInputSourceSharing::keyEventSubtype() const
{
    return usesSameInputSource() ? 3 : 1;
}

std::optional<QByteArray>
AppleKeyboardInputSourceSharing::currentSharingMessage() const
{
    if (!m_Supported || !m_SharingEnabled || !m_Controlling ||
            m_LocalIdentifier.isEmpty()) {
        return std::nullopt;
    }
    return AppleKeyboardInputSourceWire::sharingMessage(m_LocalIdentifier);
}
