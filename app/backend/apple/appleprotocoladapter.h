#pragma once

#include "appleauthenticator.h"
#include "appleconnectionstore.h"
#include "backend/protocol/protocoladapter.h"

#include <QHash>
#include <QHostAddress>
#include <QSharedPointer>

namespace QMdnsEngine {
class Browser;
class Resolver;
class Server;
class Service;
}

struct AppleDiscoveredConnection
{
    QString id;
    QString displayName;
    AppleConnectionEndpoint endpoint;
    QList<QHostAddress> addresses;
};

class AppleProtocolAdapter final : public ProtocolAdapter
{
    Q_OBJECT

public:
    explicit AppleProtocolAdapter(
            const QSharedPointer<QMdnsEngine::Server>& mdnsServer,
            QObject* parent = nullptr);
    ~AppleProtocolAdapter() override;

    ProtocolKind protocol() const override { return ProtocolKind::AppleScreenSharing; }
    bool isAvailable() const override;
    QVector<CatalogConnectionView> connections() const override;
    bool contains(const ConnectionIdentity& identity) const override;

    void startDiscovery() override;
    void stopDiscoveryAsync() override;
    void addManualConnection(const QString& address) override;

    QString generatePairingSecret() const override { return {}; }
    void pair(const ConnectionIdentity& identity, const QString& secret) override;
    void deleteConnection(const ConnectionIdentity& identity) override;
    void renameConnection(const ConnectionIdentity& identity, const QString& name) override;
    void wakeConnection(const ConnectionIdentity&) override {}
    void quitRunningActivity(const ConnectionIdentity&) override {}

    QString saveConnection(const ConnectionIdentity& identity, QString* error) override;
    void requestAuthentication(const ConnectionIdentity& identity) override;
    void confirmHostTrust(const ConnectionIdentity& identity, bool accepted) override;
    void submitCredentials(const ConnectionIdentity& identity,
                           const QString& username,
                           const QString& password) override;

    QVariantList connectionEndpoints(const ConnectionIdentity& identity) const override;
    bool hasMultipleEndpoints(const ConnectionIdentity&) const override { return false; }
    bool selectEndpoint(const ConnectionIdentity&, const QString&, int) override { return false; }
    bool selectAutomaticEndpoint(const ConnectionIdentity&) override { return false; }
    QVariantMap activeEndpoint(const ConnectionIdentity& identity) const override;

    std::unique_ptr<ResolvedLaunchPlan> resolveLaunch(const LaunchRequest& request,
                                                       QString* error) const override;
    StreamSession* createSession(std::unique_ptr<ResolvedLaunchPlan> plan,
                                 QString* error) const override;

    // Worker completions are public only to keep QRunnable independent of Qt's moc.
    // They remain implementation details because callers use ProtocolAdapter signals.
    void completeTrustProbe(QString connectionId,
                            quint64 revision,
                            quint64 generation,
                            QString fingerprint,
                            QString error);
    void completeAuthentication(QString connectionId,
                                quint64 revision,
                                quint64 generation,
                                AppleCredentials credentials,
                                QString error);

private:
    void handleServiceAddedOrUpdated(const QMdnsEngine::Service& service);
    void handleServiceRemoved(const QMdnsEngine::Service& service);
    void resolveService(const QString& serviceKey);
    ConnectionIdentity identityForDiscovery(const QString& serviceKey) const;
    AppleDiscoveredConnection discovered(const ConnectionIdentity& identity,
                                         bool* found = nullptr) const;
    CatalogConnectionView savedView(const AppleSavedConnection& connection) const;
    CatalogConnectionView discoveredView(const AppleDiscoveredConnection& connection) const;
    AppleConnectionEndpoint currentEndpoint(const AppleSavedConnection& connection) const;

    AppleConnectionStore m_Store;
    QHash<QString, AppleDiscoveredConnection> m_Discovered;
    QHash<QString, QMdnsEngine::Resolver*> m_Resolvers;
    QHash<QString, QString> m_PendingTrust;
    QHash<QString, quint64> m_AuthenticationGenerations;
    QSharedPointer<QMdnsEngine::Server> m_Server;
    QMdnsEngine::Browser* m_Browser = nullptr;
    int m_DiscoveryReferences = 0;
    quint64 m_NextAuthenticationGeneration = 1;
};
