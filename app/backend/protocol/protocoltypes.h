#pragma once

#include <QMetaType>
#include <QString>

enum class ProtocolKind
{
    Moonlight,
    AppleScreenSharing,
};

QString protocolKindName(ProtocolKind protocol);

class ConnectionIdentity
{
public:
    ConnectionIdentity() = default;
    ConnectionIdentity(ProtocolKind protocol, QString stableId);

    ProtocolKind protocol() const { return m_Protocol; }
    const QString& stableId() const { return m_StableId; }
    bool isValid() const { return !m_StableId.isEmpty(); }

    QString toString() const;
    static ConnectionIdentity fromString(const QString& value, bool* ok = nullptr);

    friend bool operator==(const ConnectionIdentity& lhs, const ConnectionIdentity& rhs)
    {
        return lhs.m_Protocol == rhs.m_Protocol && lhs.m_StableId == rhs.m_StableId;
    }

    friend bool operator!=(const ConnectionIdentity& lhs, const ConnectionIdentity& rhs)
    {
        return !(lhs == rhs);
    }

private:
    ProtocolKind m_Protocol = ProtocolKind::Moonlight;
    QString m_StableId;
};

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
inline size_t qHash(const ConnectionIdentity& identity, size_t seed = 0)
#else
inline uint qHash(const ConnectionIdentity& identity, uint seed = 0)
#endif
{
    return qHash(identity.toString(), seed);
}

enum ConnectionCapability
{
    NoConnectionCapabilities = 0,
    CanRenameConnection = 1 << 0,
    CanDeleteConnection = 1 << 1,
    CanWakeConnection = 1 << 2,
    CanPairConnection = 1 << 3,
    CanBrowseActivities = 1 << 4,
    CanQuitActivity = 1 << 5,
    CanSelectEndpoint = 1 << 6,
    CanSaveConnection = 1 << 7,
    CanAuthenticateConnection = 1 << 8,
};
Q_DECLARE_FLAGS(ConnectionCapabilities, ConnectionCapability)
Q_DECLARE_OPERATORS_FOR_FLAGS(ConnectionCapabilities)

struct CatalogConnectionView
{
    ConnectionIdentity identity;
    QString displayName;
    QString protocolName;
    QString iconSource;
    QString details;
    bool online = false;
    bool paired = false;
    bool busy = false;
    bool wakeable = false;
    bool statusUnknown = true;
    bool serverSupported = true;
    bool persistent = true;
    bool trusted = false;
    bool directLaunch = false;
    QString authenticationKind = QStringLiteral("pin");
    ConnectionCapabilities capabilities;
};

Q_DECLARE_METATYPE(ConnectionIdentity)
Q_DECLARE_METATYPE(CatalogConnectionView)
