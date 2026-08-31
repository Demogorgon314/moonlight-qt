#pragma once

#include "backend/computercatalog.h"
#include "streaming/streamsession.h"

#include <QAbstractListModel>
#include <QVariantList>

class ComputerModel : public QAbstractListModel
{
    Q_OBJECT

    enum Roles
    {
        ConnectionIdRole = Qt::UserRole,
        ProtocolRole,
        NameRole,
        OnlineRole,
        PairedRole,
        BusyRole,
        WakeableRole,
        StatusUnknownRole,
        ServerSupportedRole,
        DetailsRole,
    };

public:
    explicit ComputerModel(QObject* object = nullptr);

    Q_INVOKABLE void initialize(ComputerCatalog* catalog);

    QVariant data(const QModelIndex& index, int role) const override;
    int rowCount(const QModelIndex& parent) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void deleteComputer(QString connectionId);
    Q_INVOKABLE QString generatePinString(QString connectionId);
    Q_INVOKABLE void pairComputer(QString connectionId, QString pin);
    Q_INVOKABLE void testConnectionForComputer(QString connectionId);
    Q_INVOKABLE void wakeComputer(QString connectionId);
    Q_INVOKABLE void renameComputer(QString connectionId, QString name);
    Q_INVOKABLE StreamSession* createSessionForCurrentGame(QString connectionId);
    Q_INVOKABLE QVariantList getConnectionAddressesForComputer(QString connectionId) const;
    Q_INVOKABLE bool hasMultipleConnectionAddresses(QString connectionId) const;
    Q_INVOKABLE bool setActiveAddressForComputer(QString connectionId,
                                                 QString address,
                                                 int port);
    Q_INVOKABLE bool resetToAutomaticAddressForComputer(QString connectionId);

signals:
    void pairingCompleted(QVariant error);
    void connectionTestCompleted(int result, QString blockedPorts);

private slots:
    void handleConnectionChanged(QString connectionId);
    void handlePairingCompleted(QString connectionId, QString error);

private:
    int indexOf(const QString& connectionId) const;
    void refreshConnections();

    QVector<CatalogConnectionView> m_Connections;
    ComputerCatalog* m_Catalog = nullptr;
    QString m_PendingPairingConnectionId;
};
