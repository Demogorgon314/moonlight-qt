#include "computermodel.h"

#include <Limelight.h>

#include <QDebug>
#include <QThreadPool>

ComputerModel::ComputerModel(QObject* object)
    : QAbstractListModel(object)
{
}

void ComputerModel::initialize(ComputerCatalog* catalog)
{
    if (m_Catalog != nullptr) {
        disconnect(m_Catalog, nullptr, this, nullptr);
    }

    m_Catalog = catalog;
    connect(m_Catalog, &ComputerCatalog::connectionChanged,
            this, &ComputerModel::handleConnectionChanged);
    connect(m_Catalog, &ComputerCatalog::pairingCompleted,
            this, &ComputerModel::handlePairingCompleted);
    connect(m_Catalog, &ComputerCatalog::hostTrustRequired,
            this, &ComputerModel::hostTrustRequired);
    connect(m_Catalog, &ComputerCatalog::credentialsRequired,
            this, &ComputerModel::credentialsRequired);
    connect(m_Catalog, &ComputerCatalog::authenticationCompleted,
            this, &ComputerModel::authenticationCompleted);
    refreshConnections();
}

QVariant ComputerModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_Connections.count()) {
        return QVariant();
    }

    const CatalogConnectionView& connection = m_Connections.at(index.row());
    switch (role) {
    case ConnectionIdRole: return connection.identity.toString();
    case ProtocolRole: return connection.protocolName;
    case NameRole: return connection.displayName;
    case OnlineRole: return connection.online;
    case PairedRole: return connection.paired;
    case BusyRole: return connection.busy;
    case WakeableRole: return connection.wakeable;
    case StatusUnknownRole: return connection.statusUnknown;
    case ServerSupportedRole: return connection.serverSupported;
    case DetailsRole: return connection.details;
    case PersistentRole: return connection.persistent;
    case TrustedRole: return connection.trusted;
    case DirectLaunchRole: return connection.directLaunch;
    case AuthenticationKindRole: return connection.authenticationKind;
    case IconSourceRole: return connection.iconSource;
    default: return QVariant();
    }
}

int ComputerModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_Connections.count();
}

QHash<int, QByteArray> ComputerModel::roleNames() const
{
    return {
        {ConnectionIdRole, "connectionId"},
        {ProtocolRole, "protocol"},
        {NameRole, "name"},
        {OnlineRole, "online"},
        {PairedRole, "paired"},
        {BusyRole, "busy"},
        {WakeableRole, "wakeable"},
        {StatusUnknownRole, "statusUnknown"},
        {ServerSupportedRole, "serverSupported"},
        {DetailsRole, "details"},
        {PersistentRole, "persistent"},
        {TrustedRole, "trusted"},
        {DirectLaunchRole, "directLaunch"},
        {AuthenticationKindRole, "authenticationKind"},
        {IconSourceRole, "iconSource"},
    };
}

void ComputerModel::deleteComputer(QString connectionId)
{
    if (m_Catalog != nullptr) {
        m_Catalog->deleteConnection(connectionId);
    }
}

QString ComputerModel::generatePinString(QString connectionId)
{
    return m_Catalog != nullptr ? m_Catalog->generatePairingSecret(connectionId) : QString();
}

void ComputerModel::pairComputer(QString connectionId, QString pin)
{
    if (m_Catalog != nullptr) {
        m_PendingPairingConnectionId = connectionId;
        m_Catalog->pairConnection(connectionId, pin);
    }
}

class DeferredTestConnectionTask final : public QObject, public QRunnable
{
    Q_OBJECT

public:
    void run() override
    {
        const unsigned int result = LiTestClientConnectivity(
                "qt.conntest.moonlight-stream.org", 443, ML_PORT_FLAG_ALL);
        if (result == ML_TEST_RESULT_INCONCLUSIVE) {
            emit connectionTestCompleted(-1, QString());
            return;
        }

        char blockedPorts[512];
        LiStringifyPortFlags(result, "\n", blockedPorts, sizeof(blockedPorts));
        emit connectionTestCompleted(result, QString(blockedPorts));
    }

signals:
    void connectionTestCompleted(int result, QString blockedPorts);
};

void ComputerModel::testConnectionForComputer(QString connectionId)
{
    Q_UNUSED(connectionId);
    auto* task = new DeferredTestConnectionTask();
    connect(task, &DeferredTestConnectionTask::connectionTestCompleted,
            this, &ComputerModel::connectionTestCompleted);
    QThreadPool::globalInstance()->start(task);
}

void ComputerModel::wakeComputer(QString connectionId)
{
    if (m_Catalog != nullptr) {
        m_Catalog->wakeConnection(connectionId);
    }
}

void ComputerModel::renameComputer(QString connectionId, QString name)
{
    if (m_Catalog != nullptr) {
        m_Catalog->renameConnection(connectionId, name);
    }
}

StreamSession* ComputerModel::createSessionForCurrentGame(QString connectionId)
{
    bool found = false;
    const CatalogConnectionView connection = m_Catalog != nullptr
            ? m_Catalog->connection(connectionId, &found)
            : CatalogConnectionView();
    if (!found || !connection.busy) {
        return nullptr;
    }

    QString error;
    StreamSession* session = m_Catalog->createSession(connectionId,
                                                      QString(),
                                                      QString(),
                                                      nullptr,
                                                      &error);
    if (session == nullptr) {
        qWarning() << "Unable to create session for running activity:" << error;
    }
    return session;
}

StreamSession* ComputerModel::createDirectSession(QString connectionId)
{
    bool found = false;
    const CatalogConnectionView connection = m_Catalog != nullptr
            ? m_Catalog->connection(connectionId, &found)
            : CatalogConnectionView();
    if (!found || !connection.directLaunch || !connection.paired) {
        emit operationFailed(tr("This connection is not ready to start."));
        return nullptr;
    }

    QString error;
    StreamSession* session = m_Catalog->createSession(connectionId,
                                                      QString(),
                                                      QString(),
                                                      nullptr,
                                                      &error);
    if (session == nullptr) {
        emit operationFailed(error.isEmpty()
                ? tr("Unable to create the connection session.")
                : error);
    }
    return session;
}

QString ComputerModel::saveConnection(QString connectionId)
{
    if (m_Catalog == nullptr) {
        emit operationFailed(tr("The connection catalog is not available."));
        return {};
    }
    QString error;
    const QString savedId = m_Catalog->saveConnection(connectionId, &error);
    if (savedId.isEmpty()) {
        emit operationFailed(error.isEmpty()
                ? tr("Unable to save the selected connection.")
                : error);
    }
    return savedId;
}

void ComputerModel::requestAuthentication(QString connectionId)
{
    if (m_Catalog != nullptr) {
        m_Catalog->requestAuthentication(connectionId);
    }
}

void ComputerModel::confirmHostTrust(QString connectionId, bool accepted)
{
    if (m_Catalog != nullptr) {
        m_Catalog->confirmHostTrust(connectionId, accepted);
    }
}

void ComputerModel::cancelAuthentication(QString connectionId)
{
    if (m_Catalog != nullptr) {
        m_Catalog->cancelAuthentication(connectionId);
    }
}

void ComputerModel::submitCredentials(QString connectionId,
                                      QString username,
                                      QString password)
{
    if (m_Catalog != nullptr) {
        m_Catalog->submitCredentials(connectionId, username, password);
    }
}

QVariantList ComputerModel::getConnectionAddressesForComputer(QString connectionId) const
{
    return m_Catalog != nullptr ? m_Catalog->connectionEndpoints(connectionId) : QVariantList();
}

bool ComputerModel::hasMultipleConnectionAddresses(QString connectionId) const
{
    return m_Catalog != nullptr && m_Catalog->hasMultipleEndpoints(connectionId);
}

bool ComputerModel::setActiveAddressForComputer(QString connectionId,
                                                QString address,
                                                int port)
{
    return m_Catalog != nullptr && m_Catalog->selectEndpoint(connectionId, address, port);
}

QVariantMap ComputerModel::getSessionOptions(QString connectionId) const
{
    return m_Catalog != nullptr
            ? m_Catalog->connectionSessionOptions(connectionId)
            : QVariantMap();
}

bool ComputerModel::setSessionOptions(QString connectionId,
                                      QVariantMap options)
{
    if (m_Catalog == nullptr) {
        emit operationFailed(tr("The connection catalog is not available."));
        return false;
    }
    QString error;
    if (!m_Catalog->setConnectionSessionOptions(
                connectionId, options, &error)) {
        emit operationFailed(error.isEmpty()
                ? tr("The session options could not be saved.") : error);
        return false;
    }
    return true;
}

bool ComputerModel::resetToAutomaticAddressForComputer(QString connectionId)
{
    return m_Catalog != nullptr && m_Catalog->selectAutomaticEndpoint(connectionId);
}

void ComputerModel::handleConnectionChanged(QString connectionId)
{
    const int oldIndex = indexOf(connectionId);
    const QVector<CatalogConnectionView> updated = m_Catalog->connections();

    bool sameLayout = updated.size() == m_Connections.size();
    if (sameLayout) {
        for (int i = 0; i < updated.size(); ++i) {
            if (updated.at(i).identity != m_Connections.at(i).identity) {
                sameLayout = false;
                break;
            }
        }
    }

    if (!sameLayout) {
        beginResetModel();
        m_Connections = updated;
        endResetModel();
        return;
    }

    m_Connections = updated;
    const int changedIndex = oldIndex >= 0 ? indexOf(connectionId) : -1;
    if (changedIndex >= 0) {
        emit dataChanged(createIndex(changedIndex, 0), createIndex(changedIndex, 0));
    }
}

void ComputerModel::handlePairingCompleted(QString connectionId, QString error)
{
    if (connectionId != m_PendingPairingConnectionId) {
        return;
    }
    m_PendingPairingConnectionId.clear();
    emit pairingCompleted(error.isEmpty() ? QVariant() : error);
}

int ComputerModel::indexOf(const QString& connectionId) const
{
    for (int i = 0; i < m_Connections.size(); ++i) {
        if (m_Connections.at(i).identity.toString() == connectionId) {
            return i;
        }
    }
    return -1;
}

void ComputerModel::refreshConnections()
{
    beginResetModel();
    m_Connections = m_Catalog != nullptr ? m_Catalog->connections()
                                         : QVector<CatalogConnectionView>();
    endResetModel();
}

#include "computermodel.moc"
