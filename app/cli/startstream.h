#pragma once

#include <QObject>
#include <QVariant>

class ComputerCatalog;
class NvComputer;
class StreamSession;
class StreamingPreferences;

namespace CliStartStream
{

class Event;
class LauncherPrivate;

class Launcher : public QObject
{
    Q_OBJECT
    Q_DECLARE_PRIVATE_D(m_DPtr, Launcher)

public:
    explicit Launcher(QString computer, QString app,
                      StreamingPreferences* preferences,
                      QObject *parent = nullptr);
    ~Launcher();
    Q_INVOKABLE void execute(ComputerCatalog *catalog);
    Q_INVOKABLE void quitRunningApp();
    Q_INVOKABLE bool isExecuted() const;

signals:
    void searchingComputer();
    void searchingApp();
    void sessionCreated(QString appName, StreamSession *session);
    void failed(QString text);
    void appQuitRequired(QString appName);

private slots:
    void onComputerFound(NvComputer *computer);
    void onComputerUpdated(NvComputer *computer);
    void onTimeout();
    void onQuitAppCompleted(QVariant error);

private:
    QScopedPointer<LauncherPrivate> m_DPtr;
};

}
