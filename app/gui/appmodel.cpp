#include "appmodel.h"
#include "../backend/nvhttp.h"

#include <QReadLocker>
#include <QWriteLocker>

AppModel::AppModel(QObject *parent)
    : QAbstractListModel(parent),
      m_Computer(nullptr),
      m_Catalog(nullptr),
      m_CurrentGameId(0),
      m_ShowHiddenGames(false)
{
    connect(&m_BoxArtManager, &BoxArtManager::boxArtLoadComplete,
            this, &AppModel::handleBoxArtLoaded);
}

void AppModel::initialize(ComputerCatalog* catalog,
                          QString connectionId,
                          bool showHiddenGames)
{
    m_Catalog = catalog;
    m_ConnectionId = std::move(connectionId);
    connect(m_Catalog, &ComputerCatalog::connectionChanged,
            this, &AppModel::handleConnectionChanged);

    m_Computer = m_Catalog->moonlightComputer(m_ConnectionId);
    Q_ASSERT(m_Computer != nullptr);
    m_CurrentGameId = m_Computer->currentGameId;
    m_ShowHiddenGames = showHiddenGames;

    updateAppList(m_Computer->appList);
}

int AppModel::getRunningAppId()
{
    return m_CurrentGameId;
}

QString AppModel::getRunningAppName()
{
    if (m_CurrentGameId != 0) {
        for (int i = 0; i < m_AllApps.count(); i++) {
            if (m_AllApps[i].id == m_CurrentGameId) {
                return m_AllApps[i].name;
            }
        }
    }

    return nullptr;
}

StreamSession* AppModel::createSessionForApp(int appIndex, const QString& displayId)
{
    Q_ASSERT(appIndex < m_VisibleApps.count());
    const NvApp app = m_VisibleApps.at(appIndex);
    QString error;
    StreamSession* session = m_Catalog->createSession(m_ConnectionId,
                                                      QString::number(app.id),
                                                      displayId,
                                                      nullptr,
                                                      &error);
    if (session == nullptr) {
        qWarning() << "Unable to create stream session:" << error;
    }
    return session;
}

QVariantList AppModel::getDisplayList()
{
    QVariantList displays;

    if (!m_Computer || m_Computer->activeAddress.isNull()) {
        return displays;
    }

    try {
        NvHTTP http(m_Computer);
        displays = http.getDisplays();
    } catch (...) {
        qWarning() << "Failed to get display list";
    }

    return displays;
}

int AppModel::getDirectLaunchAppIndex()
{
    for (int i = 0; i < m_VisibleApps.count(); i++) {
        if (m_VisibleApps[i].directLaunch) {
            return i;
        }
    }

    return -1;
}

int AppModel::rowCount(const QModelIndex &parent) const
{
    // For list models only the root node (an invalid parent) should return the list's size. For all
    // other (valid) parents, rowCount() should return 0 so that it does not become a tree model.
    if (parent.isValid())
        return 0;

    return m_VisibleApps.count();
}

QVariant AppModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    Q_ASSERT(index.row() < m_VisibleApps.count());
    NvApp app = m_VisibleApps.at(index.row());

    switch (role)
    {
    case NameRole:
        return app.name;
    case RunningRole:
        return m_Computer->currentGameId == app.id;
    case BoxArtRole:
        // FIXME: const-correctness
        return const_cast<BoxArtManager&>(m_BoxArtManager).loadBoxArt(m_Computer, app);
    case HiddenRole:
        return app.hidden;
    case AppIdRole:
        return app.id;
    case DirectLaunchRole:
        return app.directLaunch;
    case AppCollectorGameRole:
        return app.isAppCollectorGame;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> AppModel::roleNames() const
{
    QHash<int, QByteArray> names;

    names[NameRole] = "name";
    names[RunningRole] = "running";
    names[BoxArtRole] = "boxart";
    names[HiddenRole] = "hidden";
    names[AppIdRole] = "appid";
    names[DirectLaunchRole] = "directLaunch";
    names[AppCollectorGameRole] = "appCollectorGame";

    return names;
}

void AppModel::quitRunningApp()
{
    m_Catalog->quitRunningActivity(m_ConnectionId);
}

bool AppModel::isAppCurrentlyVisible(const NvApp& app)
{
    for (const NvApp& visibleApp : std::as_const(m_VisibleApps)) {
        if (app.id == visibleApp.id) {
            return true;
        }
    }

    return false;
}

QVector<NvApp> AppModel::getVisibleApps(const QVector<NvApp>& appList)
{
    QVector<NvApp> visibleApps;

    for (const NvApp& app : appList) {
        // Don't immediately hide games that were previously visible. This
        // allows users to easily uncheck the "Hide App" checkbox if they
        // check it by mistake.
        if (m_ShowHiddenGames || !app.hidden || isAppCurrentlyVisible(app)) {
            visibleApps.append(app);
        }
    }

    return visibleApps;
}

void AppModel::updateAppList(QVector<NvApp> newList)
{
    m_AllApps = newList;

    QVector<NvApp> newVisibleList = getVisibleApps(newList);

    // Preserve server-provided ordering by resetting the model
    // when the list content or order changes.
    if (m_VisibleApps != newVisibleList) {
        beginResetModel();
        m_VisibleApps = newVisibleList;
        endResetModel();
    }
}

void AppModel::setAppHidden(int appIndex, bool hidden)
{
    Q_ASSERT(appIndex < m_VisibleApps.count());
    int appId = m_VisibleApps.at(appIndex).id;

    {
        QWriteLocker lock(&m_Computer->lock);

        for (NvApp& app : m_Computer->appList) {
            if (app.id == appId) {
                app.hidden = hidden;
                break;
            }
        }
    }

    m_Catalog->notifyMoonlightClientMetadataChanged(m_ConnectionId);
}

void AppModel::setAppDirectLaunch(int appIndex, bool directLaunch)
{
    Q_ASSERT(appIndex < m_VisibleApps.count());
    int appId = m_VisibleApps.at(appIndex).id;

    {
        QWriteLocker lock(&m_Computer->lock);

        for (NvApp& app : m_Computer->appList) {
            if (directLaunch) {
                // We must clear direct launch from all other apps
                // to set it on the new app.
                app.directLaunch = app.id == appId;
            }
            else if (app.id == appId) {
                // If we're clearing direct launch, we're done once we
                // find our matching app ID.
                app.directLaunch = false;
                break;
            }
        }
    }

    m_Catalog->notifyMoonlightClientMetadataChanged(m_ConnectionId);
}

QVariantList AppModel::getConnectionAddresses()
{
    return m_Catalog->connectionEndpoints(m_ConnectionId);
}

bool AppModel::hasMultipleConnectionAddresses()
{
    return m_Catalog != nullptr && m_Catalog->hasMultipleEndpoints(m_ConnectionId);
}

bool AppModel::setActiveAddress(QString address, int port)
{
    return m_Catalog != nullptr &&
           m_Catalog->selectEndpoint(m_ConnectionId, address, port);
}

bool AppModel::resetToAutomaticAddress()
{
    return m_Catalog != nullptr &&
           m_Catalog->selectAutomaticEndpoint(m_ConnectionId);
}

QVariantMap AppModel::getActiveAddressInfo()
{
    return m_Catalog != nullptr ? m_Catalog->activeEndpoint(m_ConnectionId)
                                : QVariantMap();
}

void AppModel::handleConnectionChanged(QString connectionId)
{
    if (connectionId != m_ConnectionId) {
        return;
    }

    NvComputer* computer = m_Catalog->moonlightComputer(m_ConnectionId);
    if (computer == nullptr) {
        emit computerLost();
        return;
    }
    m_Computer = computer;

    // If the computer has gone offline or we've been unpaired,
    // signal the UI so we can go back to the PC view.
    if (m_Computer->state == NvComputer::CS_OFFLINE ||
            m_Computer->pairState == NvComputer::PS_NOT_PAIRED) {
        emit computerLost();
        return;
    }

    // First, process additions/removals from the app list. This
    // is required because the new game may now be running, so
    // we can't check that first.
    if (computer->appList != m_AllApps) {
        updateAppList(computer->appList);
    }

    // Finally, process changes to the active app
    if (computer->currentGameId != m_CurrentGameId) {
        // First, invalidate the running state of newly running game
        for (int i = 0; i < m_VisibleApps.count(); i++) {
            if (m_VisibleApps[i].id == computer->currentGameId) {
                emit dataChanged(createIndex(i, 0),
                                 createIndex(i, 0),
                                 QVector<int>() << RunningRole);
                break;
            }
        }

        // Next, invalidate the running state of the old game (if it exists)
        if (m_CurrentGameId != 0) {
            for (int i = 0; i < m_VisibleApps.count(); i++) {
                if (m_VisibleApps[i].id == m_CurrentGameId) {
                    emit dataChanged(createIndex(i, 0),
                                     createIndex(i, 0),
                                     QVector<int>() << RunningRole);
                    break;
                }
            }
        }

        // Now update our internal state
        m_CurrentGameId = m_Computer->currentGameId;
    }
}

void AppModel::forceSyncCurrentGame()
{
    // Re-read currentGameId directly from the shared NvComputer state and
    // run the same dataChanged emit logic as handleComputerStateChanged
    // when it differs from our cache. This is a defensive self-heal for
    // cases where the polling thread updated NvComputer through a path
    // that didn't reach our slot (e.g. mDNS PendingAddTask fold or a
    // signal dropped due to receiver-side lifecycle timing).
    if (m_Computer == nullptr) {
        return;
    }

    int currentGameId;
    {
        QReadLocker lock(&m_Computer->lock);
        currentGameId = m_Computer->currentGameId;
    }

    if (currentGameId == m_CurrentGameId) {
        return;
    }

    // Invalidate the running state of the newly running game
    for (int i = 0; i < m_VisibleApps.count(); i++) {
        if (m_VisibleApps[i].id == currentGameId) {
            emit dataChanged(createIndex(i, 0),
                             createIndex(i, 0),
                             QVector<int>() << RunningRole);
            break;
        }
    }

    // Invalidate the running state of the previously running game (if any)
    if (m_CurrentGameId != 0) {
        for (int i = 0; i < m_VisibleApps.count(); i++) {
            if (m_VisibleApps[i].id == m_CurrentGameId) {
                emit dataChanged(createIndex(i, 0),
                                 createIndex(i, 0),
                                 QVector<int>() << RunningRole);
                break;
            }
        }
    }

    m_CurrentGameId = currentGameId;
}

void AppModel::handleBoxArtLoaded(NvComputer* computer, NvApp app, QUrl /* image */)
{
    Q_ASSERT(computer == m_Computer);

    int index = m_VisibleApps.indexOf(app);

    // Make sure we're not delivering a callback to an app that's already been removed
    if (index >= 0) {
        // Let our view know the box art data has changed for this app
        emit dataChanged(createIndex(index, 0),
                         createIndex(index, 0),
                         QVector<int>() << BoxArtRole);
    }
    else {
        qWarning() << "App not found for box art callback:" << app.name;
    }
}
