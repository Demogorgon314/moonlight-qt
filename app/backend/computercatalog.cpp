#include "computercatalog.h"

#include "protocol/moonlightprotocoladapter.h"
#include "protocol/protocoladapter.h"
#include "protocol/resolvedlaunchplan.h"
#include "streaming/streamsession.h"

#ifdef MOONLIGHT_ENABLE_APPLE_SCREEN_SHARING
#include "apple/applefeaturegate.h"
#endif

#include <QDebug>

ComputerCatalog::ComputerCatalog(StreamingPreferences* preferences, QObject* parent)
    : QObject(parent),
      m_Moonlight(new MoonlightProtocolAdapter(preferences))
{
    m_Adapters.emplace_back(m_Moonlight);

    connect(m_Moonlight, &ProtocolAdapter::connectionChanged,
            this, &ComputerCatalog::connectionChanged);
    connect(m_Moonlight, &ProtocolAdapter::pairingCompleted,
            this, &ComputerCatalog::pairingCompleted);
    connect(m_Moonlight, &ProtocolAdapter::connectionAddCompleted,
            this, &ComputerCatalog::computerAddCompleted);
    connect(m_Moonlight, &ProtocolAdapter::quitActivityCompleted,
            this, &ComputerCatalog::quitAppCompleted);
}

ComputerCatalog::~ComputerCatalog() = default;

QVector<CatalogConnectionView> ComputerCatalog::connections() const
{
    QVector<CatalogConnectionView> result;
    for (const auto& adapter : m_Adapters) {
        if (adapter->isAvailable()) {
            const QVector<CatalogConnectionView> adapterConnections = adapter->connections();
            for (const CatalogConnectionView& connection : adapterConnections) {
                result.append(connection);
            }
        }
    }
    return result;
}

CatalogConnectionView ComputerCatalog::connection(const QString& connectionId,
                                                   bool* found) const
{
    bool valid = false;
    const ConnectionIdentity identity = ConnectionIdentity::fromString(connectionId, &valid);
    if (valid) {
        if (ProtocolAdapter* adapter = adapterFor(identity)) {
            for (const CatalogConnectionView& view : adapter->connections()) {
                if (view.identity == identity) {
                    if (found != nullptr) {
                        *found = true;
                    }
                    return view;
                }
            }
        }
    }
    if (found != nullptr) {
        *found = false;
    }
    return {};
}

void ComputerCatalog::startPolling()
{
    for (const auto& adapter : m_Adapters) {
        if (adapter->isAvailable()) {
            adapter->startDiscovery();
        }
    }
}

void ComputerCatalog::stopPollingAsync()
{
    for (const auto& adapter : m_Adapters) {
        if (adapter->isAvailable()) {
            adapter->stopDiscoveryAsync();
        }
    }
}

void ComputerCatalog::addNewHostManually(QString address)
{
    // Stage 1 preserves the existing add-host behavior. Apple registration is
    // intentionally absent, so manual additions are unambiguously Moonlight.
    m_Moonlight->addManualConnection(address);
}

QString ComputerCatalog::generatePairingSecret(const QString& connectionId) const
{
    if (connectionId.isEmpty()) {
        return m_Moonlight->generatePairingSecret();
    }
    bool valid = false;
    const ConnectionIdentity identity = ConnectionIdentity::fromString(connectionId, &valid);
    ProtocolAdapter* adapter = valid ? adapterFor(identity) : nullptr;
    return adapter != nullptr ? adapter->generatePairingSecret() : QString();
}

void ComputerCatalog::pairConnection(const QString& connectionId, const QString& secret)
{
    bool valid = false;
    const ConnectionIdentity identity = ConnectionIdentity::fromString(connectionId, &valid);
    if (ProtocolAdapter* adapter = valid ? adapterFor(identity) : nullptr) {
        adapter->pair(identity, secret);
    }
}

#define ROUTE_CONNECTION_OPERATION(method, ...) \
    bool valid = false; \
    const ConnectionIdentity identity = ConnectionIdentity::fromString(connectionId, &valid); \
    if (ProtocolAdapter* adapter = valid ? adapterFor(identity) : nullptr) { \
        adapter->method(identity, ##__VA_ARGS__); \
    }

void ComputerCatalog::deleteConnection(const QString& connectionId)
{
    ROUTE_CONNECTION_OPERATION(deleteConnection)
}

void ComputerCatalog::renameConnection(const QString& connectionId, const QString& name)
{
    ROUTE_CONNECTION_OPERATION(renameConnection, name)
}

void ComputerCatalog::wakeConnection(const QString& connectionId)
{
    ROUTE_CONNECTION_OPERATION(wakeConnection)
}

void ComputerCatalog::quitRunningActivity(const QString& connectionId)
{
    ROUTE_CONNECTION_OPERATION(quitRunningActivity)
}

#undef ROUTE_CONNECTION_OPERATION

QVariantList ComputerCatalog::connectionEndpoints(const QString& connectionId) const
{
    bool valid = false;
    const ConnectionIdentity identity = ConnectionIdentity::fromString(connectionId, &valid);
    ProtocolAdapter* adapter = valid ? adapterFor(identity) : nullptr;
    return adapter != nullptr ? adapter->connectionEndpoints(identity) : QVariantList();
}

bool ComputerCatalog::hasMultipleEndpoints(const QString& connectionId) const
{
    bool valid = false;
    const ConnectionIdentity identity = ConnectionIdentity::fromString(connectionId, &valid);
    ProtocolAdapter* adapter = valid ? adapterFor(identity) : nullptr;
    return adapter != nullptr && adapter->hasMultipleEndpoints(identity);
}

bool ComputerCatalog::selectEndpoint(const QString& connectionId,
                                     const QString& address,
                                     int port)
{
    bool valid = false;
    const ConnectionIdentity identity = ConnectionIdentity::fromString(connectionId, &valid);
    ProtocolAdapter* adapter = valid ? adapterFor(identity) : nullptr;
    return adapter != nullptr && adapter->selectEndpoint(identity, address, port);
}

bool ComputerCatalog::selectAutomaticEndpoint(const QString& connectionId)
{
    bool valid = false;
    const ConnectionIdentity identity = ConnectionIdentity::fromString(connectionId, &valid);
    ProtocolAdapter* adapter = valid ? adapterFor(identity) : nullptr;
    return adapter != nullptr && adapter->selectAutomaticEndpoint(identity);
}

QVariantMap ComputerCatalog::activeEndpoint(const QString& connectionId) const
{
    bool valid = false;
    const ConnectionIdentity identity = ConnectionIdentity::fromString(connectionId, &valid);
    ProtocolAdapter* adapter = valid ? adapterFor(identity) : nullptr;
    return adapter != nullptr ? adapter->activeEndpoint(identity) : QVariantMap();
}

std::unique_ptr<ResolvedLaunchPlan> ComputerCatalog::resolveLaunch(
        const QString& connectionId,
        const QString& activityId,
        const QString& displayTarget,
        StreamingPreferences* preferences,
        QString* error) const
{
    bool valid = false;
    const ConnectionIdentity identity = ConnectionIdentity::fromString(connectionId, &valid);
    ProtocolAdapter* adapter = valid ? adapterFor(identity) : nullptr;
    if (adapter == nullptr) {
        if (error != nullptr) {
            *error = tr("The selected connection is invalid or no longer available.");
        }
        return nullptr;
    }

    LaunchRequest request;
    request.connection = identity;
    request.activityId = activityId;
    request.displayTarget = displayTarget;
    request.preferences = preferences;
    return adapter->resolveLaunch(request, error);
}

StreamSession* ComputerCatalog::createSession(const QString& connectionId,
                                              const QString& activityId,
                                              const QString& displayTarget,
                                              StreamingPreferences* preferences,
                                              QString* error)
{
    std::unique_ptr<ResolvedLaunchPlan> plan = resolveLaunch(connectionId,
                                                             activityId,
                                                             displayTarget,
                                                             preferences,
                                                             error);
    if (!plan) {
        return nullptr;
    }

    ProtocolAdapter* adapter = adapterFor(plan->connection());
    StreamSession* session =
            adapter != nullptr ? adapter->createSession(std::move(plan), error) : nullptr;
    if (session == nullptr) {
        return nullptr;
    }

    // Catalog owns the session. QML and CLI receive a borrowed pointer which remains
    // valid through readyForDeletion(), then the Qt event loop performs destruction.
    session->setParent(this);
    connect(session, &StreamSession::readyForDeletion,
            session, &QObject::deleteLater,
            Qt::QueuedConnection);
    return session;
}

bool ComputerCatalog::appleScreenSharingCompiled() const
{
#ifdef MOONLIGHT_ENABLE_APPLE_SCREEN_SHARING
    return true;
#else
    return false;
#endif
}

bool ComputerCatalog::appleScreenSharingRuntimeEnabled() const
{
#ifdef MOONLIGHT_ENABLE_APPLE_SCREEN_SHARING
    return AppleFeatureGate::isRuntimeEnabled();
#else
    return false;
#endif
}

ComputerManager* ComputerCatalog::moonlightManager() const
{
    return m_Moonlight->manager();
}

NvComputer* ComputerCatalog::moonlightComputer(const QString& connectionId) const
{
    bool valid = false;
    const ConnectionIdentity identity = ConnectionIdentity::fromString(connectionId, &valid);
    return valid ? m_Moonlight->computer(identity) : nullptr;
}

QString ComputerCatalog::moonlightConnectionId(const NvComputer* computer) const
{
    return m_Moonlight->identityFor(computer).toString();
}

void ComputerCatalog::notifyMoonlightClientMetadataChanged(const QString& connectionId)
{
    bool valid = false;
    const ConnectionIdentity identity = ConnectionIdentity::fromString(connectionId, &valid);
    if (valid) {
        m_Moonlight->notifyClientMetadataChanged(identity);
    }
}

ProtocolAdapter* ComputerCatalog::adapterFor(const ConnectionIdentity& identity) const
{
    for (const auto& adapter : m_Adapters) {
        if (adapter->protocol() == identity.protocol() && adapter->isAvailable()) {
            return adapter.get();
        }
    }
    return nullptr;
}
