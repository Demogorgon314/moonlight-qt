#include "applecredentialstore.h"

#include <QCoreApplication>
#include <QScopeGuard>
#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

namespace {

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
}

#ifdef Q_OS_WIN
QString credentialError(DWORD code)
{
    return QCoreApplication::translate(
            "AppleCredentialStore",
            "Windows Credential Manager failed with error %1.").arg(code);
}
#endif

} // namespace

bool AppleCredentials::validate(QString* error) const
{
    const QString normalizedUsername = username.trimmed();
    if (normalizedUsername.isEmpty()) {
        setError(error, QCoreApplication::translate(
                "AppleCredentialStore",
                "Enter the account name for the remote Mac."));
        return false;
    }
    if (password.isEmpty()) {
        setError(error, QCoreApplication::translate(
                "AppleCredentialStore",
                "Enter the password for the remote Mac."));
        return false;
    }
    if (normalizedUsername.contains(QChar('\0')) || password.contains(QChar('\0'))) {
        setError(error, QCoreApplication::translate(
                "AppleCredentialStore",
                "The account name and password cannot contain a null character."));
        return false;
    }
    if (normalizedUsername.toUtf8().size() >= 64 || password.toUtf8().size() >= 64) {
        setError(error, QCoreApplication::translate(
                "AppleCredentialStore",
                "The account name and password must each be shorter than 64 UTF-8 bytes."));
        return false;
    }
    return true;
}

QString AppleCredentialStore::referenceForConnection(const QString& connectionId)
{
    return QStringLiteral("MoonlightVPlus/AppleScreenSharing/%1").arg(connectionId);
}

bool AppleCredentialStore::isReferenceForConnection(const QString& reference,
                                                     const QString& connectionId)
{
    return !connectionId.isEmpty() && reference == referenceForConnection(connectionId);
}

bool AppleCredentialStore::store(const QString& reference,
                                 const AppleCredentials& credentials,
                                 QString* error) const
{
    QString validationError;
    if (reference.isEmpty() || !credentials.validate(&validationError)) {
        setError(error, validationError.isEmpty()
                ? QCoreApplication::translate("AppleCredentialStore", "Invalid credential reference.")
                : validationError);
        return false;
    }

#ifdef Q_OS_WIN
    const std::wstring target = reference.toStdWString();
    const std::wstring username = credentials.username.trimmed().toStdWString();
    const QByteArray password = credentials.password.toUtf8();
    CREDENTIALW credential = {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(target.c_str());
    credential.UserName = const_cast<wchar_t*>(username.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(password.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(
            const_cast<char*>(password.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&credential, 0)) {
        setError(error, credentialError(GetLastError()));
        return false;
    }
    return true;
#else
    Q_UNUSED(credentials);
    setError(error, QCoreApplication::translate(
            "AppleCredentialStore",
            "Apple Screen Sharing credentials are supported only on Windows x64."));
    return false;
#endif
}

bool AppleCredentialStore::load(const QString& reference,
                                AppleCredentials* credentials,
                                QString* error) const
{
    if (reference.isEmpty() || credentials == nullptr) {
        setError(error, QCoreApplication::translate("AppleCredentialStore", "Invalid credential reference."));
        return false;
    }

#ifdef Q_OS_WIN
    const std::wstring target = reference.toStdWString();
    PCREDENTIALW stored = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &stored)) {
        setError(error, GetLastError() == ERROR_NOT_FOUND
                ? QCoreApplication::translate("AppleCredentialStore", "No saved Screen Sharing password was found.")
                : credentialError(GetLastError()));
        return false;
    }
    const auto freeCredential = qScopeGuard([stored]() { CredFree(stored); });
    credentials->username = stored->UserName == nullptr
            ? QString()
            : QString::fromWCharArray(stored->UserName);
    credentials->password = QString::fromUtf8(
            reinterpret_cast<const char*>(stored->CredentialBlob),
            static_cast<int>(stored->CredentialBlobSize));
    return credentials->validate(error);
#else
    setError(error, QCoreApplication::translate(
            "AppleCredentialStore",
            "Apple Screen Sharing credentials are supported only on Windows x64."));
    return false;
#endif
}

bool AppleCredentialStore::remove(const QString& reference, QString* error) const
{
    if (reference.isEmpty()) {
        return true;
    }
#ifdef Q_OS_WIN
    const std::wstring target = reference.toStdWString();
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
        const DWORD code = GetLastError();
        if (code != ERROR_NOT_FOUND) {
            setError(error, credentialError(code));
            return false;
        }
    }
    return true;
#else
    setError(error, QCoreApplication::translate(
            "AppleCredentialStore",
            "Apple Screen Sharing credentials are supported only on Windows x64."));
    return false;
#endif
}
