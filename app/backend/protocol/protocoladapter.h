#pragma once

#include "protocoltypes.h"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <memory>

class ResolvedLaunchPlan;
class StreamSession;
class StreamingPreferences;

struct LaunchRequest
{
    ConnectionIdentity connection;
    QString activityId;
    QString displayTarget;
    StreamingPreferences* preferences = nullptr;
};

class ProtocolAdapter : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~ProtocolAdapter() override = default;

    virtual ProtocolKind protocol() const = 0;
    virtual bool isAvailable() const = 0;
    virtual QVector<CatalogConnectionView> connections() const = 0;
    virtual bool contains(const ConnectionIdentity& identity) const = 0;

    virtual void startDiscovery() = 0;
    virtual void stopDiscoveryAsync() = 0;
    virtual void addManualConnection(const QString& address) = 0;

    virtual QString generatePairingSecret() const = 0;
    virtual void pair(const ConnectionIdentity& identity, const QString& secret) = 0;
    virtual void deleteConnection(const ConnectionIdentity& identity) = 0;
    virtual void renameConnection(const ConnectionIdentity& identity, const QString& name) = 0;
    virtual void wakeConnection(const ConnectionIdentity& identity) = 0;
    virtual void quitRunningActivity(const ConnectionIdentity& identity) = 0;

    virtual QString saveConnection(const ConnectionIdentity&, QString* error)
    {
        if (error != nullptr) {
            *error = tr("This connection is already saved.");
        }
        return {};
    }
    virtual void requestAuthentication(const ConnectionIdentity&) {}
    virtual void cancelAuthentication(const ConnectionIdentity&) {}
    virtual void confirmHostTrust(const ConnectionIdentity&, bool) {}
    virtual void submitCredentials(const ConnectionIdentity&,
                                   const QString&,
                                   const QString&) {}

    virtual QVariantList connectionEndpoints(const ConnectionIdentity& identity) const = 0;
    virtual bool hasMultipleEndpoints(const ConnectionIdentity& identity) const = 0;
    virtual bool selectEndpoint(const ConnectionIdentity& identity,
                                const QString& address,
                                int port) = 0;
    virtual bool selectAutomaticEndpoint(const ConnectionIdentity& identity) = 0;
    virtual QVariantMap activeEndpoint(const ConnectionIdentity& identity) const = 0;
    virtual QVariantMap sessionOptions(const ConnectionIdentity&) const
    {
        return {};
    }
    virtual bool setSessionOptions(const ConnectionIdentity&,
                                   const QVariantMap&,
                                   QString* error)
    {
        if (error != nullptr) {
            *error = tr("This connection has no configurable session options.");
        }
        return false;
    }

    virtual std::unique_ptr<ResolvedLaunchPlan> resolveLaunch(const LaunchRequest& request,
                                                               QString* error) const = 0;
    virtual StreamSession* createSession(std::unique_ptr<ResolvedLaunchPlan> plan,
                                         QString* error) const = 0;

signals:
    void connectionChanged(QString connectionId);
    void pairingCompleted(QString connectionId, QString error);
    void connectionAddCompleted(QVariant success, QVariant detectedPortBlocking);
    void quitActivityCompleted(QVariant error);
    void connectionSaved(QString sourceConnectionId, QString savedConnectionId, QString error);
    void hostTrustRequired(QString connectionId,
                           QString displayName,
                           QString fingerprint,
                           bool identityChanged);
    void credentialsRequired(QString connectionId, QString preferredUsername);
    void authenticationCompleted(QString connectionId, QString error);
};
