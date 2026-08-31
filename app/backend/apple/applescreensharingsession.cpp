#include "applescreensharingsession.h"

#include "appleauthenticator.h"
#include "applecredentialstore.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>

namespace {

class AppleControlSessionTask final : public QRunnable
{
public:
    AppleControlSessionTask(AppleScreenSharingSession* session,
                            AppleSavedConnection connection,
                            std::atomic_bool* cancelled)
        : m_Session(session),
          m_Connection(std::move(connection)),
          m_Cancelled(cancelled)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        QString error;
        AppleTcpTransport transport;
        AppleAuthenticator authenticator;
        AppleAuthenticatedControl authenticated;
        const bool authenticatedSuccessfully = authenticator.authenticate(
                transport,
                m_Connection.endpoint,
                m_Connection.trustedHostFingerprint,
                [reference = m_Connection.credentialReference,
                 connectionId = m_Connection.id](
                        AppleCredentials* credentials,
                        QString* credentialError) {
                    if (!AppleCredentialStore::isReferenceForConnection(
                                reference, connectionId)) {
                        if (credentialError != nullptr) {
                            *credentialError = QCoreApplication::translate(
                                    "AppleScreenSharingSession",
                                    "The saved credential binding is invalid.");
                        }
                        return false;
                    }
                    return AppleCredentialStore().load(reference, credentials, credentialError);
                },
                &authenticated,
                m_Cancelled,
                &error);

        bool succeeded = authenticatedSuccessfully;
        if (succeeded) {
            AppleControlChannel control;
            succeeded = control.negotiate(
                    transport, authenticated.masterKey, m_Cancelled, &error);
            if (succeeded) {
                // Stage 2 proves an authenticated, encrypted, ordered control write.
                // Media negotiation is deliberately absent until stage 3.
                succeeded = control.sendEncrypted(
                        transport,
                        QByteArray::fromHex("0a000000"),
                        m_Cancelled,
                        &error);
            }
        }
        transport.close();

        const QPointer<AppleScreenSharingSession> session = m_Session;
        if (session != nullptr) {
            QMetaObject::invokeMethod(session,
                                      [session, succeeded, error]() {
                                          if (session != nullptr) {
                                              session->complete(succeeded, error);
                                          }
                                      },
                                      Qt::QueuedConnection);
        }
    }

private:
    QPointer<AppleScreenSharingSession> m_Session;
    AppleSavedConnection m_Connection;
    std::atomic_bool* m_Cancelled;
};

} // namespace

AppleScreenSharingSession::AppleScreenSharingSession(AppleSavedConnection connection,
                                                     QObject* parent)
    : StreamSession(parent),
      m_Connection(std::move(connection))
{
}

AppleScreenSharingSession::~AppleScreenSharingSession() = default;

bool AppleScreenSharingSession::initializeSession(QQuickWindow*)
{
    if (!m_Connection.isValid() || !m_Connection.isTrusted() ||
            !AppleCredentialStore::isReferenceForConnection(
                    m_Connection.credentialReference, m_Connection.id)) {
        return false;
    }
    addLaunchWarning(tr("Stage 2 verifies the encrypted Apple control session; media streaming is not enabled yet."));
    return true;
}

void AppleScreenSharingSession::startSession()
{
    emit stageStarting(tr("Apple authentication"));
    QThreadPool::globalInstance()->start(
            new AppleControlSessionTask(this, m_Connection, &m_Cancelled));
}

void AppleScreenSharingSession::interruptSession()
{
    m_Cancelled.store(true);
}

void AppleScreenSharingSession::setShouldExitSession(bool)
{
    interrupt();
}

void AppleScreenSharingSession::complete(bool success, const QString& error)
{
    if (success) {
        setRunning();
    }
    else if (!m_Cancelled.load() && !error.isEmpty()) {
        emit displayLaunchError(error);
    }
    finishSession(success ? 0 : -1);
    publishReadyForDeletion();
}
