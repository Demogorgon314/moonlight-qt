#pragma once

#include <QByteArray>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QtGlobal>

#include <optional>

struct AppleRemoteDeviceInfo
{
    QString modelIdentifier;
    QString deviceColor;
    QString enclosureColor;

    bool isValid() const { return !modelIdentifier.trimmed().isEmpty(); }
    std::optional<qint32> enclosureColorIndex() const;

    bool operator==(const AppleRemoteDeviceInfo& other) const
    {
        return modelIdentifier == other.modelIdentifier &&
                deviceColor == other.deviceColor &&
                enclosureColor == other.enclosureColor;
    }

    bool operator!=(const AppleRemoteDeviceInfo& other) const
    {
        return !(*this == other);
    }
};

Q_DECLARE_METATYPE(AppleRemoteDeviceInfo)

namespace AppleDeviceInfo {

AppleRemoteDeviceInfo fromTxtAttributes(
        const QMap<QByteArray, QByteArray>& attributes);
AppleRemoteDeviceInfo fromWirePayload(const QByteArray& payload);
QByteArray queryName(const QString& serviceName, const QString& serviceDomain);

// Returns an image URL only when the current platform can resolve Apple's
// model identifier. Callers retain their existing fallback when it is empty.
QString iconSource(const AppleRemoteDeviceInfo& deviceInfo);

} // namespace AppleDeviceInfo
