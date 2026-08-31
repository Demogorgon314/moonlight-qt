#pragma once

#include <QObject>
#include <QMutex>
#include <QQuickWindow>
#include <QStringList>
#include <QWaitCondition>

#include <atomic>

class StreamSession;

class ActiveStreamLease
{
public:
    static void acquire(StreamSession* session);
    static void release(StreamSession* session);
    static StreamSession* active();

private:
    static QMutex s_Mutex;
    static QWaitCondition s_Released;
    static StreamSession* s_Active;
};

class StreamSession : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList launchWarnings READ launchWarnings NOTIFY launchWarningsChanged)

public:
    enum class State
    {
        Created,
        Initialized,
        Starting,
        Running,
        Finished,
        ReadyForDeletion,
        FailedToInitialize,
    };
    Q_ENUM(State)

    explicit StreamSession(QObject* parent = nullptr);
    ~StreamSession() override;

    Q_INVOKABLE bool initialize(QQuickWindow* qtWindow);
    Q_INVOKABLE void start();
    Q_INVOKABLE void interrupt();

    void setShouldExit(bool quitHostActivity = false);
    QStringList launchWarnings() const;
    State state() const;

    static StreamSession* active() { return ActiveStreamLease::active(); }

signals:
    void stageStarting(QString stage);
    void stageFailed(QString stage, int errorCode, QString failingPorts);
    void connectionStarted();
    void displayLaunchError(QString text);
    void quitStarting();
    void sessionFinished(int protocolResult);
    void readyForDeletion();
    void launchWarningsChanged();

protected:
    virtual bool initializeSession(QQuickWindow* qtWindow) = 0;
    virtual void startSession() = 0;
    virtual void interruptSession() = 0;
    virtual void setShouldExitSession(bool quitHostActivity) = 0;

    void setRunning();
    void addLaunchWarning(const QString& warning);
    void finishSession(int protocolResult);
    void publishReadyForDeletion();

private:
    mutable QMutex m_StateMutex;
    State m_State = State::Created;
    QStringList m_LaunchWarnings;
    std::atomic_bool m_InterruptRequested{false};
    std::atomic_bool m_FinalResultPublished{false};
    std::atomic_bool m_DeletionPublished{false};
    std::atomic_bool m_HoldsLease{false};
};

