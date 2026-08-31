#pragma once

#include "protocoladapter.h"

#include <QHash>
#include <QSharedPointer>

#include <memory>

class ComputerManager;
class NvComputer;

namespace QMdnsEngine {
class Server;
}

class MoonlightProtocolAdapter final : public ProtocolAdapter
{
    Q_OBJECT

public:
    explicit MoonlightProtocolAdapter(StreamingPreferences* preferences,
                                      const QSharedPointer<QMdnsEngine::Server>& mdnsServer,
                                      QObject* parent = nullptr);
    ~MoonlightProtocolAdapter() override;

    ProtocolKind protocol() const override { return ProtocolKind::Moonlight; }
    bool isAvailable() const override { return true; }
    QVector<CatalogConnectionView> connections() const override;
    bool contains(const ConnectionIdentity& identity) const override;

    void startDiscovery() override;
    void stopDiscoveryAsync() override;
    void addManualConnection(const QString& address) override;

    QString generatePairingSecret() const override;
    void pair(const ConnectionIdentity& identity, const QString& secret) override;
    void deleteConnection(const ConnectionIdentity& identity) override;
    void renameConnection(const ConnectionIdentity& identity, const QString& name) override;
    void wakeConnection(const ConnectionIdentity& identity) override;
    void quitRunningActivity(const ConnectionIdentity& identity) override;

    QVariantList connectionEndpoints(const ConnectionIdentity& identity) const override;
    bool hasMultipleEndpoints(const ConnectionIdentity& identity) const override;
    bool selectEndpoint(const ConnectionIdentity& identity,
                        const QString& address,
                        int port) override;
    bool selectAutomaticEndpoint(const ConnectionIdentity& identity) override;
    QVariantMap activeEndpoint(const ConnectionIdentity& identity) const override;

    std::unique_ptr<ResolvedLaunchPlan> resolveLaunch(const LaunchRequest& request,
                                                       QString* error) const override;
    StreamSession* createSession(std::unique_ptr<ResolvedLaunchPlan> plan,
                                 QString* error) const override;

    ComputerManager* manager() const { return m_Manager.get(); }
    NvComputer* computer(const ConnectionIdentity& identity) const;
    ConnectionIdentity identityFor(const NvComputer* computer) const;
    void notifyClientMetadataChanged(const ConnectionIdentity& identity);

private slots:
    void handleComputerChanged(NvComputer* computer);
    void handlePairingCompleted(NvComputer* computer, QString error);

private:
    CatalogConnectionView viewFor(NvComputer* computer) const;
    quint64 revisionFor(const ConnectionIdentity& identity) const;

    std::unique_ptr<ComputerManager> m_Manager;
    mutable QHash<ConnectionIdentity, quint64> m_Revisions;
};
