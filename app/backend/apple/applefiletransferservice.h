#pragma once

#include "applefiletransfer.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>

struct AppleFileTransferEvent
{
    enum class Kind
    {
        RemoteDrag,
        Started,
        Progress,
        Paused,
        Completing,
        Completed,
        Failed,
        Cancelled,
    };

    enum class Direction
    {
        ToRemote,
        FromRemote,
    };

    Kind kind = Kind::Progress;
    Direction direction = Direction::ToRemote;
    quint32 sessionId = 0;
    QString name;
    QString path;
    QString errorText;
    double progress = 0.0;
    double bytesPerSecond = 0.0;
    AppleRemoteFileDrag remoteDrag;
};

enum class AppleFileTransferWaitResult
{
    Pending,
    Completed,
    Failed,
};

// Owns all file-copy state behind a small thread-safe interface. Encrypted
// record I/O remains in AppleScreenSharingSession's network loop; background
// workers can only fill this module's bounded, low-priority queue.
class AppleFileTransferService
{
public:
    AppleFileTransferService();
    ~AppleFileTransferService();

    void setAvailable(bool available);
    void setControlling(bool controlling);
    bool isAvailable() const;

    bool beginLocalDrop(const QStringList& paths,
                        QList<QByteArray>* messages,
                        QString* error = nullptr);
    void cancelLocalDrop(QList<QByteArray>* messages = nullptr);
    bool acceptRemoteDrag(const AppleRemoteFileDrag& drag,
                          const QString& destinationDirectory,
                          QString* error = nullptr,
                          QList<quint32>* sessionIds = nullptr);

    // Materializes one native file-promise batch. This is intentionally a
    // blocking interface for platform adapters to call from their extraction
    // worker; all session bookkeeping, cancellation, and partial outcomes stay
    // inside this module.
    bool materializeRemoteDrag(
            const AppleRemoteFileDrag& drag,
            const QString& destinationDirectory,
            const std::atomic_bool& cancelled,
            QStringList* completedPaths,
            QString* error = nullptr);

    // Materializes one AppKit file promise at Finder's exact coordinated URL.
    // The wire protocol names the remote root independently, so Finder's
    // de-duplicated name must be supplied to the receiver explicitly.
    bool materializeRemoteFile(
            const QString& sourcePath,
            const QString& destinationPath,
            const std::atomic_bool& cancelled,
            QString* completedPath,
            QString* error = nullptr);

    // Returns true when the encrypted plaintext belongs to file transfer,
    // including incomplete fragments and malformed file messages.
    bool receive(const QByteArray& fragment, QString* diagnostic = nullptr);
    // Returns encrypted-record fragments for up to maximumMessages complete
    // logical file-copy messages. Fragments from one message are never split
    // across calls because other control records cannot be interleaved.
    QList<QByteArray> takeOutbound(int maximumMessages = 1);
    QList<AppleFileTransferEvent> takeEvents();

    bool setPaused(quint32 sessionId, bool paused);
    bool cancel(quint32 sessionId);
    void reset();
    void close();

private:
    bool acceptRemoteFiles(
            const QStringList& sourcePaths,
            const QStringList& requestedNames,
            const QString& destinationDirectory,
            QString* error,
            QList<quint32>* sessionIds);
    bool materializeRemoteFiles(
            const QStringList& sourcePaths,
            const QStringList& requestedNames,
            const QString& destinationDirectory,
            const std::atomic_bool& cancelled,
            QStringList* completedPaths,
            QString* error);
    AppleFileTransferWaitResult waitForRemoteFiles(
            const QList<quint32>& sessionIds,
            int timeoutMilliseconds,
            QStringList* completedPaths,
            QString* error = nullptr);

    struct State;
    std::unique_ptr<State> m_State;
};
