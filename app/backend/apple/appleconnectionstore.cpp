#include "appleconnectionstore.h"

#include <QSettings>
#include <QUuid>

#include <memory>

namespace {

QString normalizedServicePart(const QString& value)
{
    return value.trimmed().toLower();
}

std::unique_ptr<QSettings> createSettings(const QString& settingsFile)
{
    if (settingsFile.isEmpty()) {
        return std::make_unique<QSettings>();
    }
    return std::make_unique<QSettings>(settingsFile, QSettings::IniFormat);
}

} // namespace

bool AppleConnectionEndpoint::isValid() const
{
    return !host.trimmed().isEmpty() && port != 0;
}

QString AppleConnectionEndpoint::serviceKey() const
{
    if (serviceName.isEmpty() || serviceType.isEmpty()) {
        return {};
    }
    return normalizedServicePart(serviceName) + QLatin1Char('\n') +
           normalizedServicePart(serviceType) + QLatin1Char('\n') +
           normalizedServicePart(serviceDomain);
}

QString AppleConnectionEndpoint::displayAddress() const
{
    const bool needsBrackets = host.contains(QLatin1Char(':')) &&
                               !host.startsWith(QLatin1Char('['));
    const QString formattedHost = needsBrackets ? QStringLiteral("[%1]").arg(host) : host;
    return port == 5900 ? formattedHost
                        : QStringLiteral("%1:%2").arg(formattedHost).arg(port);
}

bool AppleSavedConnection::isValid() const
{
    return !id.isEmpty() && !displayName.trimmed().isEmpty() && endpoint.isValid();
}

AppleConnectionStore::AppleConnectionStore(QString settingsFile)
    : m_SettingsFile(std::move(settingsFile))
{
    load();
}

AppleSavedConnection AppleConnectionStore::connection(const QString& id, bool* found) const
{
    const int index = indexOf(id);
    if (found != nullptr) {
        *found = index >= 0;
    }
    return index >= 0 ? m_Connections.at(index) : AppleSavedConnection();
}

AppleSavedConnection AppleConnectionStore::saveDiscovered(
        const AppleConnectionEndpoint& endpoint,
        const QString& displayName,
        const AppleRemoteDeviceInfo& deviceInfo)
{
    const QString serviceKey = endpoint.serviceKey();
    if (!serviceKey.isEmpty()) {
        for (AppleSavedConnection& existing : m_Connections) {
            if (existing.endpoint.serviceKey() == serviceKey) {
                existing.endpoint = endpoint;
                if (deviceInfo.isValid()) {
                    existing.deviceInfo = deviceInfo;
                }
                ++existing.revision;
                persist();
                return existing;
            }
        }
    }

    AppleSavedConnection connection;
    connection.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    connection.displayName = displayName.trimmed().isEmpty()
            ? endpoint.displayAddress()
            : displayName.trimmed();
    connection.endpoint = endpoint;
    if (deviceInfo.isValid()) {
        connection.deviceInfo = deviceInfo;
    }
    m_Connections.append(connection);
    persist();
    return connection;
}

bool AppleConnectionStore::remove(const QString& id, AppleSavedConnection* removed)
{
    const int index = indexOf(id);
    if (index < 0) {
        return false;
    }
    if (removed != nullptr) {
        *removed = m_Connections.at(index);
    }
    m_Connections.removeAt(index);
    persist();
    return true;
}

bool AppleConnectionStore::rename(const QString& id, const QString& displayName)
{
    const QString normalizedName = displayName.trimmed();
    const int index = indexOf(id);
    if (index < 0 || normalizedName.isEmpty()) {
        return false;
    }
    AppleSavedConnection& connection = m_Connections[index];
    if (connection.displayName == normalizedName) {
        return true;
    }
    connection.displayName = normalizedName;
    ++connection.revision;
    persist();
    return true;
}

bool AppleConnectionStore::updateDiscoveredEndpoint(
        const QString& serviceKey,
        const AppleConnectionEndpoint& endpoint)
{
    if (serviceKey.isEmpty() || !endpoint.isValid()) {
        return false;
    }
    for (AppleSavedConnection& connection : m_Connections) {
        if (connection.endpoint.serviceKey() != serviceKey) {
            continue;
        }
        if (connection.endpoint.host == endpoint.host &&
                connection.endpoint.port == endpoint.port &&
                connection.endpoint.serviceName == endpoint.serviceName &&
                connection.endpoint.serviceType == endpoint.serviceType &&
                connection.endpoint.serviceDomain == endpoint.serviceDomain) {
            return false;
        }
        connection.endpoint = endpoint;
        ++connection.revision;
        persist();
        return true;
    }
    return false;
}

bool AppleConnectionStore::setTrust(const QString& id, const QString& fingerprint)
{
    const int index = indexOf(id);
    const QString normalizedFingerprint = fingerprint.trimmed().toLower();
    if (index < 0 || normalizedFingerprint.isEmpty()) {
        return false;
    }
    AppleSavedConnection& connection = m_Connections[index];
    if (connection.trustedHostFingerprint == normalizedFingerprint) {
        return true;
    }

    // A changed host identity invalidates the old credential binding. This prevents
    // an explicitly accepted replacement host from inheriting the previous secret.
    connection.trustedHostFingerprint = normalizedFingerprint;
    connection.credentialReference.clear();
    connection.preferredUsername.clear();
    ++connection.revision;
    persist();
    return true;
}

bool AppleConnectionStore::setCredentialBinding(
        const QString& id,
        const QString& credentialReference,
        const QString& preferredUsername)
{
    const int index = indexOf(id);
    if (index < 0 || credentialReference.isEmpty() ||
            m_Connections.at(index).trustedHostFingerprint.isEmpty()) {
        return false;
    }
    AppleSavedConnection& connection = m_Connections[index];
    connection.credentialReference = credentialReference;
    connection.preferredUsername = preferredUsername.trimmed();
    ++connection.revision;
    persist();
    return true;
}

bool AppleConnectionStore::clearCredentialBinding(const QString& id)
{
    const int index = indexOf(id);
    if (index < 0) {
        return false;
    }
    AppleSavedConnection& connection = m_Connections[index];
    if (connection.credentialReference.isEmpty() && connection.preferredUsername.isEmpty()) {
        return true;
    }
    connection.credentialReference.clear();
    connection.preferredUsername.clear();
    ++connection.revision;
    persist();
    return true;
}

bool AppleConnectionStore::setDeviceInfo(
        const QString& id,
        const AppleRemoteDeviceInfo& deviceInfo)
{
    const int index = indexOf(id);
    if (index < 0 || !deviceInfo.isValid()) {
        return false;
    }
    AppleSavedConnection& connection = m_Connections[index];
    if (connection.deviceInfo == deviceInfo) {
        return true;
    }
    connection.deviceInfo = deviceInfo;
    ++connection.revision;
    persist();
    return true;
}

bool AppleConnectionStore::setVirtualDisplayCount(const QString& id,
                                                   int displayCount)
{
    const int index = indexOf(id);
    if (index < 0 || displayCount < 1 || displayCount > 2) {
        return false;
    }
    AppleSavedConnection& connection = m_Connections[index];
    if (connection.virtualDisplayCount == displayCount) {
        return true;
    }
    connection.virtualDisplayCount = displayCount;
    ++connection.revision;
    persist();
    return true;
}

bool AppleConnectionStore::setSharedClipboardEnabled(
        const QString& id,
        bool enabled)
{
    const int index = indexOf(id);
    if (index < 0) {
        return false;
    }
    AppleSavedConnection& connection = m_Connections[index];
    if (connection.sharedClipboardEnabled == enabled) {
        return true;
    }
    connection.sharedClipboardEnabled = enabled;
    ++connection.revision;
    persist();
    return true;
}

bool AppleConnectionStore::setKeyboardInputSourceSharingEnabled(
        const QString& id,
        bool enabled)
{
    const int index = indexOf(id);
    if (index < 0) {
        return false;
    }
    AppleSavedConnection& connection = m_Connections[index];
    if (connection.keyboardInputSourceSharingEnabled == enabled) {
        return true;
    }
    connection.keyboardInputSourceSharingEnabled = enabled;
    ++connection.revision;
    persist();
    return true;
}

void AppleConnectionStore::load()
{
    const std::unique_ptr<QSettings> settings = createSettings(m_SettingsFile);
    settings->beginGroup(QStringLiteral("appleScreenSharing"));
    const int count = settings->beginReadArray(QStringLiteral("connections"));
    for (int i = 0; i < count; ++i) {
        settings->setArrayIndex(i);
        AppleSavedConnection connection;
        connection.id = settings->value(QStringLiteral("id")).toString();
        connection.displayName = settings->value(QStringLiteral("displayName")).toString();
        connection.endpoint.host = settings->value(QStringLiteral("host")).toString();
        connection.endpoint.port = static_cast<quint16>(
                settings->value(QStringLiteral("port"), 5900).toUInt());
        connection.endpoint.serviceName = settings->value(QStringLiteral("serviceName")).toString();
        connection.endpoint.serviceType = settings->value(QStringLiteral("serviceType")).toString();
        connection.endpoint.serviceDomain = settings->value(QStringLiteral("serviceDomain")).toString();
        connection.trustedHostFingerprint =
                settings->value(QStringLiteral("trustedHostFingerprint")).toString().toLower();
        connection.credentialReference =
                settings->value(QStringLiteral("credentialReference")).toString();
        connection.preferredUsername =
                settings->value(QStringLiteral("preferredUsername")).toString();
        connection.deviceInfo.modelIdentifier =
                settings->value(QStringLiteral("deviceModelIdentifier")).toString();
        connection.deviceInfo.deviceColor =
                settings->value(QStringLiteral("deviceColor")).toString();
        connection.deviceInfo.enclosureColor =
                settings->value(QStringLiteral("deviceEnclosureColor")).toString();
        connection.virtualDisplayCount = qBound(
                1,
                settings->value(QStringLiteral("virtualDisplayCount"), 1).toInt(),
                2);
        connection.sharedClipboardEnabled = settings->value(
                QStringLiteral("sharedClipboardEnabled"), true).toBool();
        connection.keyboardInputSourceSharingEnabled = settings->value(
                QStringLiteral("keyboardInputSourceSharingEnabled"),
                false).toBool();
        connection.revision = settings->value(QStringLiteral("revision"), 1).toULongLong();
        if (connection.isValid()) {
            m_Connections.append(connection);
        }
    }
    settings->endArray();
    settings->endGroup();
}

void AppleConnectionStore::persist() const
{
    const std::unique_ptr<QSettings> settings = createSettings(m_SettingsFile);
    settings->beginGroup(QStringLiteral("appleScreenSharing"));
    settings->remove(QStringLiteral("connections"));
    settings->beginWriteArray(QStringLiteral("connections"), m_Connections.count());
    for (int i = 0; i < m_Connections.count(); ++i) {
        settings->setArrayIndex(i);
        const AppleSavedConnection& connection = m_Connections.at(i);
        settings->setValue(QStringLiteral("id"), connection.id);
        settings->setValue(QStringLiteral("displayName"), connection.displayName);
        settings->setValue(QStringLiteral("host"), connection.endpoint.host);
        settings->setValue(QStringLiteral("port"), connection.endpoint.port);
        settings->setValue(QStringLiteral("serviceName"), connection.endpoint.serviceName);
        settings->setValue(QStringLiteral("serviceType"), connection.endpoint.serviceType);
        settings->setValue(QStringLiteral("serviceDomain"), connection.endpoint.serviceDomain);
        settings->setValue(QStringLiteral("trustedHostFingerprint"),
                           connection.trustedHostFingerprint);
        settings->setValue(QStringLiteral("credentialReference"),
                           connection.credentialReference);
        settings->setValue(QStringLiteral("preferredUsername"), connection.preferredUsername);
        settings->setValue(QStringLiteral("deviceModelIdentifier"),
                           connection.deviceInfo.modelIdentifier);
        settings->setValue(QStringLiteral("deviceColor"),
                           connection.deviceInfo.deviceColor);
        settings->setValue(QStringLiteral("deviceEnclosureColor"),
                           connection.deviceInfo.enclosureColor);
        settings->setValue(QStringLiteral("virtualDisplayCount"),
                           connection.virtualDisplayCount);
        settings->setValue(QStringLiteral("sharedClipboardEnabled"),
                           connection.sharedClipboardEnabled);
        settings->setValue(
                QStringLiteral("keyboardInputSourceSharingEnabled"),
                connection.keyboardInputSourceSharingEnabled);
        settings->setValue(QStringLiteral("revision"), connection.revision);
    }
    settings->endArray();
    settings->endGroup();
    settings->sync();
}

int AppleConnectionStore::indexOf(const QString& id) const
{
    for (int i = 0; i < m_Connections.count(); ++i) {
        if (m_Connections.at(i).id == id) {
            return i;
        }
    }
    return -1;
}
