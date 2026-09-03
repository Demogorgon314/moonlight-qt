#pragma once

#include <QList>
#include <QString>

struct AppleConnectionEndpoint
{
    QString host;
    quint16 port = 5900;
    QString serviceName;
    QString serviceType;
    QString serviceDomain;

    bool isValid() const;
    QString serviceKey() const;
    QString displayAddress() const;
};

struct AppleSavedConnection
{
    QString id;
    QString displayName;
    AppleConnectionEndpoint endpoint;
    QString trustedHostFingerprint;
    QString credentialReference;
    QString preferredUsername;
    int virtualDisplayCount = 1;
    bool sharedClipboardEnabled = true;
    bool keyboardInputSourceSharingEnabled = false;
    quint64 revision = 1;

    bool isValid() const;
    bool isTrusted() const { return !trustedHostFingerprint.isEmpty(); }
    bool hasCredentialBinding() const { return !credentialReference.isEmpty(); }
};

class AppleConnectionStore
{
public:
    explicit AppleConnectionStore(QString settingsFile = QString());

    QList<AppleSavedConnection> connections() const { return m_Connections; }
    AppleSavedConnection connection(const QString& id, bool* found = nullptr) const;

    AppleSavedConnection saveDiscovered(const AppleConnectionEndpoint& endpoint,
                                         const QString& displayName);
    bool remove(const QString& id, AppleSavedConnection* removed = nullptr);
    bool rename(const QString& id, const QString& displayName);
    bool updateDiscoveredEndpoint(const QString& serviceKey,
                                  const AppleConnectionEndpoint& endpoint);
    bool setTrust(const QString& id, const QString& fingerprint);
    bool setCredentialBinding(const QString& id,
                              const QString& credentialReference,
                              const QString& preferredUsername);
    bool clearCredentialBinding(const QString& id);
    bool setVirtualDisplayCount(const QString& id, int displayCount);
    bool setSharedClipboardEnabled(const QString& id, bool enabled);
    bool setKeyboardInputSourceSharingEnabled(const QString& id, bool enabled);

private:
    void load();
    void persist() const;
    int indexOf(const QString& id) const;

    QString m_SettingsFile;
    QList<AppleSavedConnection> m_Connections;
};
