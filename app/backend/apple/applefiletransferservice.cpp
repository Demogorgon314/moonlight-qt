#include "applefiletransferservice.h"

#include "applefilecopy.h"
#include "appleprotocol.h"

#include <QDir>
#include <QDebug>
#include <QFileInfo>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr qsizetype MaximumQueuedFileBytes = 20 * 1024 * 1024;
constexpr int MaximumConcurrentSenders = 4;

quint64 monotonicNanoseconds()
{
    return static_cast<quint64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
}

void setError(QString* error, const QString& value)
{
    if (error != nullptr) *error = value;
}

QString remoteBaseName(const QString& path)
{
    const QString normalized = QString(path).replace('\\', '/');
    return normalized.section('/', -1, -1);
}

QString remoteRejectionMessage(const QString& destinationPath,
                               quint16 errorCode)
{
    const QString normalized = QString(destinationPath).replace('\\', '/');
    if (errorCode == 5 &&
            (normalized == QStringLiteral("/Volumes") ||
             normalized.startsWith(QStringLiteral("/Volumes/")))) {
        return QStringLiteral(
                "The Mac could not commit the file to the selected external "
                "volume (error code 5). macOS Screen Sharing may reject "
                "direct transfers to external volumes; transfer to a folder "
                "on the Mac's internal disk first.");
    }
    return QStringLiteral(
            "The Mac rejected the completed file transfer (error code %1).")
            .arg(errorCode);
}

} // namespace

struct AppleFileTransferService::State
{
    struct PendingDrop
    {
        quint32 sessionId = 0;
        QStringList paths;
    };

    struct Outgoing
    {
        quint32 sessionId = 0;
        QString path;
        QString destinationPath;
        quint64 totalBytes = 0;
        quint64 transferredBytes = 0;
        quint64 rateSampleStartedAt = 0;
        quint64 rateSampleBytes = 0;
        quint64 queuedProducerBytes = 0;
        quint64 totalProducerBytes = 0;
        double lastPublishedProgress = 0.0;
        bool hasStarted = false;
        bool paused = false;
        bool cancelled = false;
        bool senderFinished = false;
        bool remoteFinished = false;
        quint16 remoteError = 0;
    };

    struct Outbound
    {
        QByteArray message;
        std::weak_ptr<Outgoing> owner;
        quint64 producerBytes = 0;
    };

    struct Incoming
    {
        quint32 sessionId = 0;
        QString sourcePath;
        QString requestedName;
        std::unique_ptr<AppleFileCopyReceiver> receiver;
        double lastPublishedProgress = 0.0;
        bool hasStarted = false;
        bool paused = false;
        bool tracksCompletion = false;
    };

    struct IncomingOutcome
    {
        bool succeeded = false;
        QString path;
        QString error;
    };

    struct Queued
    {
        AppleFileTransferEvent::Direction direction =
                AppleFileTransferEvent::Direction::ToRemote;
        quint32 sessionId = 0;
    };

    mutable std::mutex mutex;
    std::condition_variable changed;
    bool available = false;
    bool controlling = true;
    bool closed = false;
    quint32 nextSessionId = 0;
    int activeSenderCount = 0;
    PendingDrop pendingDrop;
    quint32 remoteDragSessionId = 0;
    QHash<quint32, std::shared_ptr<Outgoing>> outgoing;
    QHash<quint32, std::shared_ptr<Incoming>> incoming;
    QHash<quint32, IncomingOutcome> incomingOutcomes;
    std::deque<Queued> queued;
    std::deque<Outbound> outbound;
    qsizetype outboundBytes = 0;
    QList<AppleFileTransferEvent> events;
    std::vector<std::thread> workers;
    AppleFileTransferReassembler reassembler;

    int activeFileCopyCountLocked() const
    {
        int count = 0;
        for (const auto& transfer : outgoing) {
            if (transfer->hasStarted) ++count;
        }
        for (const auto& transfer : incoming) {
            if (transfer->hasStarted) ++count;
        }
        return count;
    }

    void removeQueuedLocked(quint32 sessionId,
                            AppleFileTransferEvent::Direction direction)
    {
        for (auto iterator = queued.begin(); iterator != queued.end();) {
            if (iterator->sessionId == sessionId &&
                    iterator->direction == direction) {
                iterator = queued.erase(iterator);
            }
            else {
                ++iterator;
            }
        }
    }

    quint32 allocateSessionIdLocked()
    {
        do {
            ++nextSessionId;
            if (nextSessionId == 0) ++nextSessionId;
        } while (outgoing.contains(nextSessionId) ||
                 incoming.contains(nextSessionId) ||
                 pendingDrop.sessionId == nextSessionId);
        return nextSessionId;
    }

    void queueLocked(const QByteArray& message)
    {
        if (message.isEmpty()) return;
        outbound.push_back({message, {}, 0});
        outboundBytes += message.size();
    }

    void discardOutboundLocked(const std::shared_ptr<Outgoing>& transfer)
    {
        for (auto iterator = outbound.begin(); iterator != outbound.end();) {
            const std::shared_ptr<Outgoing> owner = iterator->owner.lock();
            if (owner != transfer) {
                ++iterator;
                continue;
            }
            outboundBytes -= iterator->message.size();
            transfer->queuedProducerBytes =
                    transfer->queuedProducerBytes >= iterator->producerBytes
                    ? transfer->queuedProducerBytes - iterator->producerBytes
                    : 0;
            iterator = outbound.erase(iterator);
        }
    }

    void eventLocked(AppleFileTransferEvent::Kind kind,
                     AppleFileTransferEvent::Direction direction,
                     quint32 sessionId,
                     const QString& name,
                     const QString& path = {},
                     double progress = 0.0,
                     double bytesPerSecond = 0.0,
                     const QString& errorText = {})
    {
        AppleFileTransferEvent event;
        event.kind = kind;
        event.direction = direction;
        event.sessionId = sessionId;
        event.name = name;
        event.path = path;
        event.errorText = errorText;
        event.progress = progress;
        event.bytesPerSecond = bytesPerSecond;
        events.append(std::move(event));
    }

    void recordIncomingOutcomeLocked(
            const std::shared_ptr<Incoming>& transfer,
            bool succeeded,
            const QString& path,
            const QString& error)
    {
        if (transfer->tracksCompletion) {
            incomingOutcomes.insert(
                    transfer->sessionId,
                    {succeeded, path, error});
        }
        changed.notify_all();
    }

    bool enqueueWorkerMessage(const std::shared_ptr<Outgoing>& transfer,
                              const QByteArray& message,
                              quint64 transferredBytes,
                              QString* error)
    {
        AppleFileCopyReceiverFrame frame;
        QString parseError;
        if (AppleFileTransferProtocol::parseReceiverFrame(
                    message, &frame, &parseError)) {
            if (frame.kind == AppleFileCopyReceiverFrame::Kind::Summary) {
                std::lock_guard<std::mutex> lock(mutex);
                transfer->totalBytes = frame.summary.logicalBytes;
            }
        }
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&]() {
                // Match the native producer: inspect the existing backlog
                // before appending, so one complete logical message may take
                // the queue slightly beyond the soft limit.
                return closed || transfer->cancelled ||
                        (!transfer->paused &&
                         transfer->queuedProducerBytes <=
                                 MaximumQueuedFileBytes);
            });
            if (closed || transfer->cancelled) {
                setError(error, QStringLiteral("The file transfer was cancelled."));
                return false;
            }
            // A type-34 file-copy message may span encrypted records. Queue
            // it as one unit so input or display controls cannot appear
            // between its fragments and corrupt the Mac's reassembly stream.
            const quint64 producerBytes =
                    static_cast<quint64>(message.size()) + 6;
            outbound.push_back({message, transfer, producerBytes});
            outboundBytes += message.size();
            transfer->queuedProducerBytes += producerBytes;
            transfer->totalProducerBytes += producerBytes;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (transferredBytes > 0) {
            transfer->transferredBytes += transferredBytes;
            transfer->rateSampleBytes += transferredBytes;
            const quint64 now = monotonicNanoseconds();
            if (transfer->rateSampleStartedAt == 0) {
                transfer->rateSampleStartedAt = now;
            }
            double rate = 0.0;
            if (now - transfer->rateSampleStartedAt >= 250'000'000) {
                rate = transfer->rateSampleBytes * 1'000'000'000.0 /
                        (now - transfer->rateSampleStartedAt);
                transfer->rateSampleStartedAt = now;
                transfer->rateSampleBytes = 0;
            }
            const double progress = transfer->totalBytes == 0 ? 0.0
                    : qMin(1.0,
                           static_cast<double>(transfer->transferredBytes) /
                                   transfer->totalBytes);
            if (progress >= 1.0 ||
                    progress - transfer->lastPublishedProgress >= 0.025 ||
                    rate > 0.0) {
                transfer->lastPublishedProgress = progress;
                eventLocked(AppleFileTransferEvent::Kind::Progress,
                            AppleFileTransferEvent::Direction::ToRemote,
                            transfer->sessionId,
                            QFileInfo(transfer->path).fileName(),
                            transfer->path,
                            progress,
                            rate);
            }
        }
        return true;
    }

    void startQueuedLocked()
    {
        while (!closed && activeFileCopyCountLocked() <
                        MaximumConcurrentSenders && !queued.empty()) {
            const Queued next = queued.front();
            queued.pop_front();
            if (next.direction ==
                    AppleFileTransferEvent::Direction::ToRemote) {
                const auto transfer = outgoing.value(next.sessionId);
                if (!transfer || transfer->hasStarted || transfer->cancelled) {
                    continue;
                }
                transfer->hasStarted = true;
                queueLocked(AppleFileTransferProtocol::startFileReceive(
                        transfer->sessionId, transfer->destinationPath));
                qInfo().noquote()
                        << "Apple file upload started: session="
                        << transfer->sessionId
                        << "name=" << QFileInfo(transfer->path).fileName()
                        << "destination=" << transfer->destinationPath;
                eventLocked(AppleFileTransferEvent::Kind::Started,
                            AppleFileTransferEvent::Direction::ToRemote,
                            transfer->sessionId,
                            QFileInfo(transfer->path).fileName(),
                            transfer->path);
                workers.emplace_back([this, transfer]() {
                    runSender(transfer);
                });
            }
            else {
                const auto transfer = incoming.value(next.sessionId);
                if (!transfer || transfer->hasStarted) continue;
                transfer->hasStarted = true;
                queueLocked(AppleFileTransferProtocol::startFileSend(
                        transfer->sessionId, transfer->sourcePath));
                eventLocked(AppleFileTransferEvent::Kind::Started,
                            AppleFileTransferEvent::Direction::FromRemote,
                            transfer->sessionId,
                            transfer->requestedName);
            }
        }
    }

    void finishOutgoingLocked(const std::shared_ptr<Outgoing>& transfer)
    {
        if (!transfer->senderFinished || !transfer->remoteFinished) return;
        const bool succeeded = transfer->remoteError == 0;
        const QString failure = succeeded
                ? QString()
                : remoteRejectionMessage(
                          transfer->destinationPath, transfer->remoteError);
        eventLocked(succeeded ? AppleFileTransferEvent::Kind::Completed
                              : AppleFileTransferEvent::Kind::Failed,
                    AppleFileTransferEvent::Direction::ToRemote,
                    transfer->sessionId,
                    QFileInfo(transfer->path).fileName(),
                    transfer->path,
                    succeeded ? 1.0 : 0.0,
                    0.0,
                    failure);
        outgoing.remove(transfer->sessionId);
        startQueuedLocked();
    }

    void runSender(const std::shared_ptr<Outgoing>& transfer)
    {
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&]() {
                return closed || transfer->cancelled ||
                        activeSenderCount < MaximumConcurrentSenders;
            });
            if (closed || transfer->cancelled) return;
            ++activeSenderCount;
            transfer->rateSampleStartedAt = monotonicNanoseconds();
        }

        std::atomic_bool cancelled{false};
        AppleFileCopySender sender;
        QString error;
        const bool succeeded = sender.run(
                transfer->path,
                transfer->sessionId,
                [this, transfer, &cancelled](const QByteArray& message,
                                             quint64 bytes,
                                             QString* callbackError) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        cancelled.store(closed || transfer->cancelled);
                    }
                    return enqueueWorkerMessage(
                            transfer, message, bytes, callbackError);
                },
                [this, transfer]() {
                    std::lock_guard<std::mutex> lock(mutex);
                    AppleFileCopySender::OutputMetrics metrics;
                    metrics.pendingBytes = transfer->queuedProducerBytes;
                    metrics.totalBytesEnqueued = transfer->totalProducerBytes;
                    return metrics;
                },
                &cancelled,
                &error);

        std::lock_guard<std::mutex> lock(mutex);
        --activeSenderCount;
        changed.notify_all();
        if (closed || transfer->cancelled) return;
        if (!succeeded) {
            eventLocked(AppleFileTransferEvent::Kind::Failed,
                        AppleFileTransferEvent::Direction::ToRemote,
                        transfer->sessionId,
                        QFileInfo(transfer->path).fileName(),
                        transfer->path,
                        0.0,
                        0.0,
                        error);
            outgoing.remove(transfer->sessionId);
            startQueuedLocked();
            return;
        }
        transfer->senderFinished = true;
        transfer->lastPublishedProgress = 1.0;
        eventLocked(AppleFileTransferEvent::Kind::Completing,
                    AppleFileTransferEvent::Direction::ToRemote,
                    transfer->sessionId,
                    QFileInfo(transfer->path).fileName(),
                    transfer->path,
                    1.0);
        finishOutgoingLocked(transfer);
    }
};

AppleFileTransferService::AppleFileTransferService()
    : m_State(std::make_unique<State>())
{
}

AppleFileTransferService::~AppleFileTransferService()
{
    close();
}

void AppleFileTransferService::setAvailable(bool available)
{
    std::lock_guard<std::mutex> lock(m_State->mutex);
    m_State->available = available;
}

void AppleFileTransferService::setControlling(bool controlling)
{
    std::lock_guard<std::mutex> lock(m_State->mutex);
    m_State->controlling = controlling;
    if (!controlling) m_State->pendingDrop = {};
}

bool AppleFileTransferService::isAvailable() const
{
    std::lock_guard<std::mutex> lock(m_State->mutex);
    return m_State->available && m_State->controlling && !m_State->closed;
}

bool AppleFileTransferService::beginLocalDrop(
        const QStringList& paths,
        QList<QByteArray>* messages,
        QString* error)
{
    if (messages == nullptr || paths.isEmpty()) {
        setError(error, QStringLiteral("No files were selected for transfer."));
        return false;
    }
    QList<QUrl> urls;
    QStringList normalized;
    for (const QString& path : paths) {
        const QFileInfo info(path);
        if ((!info.exists() && !info.isSymLink()) ||
                (!info.isFile() && !info.isDir() && !info.isSymLink())) {
            setError(error, QStringLiteral("The selected item cannot be transferred: %1")
                               .arg(QDir::toNativeSeparators(path)));
            return false;
        }
        normalized.append(info.absoluteFilePath());
        urls.append(QUrl::fromLocalFile(info.absoluteFilePath()));
    }

    std::lock_guard<std::mutex> lock(m_State->mutex);
    if (!m_State->available || !m_State->controlling || m_State->closed) {
        setError(error, QStringLiteral("File transfer is not available in this session."));
        return false;
    }
    if (m_State->pendingDrop.sessionId != 0) {
        messages->append(AppleFileTransferProtocol::cancelDrop(
                m_State->pendingDrop.sessionId));
    }
    const quint32 sessionId = m_State->allocateSessionIdLocked();
    const QByteArray announcement = AppleFileTransferProtocol::beginDrop(
            urls, sessionId, error);
    if (announcement.isEmpty()) return false;
    m_State->pendingDrop = {sessionId, normalized};
    messages->append(announcement);
    return true;
}

void AppleFileTransferService::cancelLocalDrop(QList<QByteArray>* messages)
{
    std::lock_guard<std::mutex> lock(m_State->mutex);
    if (m_State->pendingDrop.sessionId != 0 && messages != nullptr) {
        messages->append(AppleFileTransferProtocol::cancelDrop(
                m_State->pendingDrop.sessionId));
    }
    m_State->pendingDrop = {};
}

bool AppleFileTransferService::cancelRemoteDrag(
        quint32 sessionId,
        QList<QByteArray>* messages)
{
    std::lock_guard<std::mutex> lock(m_State->mutex);
    if (sessionId == 0 || m_State->remoteDragSessionId != sessionId) {
        return false;
    }
    m_State->remoteDragSessionId = 0;
    if (!m_State->closed && messages != nullptr) {
        messages->append(AppleFileTransferProtocol::cancelDrop(sessionId));
    }
    return true;
}

bool AppleFileTransferService::acceptRemoteDrag(
        const AppleRemoteFileDrag& drag,
        const QString& destinationDirectory,
        QString* error,
        QList<quint32>* sessionIds)
{
    QStringList requestedNames;
    requestedNames.reserve(drag.sourcePaths.size());
    for (const QString& sourcePath : drag.sourcePaths) {
        requestedNames.append(remoteBaseName(sourcePath));
    }
    return acceptRemoteFiles(
            drag.sourcePaths,
            requestedNames,
            destinationDirectory,
            error,
            sessionIds);
}

bool AppleFileTransferService::acceptRemoteFiles(
        const QStringList& sourcePaths,
        const QStringList& requestedNames,
        const QString& destinationDirectory,
        QString* error,
        QList<quint32>* sessionIds)
{
    if (sessionIds != nullptr) sessionIds->clear();
    if (sourcePaths.isEmpty() ||
            sourcePaths.size() != requestedNames.size() ||
            !QDir().mkpath(destinationDirectory)) {
        setError(error, QStringLiteral("The remote file destination is invalid."));
        return false;
    }
    std::vector<std::unique_ptr<AppleFileCopyReceiver>> receivers;
    receivers.reserve(static_cast<size_t>(requestedNames.size()));
    for (const QString& name : requestedNames) {
        QString receiverError;
        auto receiver = std::make_unique<AppleFileCopyReceiver>(
                destinationDirectory, name, &receiverError);
        if (!receiver->isValid()) {
            setError(error, receiverError);
            return false;
        }
        receivers.push_back(std::move(receiver));
    }
    std::lock_guard<std::mutex> lock(m_State->mutex);
    if (!m_State->available || !m_State->controlling || m_State->closed) {
        setError(error, QStringLiteral("File transfer is not available in this session."));
        return false;
    }
    for (qsizetype index = 0; index < sourcePaths.size(); ++index) {
        const QString& sourcePath = sourcePaths.at(index);
        const QString& name = requestedNames.at(index);
        const quint32 sessionId = m_State->allocateSessionIdLocked();
        auto transfer = std::make_shared<State::Incoming>();
        transfer->sessionId = sessionId;
        transfer->sourcePath = sourcePath;
        transfer->requestedName = name;
        transfer->receiver = std::move(
                receivers[static_cast<size_t>(index)]);
        transfer->tracksCompletion = sessionIds != nullptr;
        m_State->incoming.insert(sessionId, transfer);
        m_State->queued.push_back(
                {AppleFileTransferEvent::Direction::FromRemote, sessionId});
        if (sessionIds != nullptr) sessionIds->append(sessionId);
    }
    m_State->startQueuedLocked();
    return true;
}

bool AppleFileTransferService::materializeRemoteDrag(
        const AppleRemoteFileDrag& drag,
        const QString& destinationDirectory,
        const std::atomic_bool& cancelled,
        QStringList* completedPaths,
        QString* error)
{
    QStringList requestedNames;
    requestedNames.reserve(drag.sourcePaths.size());
    for (const QString& sourcePath : drag.sourcePaths) {
        requestedNames.append(remoteBaseName(sourcePath));
    }
    return materializeRemoteFiles(
            drag.sourcePaths,
            requestedNames,
            destinationDirectory,
            cancelled,
            completedPaths,
            error);
}

bool AppleFileTransferService::materializeRemoteFiles(
        const QStringList& sourcePaths,
        const QStringList& requestedNames,
        const QString& destinationDirectory,
        const std::atomic_bool& cancelled,
        QStringList* completedPaths,
        QString* error)
{
    if (completedPaths != nullptr) completedPaths->clear();
    QList<quint32> sessionIds;
    if (!acceptRemoteFiles(
                sourcePaths,
                requestedNames,
                destinationDirectory,
                error,
                &sessionIds)) {
        return false;
    }

    while (!cancelled.load()) {
        const AppleFileTransferWaitResult result = waitForRemoteFiles(
                sessionIds, 100, completedPaths, error);
        if (result == AppleFileTransferWaitResult::Completed) return true;
        if (result == AppleFileTransferWaitResult::Failed) {
            for (const quint32 sessionId : std::as_const(sessionIds)) {
                cancel(sessionId);
            }
            return false;
        }
    }

    for (const quint32 sessionId : std::as_const(sessionIds)) {
        cancel(sessionId);
    }
    setError(error, QStringLiteral(
            "The promised-file transfer was cancelled."));
    return false;
}

bool AppleFileTransferService::materializeRemoteFile(
        const QString& sourcePath,
        const QString& destinationPath,
        const std::atomic_bool& cancelled,
        QString* completedPath,
        QString* error)
{
    if (completedPath != nullptr) completedPath->clear();
    const QFileInfo destinationInfo(destinationPath);
    const QString requestedName = destinationInfo.fileName();
    const QString destinationDirectory = destinationInfo.absolutePath();
    if (sourcePath.isEmpty() || requestedName.isEmpty() ||
            destinationDirectory.isEmpty()) {
        setError(error, QStringLiteral(
                "The promised-file destination is invalid."));
        return false;
    }

    QStringList paths;
    if (!materializeRemoteFiles(
                {sourcePath},
                {requestedName},
                destinationDirectory,
                cancelled,
                &paths,
                error)) {
        return false;
    }

    const QString promisedPath = destinationInfo.absoluteFilePath();
    if (paths.size() != 1 ||
            QFileInfo(paths.first()).absoluteFilePath() != promisedPath) {
        setError(error, QStringLiteral(
                "The promised file could not be written to Finder's destination."));
        return false;
    }
    if (completedPath != nullptr) *completedPath = promisedPath;
    return true;
}

AppleFileTransferWaitResult AppleFileTransferService::waitForRemoteFiles(
        const QList<quint32>& sessionIds,
        int timeoutMilliseconds,
        QStringList* completedPaths,
        QString* error)
{
    if (completedPaths != nullptr) completedPaths->clear();
    if (sessionIds.isEmpty()) {
        setError(error, QStringLiteral("The promised-file batch is empty."));
        return AppleFileTransferWaitResult::Failed;
    }

    std::unique_lock<std::mutex> lock(m_State->mutex);
    const auto state = [&]() {
        bool pending = false;
        for (const quint32 sessionId : sessionIds) {
            const auto outcome = m_State->incomingOutcomes.constFind(sessionId);
            if (outcome != m_State->incomingOutcomes.cend()) {
                if (!outcome->succeeded) {
                    setError(error, outcome->error.isEmpty()
                                     ? QStringLiteral("The Mac could not provide a promised file.")
                                     : outcome->error);
                    return AppleFileTransferWaitResult::Failed;
                }
                continue;
            }
            if (m_State->incoming.contains(sessionId)) {
                pending = true;
                continue;
            }
            setError(error, m_State->closed
                             ? QStringLiteral("The file-transfer session ended.")
                             : QStringLiteral("A promised-file transfer ended without a result."));
            return AppleFileTransferWaitResult::Failed;
        }
        return pending ? AppleFileTransferWaitResult::Pending
                       : AppleFileTransferWaitResult::Completed;
    };

    AppleFileTransferWaitResult result = state();
    if (result == AppleFileTransferWaitResult::Pending &&
            timeoutMilliseconds > 0) {
        m_State->changed.wait_for(
                lock, std::chrono::milliseconds(timeoutMilliseconds));
        result = state();
    }
    if (result != AppleFileTransferWaitResult::Completed) return result;

    if (completedPaths != nullptr) {
        for (const quint32 sessionId : sessionIds) {
            completedPaths->append(
                    m_State->incomingOutcomes.value(sessionId).path);
        }
    }
    for (const quint32 sessionId : sessionIds) {
        m_State->incomingOutcomes.remove(sessionId);
    }
    return AppleFileTransferWaitResult::Completed;
}

bool AppleFileTransferService::receive(
        const QByteArray& fragment,
        QString* diagnostic)
{
    std::optional<QByteArray> complete;
    QString error;
    if (!m_State->reassembler.receive(fragment, &complete, &error)) {
        return false;
    }
    if (!error.isEmpty()) {
        setError(diagnostic, error);
        return true;
    }
    if (!complete.has_value()) return true;
    const QByteArray& message = *complete;
    const quint8 type = static_cast<quint8>(message.at(0));

    if (type == 0x1e) {
        AppleFileTransferRequest request;
        if (!AppleFileTransferProtocol::parseFileRequest(
                    message, &request, &error)) {
            setError(diagnostic, error);
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(m_State->mutex);
            if (m_State->closed ||
                    request.sessionId != m_State->pendingDrop.sessionId) {
                return true;
            }
            const QStringList paths = m_State->pendingDrop.paths;
            qInfo().noquote()
                    << "Apple file drop accepted by Mac: session="
                    << request.sessionId
                    << "destination=" << request.destinationPath
                    << "items=" << paths.size();
            m_State->pendingDrop = {};
            for (const QString& path : paths) {
                auto transfer = std::make_shared<State::Outgoing>();
                transfer->sessionId = m_State->allocateSessionIdLocked();
                transfer->path = path;
                transfer->destinationPath = request.destinationPath;
                m_State->outgoing.insert(transfer->sessionId, transfer);
                m_State->queued.push_back(
                        {AppleFileTransferEvent::Direction::ToRemote,
                         transfer->sessionId});
            }
            m_State->startQueuedLocked();
        }
        return true;
    }

    if (type == 0x20) {
        std::optional<AppleRemoteFileDrag> drag;
        if (!AppleFileTransferProtocol::parseRemoteDrag(
                    message, &drag, &error)) {
            setError(diagnostic, error);
            return true;
        }
        std::lock_guard<std::mutex> lock(m_State->mutex);
        if (m_State->closed) return true;
        AppleFileTransferEvent event;
        event.kind = AppleFileTransferEvent::Kind::RemoteDrag;
        event.direction = AppleFileTransferEvent::Direction::FromRemote;
        if (drag.has_value()) {
            event.sessionId = drag->sessionId;
            event.remoteDrag = *drag;
            m_State->remoteDragSessionId = drag->sessionId;
        }
        else {
            m_State->remoteDragSessionId = 0;
        }
        m_State->events.append(std::move(event));
        return true;
    }

    bool ok = false;
    const quint16 command = AppleWire::readUInt16(message, 8, &ok);
    const quint32 sessionId = AppleWire::readUInt32(message, 10, &ok);
    if (!ok) {
        setError(diagnostic, QStringLiteral("The Mac sent a truncated file-copy message."));
        return true;
    }

    std::lock_guard<std::mutex> lock(m_State->mutex);
    if (const auto incoming = m_State->incoming.value(sessionId)) {
        AppleFileCopyReceiverFrame frame;
        if (!AppleFileTransferProtocol::parseReceiverFrame(
                    message, &frame, &error)) {
            incoming->receiver->abort();
            m_State->queueLocked(AppleFileTransferProtocol::control(
                    sessionId, AppleFileTransferControl::Stop));
            m_State->eventLocked(AppleFileTransferEvent::Kind::Failed,
                                 AppleFileTransferEvent::Direction::FromRemote,
                                 sessionId, incoming->requestedName, {},
                                 0.0, 0.0, error);
            m_State->recordIncomingOutcomeLocked(
                    incoming, false, {}, error);
            m_State->incoming.remove(sessionId);
            m_State->startQueuedLocked();
            setError(diagnostic, error);
            return true;
        }
        AppleFileCopyReceiver::Update update;
        if (!incoming->receiver->receive(frame, &update, &error)) {
            incoming->receiver->abort();
            if (!(frame.kind == AppleFileCopyReceiverFrame::Kind::Done &&
                  frame.status != 0)) {
                m_State->queueLocked(AppleFileTransferProtocol::control(
                        sessionId, AppleFileTransferControl::Stop));
            }
            m_State->eventLocked(AppleFileTransferEvent::Kind::Failed,
                                 AppleFileTransferEvent::Direction::FromRemote,
                                 sessionId, incoming->requestedName, {},
                                 0.0, 0.0, error);
            m_State->recordIncomingOutcomeLocked(
                    incoming, false, {}, error);
            m_State->incoming.remove(sessionId);
            m_State->startQueuedLocked();
            setError(diagnostic, error);
            return true;
        }
        if (update.progress.has_value()) {
            const double progress = *update.progress;
            if (progress >= 1.0 ||
                    progress - incoming->lastPublishedProgress >= 0.025) {
                incoming->lastPublishedProgress = progress;
                m_State->eventLocked(AppleFileTransferEvent::Kind::Progress,
                                     AppleFileTransferEvent::Direction::FromRemote,
                                     sessionId, incoming->requestedName, {},
                                     progress);
            }
        }
        if (!update.completedPath.isEmpty()) {
            m_State->eventLocked(AppleFileTransferEvent::Kind::Completed,
                                 AppleFileTransferEvent::Direction::FromRemote,
                                 sessionId, incoming->requestedName,
                                 update.completedPath, 1.0);
            m_State->recordIncomingOutcomeLocked(
                    incoming, true, update.completedPath, {});
            m_State->incoming.remove(sessionId);
            m_State->startQueuedLocked();
        }
        return true;
    }

    if (const auto outgoing = m_State->outgoing.value(sessionId)) {
        if (command == 3 || command == 4 || command == 5) {
            outgoing->paused = command == 3;
            if (command == 5) {
                outgoing->cancelled = true;
                m_State->discardOutboundLocked(outgoing);
                m_State->eventLocked(
                        AppleFileTransferEvent::Kind::Cancelled,
                        AppleFileTransferEvent::Direction::ToRemote,
                        sessionId,
                        QFileInfo(outgoing->path).fileName(),
                        outgoing->path);
                m_State->outgoing.remove(sessionId);
                m_State->startQueuedLocked();
            }
            else {
                m_State->eventLocked(
                        command == 3
                                ? AppleFileTransferEvent::Kind::Paused
                                : AppleFileTransferEvent::Kind::Started,
                        AppleFileTransferEvent::Direction::ToRemote,
                        sessionId,
                        QFileInfo(outgoing->path).fileName(),
                        outgoing->path);
            }
            m_State->changed.notify_all();
            return true;
        }
        AppleFileTransferResponse response;
        if (!AppleFileTransferProtocol::parseResponse(
                    message, &response, &error)) {
            setError(diagnostic, error);
            return true;
        }
        if (response.kind == AppleFileTransferResponse::Kind::Progress) {
            const double progress = qBound(
                    outgoing->lastPublishedProgress,
                    response.fraction,
                    1.0);
            if (progress > outgoing->lastPublishedProgress) {
                outgoing->lastPublishedProgress = progress;
                m_State->eventLocked(AppleFileTransferEvent::Kind::Progress,
                                     AppleFileTransferEvent::Direction::ToRemote,
                                     sessionId,
                                     QFileInfo(outgoing->path).fileName(),
                                     outgoing->path,
                                     progress);
            }
        }
        else {
            outgoing->remoteFinished = true;
            outgoing->remoteError = response.errorCode;
            qInfo().noquote()
                    << "Apple file upload completion: session="
                    << response.sessionId
                    << "errorCode=" << response.errorCode
                    << "name=" << response.name
                    << "producerBytes=" << outgoing->transferredBytes
                    << "destination=" << outgoing->destinationPath;
            if (response.errorCode != 0) {
                outgoing->cancelled = true;
                m_State->discardOutboundLocked(outgoing);
                m_State->eventLocked(
                        AppleFileTransferEvent::Kind::Failed,
                        AppleFileTransferEvent::Direction::ToRemote,
                        sessionId,
                        QFileInfo(outgoing->path).fileName(),
                        outgoing->path,
                        0.0,
                        0.0,
                        remoteRejectionMessage(
                                outgoing->destinationPath,
                                response.errorCode));
                m_State->outgoing.remove(sessionId);
                m_State->startQueuedLocked();
                m_State->changed.notify_all();
                return true;
            }
            m_State->finishOutgoingLocked(outgoing);
        }
        return true;
    }
    return true;
}

QList<QByteArray> AppleFileTransferService::takeOutbound(
        int maximumMessages)
{
    QList<QByteArray> result;
    std::lock_guard<std::mutex> lock(m_State->mutex);
    while (maximumMessages-- > 0 && !m_State->outbound.empty()) {
        State::Outbound outbound = std::move(m_State->outbound.front());
        m_State->outbound.pop_front();
        m_State->outboundBytes -= outbound.message.size();
        if (const std::shared_ptr<State::Outgoing> owner =
                    outbound.owner.lock()) {
            owner->queuedProducerBytes =
                    owner->queuedProducerBytes >= outbound.producerBytes
                    ? owner->queuedProducerBytes - outbound.producerBytes
                    : 0;
        }
        result.append(AppleFileTransferProtocol::fragments(outbound.message));
    }
    m_State->changed.notify_all();
    return result;
}

QList<AppleFileTransferEvent> AppleFileTransferService::takeEvents()
{
    std::lock_guard<std::mutex> lock(m_State->mutex);
    QList<AppleFileTransferEvent> result = std::move(m_State->events);
    m_State->events.clear();
    return result;
}

bool AppleFileTransferService::setPaused(quint32 sessionId, bool paused)
{
    std::lock_guard<std::mutex> lock(m_State->mutex);
    if (const auto transfer = m_State->outgoing.value(sessionId)) {
        if (!transfer->hasStarted || transfer->senderFinished ||
                transfer->paused == paused ||
                transfer->cancelled) return false;
        transfer->paused = paused;
        m_State->queueLocked(AppleFileTransferProtocol::control(
                sessionId, paused ? AppleFileTransferControl::Pause
                                  : AppleFileTransferControl::Resume));
        m_State->eventLocked(paused ? AppleFileTransferEvent::Kind::Paused
                                    : AppleFileTransferEvent::Kind::Started,
                             AppleFileTransferEvent::Direction::ToRemote,
                             sessionId, QFileInfo(transfer->path).fileName(),
                             transfer->path);
        m_State->changed.notify_all();
        return true;
    }
    if (const auto transfer = m_State->incoming.value(sessionId)) {
        if (!transfer->hasStarted || transfer->paused == paused) return false;
        transfer->paused = paused;
        m_State->queueLocked(AppleFileTransferProtocol::control(
                sessionId, paused ? AppleFileTransferControl::Pause
                                  : AppleFileTransferControl::Resume));
        m_State->eventLocked(paused ? AppleFileTransferEvent::Kind::Paused
                                    : AppleFileTransferEvent::Kind::Started,
                             AppleFileTransferEvent::Direction::FromRemote,
                             sessionId, transfer->requestedName);
        return true;
    }
    return false;
}

bool AppleFileTransferService::cancel(quint32 sessionId)
{
    std::lock_guard<std::mutex> lock(m_State->mutex);
    if (const auto transfer = m_State->outgoing.value(sessionId)) {
        transfer->cancelled = true;
        m_State->discardOutboundLocked(transfer);
        if (transfer->hasStarted) {
            m_State->queueLocked(AppleFileTransferProtocol::control(
                    sessionId, AppleFileTransferControl::Stop));
        }
        m_State->eventLocked(AppleFileTransferEvent::Kind::Cancelled,
                             AppleFileTransferEvent::Direction::ToRemote,
                             sessionId, QFileInfo(transfer->path).fileName(),
                             transfer->path);
        m_State->outgoing.remove(sessionId);
        m_State->removeQueuedLocked(
                sessionId, AppleFileTransferEvent::Direction::ToRemote);
        m_State->startQueuedLocked();
        m_State->changed.notify_all();
        return true;
    }
    if (const auto transfer = m_State->incoming.value(sessionId)) {
        transfer->receiver->abort();
        if (transfer->hasStarted) {
            m_State->queueLocked(AppleFileTransferProtocol::control(
                    sessionId, AppleFileTransferControl::Stop));
        }
        m_State->eventLocked(AppleFileTransferEvent::Kind::Cancelled,
                             AppleFileTransferEvent::Direction::FromRemote,
                             sessionId, transfer->requestedName);
        m_State->recordIncomingOutcomeLocked(
                transfer, false, {},
                QStringLiteral("The promised-file transfer was cancelled."));
        m_State->incoming.remove(sessionId);
        m_State->removeQueuedLocked(
                sessionId, AppleFileTransferEvent::Direction::FromRemote);
        m_State->startQueuedLocked();
        return true;
    }
    return false;
}

void AppleFileTransferService::reset()
{
    close();
    std::lock_guard<std::mutex> lock(m_State->mutex);
    m_State->closed = false;
    m_State->available = false;
    m_State->controlling = true;
    m_State->pendingDrop = {};
    m_State->remoteDragSessionId = 0;
    m_State->queued.clear();
    m_State->incomingOutcomes.clear();
    m_State->events.clear();
    m_State->reassembler.reset();
}

void AppleFileTransferService::close()
{
    if (!m_State) return;
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(m_State->mutex);
        if (m_State->closed && m_State->workers.empty()) return;
        m_State->closed = true;
        m_State->pendingDrop = {};
        m_State->remoteDragSessionId = 0;
        m_State->queued.clear();
        for (const auto& transfer : std::as_const(m_State->outgoing)) {
            transfer->cancelled = true;
        }
        for (const auto& transfer : std::as_const(m_State->incoming)) {
            transfer->receiver->abort();
        }
        m_State->outgoing.clear();
        m_State->incoming.clear();
        m_State->incomingOutcomes.clear();
        m_State->outbound.clear();
        m_State->outboundBytes = 0;
        workers = std::move(m_State->workers);
        m_State->changed.notify_all();
    }
    for (std::thread& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}
