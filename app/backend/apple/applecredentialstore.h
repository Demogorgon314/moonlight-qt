#pragma once

#include <QString>

struct AppleCredentials
{
    QString username;
    QString password;

    bool validate(QString* error = nullptr) const;
};

class AppleCredentialStore
{
public:
    static QString referenceForConnection(const QString& connectionId);
    static bool isReferenceForConnection(const QString& reference,
                                         const QString& connectionId);
    static QString displayName();

    bool store(const QString& reference,
               const AppleCredentials& credentials,
               QString* error = nullptr) const;
    bool load(const QString& reference,
              AppleCredentials* credentials,
              QString* error = nullptr) const;
    bool remove(const QString& reference, QString* error = nullptr) const;
};
