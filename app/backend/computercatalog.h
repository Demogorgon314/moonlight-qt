#pragma once

#include "protocol/protocoltypes.h"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <vector>

class ComputerManager;
class AppleProtocolAdapter;
class MoonlightProtocolAdapter;
class NvComputer;
class ResolvedLaunchPlan;
class StreamSession;
class StreamingPreferences;

class ComputerCatalog final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool appleScreenSharingCompiled READ appleScreenSharingCompiled CONSTANT)
    Q_PROPERTY(bool appleScreenSharingRuntimeEnabled READ appleScreenSharingRuntimeEnabled CONSTANT)

public:
    explicit ComputerCatalog(StreamingPreferences* preferences, QObject* parent = nullptr);
    ~ComputerCatalog() override;

    QVector<CatalogConnectionView> connections() const;
    CatalogConnectionView connection(const QString& connectionId, bool* found = nullptr) const;

    Q_INVOKABLE void startPolling();
    Q_INVOKABLE void stopPollingAsync();
    Q_INVOKABLE void addNewHostManually(QString address);

    QString generatePairingSecret(const QString& connectionId = QString()) const;
    void pairConnection(const QString& connectionId, const QString& secret);
    void deleteConnection(const QString& connectionId);
    void renameConnection(const QString& connectionId, const QString& name);
    void wakeConnection(const QString& connectionId);
    void quitRunningActivity(const QString& connectionId);
    QString saveConnection(const QString& connectionId, QString* error = nullptr);
    void requestAuthentication(const QString& connectionId);
    void confirmHostTrust(const QString& connectionId, bool accepted);
    void submitCredentials(const QString& connectionId,
                           const QString& username,
                           const QString& password);

    QVariantList connectionEndpoints(const QString& connectionId) const;
    bool hasMultipleEndpoints(const QString& connectionId) const;
    bool selectEndpoint(const QString& connectionId, const QString& address, int port);
    bool selectAutomaticEndpoint(const QString& connectionId);
    QVariantMap activeEndpoint(const QString& connectionId) const;

    std::unique_ptr<ResolvedLaunchPlan> resolveLaunch(const QString& connectionId,
                                                       const QString& activityId,
                                                       const QString& displayTarget,
                                                       StreamingPreferences* preferences,
                                                       QString* error = nullptr) const;
    StreamSession* createSession(const QString& connectionId,
                                 const QString& activityId,
                                 const QString& displayTarget = QString(),
                                 StreamingPreferences* preferences = nullptr,
                                 QString* error = nullptr);

    bool appleScreenSharingCompiled() const;
    bool appleScreenSharingRuntimeEnabled() const;

    // Moonlight's application library remains protocol-specific in stage 1. These
    // accessors keep that implementation behind the catalog while its QML model is
    // migrated to stable connection identity and catalog-owned session creation.
    ComputerManager* moonlightManager() const;
    NvComputer* moonlightComputer(const QString& connectionId) const;
    QString moonlightConnectionId(const NvComputer* computer) const;
    void notifyMoonlightClientMetadataChanged(const QString& connectionId);

signals:
    void connectionChanged(QString connectionId);
    void pairingCompleted(QString connectionId, QString error);
    void computerAddCompleted(QVariant success, QVariant detectedPortBlocking);
    void quitAppCompleted(QVariant error);
    void connectionSaved(QString sourceConnectionId, QString savedConnectionId, QString error);
    void hostTrustRequired(QString connectionId,
                           QString displayName,
                           QString fingerprint,
                           bool identityChanged);
    void credentialsRequired(QString connectionId, QString preferredUsername);
    void authenticationCompleted(QString connectionId, QString error);

private:
    class ProtocolAdapter* adapterFor(const ConnectionIdentity& identity) const;
    MoonlightProtocolAdapter* m_Moonlight;
    AppleProtocolAdapter* m_Apple = nullptr;
    std::vector<std::unique_ptr<class ProtocolAdapter>> m_Adapters;
};
