#include "applecredentialstore.h"

#include <QCoreApplication>
#include <QScopeGuard>
#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#elif defined(Q_OS_DARWIN)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
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
#elif defined(Q_OS_DARWIN)
QString credentialError(OSStatus status)
{
    CFStringRef description = SecCopyErrorMessageString(status, nullptr);
    if (description == nullptr) {
        return QCoreApplication::translate(
                "AppleCredentialStore", "Keychain failed with status %1.")
                .arg(status);
    }
    const CFIndex maximum = CFStringGetMaximumSizeForEncoding(
            CFStringGetLength(description), kCFStringEncodingUTF8) + 1;
    QByteArray utf8(static_cast<int>(maximum), '\0');
    const bool converted = CFStringGetCString(
            description, utf8.data(), maximum, kCFStringEncodingUTF8);
    CFRelease(description);
    return converted
            ? QString::fromUtf8(utf8.constData())
            : QCoreApplication::translate(
                      "AppleCredentialStore", "Keychain failed with status %1.")
                      .arg(status);
}

CFStringRef keychainString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return CFStringCreateWithBytes(kCFAllocatorDefault,
                                   reinterpret_cast<const UInt8*>(utf8.constData()),
                                   utf8.size(),
                                   kCFStringEncodingUTF8,
                                   false);
}

QString qtString(CFStringRef value)
{
    if (value == nullptr) {
        return {};
    }
    const CFIndex maximum = CFStringGetMaximumSizeForEncoding(
            CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
    QByteArray utf8(static_cast<int>(maximum), '\0');
    return CFStringGetCString(value, utf8.data(), maximum,
                              kCFStringEncodingUTF8)
            ? QString::fromUtf8(utf8.constData()) : QString{};
}

CFMutableDictionaryRef keychainQuery(const QString& reference)
{
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
    if (query == nullptr) {
        return nullptr;
    }
    CFStringRef service = keychainString(reference);
    if (service == nullptr) {
        CFRelease(query);
        return nullptr;
    }
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service);
    CFRelease(service);
    return query;
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

QString AppleCredentialStore::displayName()
{
#ifdef Q_OS_WIN
    return QCoreApplication::translate(
            "AppleCredentialStore", "Windows Credential Manager");
#elif defined(Q_OS_DARWIN)
    return QCoreApplication::translate(
            "AppleCredentialStore", "macOS Keychain");
#else
    return QCoreApplication::translate(
            "AppleCredentialStore", "System credential store");
#endif
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
#elif defined(Q_OS_DARWIN)
    CFMutableDictionaryRef item = keychainQuery(reference);
    if (item == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleCredentialStore",
                "Couldn’t allocate a macOS Keychain request."));
        return false;
    }
    const auto releaseItem = qScopeGuard([item]() { CFRelease(item); });
    SecItemDelete(item);
    const QString normalizedUsername = credentials.username.trimmed();
    CFStringRef account = keychainString(normalizedUsername);
    CFStringRef label = keychainString(
            QCoreApplication::translate(
                    "AppleCredentialStore", "Moonlight Apple Screen Sharing — %1")
                    .arg(normalizedUsername));
    const QByteArray password = credentials.password.toUtf8();
    CFDataRef passwordData = CFDataCreate(
            kCFAllocatorDefault,
            reinterpret_cast<const UInt8*>(password.constData()),
            password.size());
    if (account == nullptr || label == nullptr || passwordData == nullptr) {
        if (account != nullptr) CFRelease(account);
        if (label != nullptr) CFRelease(label);
        if (passwordData != nullptr) CFRelease(passwordData);
        setError(error, QCoreApplication::translate(
                "AppleCredentialStore",
                "Couldn’t allocate the macOS Keychain credential."));
        return false;
    }
    CFDictionarySetValue(item, kSecAttrAccount, account);
    CFDictionarySetValue(item, kSecAttrLabel, label);
    CFDictionarySetValue(item, kSecAttrAccessible,
                         kSecAttrAccessibleAfterFirstUnlock);
    CFDictionarySetValue(item, kSecValueData, passwordData);
    const OSStatus status = SecItemAdd(item, nullptr);
    CFRelease(account);
    CFRelease(label);
    CFRelease(passwordData);
    if (status != errSecSuccess) {
        setError(error, credentialError(status));
        return false;
    }
    return true;
#else
    Q_UNUSED(credentials);
    setError(error, QCoreApplication::translate(
            "AppleCredentialStore",
            "Apple Screen Sharing credentials are unsupported on this platform."));
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
#elif defined(Q_OS_DARWIN)
    CFMutableDictionaryRef query = keychainQuery(reference);
    if (query == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleCredentialStore",
                "Couldn’t allocate a macOS Keychain request."));
        return false;
    }
    const auto releaseQuery = qScopeGuard([query]() { CFRelease(query); });
    CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    if (status != errSecSuccess || result == nullptr) {
        setError(error, status == errSecItemNotFound
                ? QCoreApplication::translate(
                          "AppleCredentialStore",
                          "No saved Screen Sharing password was found.")
                : credentialError(status));
        if (result != nullptr) {
            CFRelease(result);
        }
        return false;
    }
    const auto releaseResult = qScopeGuard([result]() { CFRelease(result); });
    if (CFGetTypeID(result) != CFDictionaryGetTypeID()) {
        setError(error, QCoreApplication::translate(
                "AppleCredentialStore", "Keychain returned an invalid credential."));
        return false;
    }
    CFDictionaryRef item = static_cast<CFDictionaryRef>(result);
    CFStringRef account = static_cast<CFStringRef>(
            CFDictionaryGetValue(item, kSecAttrAccount));
    CFDataRef passwordData = static_cast<CFDataRef>(
            CFDictionaryGetValue(item, kSecValueData));
    if (account == nullptr || passwordData == nullptr ||
            CFGetTypeID(account) != CFStringGetTypeID() ||
            CFGetTypeID(passwordData) != CFDataGetTypeID()) {
        setError(error, QCoreApplication::translate(
                "AppleCredentialStore", "Keychain returned an invalid credential."));
        return false;
    }
    credentials->username = qtString(account);
    credentials->password = QString::fromUtf8(
            reinterpret_cast<const char*>(CFDataGetBytePtr(passwordData)),
            static_cast<int>(CFDataGetLength(passwordData)));
    return credentials->validate(error);
#else
    setError(error, QCoreApplication::translate(
            "AppleCredentialStore",
            "Apple Screen Sharing credentials are unsupported on this platform."));
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
#elif defined(Q_OS_DARWIN)
    CFMutableDictionaryRef query = keychainQuery(reference);
    if (query == nullptr) {
        setError(error, QCoreApplication::translate(
                "AppleCredentialStore",
                "Couldn’t allocate a macOS Keychain request."));
        return false;
    }
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    if (status != errSecSuccess && status != errSecItemNotFound) {
        setError(error, credentialError(status));
        return false;
    }
    return true;
#else
    setError(error, QCoreApplication::translate(
            "AppleCredentialStore",
            "Apple Screen Sharing credentials are unsupported on this platform."));
    return false;
#endif
}
