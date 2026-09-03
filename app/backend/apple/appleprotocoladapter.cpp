#include "appleprotocoladapter.h"

#include "applefeaturegate.h"
#include "applescreensharingsession.h"
#include "backend/protocol/resolvedlaunchplan.h"

#include <qmdnsengine/browser.h>
#include <qmdnsengine/resolver.h>
#include <qmdnsengine/server.h>
#include <qmdnsengine/service.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>
#include <QUrl>

#include <utility>

namespace {

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
}

QString serviceDomain(const QString& type)
{
    return type.endsWith(QStringLiteral(".local."), Qt::CaseInsensitive)
            ? QStringLiteral("local.")
            : QString();
}

class AppleResolvedLaunchPlan final : public ResolvedLaunchPlan
{
public:
    explicit AppleResolvedLaunchPlan(AppleSavedConnection connection)
        : ResolvedLaunchPlan(
                  ConnectionIdentity(ProtocolKind::AppleScreenSharing, connection.id),
                  connection.endpoint.displayAddress(),
                  QStringLiteral("apple-high-performance"),
                  connection.revision),
          connection(std::move(connection))
    {
    }

    AppleSavedConnection connection;
};

class AppleTrustProbeTask final : public QRunnable
{
public:
    AppleTrustProbeTask(AppleProtocolAdapter* adapter,
                        QString connectionId,
                        quint64 revision,
                        quint64 generation,
                        AppleConnectionEndpoint endpoint)
        : m_Adapter(adapter),
          m_ConnectionId(std::move(connectionId)),
          m_Revision(revision),
          m_Generation(generation),
          m_Endpoint(std::move(endpoint))
    {
        setAutoDelete(true);
    }

    void run() override
    {
        AppleTcpTransport transport;
        AppleAuthenticator authenticator;
        AppleHostIdentity identity;
        std::atomic_bool cancelled{false};
        QString error;
        if (!authenticator.probe(transport, m_Endpoint, &identity, &cancelled, &error)) {
            identity.fingerprint.clear();
        }
        transport.close();

        const QPointer<AppleProtocolAdapter> adapter = m_Adapter;
        if (adapter != nullptr) {
            QMetaObject::invokeMethod(adapter,
                                      [adapter,
                                       connectionId = m_ConnectionId,
                                       revision = m_Revision,
                                       generation = m_Generation,
                                       fingerprint = identity.fingerprint,
                                       error]() {
                                          if (adapter != nullptr) {
                                              adapter->completeTrustProbe(
                                                      connectionId,
                                                      revision,
                                                      generation,
                                                      fingerprint,
                                                      error);
                                          }
                                      },
                                      Qt::QueuedConnection);
        }
    }

private:
    QPointer<AppleProtocolAdapter> m_Adapter;
    QString m_ConnectionId;
    quint64 m_Revision;
    quint64 m_Generation;
    AppleConnectionEndpoint m_Endpoint;
};

class AppleCredentialVerificationTask final : public QRunnable
{
public:
    AppleCredentialVerificationTask(AppleProtocolAdapter* adapter,
                                    AppleSavedConnection connection,
                                    quint64 generation,
                                    AppleCredentials credentials)
        : m_Adapter(adapter),
          m_Connection(std::move(connection)),
          m_Generation(generation),
          m_Credentials(std::move(credentials))
    {
        setAutoDelete(true);
    }

    void run() override
    {
        AppleTcpTransport transport;
        AppleAuthenticator authenticator;
        AppleAuthenticatedControl result;
        std::atomic_bool cancelled{false};
        QString error;
        const bool success = authenticator.authenticate(
                transport,
                m_Connection.endpoint,
                m_Connection.trustedHostFingerprint,
                [credentials = m_Credentials](AppleCredentials* output, QString*) {
                    *output = credentials;
                    return true;
                },
                &result,
                &cancelled,
                &error);
        transport.close();
        if (!success && error.isEmpty()) {
            error = QCoreApplication::translate(
                    "AppleProtocolAdapter",
                    "Apple Screen Sharing authentication failed.");
        }

        const QPointer<AppleProtocolAdapter> adapter = m_Adapter;
        if (adapter != nullptr) {
            QMetaObject::invokeMethod(adapter,
                                      [adapter,
                                       connectionId = m_Connection.id,
                                       revision = m_Connection.revision,
                                       generation = m_Generation,
                                       credentials = m_Credentials,
                                       error]() {
                                          if (adapter != nullptr) {
                                              adapter->completeAuthentication(
                                                      connectionId,
                                                      revision,
                                                      generation,
                                                      credentials,
                                                      error);
                                          }
                                      },
                                      Qt::QueuedConnection);
        }
    }

private:
    QPointer<AppleProtocolAdapter> m_Adapter;
    AppleSavedConnection m_Connection;
    quint64 m_Generation;
    AppleCredentials m_Credentials;
};

} // namespace

AppleProtocolAdapter::AppleProtocolAdapter(
        const QSharedPointer<QMdnsEngine::Server>& mdnsServer,
        QObject* parent)
    : ProtocolAdapter(parent),
      m_Server(mdnsServer)
{
}

AppleProtocolAdapter::~AppleProtocolAdapter()
{
    stopDiscoveryAsync();
}

bool AppleProtocolAdapter::isAvailable() const
{
    return AppleFeatureGate::isRuntimeEnabled();
}

QVector<CatalogConnectionView> AppleProtocolAdapter::connections() const
{
    QVector<CatalogConnectionView> result;
    const QList<AppleSavedConnection> savedConnections = m_Store.connections();
    result.reserve(savedConnections.count() + m_Discovered.count());
    for (const AppleSavedConnection& saved : savedConnections) {
        result.append(savedView(saved));
    }
    for (auto it = m_Discovered.cbegin(); it != m_Discovered.cend(); ++it) {
        bool alreadySaved = false;
        for (const AppleSavedConnection& saved : savedConnections) {
            if (!it.key().isEmpty() && saved.endpoint.serviceKey() == it.key()) {
                alreadySaved = true;
                break;
            }
        }
        if (!alreadySaved) {
            result.append(discoveredView(it.value()));
        }
    }
    return result;
}

bool AppleProtocolAdapter::contains(const ConnectionIdentity& identity) const
{
    if (!identity.isValid() || identity.protocol() != ProtocolKind::AppleScreenSharing) {
        return false;
    }
    bool found = false;
    m_Store.connection(identity.stableId(), &found);
    if (found) {
        return true;
    }
    discovered(identity, &found);
    return found;
}

void AppleProtocolAdapter::startDiscovery()
{
    if (!isAvailable() || ++m_DiscoveryReferences > 1) {
        return;
    }
    m_Browser = new QMdnsEngine::Browser(m_Server.data(), "_rfb._tcp.local.", nullptr, this);
    connect(m_Browser, &QMdnsEngine::Browser::serviceAdded,
            this, &AppleProtocolAdapter::handleServiceAddedOrUpdated);
    connect(m_Browser, &QMdnsEngine::Browser::serviceUpdated,
            this, &AppleProtocolAdapter::handleServiceAddedOrUpdated);
    connect(m_Browser, &QMdnsEngine::Browser::serviceRemoved,
            this, &AppleProtocolAdapter::handleServiceRemoved);
}

void AppleProtocolAdapter::stopDiscoveryAsync()
{
    if (m_DiscoveryReferences == 0 || --m_DiscoveryReferences > 0) {
        return;
    }
    delete m_Browser;
    m_Browser = nullptr;
    for (QMdnsEngine::Resolver* resolver : std::as_const(m_Resolvers)) {
        delete resolver;
    }
    m_Resolvers.clear();
    const QList<AppleDiscoveredConnection> removed = m_Discovered.values();
    m_Discovered.clear();
    for (const AppleDiscoveredConnection& connection : removed) {
        emit connectionChanged(ConnectionIdentity(
                ProtocolKind::AppleScreenSharing, connection.id).toString());
    }
}

void AppleProtocolAdapter::addManualConnection(const QString& address)
{
    QString candidate = address.trimmed();
    if (candidate.isEmpty()) {
        emit connectionAddCompleted(false, false);
        return;
    }
    if (!candidate.contains(QStringLiteral("://"))) {
        candidate.prepend(QStringLiteral("vnc://"));
    }
    const QUrl url(candidate);
    const int port = url.port(5900);
    if (!url.isValid() || url.scheme().compare(QStringLiteral("vnc"), Qt::CaseInsensitive) != 0 ||
            url.host().isEmpty() || port <= 0 || port > 65535 ||
            !url.userInfo().isEmpty() || !url.path().isEmpty() ||
            !url.query().isEmpty() || !url.fragment().isEmpty()) {
        emit connectionAddCompleted(false, false);
        return;
    }
    AppleConnectionEndpoint endpoint;
    endpoint.host = url.host();
    endpoint.port = static_cast<quint16>(port);
    const AppleSavedConnection saved = m_Store.saveDiscovered(endpoint, endpoint.displayAddress());
    emit connectionChanged(ConnectionIdentity(
            ProtocolKind::AppleScreenSharing, saved.id).toString());
    emit connectionAddCompleted(true, false);
}

void AppleProtocolAdapter::pair(const ConnectionIdentity& identity, const QString&)
{
    emit pairingCompleted(identity.toString(), tr("This connection requires a Mac account name and password."));
}

void AppleProtocolAdapter::deleteConnection(const ConnectionIdentity& identity)
{
    AppleSavedConnection removed;
    if (!m_Store.remove(identity.stableId(), &removed)) {
        return;
    }
    if (AppleCredentialStore::isReferenceForConnection(
                removed.credentialReference, removed.id)) {
        QString ignored;
        AppleCredentialStore().remove(removed.credentialReference, &ignored);
    }
    m_PendingTrust.remove(identity.stableId());
    m_AuthenticationGenerations.remove(identity.stableId());
    emit connectionChanged(identity.toString());
}

void AppleProtocolAdapter::renameConnection(const ConnectionIdentity& identity,
                                            const QString& name)
{
    if (m_Store.rename(identity.stableId(), name)) {
        emit connectionChanged(identity.toString());
    }
}

QString AppleProtocolAdapter::saveConnection(const ConnectionIdentity& identity,
                                             QString* error)
{
    bool found = false;
    const AppleDiscoveredConnection source = discovered(identity, &found);
    if (!found) {
        bool savedFound = false;
        m_Store.connection(identity.stableId(), &savedFound);
        if (savedFound) {
            return identity.toString();
        }
        setError(error, tr("The discovered Mac is no longer available."));
        return {};
    }
    const AppleSavedConnection saved = m_Store.saveDiscovered(
            source.endpoint, source.displayName);
    const QString savedId = ConnectionIdentity(
            ProtocolKind::AppleScreenSharing, saved.id).toString();
    emit connectionSaved(identity.toString(), savedId, QString());
    emit connectionChanged(identity.toString());
    emit connectionChanged(savedId);
    return savedId;
}

void AppleProtocolAdapter::requestAuthentication(const ConnectionIdentity& identity)
{
    bool found = false;
    AppleSavedConnection connection = m_Store.connection(identity.stableId(), &found);
    if (!found) {
        emit authenticationCompleted(identity.toString(), tr("Save this discovered Mac before authenticating."));
        return;
    }
    connection.endpoint = currentEndpoint(connection);
    const quint64 generation = m_NextAuthenticationGeneration++;
    if (m_NextAuthenticationGeneration == 0) {
        m_NextAuthenticationGeneration = 1;
    }
    m_AuthenticationGenerations[connection.id] = generation;
    QThreadPool::globalInstance()->start(new AppleTrustProbeTask(
            this, connection.id, connection.revision, generation, connection.endpoint));
}

void AppleProtocolAdapter::confirmHostTrust(const ConnectionIdentity& identity,
                                            bool accepted)
{
    const QString pendingFingerprint = m_PendingTrust.take(identity.stableId());
    if (!accepted || pendingFingerprint.isEmpty()) {
        if (!accepted) {
            m_AuthenticationGenerations.remove(identity.stableId());
        }
        return;
    }
    bool found = false;
    const AppleSavedConnection before = m_Store.connection(identity.stableId(), &found);
    if (!found) {
        m_AuthenticationGenerations.remove(identity.stableId());
        return;
    }
    if (AppleCredentialStore::isReferenceForConnection(
                before.credentialReference, before.id) &&
            before.trustedHostFingerprint.compare(pendingFingerprint, Qt::CaseInsensitive) != 0) {
        QString ignored;
        AppleCredentialStore().remove(before.credentialReference, &ignored);
    }
    if (!m_Store.setTrust(identity.stableId(), pendingFingerprint)) {
        m_AuthenticationGenerations.remove(identity.stableId());
        return;
    }
    const AppleSavedConnection after = m_Store.connection(identity.stableId());
    emit connectionChanged(identity.toString());
    emit credentialsRequired(identity.toString(), after.preferredUsername);
}

void AppleProtocolAdapter::submitCredentials(const ConnectionIdentity& identity,
                                             const QString& username,
                                             const QString& password)
{
    bool found = false;
    AppleSavedConnection connection = m_Store.connection(identity.stableId(), &found);
    if (!found || !connection.isTrusted()) {
        m_AuthenticationGenerations.remove(identity.stableId());
        emit authenticationCompleted(identity.toString(), tr("Trust the Mac’s host identity before entering credentials."));
        return;
    }
    AppleCredentials credentials{username.trimmed(), password};
    QString error;
    if (!credentials.validate(&error)) {
        m_AuthenticationGenerations.remove(identity.stableId());
        emit authenticationCompleted(identity.toString(), error);
        return;
    }
    connection.endpoint = currentEndpoint(connection);
    const quint64 generation = m_NextAuthenticationGeneration++;
    if (m_NextAuthenticationGeneration == 0) {
        m_NextAuthenticationGeneration = 1;
    }
    m_AuthenticationGenerations[connection.id] = generation;
    QThreadPool::globalInstance()->start(new AppleCredentialVerificationTask(
            this, std::move(connection), generation, std::move(credentials)));
}

QVariantList AppleProtocolAdapter::connectionEndpoints(const ConnectionIdentity& identity) const
{
    QVariantList endpoints;
    bool found = false;
    AppleSavedConnection saved = m_Store.connection(identity.stableId(), &found);
    AppleConnectionEndpoint endpoint;
    if (found) {
        endpoint = currentEndpoint(saved);
    }
    else {
        const AppleDiscoveredConnection discovery = discovered(identity, &found);
        if (found) {
            endpoint = discovery.endpoint;
        }
    }
    if (!found) {
        return endpoints;
    }
    QVariantMap item;
    item[QStringLiteral("address")] = endpoint.host;
    item[QStringLiteral("port")] = endpoint.port;
    item[QStringLiteral("display")] = endpoint.displayAddress();
    item[QStringLiteral("type")] = tr("Apple Screen Sharing");
    item[QStringLiteral("isActive")] = true;
    item[QStringLiteral("isAuto")] = !endpoint.serviceKey().isEmpty();
    endpoints.append(item);
    return endpoints;
}

QVariantMap AppleProtocolAdapter::activeEndpoint(const ConnectionIdentity& identity) const
{
    const QVariantList endpoints = connectionEndpoints(identity);
    return endpoints.isEmpty() ? QVariantMap() : endpoints.first().toMap();
}

QVariantMap AppleProtocolAdapter::sessionOptions(
        const ConnectionIdentity& identity) const
{
    bool found = false;
    const AppleSavedConnection connection = m_Store.connection(
            identity.stableId(), &found);
    if (!found) {
        return {};
    }
    return {
        {QStringLiteral("virtualDisplayCount"),
         connection.virtualDisplayCount},
        {QStringLiteral("sharedClipboardEnabled"),
         connection.sharedClipboardEnabled},
        {QStringLiteral("keyboardInputSourceSharingEnabled"),
         connection.keyboardInputSourceSharingEnabled},
    };
}

bool AppleProtocolAdapter::setSessionOptions(
        const ConnectionIdentity& identity,
        const QVariantMap& options,
        QString* error)
{
    bool conversionOk = false;
    const int displayCount = options.value(
            QStringLiteral("virtualDisplayCount")).toInt(&conversionOk);
    if (!conversionOk || displayCount < 1 || displayCount > 2) {
        setError(error, tr("Choose either one or two virtual displays."));
        return false;
    }
    bool found = false;
    const AppleSavedConnection connection = m_Store.connection(
            identity.stableId(), &found);
    const bool sharedClipboardEnabled = options.value(
            QStringLiteral("sharedClipboardEnabled"),
            found ? connection.sharedClipboardEnabled : true).toBool();
    const bool keyboardInputSourceSharingEnabled = options.value(
            QStringLiteral("keyboardInputSourceSharingEnabled"),
            found ? connection.keyboardInputSourceSharingEnabled
                  : false).toBool();
    if (!m_Store.setVirtualDisplayCount(identity.stableId(), displayCount) ||
            !m_Store.setSharedClipboardEnabled(
                    identity.stableId(), sharedClipboardEnabled) ||
            !m_Store.setKeyboardInputSourceSharingEnabled(
                    identity.stableId(),
                    keyboardInputSourceSharingEnabled)) {
        setError(error, tr("The Screen Sharing options could not be saved."));
        return false;
    }
    emit connectionChanged(identity.toString());
    return true;
}

std::unique_ptr<ResolvedLaunchPlan> AppleProtocolAdapter::resolveLaunch(
        const LaunchRequest& request,
        QString* error) const
{
    bool found = false;
    AppleSavedConnection connection = m_Store.connection(
            request.connection.stableId(), &found);
    if (!found) {
        setError(error, tr("The selected Apple Screen Sharing connection is not saved."));
        return nullptr;
    }
    if (!connection.isTrusted()) {
        setError(error, tr("The Mac’s host identity has not been trusted."));
        return nullptr;
    }
    if (!AppleCredentialStore::isReferenceForConnection(
                connection.credentialReference, connection.id)) {
        setError(error, tr("No %1 binding exists for this Mac.")
                        .arg(AppleCredentialStore::displayName()));
        return nullptr;
    }
    connection.endpoint = currentEndpoint(connection);
    return std::make_unique<AppleResolvedLaunchPlan>(std::move(connection));
}

StreamSession* AppleProtocolAdapter::createSession(
        std::unique_ptr<ResolvedLaunchPlan> plan,
        QString* error) const
{
    auto* applePlan = dynamic_cast<AppleResolvedLaunchPlan*>(plan.get());
    if (applePlan == nullptr ||
            plan->connection().protocol() != ProtocolKind::AppleScreenSharing) {
        setError(error, tr("The launch plan belongs to a different protocol."));
        return nullptr;
    }
    if (!plan->consume()) {
        setError(error, tr("The launch plan has already been used."));
        return nullptr;
    }
    auto* session = new AppleScreenSharingSession(
            std::move(applePlan->connection));
    auto* adapter = const_cast<AppleProtocolAdapter*>(this);
    connect(session,
            &AppleScreenSharingSession::clipboardSharingChanged,
            adapter,
            [adapter](const QString& connectionId, bool enabled) {
                if (!adapter->m_Store.setSharedClipboardEnabled(
                            connectionId, enabled)) {
                    qWarning() << "Apple clipboard preference could not be saved";
                    return;
                }
                emit adapter->connectionChanged(ConnectionIdentity(
                        ProtocolKind::AppleScreenSharing,
                        connectionId).toString());
            });
    connect(session,
            &AppleScreenSharingSession::keyboardInputSourceSharingChanged,
            adapter,
            [adapter](const QString& connectionId, bool enabled) {
                if (!adapter->m_Store.setKeyboardInputSourceSharingEnabled(
                            connectionId, enabled)) {
                    qWarning() << "Apple keyboard input source preference could not be saved";
                    return;
                }
                emit adapter->connectionChanged(ConnectionIdentity(
                        ProtocolKind::AppleScreenSharing,
                        connectionId).toString());
            });
    return session;
}

void AppleProtocolAdapter::completeTrustProbe(QString connectionId,
                                              quint64 revision,
                                              quint64 generation,
                                              QString fingerprint,
                                              QString error)
{
    bool found = false;
    const AppleSavedConnection connection = m_Store.connection(connectionId, &found);
    const QString qualifiedId = ConnectionIdentity(
            ProtocolKind::AppleScreenSharing, connectionId).toString();
    if (!found || connection.revision != revision ||
            m_AuthenticationGenerations.value(connectionId) != generation) {
        return;
    }
    if (!error.isEmpty() || fingerprint.isEmpty()) {
        m_AuthenticationGenerations.remove(connectionId);
        emit authenticationCompleted(qualifiedId, error.isEmpty()
                ? tr("Couldn’t read the Mac’s host identity.")
                : error);
        return;
    }
    if (connection.trustedHostFingerprint.compare(
                fingerprint, Qt::CaseInsensitive) == 0) {
        emit credentialsRequired(qualifiedId, connection.preferredUsername);
        return;
    }

    m_PendingTrust[connectionId] = fingerprint;
    emit hostTrustRequired(
            qualifiedId,
            connection.displayName,
            fingerprint,
            !connection.trustedHostFingerprint.isEmpty() &&
                    connection.trustedHostFingerprint.compare(fingerprint, Qt::CaseInsensitive) != 0);
}

void AppleProtocolAdapter::completeAuthentication(QString connectionId,
                                                  quint64 revision,
                                                  quint64 generation,
                                                  AppleCredentials credentials,
                                                  QString error)
{
    bool found = false;
    const AppleSavedConnection connection = m_Store.connection(connectionId, &found);
    const QString qualifiedId = ConnectionIdentity(
            ProtocolKind::AppleScreenSharing, connectionId).toString();
    if (!found || connection.revision != revision ||
            m_AuthenticationGenerations.value(connectionId) != generation) {
        return;
    }
    if (!error.isEmpty()) {
        m_AuthenticationGenerations.remove(connectionId);
        emit authenticationCompleted(qualifiedId, error);
        return;
    }
    const QString reference = AppleCredentialStore::referenceForConnection(connectionId);
    if (!AppleCredentialStore().store(reference, credentials, &error) ||
            !m_Store.setCredentialBinding(
                    connectionId, reference, credentials.username.trimmed())) {
        if (error.isEmpty()) {
            error = tr("Couldn’t bind the verified credentials to this connection.");
        }
        m_AuthenticationGenerations.remove(connectionId);
        emit authenticationCompleted(qualifiedId, error);
        return;
    }
    m_AuthenticationGenerations.remove(connectionId);
    emit connectionChanged(qualifiedId);
    emit authenticationCompleted(qualifiedId, QString());
}

void AppleProtocolAdapter::handleServiceAddedOrUpdated(
        const QMdnsEngine::Service& service)
{
    qInfo() << "Discovered Apple Screen Sharing service:"
            << service.name() << service.hostname() << service.port();
    AppleDiscoveredConnection discovery;
    discovery.displayName = QString::fromUtf8(service.name());
    discovery.endpoint.host = QString::fromUtf8(service.hostname());
    discovery.endpoint.port = service.port() == 0 ? 5900 : service.port();
    discovery.endpoint.serviceName = discovery.displayName;
    discovery.endpoint.serviceType = QString::fromUtf8(service.type());
    discovery.endpoint.serviceDomain = serviceDomain(discovery.endpoint.serviceType);
    const QString key = discovery.endpoint.serviceKey();
    if (key.isEmpty() || !discovery.endpoint.isValid()) {
        return;
    }
    const auto existing = m_Discovered.constFind(key);
    if (existing != m_Discovered.cend()) {
        discovery.id = existing->id;
        discovery.addresses = existing->addresses;
    }
    else {
        discovery.id = identityForDiscovery(key).stableId();
    }
    m_Discovered[key] = discovery;
    resolveService(key);
    m_Store.updateDiscoveredEndpoint(key, discovery.endpoint);
    emit connectionChanged(ConnectionIdentity(
            ProtocolKind::AppleScreenSharing, discovery.id).toString());
}

void AppleProtocolAdapter::handleServiceRemoved(const QMdnsEngine::Service& service)
{
    AppleConnectionEndpoint endpoint;
    endpoint.serviceName = QString::fromUtf8(service.name());
    endpoint.serviceType = QString::fromUtf8(service.type());
    endpoint.serviceDomain = serviceDomain(endpoint.serviceType);
    const QString key = endpoint.serviceKey();
    const AppleDiscoveredConnection removed = m_Discovered.take(key);
    if (QMdnsEngine::Resolver* resolver = m_Resolvers.take(key)) {
        delete resolver;
    }
    if (!removed.id.isEmpty()) {
        emit connectionChanged(ConnectionIdentity(
                ProtocolKind::AppleScreenSharing, removed.id).toString());
    }
}

void AppleProtocolAdapter::resolveService(const QString& serviceKey)
{
    if (!m_Server || !m_Discovered.contains(serviceKey)) {
        return;
    }
    if (QMdnsEngine::Resolver* old = m_Resolvers.take(serviceKey)) {
        delete old;
    }
    AppleDiscoveredConnection& discovery = m_Discovered[serviceKey];
    auto* resolver = new QMdnsEngine::Resolver(
            m_Server.data(), discovery.endpoint.host.toUtf8(), nullptr, this);
    m_Resolvers[serviceKey] = resolver;
    connect(resolver, &QMdnsEngine::Resolver::resolved,
            this, [this, serviceKey](const QHostAddress& address) {
        auto it = m_Discovered.find(serviceKey);
        if (it == m_Discovered.end()) {
            return;
        }
        if (!it->addresses.contains(address)) {
            it->addresses.append(address);
        }
        QHostAddress selected;
        for (const QHostAddress& candidate : std::as_const(it->addresses)) {
            if (candidate.protocol() == QAbstractSocket::IPv4Protocol) {
                selected = candidate;
                break;
            }
            if (selected.isNull()) {
                selected = candidate;
            }
        }
        if (!selected.isNull()) {
            it->endpoint.host = selected.toString();
            m_Store.updateDiscoveredEndpoint(serviceKey, it->endpoint);
            emit connectionChanged(ConnectionIdentity(
                    ProtocolKind::AppleScreenSharing, it->id).toString());
            for (const AppleSavedConnection& saved : m_Store.connections()) {
                if (saved.endpoint.serviceKey() == serviceKey) {
                    emit connectionChanged(ConnectionIdentity(
                            ProtocolKind::AppleScreenSharing, saved.id).toString());
                }
            }
        }
    });
}

ConnectionIdentity AppleProtocolAdapter::identityForDiscovery(const QString& serviceKey) const
{
    const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(serviceKey.toUtf8(), QCryptographicHash::Sha256).toHex());
    return ConnectionIdentity(
            ProtocolKind::AppleScreenSharing,
            QStringLiteral("discovery-%1").arg(digest));
}

AppleDiscoveredConnection AppleProtocolAdapter::discovered(
        const ConnectionIdentity& identity,
        bool* found) const
{
    for (auto it = m_Discovered.cbegin(); it != m_Discovered.cend(); ++it) {
        if (it->id == identity.stableId()) {
            if (found != nullptr) {
                *found = true;
            }
            return it.value();
        }
    }
    if (found != nullptr) {
        *found = false;
    }
    return {};
}

CatalogConnectionView AppleProtocolAdapter::savedView(
        const AppleSavedConnection& connection) const
{
    CatalogConnectionView view;
    view.identity = ConnectionIdentity(ProtocolKind::AppleScreenSharing, connection.id);
    view.displayName = connection.displayName;
    view.protocolName = QStringLiteral("Apple Screen Sharing");
    view.online = connection.endpoint.isValid();
    const bool hasCredentialBinding = AppleCredentialStore::isReferenceForConnection(
            connection.credentialReference, connection.id);
    view.paired = connection.isTrusted() && hasCredentialBinding;
    view.busy = view.paired;
    view.statusUnknown = false;
    view.serverSupported = true;
    view.persistent = true;
    view.trusted = connection.isTrusted();
    view.directLaunch = true;
    view.authenticationKind = QStringLiteral("credentials");
    view.capabilities = ConnectionCapabilities(CanRenameConnection |
                                               CanDeleteConnection |
                                               CanAuthenticateConnection);
    view.details = tr("Name: %1\nStatus: Saved\nProtocol: Apple Screen Sharing\nEndpoint: %2\nConnection ID: %3\nHost identity: %4\nCredentials: %5")
            .arg(connection.displayName,
                 currentEndpoint(connection).displayAddress(),
                 connection.id,
                 connection.isTrusted() ? connection.trustedHostFingerprint : tr("Not trusted"),
                 hasCredentialBinding ? AppleCredentialStore::displayName()
                                      : tr("Not saved"));
    return view;
}

CatalogConnectionView AppleProtocolAdapter::discoveredView(
        const AppleDiscoveredConnection& connection) const
{
    CatalogConnectionView view;
    view.identity = ConnectionIdentity(ProtocolKind::AppleScreenSharing, connection.id);
    view.displayName = connection.displayName;
    view.protocolName = QStringLiteral("Apple Screen Sharing");
    view.online = true;
    view.paired = false;
    view.statusUnknown = false;
    view.serverSupported = true;
    view.persistent = false;
    view.directLaunch = true;
    view.authenticationKind = QStringLiteral("credentials");
    view.capabilities = ConnectionCapabilities(CanSaveConnection);
    view.details = tr("Name: %1\nStatus: Discovered (not saved)\nProtocol: Apple Screen Sharing\nEndpoint: %2")
            .arg(connection.displayName, connection.endpoint.displayAddress());
    return view;
}

AppleConnectionEndpoint AppleProtocolAdapter::currentEndpoint(
        const AppleSavedConnection& connection) const
{
    const QString serviceKey = connection.endpoint.serviceKey();
    return !serviceKey.isEmpty() && m_Discovered.contains(serviceKey)
            ? m_Discovered.value(serviceKey).endpoint
            : connection.endpoint;
}
