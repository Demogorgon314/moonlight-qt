#include "moonlightprotocoladapter.h"

#include "resolvedlaunchplan.h"
#include "backend/computermanager.h"
#include "backend/nvcomputer.h"
#include "streaming/session.h"

#include <QCoreApplication>
#include <QDebug>
#include <QReadLocker>
#include <QThreadPool>

namespace {

QString appModelText(const char* source)
{
    return QCoreApplication::translate("AppModel", source);
}

QString addressType(const NvAddress& address,
                    const NvAddress& localAddress,
                    const NvAddress& remoteAddress,
                    const NvAddress& manualAddress,
                    const NvAddress& ipv6Address)
{
    if (address == localAddress) {
        return appModelText("Local network");
    }
    if (address == remoteAddress) {
        return appModelText("Remote network");
    }
    if (address == manualAddress) {
        return appModelText("Manual");
    }
    if (address == ipv6Address) {
        return appModelText("IPv6 network");
    }
    return appModelText("Other network");
}

class WakeMoonlightConnectionTask final : public QRunnable
{
public:
    explicit WakeMoonlightConnectionTask(std::unique_ptr<NvComputer> computer)
        : m_Computer(std::move(computer))
    {
    }

    void run() override { m_Computer->wake(); }

private:
    std::unique_ptr<NvComputer> m_Computer;
};

class MoonlightResolvedLaunchPlan final : public ResolvedLaunchPlan
{
public:
    MoonlightResolvedLaunchPlan(ConnectionIdentity identity,
                                QString endpoint,
                                quint64 revision,
                                std::unique_ptr<NvComputer> computer,
                                NvApp app,
                                StreamingPreferences* preferences,
                                QString launchDisplayName,
                                std::optional<bool> launchUseVdd)
        : ResolvedLaunchPlan(std::move(identity),
                             std::move(endpoint),
                             QStringLiteral("moonlight"),
                             revision),
          computer(std::move(computer)),
          app(std::move(app)),
          preferences(preferences),
          launchDisplayName(std::move(launchDisplayName)),
          launchUseVdd(launchUseVdd)
    {
    }

    std::unique_ptr<NvComputer> computer;
    NvApp app;
    StreamingPreferences* preferences;
    QString launchDisplayName;
    std::optional<bool> launchUseVdd;
};

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
}

} // namespace

MoonlightProtocolAdapter::MoonlightProtocolAdapter(
        StreamingPreferences* preferences,
        const QSharedPointer<QMdnsEngine::Server>& mdnsServer,
        QObject* parent)
    : ProtocolAdapter(parent),
      m_Manager(std::make_unique<ComputerManager>(preferences, mdnsServer))
{
    connect(m_Manager.get(), &ComputerManager::computerStateChanged,
            this, &MoonlightProtocolAdapter::handleComputerChanged);
    connect(m_Manager.get(), &ComputerManager::pairingCompleted,
            this, &MoonlightProtocolAdapter::handlePairingCompleted);
    connect(m_Manager.get(), &ComputerManager::computerAddCompleted,
            this, &MoonlightProtocolAdapter::connectionAddCompleted);
    connect(m_Manager.get(), &ComputerManager::quitAppCompleted,
            this, &MoonlightProtocolAdapter::quitActivityCompleted);
}

MoonlightProtocolAdapter::~MoonlightProtocolAdapter() = default;

QVector<CatalogConnectionView> MoonlightProtocolAdapter::connections() const
{
    QVector<CatalogConnectionView> result;
    const QVector<NvComputer*> computers = m_Manager->getComputers();
    result.reserve(computers.size());
    for (NvComputer* computer : computers) {
        result.append(viewFor(computer));
    }
    return result;
}

bool MoonlightProtocolAdapter::contains(const ConnectionIdentity& identity) const
{
    return computer(identity) != nullptr;
}

void MoonlightProtocolAdapter::startDiscovery()
{
    m_Manager->startPolling();
}

void MoonlightProtocolAdapter::stopDiscoveryAsync()
{
    m_Manager->stopPollingAsync();
}

void MoonlightProtocolAdapter::addManualConnection(const QString& address)
{
    m_Manager->addNewHostManually(address);
}

QString MoonlightProtocolAdapter::generatePairingSecret() const
{
    return m_Manager->generatePinString();
}

void MoonlightProtocolAdapter::pair(const ConnectionIdentity& identity,
                                    const QString& secret)
{
    if (NvComputer* target = computer(identity)) {
        m_Manager->pairHost(target, secret);
    }
}

void MoonlightProtocolAdapter::deleteConnection(const ConnectionIdentity& identity)
{
    if (NvComputer* target = computer(identity)) {
        m_Manager->deleteHost(target);
        m_Revisions.remove(identity);
        emit connectionChanged(identity.toString());
    }
}

void MoonlightProtocolAdapter::renameConnection(const ConnectionIdentity& identity,
                                                const QString& name)
{
    if (NvComputer* target = computer(identity)) {
        m_Manager->renameHost(target, name);
    }
}

void MoonlightProtocolAdapter::wakeConnection(const ConnectionIdentity& identity)
{
    if (NvComputer* target = computer(identity)) {
        std::unique_ptr<NvComputer> snapshot;
        {
            QReadLocker lock(&target->lock);
            snapshot = std::make_unique<NvComputer>(*target);
        }
        QThreadPool::globalInstance()->start(
                new WakeMoonlightConnectionTask(std::move(snapshot)));
    }
}

void MoonlightProtocolAdapter::quitRunningActivity(const ConnectionIdentity& identity)
{
    if (NvComputer* target = computer(identity)) {
        m_Manager->quitRunningApp(target);
    }
}

QVariantList MoonlightProtocolAdapter::connectionEndpoints(const ConnectionIdentity& identity) const
{
    QVariantList endpoints;
    NvComputer* target = computer(identity);
    if (target == nullptr) {
        return endpoints;
    }

    const QVector<NvAddress> allAddresses = target->uniqueAddresses();
    NvAddress localAddress;
    NvAddress remoteAddress;
    NvAddress manualAddress;
    NvAddress ipv6Address;
    NvAddress pinnedAddress;
    {
        QReadLocker lock(&target->lock);
        localAddress = target->localAddress;
        remoteAddress = target->remoteAddress;
        manualAddress = target->manualAddress;
        ipv6Address = target->ipv6Address;
        pinnedAddress = target->pinnedAddress;
    }

    QVariantMap automatic;
    automatic[QStringLiteral("address")] = QString();
    automatic[QStringLiteral("port")] = 0;
    automatic[QStringLiteral("display")] = appModelText("Auto (default)");
    automatic[QStringLiteral("type")] = appModelText("Automatic selection with fallback");
    automatic[QStringLiteral("isActive")] = pinnedAddress.isNull();
    automatic[QStringLiteral("isAuto")] = true;
    endpoints.append(automatic);

    for (const NvAddress& address : allAddresses) {
        QVariantMap item;
        item[QStringLiteral("address")] = address.address();
        item[QStringLiteral("port")] = static_cast<int>(address.port());
        item[QStringLiteral("display")] = address.toString();
        item[QStringLiteral("type")] = addressType(address,
                                                   localAddress,
                                                   remoteAddress,
                                                   manualAddress,
                                                   ipv6Address);
        item[QStringLiteral("isActive")] = !pinnedAddress.isNull() && address == pinnedAddress;
        item[QStringLiteral("isAuto")] = false;
        item[QStringLiteral("isTested")] = target->hasAddressTestSucceeded(address);
        endpoints.append(item);
    }
    return endpoints;
}

bool MoonlightProtocolAdapter::hasMultipleEndpoints(const ConnectionIdentity& identity) const
{
    NvComputer* target = computer(identity);
    return target != nullptr && target->uniqueAddresses().count() > 1;
}

bool MoonlightProtocolAdapter::selectEndpoint(const ConnectionIdentity& identity,
                                              const QString& address,
                                              int port)
{
    NvComputer* target = computer(identity);
    if (target == nullptr || address.isEmpty() || port <= 0 || port > 65535) {
        return false;
    }

    const NvAddress selected(address, static_cast<uint16_t>(port));
    if (!target->uniqueAddresses().contains(selected)) {
        qWarning() << "Address is not a known Moonlight endpoint:" << selected.toString();
        return false;
    }

    target->pinAddress(selected);
    emit connectionChanged(identity.toString());
    return true;
}

bool MoonlightProtocolAdapter::selectAutomaticEndpoint(const ConnectionIdentity& identity)
{
    NvComputer* target = computer(identity);
    if (target == nullptr || !target->resetToAutomaticAddress()) {
        return false;
    }
    emit connectionChanged(identity.toString());
    return true;
}

QVariantMap MoonlightProtocolAdapter::activeEndpoint(const ConnectionIdentity& identity) const
{
    QVariantMap info;
    NvComputer* target = computer(identity);
    if (target == nullptr) {
        return info;
    }

    NvAddress activeAddress;
    NvAddress localAddress;
    NvAddress remoteAddress;
    NvAddress manualAddress;
    NvAddress ipv6Address;
    {
        QReadLocker lock(&target->lock);
        activeAddress = target->activeAddress;
        localAddress = target->localAddress;
        remoteAddress = target->remoteAddress;
        manualAddress = target->manualAddress;
        ipv6Address = target->ipv6Address;
    }

    info[QStringLiteral("address")] = activeAddress.address();
    info[QStringLiteral("port")] = static_cast<int>(activeAddress.port());
    info[QStringLiteral("display")] = activeAddress.toString();
    info[QStringLiteral("type")] = addressType(activeAddress,
                                               localAddress,
                                               remoteAddress,
                                               manualAddress,
                                               ipv6Address);
    return info;
}

std::unique_ptr<ResolvedLaunchPlan> MoonlightProtocolAdapter::resolveLaunch(
        const LaunchRequest& request,
        QString* error) const
{
    NvComputer* target = computer(request.connection);
    if (target == nullptr) {
        setError(error, tr("The selected Moonlight connection no longer exists."));
        return nullptr;
    }

    bool appIdParsed = false;
    int appId = request.activityId.toInt(&appIdParsed);
    std::unique_ptr<NvComputer> snapshot;
    NvApp selectedApp;
    {
        QReadLocker lock(&target->lock);
        if (!appIdParsed) {
            appId = target->currentGameId;
        }
        bool appFound = false;
        for (const NvApp& app : target->appList) {
            if (app.id == appId) {
                selectedApp = app;
                appFound = true;
                break;
            }
        }
        if (!appFound || appId == 0) {
            setError(error, tr("The selected Moonlight application is no longer available."));
            return nullptr;
        }

        // Freeze endpoint, host certificate, capabilities, and app configuration in
        // one snapshot. Subsequent polling cannot silently retarget this launch.
        snapshot = std::make_unique<NvComputer>(*target);
    }

    if (snapshot->activeAddress.isNull()) {
        setError(error, tr("The selected Moonlight connection has no resolved endpoint."));
        return nullptr;
    }

    QString launchDisplayName;
    std::optional<bool> launchUseVdd;
    if (request.displayTarget == QStringLiteral("vdd")) {
        launchUseVdd = true;
    }
    else if (!request.displayTarget.isEmpty()) {
        launchDisplayName = request.displayTarget;
        launchUseVdd = false;
    }

    return std::make_unique<MoonlightResolvedLaunchPlan>(
            request.connection,
            snapshot->activeAddress.toString(),
            revisionFor(request.connection),
            std::move(snapshot),
            selectedApp,
            request.preferences,
            std::move(launchDisplayName),
            launchUseVdd);
}

StreamSession* MoonlightProtocolAdapter::createSession(
        std::unique_ptr<ResolvedLaunchPlan> plan,
        QString* error) const
{
    auto* moonlightPlan = dynamic_cast<MoonlightResolvedLaunchPlan*>(plan.get());
    if (moonlightPlan == nullptr || plan->connection().protocol() != ProtocolKind::Moonlight) {
        setError(error, tr("The launch plan belongs to a different protocol."));
        return nullptr;
    }
    if (!plan->consume()) {
        setError(error, tr("The launch plan has already been used."));
        return nullptr;
    }

    return new Session(std::move(moonlightPlan->computer),
                       std::move(moonlightPlan->app),
                       moonlightPlan->preferences,
                       std::move(moonlightPlan->launchDisplayName),
                       moonlightPlan->launchUseVdd);
}

NvComputer* MoonlightProtocolAdapter::computer(const ConnectionIdentity& identity) const
{
    if (!identity.isValid() || identity.protocol() != ProtocolKind::Moonlight) {
        return nullptr;
    }

    const QVector<NvComputer*> computers = m_Manager->getComputers();
    for (NvComputer* candidate : computers) {
        QReadLocker lock(&candidate->lock);
        if (candidate->uuid == identity.stableId()) {
            return candidate;
        }
    }
    return nullptr;
}

ConnectionIdentity MoonlightProtocolAdapter::identityFor(const NvComputer* computer) const
{
    if (computer == nullptr) {
        return {};
    }
    QReadLocker lock(&computer->lock);
    return ConnectionIdentity(ProtocolKind::Moonlight, computer->uuid);
}

void MoonlightProtocolAdapter::notifyClientMetadataChanged(const ConnectionIdentity& identity)
{
    if (NvComputer* target = computer(identity)) {
        m_Manager->clientSideAttributeUpdated(target);
    }
}

void MoonlightProtocolAdapter::handleComputerChanged(NvComputer* computer)
{
    const ConnectionIdentity identity = identityFor(computer);
    m_Revisions[identity] = revisionFor(identity) + 1;
    emit connectionChanged(identity.toString());
}

void MoonlightProtocolAdapter::handlePairingCompleted(NvComputer* computer, QString error)
{
    emit pairingCompleted(identityFor(computer).toString(), std::move(error));
}

CatalogConnectionView MoonlightProtocolAdapter::viewFor(NvComputer* computer) const
{
    CatalogConnectionView view;
    QReadLocker lock(&computer->lock);
    view.identity = ConnectionIdentity(ProtocolKind::Moonlight, computer->uuid);
    view.displayName = computer->name;
    view.protocolName = QStringLiteral("Moonlight");
    view.online = computer->state == NvComputer::CS_ONLINE;
    view.paired = computer->pairState == NvComputer::PS_PAIRED;
    view.busy = computer->currentGameId != 0;
    view.wakeable = !computer->macAddress.isEmpty();
    view.statusUnknown = computer->state == NvComputer::CS_UNKNOWN;
    view.serverSupported = computer->isSupportedServerVersion;
    view.capabilities = ConnectionCapabilities(CanRenameConnection |
                                               CanDeleteConnection |
                                               CanBrowseActivities |
                                               CanSelectEndpoint |
                                               CanQuitActivity);
    if (view.wakeable) {
        view.capabilities |= CanWakeConnection;
    }
    if (!view.paired) {
        view.capabilities |= CanPairConnection;
    }

    QString state;
    switch (computer->state) {
    case NvComputer::CS_ONLINE: state = tr("Online"); break;
    case NvComputer::CS_OFFLINE: state = tr("Offline"); break;
    default: state = tr("Unknown"); break;
    }
    QString pairState;
    switch (computer->pairState) {
    case NvComputer::PS_PAIRED: pairState = tr("Paired"); break;
    case NvComputer::PS_NOT_PAIRED: pairState = tr("Unpaired"); break;
    default: pairState = tr("Unknown"); break;
    }
    const QString pairname = NvComputer::getPairname(computer->uuid);
    view.details = tr("Name: %1").arg(computer->name) + '\n' +
                   tr("Status: %1").arg(state) + '\n' +
                   tr("Protocol: %1").arg(view.protocolName) + '\n' +
                   tr("Active Address: %1").arg(computer->activeAddress.toString()) + '\n' +
                   tr("UUID: %1").arg(computer->uuid) + '\n' +
                   tr("Pair Name: %1").arg(pairname.isEmpty() ? tr("Unknown") : pairname) + '\n' +
                   tr("Local Address: %1").arg(computer->localAddress.toString()) + '\n' +
                   tr("Remote Address: %1").arg(computer->remoteAddress.toString()) + '\n' +
                   tr("IPv6 Address: %1").arg(computer->ipv6Address.toString()) + '\n' +
                   tr("Manual Address: %1").arg(computer->manualAddress.toString()) + '\n' +
                   tr("MAC Address: %1").arg(computer->macAddress.isEmpty()
                                                   ? tr("Unknown")
                                                   : QString(computer->macAddress.toHex(':'))) + '\n' +
                   tr("Pair State: %1").arg(pairState) + '\n' +
                   tr("Running Game ID: %1").arg(view.online
                                                          ? QString::number(computer->currentGameId)
                                                          : tr("Unknown")) + '\n' +
                   tr("HTTPS Port: %1").arg(view.online
                                                    ? QString::number(computer->activeHttpsPort)
                                                    : tr("Unknown"));
    return view;
}

quint64 MoonlightProtocolAdapter::revisionFor(const ConnectionIdentity& identity) const
{
    return m_Revisions.value(identity, 1);
}
