#include "protocoltypes.h"

#include <QStringView>

QString protocolKindName(ProtocolKind protocol)
{
    switch (protocol) {
    case ProtocolKind::Moonlight:
        return QStringLiteral("moonlight");
    case ProtocolKind::AppleScreenSharing:
        return QStringLiteral("apple-screen-sharing");
    }

    Q_UNREACHABLE();
}

ConnectionIdentity::ConnectionIdentity(ProtocolKind protocol, QString stableId)
    : m_Protocol(protocol),
      m_StableId(std::move(stableId))
{
}

QString ConnectionIdentity::toString() const
{
    if (!isValid()) {
        return QString();
    }

    return protocolKindName(m_Protocol) + QLatin1Char(':') + m_StableId;
}

ConnectionIdentity ConnectionIdentity::fromString(const QString& value, bool* ok)
{
    const qsizetype separator = value.indexOf(QLatin1Char(':'));
    if (separator <= 0 || separator == value.size() - 1) {
        if (ok != nullptr) {
            *ok = false;
        }
        return {};
    }

    const QStringView protocol = QStringView(value).left(separator);
    ProtocolKind kind;
    if (protocol == QStringLiteral("moonlight")) {
        kind = ProtocolKind::Moonlight;
    }
    else if (protocol == QStringLiteral("apple-screen-sharing")) {
        kind = ProtocolKind::AppleScreenSharing;
    }
    else {
        if (ok != nullptr) {
            *ok = false;
        }
        return {};
    }

    if (ok != nullptr) {
        *ok = true;
    }
    return ConnectionIdentity(kind, value.mid(separator + 1));
}

