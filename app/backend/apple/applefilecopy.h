#pragma once

#include "applefiletransfer.h"

#include <QFile>
#include <QHash>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

// Produces one native file-copy message at a time. The callback owns flow
// control, so a bounded transport queue can stop disk reads without buffering
// an entire file in memory.
class AppleFileCopySender
{
public:
    struct OutputMetrics
    {
        quint64 pendingBytes = 0;
        quint64 totalBytesEnqueued = 0;
    };

    using Emit = std::function<bool(const QByteArray& message,
                                    quint64 transferredBytes,
                                    QString* error)>;
    using ReadOutputMetrics = std::function<OutputMetrics()>;
    using ReadTick = std::function<quint32()>;

    explicit AppleFileCopySender(ReadTick readTick = {});

    bool run(const QString& sourcePath,
             quint32 sessionId,
             const Emit& output,
             const ReadOutputMetrics& readOutputMetrics,
             std::atomic_bool* cancelled,
             QString* error = nullptr) const;

private:
    ReadTick m_ReadTick;
};

class AppleFileCopyReceiver
{
public:
    struct Update
    {
        std::optional<double> progress;
        QString completedPath;
    };

    AppleFileCopyReceiver(const QString& destinationDirectory,
                          const QString& requestedName,
                          QString* error = nullptr);
    ~AppleFileCopyReceiver();

    bool isValid() const;
    bool receive(const AppleFileCopyReceiverFrame& frame,
                 Update* update,
                 QString* error = nullptr);
    void abort();

private:
    class Inflater;

    enum class Fork
    {
        Data,
        Resource,
    };

    struct CurrentItem
    {
        QString path;
        AppleFileCopyItemMetadata metadata;
        Fork fork = Fork::Data;
        quint64 remainingBytes = 0;
        std::unique_ptr<QFile> file;
        bool discardsBytes = false;
    };

    bool receiveItem(const AppleFileCopyItemMetadata& item, QString* error);
    bool receiveData(const QByteArray& data,
                     Update* update,
                     QString* error);
    bool finishCurrentFork(QString* error);
    bool applyMetadata(const QString& path,
                       const AppleFileCopyItemMetadata& item,
                       QString* error) const;
    QString createItem(const AppleFileCopyItemMetadata& item,
                       const QString& parent,
                       const QString& preferredName,
                       QString* error);
    static bool isValidComponent(const QString& name);
    static QString uniqueName(const QString& name, int suffix);
    static bool removeCreatedRoot(const QString& path);

    QString m_DestinationDirectory;
    QString m_RequestedName;
    std::optional<AppleFileCopySummary> m_Summary;
    QHash<quint16, QString> m_ParentDirectories;
    std::optional<CurrentItem> m_CurrentItem;
    std::unique_ptr<Inflater> m_Inflater;
    QString m_RootPath;
    AppleFileCopyItemMetadata m_RootMetadata;
    quint64 m_TransferredBytes = 0;
    bool m_Valid = false;
    bool m_DidFinish = false;
};
