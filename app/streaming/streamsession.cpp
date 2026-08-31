#include "streamsession.h"

#include <QMetaObject>
#include <QMutexLocker>

QMutex ActiveStreamLease::s_Mutex;
QWaitCondition ActiveStreamLease::s_Released;
StreamSession* ActiveStreamLease::s_Active = nullptr;

void ActiveStreamLease::acquire(StreamSession* session)
{
    Q_ASSERT(session != nullptr);

    QMutexLocker locker(&s_Mutex);
    while (s_Active != nullptr && s_Active != session) {
        s_Released.wait(&s_Mutex);
    }
    s_Active = session;
}

void ActiveStreamLease::release(StreamSession* session)
{
    QMutexLocker locker(&s_Mutex);
    if (s_Active != session) {
        return;
    }

    s_Active = nullptr;
    s_Released.wakeAll();
}

StreamSession* ActiveStreamLease::active()
{
    QMutexLocker locker(&s_Mutex);
    return s_Active;
}

StreamSession::StreamSession(QObject* parent)
    : QObject(parent)
{
}

StreamSession::~StreamSession()
{
    if (m_HoldsLease.exchange(false)) {
        ActiveStreamLease::release(this);
    }
}

bool StreamSession::initialize(QQuickWindow* qtWindow)
{
    {
        QMutexLocker locker(&m_StateMutex);
        if (m_State != State::Created) {
            return m_State == State::Initialized;
        }
    }

    const bool initialized = initializeSession(qtWindow);
    {
        QMutexLocker locker(&m_StateMutex);
        m_State = initialized ? State::Initialized : State::FailedToInitialize;
    }
    if (!initialized) {
        // Catalog owns sessions, including sessions rejected during preflight. Publish
        // deletion readiness on the next event-loop turn so the QML initialize() call
        // can finish before its borrowed pointer is invalidated.
        QMetaObject::invokeMethod(this,
                                  [this]() {
                                      finishSession(0);
                                      publishReadyForDeletion();
                                  },
                                  Qt::QueuedConnection);
    }
    return initialized;
}

void StreamSession::start()
{
    {
        QMutexLocker locker(&m_StateMutex);
        if (m_State != State::Initialized) {
            return;
        }
        m_State = State::Starting;
    }

    ActiveStreamLease::acquire(this);
    m_HoldsLease.store(true);
    startSession();
}

void StreamSession::interrupt()
{
    if (!m_InterruptRequested.exchange(true)) {
        interruptSession();
    }
}

void StreamSession::setShouldExit(bool quitHostActivity)
{
    setShouldExitSession(quitHostActivity);
}

QStringList StreamSession::launchWarnings() const
{
    QMutexLocker locker(&m_StateMutex);
    return m_LaunchWarnings;
}

StreamSession::State StreamSession::state() const
{
    QMutexLocker locker(&m_StateMutex);
    return m_State;
}

void StreamSession::setRunning()
{
    QMutexLocker locker(&m_StateMutex);
    if (m_State == State::Starting) {
        m_State = State::Running;
    }
}

void StreamSession::addLaunchWarning(const QString& warning)
{
    if (warning.isEmpty()) {
        return;
    }

    {
        QMutexLocker locker(&m_StateMutex);
        m_LaunchWarnings.append(warning);
    }
    emit launchWarningsChanged();
}

void StreamSession::finishSession(int protocolResult)
{
    if (m_FinalResultPublished.exchange(true)) {
        return;
    }

    {
        QMutexLocker locker(&m_StateMutex);
        m_State = State::Finished;
    }
    emit sessionFinished(protocolResult);
}

void StreamSession::publishReadyForDeletion()
{
    if (m_DeletionPublished.exchange(true)) {
        return;
    }

    {
        QMutexLocker locker(&m_StateMutex);
        m_State = State::ReadyForDeletion;
    }
    if (m_HoldsLease.exchange(false)) {
        ActiveStreamLease::release(this);
    }
    emit readyForDeletion();
}
