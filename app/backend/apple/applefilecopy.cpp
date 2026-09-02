#include "applefilecopy.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>

#include <zlib.h>

#include <chrono>
#include <limits>
#include <utility>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef Q_OS_DARWIN
#include <sys/xattr.h>
#endif
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr qint64 MacEpochOffsetSeconds = 2'082'844'800;

void setError(QString* error, const QString& value)
{
    if (error != nullptr) {
        *error = value;
    }
}

bool fail(QString* error, const QString& value)
{
    setError(error, value);
    return false;
}

struct TransferItem
{
    QString path;
    AppleFileCopyItemType type = AppleFileCopyItemType::File;
    quint16 level = 0;
    quint64 size = 0;
    quint64 resourceForkSize = 0;
};

QByteArray symbolicLinkTarget(const QString& path)
{
#ifdef Q_OS_UNIX
    const QByteArray encodedPath = QFile::encodeName(path);
    QByteArray value(4096, Qt::Uninitialized);
    const ssize_t count = ::readlink(
            encodedPath.constData(), value.data(), value.size());
    if (count > 0) {
        value.resize(static_cast<int>(count));
        return value;
    }
#endif
    return QFile::encodeName(QFileInfo(path).symLinkTarget());
}

#ifdef Q_OS_DARWIN
QByteArray extendedAttribute(const QString& path, const char* name)
{
    const QByteArray encodedPath = QFile::encodeName(path);
    const ssize_t size = getxattr(encodedPath.constData(), name,
                                  nullptr, 0, 0, XATTR_NOFOLLOW);
    if (size < 0 || size > AppleFileTransferProtocol::MaximumFileCopyBodyLength) {
        return {};
    }
    QByteArray value(static_cast<int>(size), Qt::Uninitialized);
    const ssize_t count = getxattr(encodedPath.constData(), name,
                                   value.data(), value.size(), 0,
                                   XATTR_NOFOLLOW);
    return count == size ? value : QByteArray();
}

quint64 resourceForkSize(const QString& path)
{
    const QByteArray encodedPath = QFile::encodeName(path);
    const ssize_t size = getxattr(encodedPath.constData(),
                                  "com.apple.ResourceFork",
                                  nullptr, 0, 0, XATTR_NOFOLLOW);
    return size > 0 ? static_cast<quint64>(size) : 0;
}
#else
quint64 resourceForkSize(const QString&)
{
    return 0;
}
#endif

AppleFileCopyUtcDateTime wireDate(const QDateTime& value)
{
    if (!value.isValid()) return {};
    const qint64 milliseconds = value.toMSecsSinceEpoch();
    const qint64 seconds = milliseconds / 1000;
    const qint64 remainder = qAbs(milliseconds % 1000);
    AppleFileCopyUtcDateTime result;
    const qint64 macSeconds = seconds + MacEpochOffsetSeconds;
    result.seconds = macSeconds > 0 ? static_cast<quint64>(macSeconds) : 0;
    result.fraction = static_cast<quint16>(
            remainder * std::numeric_limits<quint16>::max() / 1000);
    return result;
}

QDateTime localDate(const AppleFileCopyUtcDateTime& value)
{
    if (value.seconds == 0) return {};
    const quint64 maximumSeconds = static_cast<quint64>(
            std::numeric_limits<qint64>::max() / 1000);
    if (value.seconds > maximumSeconds) return {};
    const qint64 unixSeconds = static_cast<qint64>(value.seconds) -
            MacEpochOffsetSeconds;
    const qint64 milliseconds = unixSeconds * 1000 +
            static_cast<qint64>(value.fraction) * 1000 /
                    std::numeric_limits<quint16>::max();
    return QDateTime::fromMSecsSinceEpoch(milliseconds, QTimeZone::UTC);
}

quint16 wireMode(const QFileInfo& info, AppleFileCopyItemType type)
{
#ifdef Q_OS_WIN
    // QFileInfo maps the current token's Windows ACL access into every POSIX
    // permission class. Forwarding that synthetic value produces 0666 files,
    // unlike Apple's native sender (0644 files and 0755 directories), and the
    // Mac applies this catalog mode while committing the completed transfer.
    // Preserve the only portable Windows permission distinction: read-only.
    const bool writable = info.isWritable();
    switch (type) {
    case AppleFileCopyItemType::Directory:
        return static_cast<quint16>(0040000 | (writable ? 0755 : 0555));
    case AppleFileCopyItemType::SymbolicLink:
        return static_cast<quint16>(0120000 | 0755);
    case AppleFileCopyItemType::File:
        return static_cast<quint16>(0100000 | (writable ? 0644 : 0444));
    }
#endif
    quint16 mode = type == AppleFileCopyItemType::Directory ? 0040000
            : type == AppleFileCopyItemType::SymbolicLink ? 0120000
                                                          : 0100000;
    const QFile::Permissions permissions = info.permissions();
    if (permissions & QFileDevice::ReadOwner) mode |= 0400;
    if (permissions & QFileDevice::WriteOwner) mode |= 0200;
    if (permissions & QFileDevice::ExeOwner) mode |= 0100;
    if (permissions & QFileDevice::ReadGroup) mode |= 0040;
    if (permissions & QFileDevice::WriteGroup) mode |= 0020;
    if (permissions & QFileDevice::ExeGroup) mode |= 0010;
    if (permissions & QFileDevice::ReadOther) mode |= 0004;
    if (permissions & QFileDevice::WriteOther) mode |= 0002;
    if (permissions & QFileDevice::ExeOther) mode |= 0001;
    return mode;
}

quint64 allocatedFileSize(const QString& path, quint64 fallback)
{
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    const HANDLE handle = CreateFileW(
            reinterpret_cast<LPCWSTR>(nativePath.utf16()),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        FILE_STANDARD_INFO information{};
        const bool valid = GetFileInformationByHandleEx(
                handle,
                FileStandardInfo,
                &information,
                sizeof(information)) != FALSE;
        CloseHandle(handle);
        if (valid && information.AllocationSize.QuadPart >= 0) {
            return static_cast<quint64>(information.AllocationSize.QuadPart);
        }
    }
#elif defined(Q_OS_UNIX)
    const QByteArray encodedPath = QFile::encodeName(path);
    struct stat information{};
    if (::lstat(encodedPath.constData(), &information) == 0 &&
            information.st_blocks >= 0) {
        return static_cast<quint64>(information.st_blocks) * 512;
    }
#endif
    return fallback;
}

bool appendTree(const QString& path,
                quint16 level,
                QList<TransferItem>* items,
                AppleFileCopySummary* summary,
                QString* error)
{
    if (items == nullptr || summary == nullptr) return false;
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        return fail(error, QStringLiteral("The source item no longer exists: %1")
                                   .arg(QDir::toNativeSeparators(path)));
    }
    TransferItem item;
    item.path = info.absoluteFilePath();
    item.level = level;
    if (info.isSymLink()) {
        item.type = AppleFileCopyItemType::SymbolicLink;
        item.size = static_cast<quint64>(
                symbolicLinkTarget(item.path).size());
        ++summary->fileCount;
        summary->logicalBytes += item.size;
        items->append(std::move(item));
        return true;
    }
    if (info.isDir()) {
        item.type = AppleFileCopyItemType::Directory;
        ++summary->folderCount;
        items->append(std::move(item));
        if (level == std::numeric_limits<quint16>::max()) {
            return fail(error, QStringLiteral("The source directory is nested too deeply."));
        }
        const QFileInfoList children = QDir(path).entryInfoList(
                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden |
                        QDir::System,
                QDir::Name | QDir::DirsFirst);
        for (const QFileInfo& child : children) {
            if (!appendTree(child.absoluteFilePath(), level + 1,
                            items, summary, error)) {
                return false;
            }
        }
        return true;
    }
    if (!info.isFile()) {
        return fail(error, QStringLiteral("The source contains an unsupported item: %1")
                                   .arg(QDir::toNativeSeparators(path)));
    }
    item.type = AppleFileCopyItemType::File;
    item.size = static_cast<quint64>(qMax<qint64>(0, info.size()));
    item.resourceForkSize = resourceForkSize(item.path);
    ++summary->fileCount;
    summary->logicalBytes += item.size + item.resourceForkSize;
    summary->physicalBytes += allocatedFileSize(item.path, item.size);
    items->append(std::move(item));
    return true;
}

AppleFileCopyItemMetadata metadataFor(const TransferItem& item)
{
    const QFileInfo info(item.path);
    AppleFileCopyItemMetadata metadata;
    metadata.type = item.type;
    if (item.type == AppleFileCopyItemType::SymbolicLink) {
        metadata.finderInfo = QByteArrayLiteral("slnkrhap");
        metadata.finderInfo.append(char(0x80));
        metadata.finderInfo.append(char(0));
        metadata.finderInfo.append(QByteArray(22, '\0'));
        metadata.symbolicLinkTarget = symbolicLinkTarget(item.path);
    }
    metadata.dataForkSize = item.type == AppleFileCopyItemType::Directory
            ? 0 : item.size;
    metadata.resourceForkSize = item.resourceForkSize;
    metadata.creationDate = wireDate(info.birthTime());
    metadata.contentModificationDate = wireDate(info.lastModified());
    metadata.attributeModificationDate = wireDate(info.metadataChangeTime());
    metadata.accessDate = wireDate(info.lastRead());
    metadata.nodeFlags = item.type == AppleFileCopyItemType::Directory
            ? quint16(0x10) : quint16(0);
    metadata.level = item.level;
    metadata.mode = wireMode(info, item.type);
    metadata.textEncodingHint = 0x7e;
    metadata.name = info.fileName();
#ifdef Q_OS_DARWIN
    if (item.type != AppleFileCopyItemType::SymbolicLink) {
        const QByteArray finderInfo = extendedAttribute(
                item.path, "com.apple.FinderInfo");
        if (finderInfo.size() >= 32) {
            metadata.finderInfo = finderInfo.left(32);
        }
        for (const char* name : {"com.apple.quarantine",
                                 "com.apple.metadata:kMDItemWhereFroms"}) {
            const QByteArray value = extendedAttribute(item.path, name);
            if (!value.isNull()) {
                metadata.extendedAttributes.append(
                        {QString::fromUtf8(name), value});
            }
        }
    }
#endif
    return metadata;
}

bool extensionIsAlreadyCompressed(const QString& name)
{
    static const QSet<QString> extensions = {
        QStringLiteral("cpio"), QStringLiteral("gz"), QStringLiteral("bz2"),
        QStringLiteral("zip"), QStringLiteral("z"), QStringLiteral("lz"),
        QStringLiteral("lzo"), QStringLiteral("rz"), QStringLiteral("Z"),
        QStringLiteral("7z"), QStringLiteral("ace"), QStringLiteral("rar"),
        QStringLiteral("arc"), QStringLiteral("cab"), QStringLiteral("cpt"),
        QStringLiteral("sit"), QStringLiteral("tgz"), QStringLiteral("aac"),
        QStringLiteral("mp3"), QStringLiteral("mp2"), QStringLiteral("mpa"),
        QStringLiteral("avi"), QStringLiteral("h264"), QStringLiteral("mov"),
        QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mjpg"),
        QStringLiteral("mkv"), QStringLiteral("mob"), QStringLiteral("moov"),
        QStringLiteral("mpg2"),
    };
    return extensions.contains(QFileInfo(name).suffix());
}

class Deflater
{
public:
    Deflater()
    {
        m_Valid = deflateInit2(
                &m_Stream,
                9,
                Z_DEFLATED,
                15,
                9,
                Z_DEFAULT_STRATEGY) == Z_OK;
    }

    ~Deflater()
    {
        if (m_Valid) deflateEnd(&m_Stream);
    }

    QByteArray compress(const QByteArray& input)
    {
        if (!m_Valid || input.isEmpty()) return {};
        QByteArray output(static_cast<int>(deflateBound(
                                  &m_Stream,
                                  static_cast<uLong>(input.size()))) + 16,
                          Qt::Uninitialized);
        m_Stream.next_in = reinterpret_cast<Bytef*>(
                const_cast<char*>(input.constData()));
        m_Stream.avail_in = static_cast<uInt>(input.size());
        m_Stream.next_out = reinterpret_cast<Bytef*>(output.data());
        m_Stream.avail_out = static_cast<uInt>(output.size());
        const int result = deflate(&m_Stream, Z_SYNC_FLUSH);
        if (result != Z_OK || m_Stream.avail_in != 0) return {};
        output.resize(output.size() - static_cast<int>(m_Stream.avail_out));
        return output;
    }

private:
    z_stream m_Stream = {};
    bool m_Valid = false;
};

class CompressionPolicy
{
public:
    explicit CompressionPolicy(quint32 startTick)
        : m_StartTick(startTick)
    {
    }

    bool shouldCompress(
            const QString& fileName,
            const AppleFileCopySender::OutputMetrics& metrics,
            quint32 currentTick)
    {
        if (m_SavingsAreTooSmall || extensionIsAlreadyCompressed(fileName) ||
                metrics.pendingBytes == 0) {
            return false;
        }
        const quint32 elapsedTicks = currentTick - m_StartTick;
        if (elapsedTicks < 9) return false;

        const quint64 rateThreshold = metrics.totalBytesEnqueued >
                        std::numeric_limits<quint64>::max() / 6
                ? std::numeric_limits<quint64>::max() >> 1
                : (metrics.totalBytesEnqueued * 6 / elapsedTicks) >> 1;
        if (metrics.pendingBytes <= rateThreshold) return false;
        if (m_CompressedBlockCount >= 6 &&
                m_CompressionSavings <= m_CompressedInputBytes >> 3) {
            m_SavingsAreTooSmall = true;
            return false;
        }
        return true;
    }

    void recordCompressedBlock(int inputBytes, int outputBytes)
    {
        if (inputBytes <= 0 || outputBytes <= 0) return;
        ++m_CompressedBlockCount;
        m_CompressedInputBytes += static_cast<quint64>(inputBytes);
        if (outputBytes < inputBytes) {
            m_CompressionSavings +=
                    static_cast<quint64>(inputBytes - outputBytes);
        }
    }

private:
    quint32 m_StartTick = 0;
    quint64 m_CompressedBlockCount = 0;
    quint64 m_CompressedInputBytes = 0;
    quint64 m_CompressionSavings = 0;
    bool m_SavingsAreTooSmall = false;
};

quint32 currentTick()
{
    const quint64 nanoseconds = static_cast<quint64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
    const quint64 seconds = nanoseconds / 1'000'000'000;
    const quint64 remainder = nanoseconds % 1'000'000'000;
    return static_cast<quint32>(seconds * 60 +
                                remainder * 60 / 1'000'000'000);
}

} // namespace

AppleFileCopySender::AppleFileCopySender(ReadTick readTick)
    : m_ReadTick(std::move(readTick))
{
}

bool AppleFileCopySender::run(const QString& sourcePath,
                              quint32 sessionId,
                              const Emit& output,
                              const ReadOutputMetrics& readOutputMetrics,
                              std::atomic_bool* cancelled,
                              QString* error) const
{
    if (sessionId == 0 || !output) {
        return fail(error, QStringLiteral("The file sender is not configured."));
    }
    QList<TransferItem> items;
    AppleFileCopySummary summary;
    if (!appendTree(sourcePath, 0, &items, &summary, error) ||
            items.isEmpty()) {
        return false;
    }
    summary.rootIsFile = items.first().type != AppleFileCopyItemType::Directory;
    QString protocolError;
    const auto send = [&](const QByteArray& message, quint64 bytes) {
        if (cancelled != nullptr && cancelled->load()) {
            setError(error, QStringLiteral("The file transfer was cancelled."));
            return false;
        }
        if (message.isEmpty()) {
            setError(error, protocolError.isEmpty()
                             ? QStringLiteral("The file transfer message is invalid.")
                             : protocolError);
            return false;
        }
        return output(message, bytes, error);
    };
    if (!send(AppleFileTransferProtocol::senderSummary(
                      sessionId, summary, &protocolError), 0)) {
        return false;
    }

    const auto tickCount = [this]() {
        return m_ReadTick ? m_ReadTick() : currentTick();
    };
    Deflater compressor;
    CompressionPolicy compressionPolicy(tickCount());
    for (const TransferItem& item : std::as_const(items)) {
        const AppleFileCopyItemMetadata metadata = metadataFor(item);
        if (!send(AppleFileTransferProtocol::senderItem(
                          sessionId, metadata, &protocolError), 0)) {
            return false;
        }
        if (item.type != AppleFileCopyItemType::File ||
                (item.size == 0 && item.resourceForkSize == 0)) {
            continue;
        }
        const auto sendFork = [&](const QString& path, quint64 size) {
            if (size == 0) return true;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                setError(error, QStringLiteral("The source file could not be read: %1")
                                   .arg(QDir::toNativeSeparators(path)));
                return false;
            }
            quint64 remaining = size;
            while (remaining > 0) {
                const qint64 requested = static_cast<qint64>(qMin<quint64>(
                        AppleFileTransferProtocol::MaximumDataBlockLength,
                        remaining));
                const QByteArray data = file.read(requested);
                if (data.size() != requested) {
                    setError(error, QStringLiteral("The source file changed while it was being read."));
                    return false;
                }
                QByteArray message;
                const OutputMetrics metrics = readOutputMetrics
                        ? readOutputMetrics() : OutputMetrics{};
                if (compressionPolicy.shouldCompress(
                            metadata.name, metrics, tickCount())) {
                    const QByteArray compressed = compressor.compress(data);
                    if (compressed.isEmpty()) {
                        setError(error, QStringLiteral("The source file could not be compressed."));
                        return false;
                    }
                    message = AppleFileTransferProtocol::senderCompressedData(
                            sessionId, data.size(), compressed, &protocolError);
                    compressionPolicy.recordCompressedBlock(
                            data.size(), compressed.size());
                }
                else {
                    message = AppleFileTransferProtocol::senderData(
                            sessionId, data, &protocolError);
                }
                if (!send(message, static_cast<quint64>(data.size()))) {
                    return false;
                }
                remaining -= static_cast<quint64>(data.size());
            }
            return true;
        };
        if (!sendFork(item.path, item.size)) {
            send(AppleFileTransferProtocol::senderDone(sessionId, -1), 0);
            return false;
        }
#ifdef Q_OS_DARWIN
        if (!sendFork(item.path + QStringLiteral("/..namedfork/rsrc"),
                      item.resourceForkSize)) {
            send(AppleFileTransferProtocol::senderDone(sessionId, -1), 0);
            return false;
        }
#endif
    }
    return send(AppleFileTransferProtocol::senderDone(sessionId, 0), 0);
}

class AppleFileCopyReceiver::Inflater
{
public:
    Inflater()
    {
        m_Valid = inflateInit(&m_Stream) == Z_OK;
    }

    ~Inflater()
    {
        if (m_Valid) inflateEnd(&m_Stream);
    }

    QByteArray decompress(const QByteArray& input, int expectedLength)
    {
        if (!m_Valid || input.isEmpty() || expectedLength <= 0) return {};
        QByteArray output(expectedLength, Qt::Uninitialized);
        m_Stream.next_in = reinterpret_cast<Bytef*>(
                const_cast<char*>(input.constData()));
        m_Stream.avail_in = static_cast<uInt>(input.size());
        m_Stream.next_out = reinterpret_cast<Bytef*>(output.data());
        m_Stream.avail_out = static_cast<uInt>(output.size());
        const int result = inflate(&m_Stream, Z_SYNC_FLUSH);
        if ((result != Z_OK && result != Z_BUF_ERROR) ||
                m_Stream.avail_in != 0 || m_Stream.avail_out != 0) {
            return {};
        }
        return output;
    }

private:
    z_stream m_Stream = {};
    bool m_Valid = false;
};

AppleFileCopyReceiver::AppleFileCopyReceiver(
        const QString& destinationDirectory,
        const QString& requestedName,
        QString* error)
    : m_DestinationDirectory(QDir(destinationDirectory).absolutePath()),
      m_RequestedName(requestedName)
{
    if (!isValidComponent(requestedName) ||
            !QDir().mkpath(m_DestinationDirectory) ||
            !QFileInfo(m_DestinationDirectory).isDir()) {
        setError(error, QStringLiteral("The destination folder is invalid or cannot be created."));
        return;
    }
    m_Valid = true;
}

AppleFileCopyReceiver::~AppleFileCopyReceiver()
{
    if (!m_DidFinish) abort();
}

bool AppleFileCopyReceiver::isValid() const
{
    return m_Valid;
}

bool AppleFileCopyReceiver::receive(
        const AppleFileCopyReceiverFrame& frame,
        Update* update,
        QString* error)
{
    if (update == nullptr || !m_Valid || m_DidFinish) {
        return fail(error, QStringLiteral("The file receiver is not active."));
    }
    *update = {};
    switch (frame.kind) {
    case AppleFileCopyReceiverFrame::Kind::Summary:
        if (m_Summary.has_value() || m_CurrentItem.has_value() ||
                !m_RootPath.isEmpty()) {
            return fail(error, QStringLiteral("The Mac sent the file summary out of sequence."));
        }
        m_Summary = frame.summary;
        if (frame.summary.logicalBytes == 0) update->progress = 0.0;
        return true;
    case AppleFileCopyReceiverFrame::Kind::Item:
        if (!m_Summary.has_value() || m_CurrentItem.has_value()) {
            return fail(error, QStringLiteral("The Mac sent file metadata out of sequence."));
        }
        return receiveItem(frame.item, error);
    case AppleFileCopyReceiverFrame::Kind::Data:
        return receiveData(frame.data, update, error);
    case AppleFileCopyReceiverFrame::Kind::CompressedData: {
        if (!m_Inflater) m_Inflater = std::make_unique<Inflater>();
        const QByteArray data = m_Inflater->decompress(
                frame.data, frame.uncompressedLength);
        if (data.size() != frame.uncompressedLength) {
            return fail(error, QStringLiteral("The Mac sent invalid compressed file data."));
        }
        return receiveData(data, update, error);
    }
    case AppleFileCopyReceiverFrame::Kind::Done:
        if (frame.status != 0) {
            return fail(error, QStringLiteral("The Mac could not send the requested file (%1).")
                                   .arg(frame.status));
        }
        if (!m_Summary.has_value() || m_CurrentItem.has_value() ||
                m_RootPath.isEmpty()) {
            return fail(error, QStringLiteral("The Mac ended the file transfer out of sequence."));
        }
        if (!applyMetadata(m_RootPath, m_RootMetadata, error)) return false;
        m_DidFinish = true;
        update->progress = 1.0;
        update->completedPath = m_RootPath;
        return true;
    }
    return false;
}

bool AppleFileCopyReceiver::receiveItem(
        const AppleFileCopyItemMetadata& item,
        QString* error)
{
    if (!isValidComponent(item.name) ||
            (item.level == 0 ? !m_RootPath.isEmpty()
                             : !m_ParentDirectories.contains(item.level - 1))) {
        return fail(error, QStringLiteral("The Mac sent an invalid file-tree path."));
    }
    const QString parent = item.level == 0
            ? m_DestinationDirectory
            : m_ParentDirectories.value(item.level - 1);
    const QString path = createItem(
            item, parent, item.level == 0 ? m_RequestedName : item.name,
            error);
    if (path.isEmpty()) return false;

    for (auto iterator = m_ParentDirectories.begin();
         iterator != m_ParentDirectories.end();) {
        if (iterator.key() >= item.level) {
            iterator = m_ParentDirectories.erase(iterator);
        }
        else {
            ++iterator;
        }
    }
    if (item.type == AppleFileCopyItemType::Directory) {
        m_ParentDirectories.insert(item.level, path);
    }
    if (item.level == 0) {
        m_RootPath = path;
        m_RootMetadata = item;
    }
    if (item.type == AppleFileCopyItemType::SymbolicLink) return true;

    if (item.dataForkSize > 0) {
        CurrentItem current;
        current.path = path;
        current.metadata = item;
        current.remainingBytes = item.dataForkSize;
        current.file = std::make_unique<QFile>(path);
        if (!current.file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return fail(error, QStringLiteral("The received file could not be opened for writing."));
        }
        m_CurrentItem = std::move(current);
        return true;
    }
    if (item.resourceForkSize > 0) {
        CurrentItem current;
        current.path = path;
        current.metadata = item;
        current.fork = Fork::Resource;
        current.remainingBytes = item.resourceForkSize;
#ifdef Q_OS_DARWIN
        current.file = std::make_unique<QFile>(path + QStringLiteral("/..namedfork/rsrc"));
#elif defined(Q_OS_WIN)
        current.file = std::make_unique<QFile>(path + QStringLiteral(":com.apple.ResourceFork"));
#endif
        if (!current.file ||
                !current.file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            current.file.reset();
            current.discardsBytes = true;
        }
        m_CurrentItem = std::move(current);
        return true;
    }
    return applyMetadata(path, item, error);
}

bool AppleFileCopyReceiver::receiveData(
        const QByteArray& data,
        Update* update,
        QString* error)
{
    if (data.isEmpty() || !m_CurrentItem.has_value() ||
            static_cast<quint64>(data.size()) >
                    m_CurrentItem->remainingBytes) {
        return fail(error, QStringLiteral("The Mac sent file data out of sequence."));
    }
    if (!m_CurrentItem->discardsBytes &&
            (!m_CurrentItem->file ||
             m_CurrentItem->file->write(data) != data.size())) {
        return fail(error, QStringLiteral("The received file could not be written."));
    }
    m_CurrentItem->remainingBytes -= static_cast<quint64>(data.size());
    m_TransferredBytes += static_cast<quint64>(data.size());
    if (m_CurrentItem->remainingBytes == 0 && !finishCurrentFork(error)) {
        return false;
    }
    if (m_Summary->logicalBytes > 0) {
        update->progress = qMin(
                1.0,
                static_cast<double>(qMin(m_TransferredBytes,
                                         m_Summary->logicalBytes)) /
                        static_cast<double>(m_Summary->logicalBytes));
    }
    return true;
}

bool AppleFileCopyReceiver::finishCurrentFork(QString* error)
{
    if (!m_CurrentItem.has_value()) return false;
    CurrentItem current = std::move(*m_CurrentItem);
    m_CurrentItem.reset();
    if (current.file) current.file->close();
    if (current.fork == Fork::Data &&
            current.metadata.resourceForkSize > 0) {
        current.fork = Fork::Resource;
        current.remainingBytes = current.metadata.resourceForkSize;
#ifdef Q_OS_DARWIN
        current.file = std::make_unique<QFile>(
                current.path + QStringLiteral("/..namedfork/rsrc"));
#elif defined(Q_OS_WIN)
        current.file = std::make_unique<QFile>(
                current.path + QStringLiteral(":com.apple.ResourceFork"));
#else
        current.file.reset();
#endif
        current.discardsBytes = !current.file ||
                !current.file->open(QIODevice::WriteOnly | QIODevice::Truncate);
        if (current.discardsBytes) current.file.reset();
        m_CurrentItem = std::move(current);
        return true;
    }
    return applyMetadata(current.path, current.metadata, error);
}

bool AppleFileCopyReceiver::applyMetadata(
        const QString& path,
        const AppleFileCopyItemMetadata& item,
        QString*) const
{
    if (item.type == AppleFileCopyItemType::SymbolicLink) return true;
    QFile::Permissions permissions;
    if (item.mode & 0400) permissions |= QFileDevice::ReadOwner;
    if (item.mode & 0200) permissions |= QFileDevice::WriteOwner;
    if (item.mode & 0100) permissions |= QFileDevice::ExeOwner;
    if (item.mode & 0040) permissions |= QFileDevice::ReadGroup;
    if (item.mode & 0020) permissions |= QFileDevice::WriteGroup;
    if (item.mode & 0010) permissions |= QFileDevice::ExeGroup;
    if (item.mode & 0004) permissions |= QFileDevice::ReadOther;
    if (item.mode & 0002) permissions |= QFileDevice::WriteOther;
    if (item.mode & 0001) permissions |= QFileDevice::ExeOther;
    QFile::setPermissions(path, permissions);

    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QDateTime modified = localDate(item.contentModificationDate);
        const QDateTime accessed = localDate(item.accessDate);
        if (modified.isValid()) {
            file.setFileTime(modified, QFileDevice::FileModificationTime);
        }
        if (accessed.isValid()) {
            file.setFileTime(accessed, QFileDevice::FileAccessTime);
        }
    }
    return true;
}

QString AppleFileCopyReceiver::createItem(
        const AppleFileCopyItemMetadata& item,
        const QString& parent,
        const QString& preferredName,
        QString* error)
{
    for (int suffix = 1; suffix <= 65'535; ++suffix) {
        const QString name = suffix == 1
                ? preferredName : uniqueName(preferredName, suffix);
        if (!isValidComponent(name)) continue;
        const QString path = QDir(parent).filePath(name);
        bool created = false;
        switch (item.type) {
        case AppleFileCopyItemType::File: {
            QFile file(path);
            created = file.open(QIODevice::WriteOnly | QIODevice::NewOnly);
            break;
        }
        case AppleFileCopyItemType::Directory:
            created = QDir(parent).mkdir(name);
            break;
        case AppleFileCopyItemType::SymbolicLink:
            if (item.symbolicLinkTarget.isEmpty() ||
                    item.symbolicLinkTarget.contains('\0')) {
                return fail(error, QStringLiteral("The Mac sent an invalid symbolic link.")),
                       QString();
            }
            created = QFile::link(
                    QFile::decodeName(item.symbolicLinkTarget), path);
            break;
        }
        if (created) return path;
        if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) continue;
        return fail(error, QStringLiteral("The received item could not be created: %1")
                                   .arg(QDir::toNativeSeparators(path))),
               QString();
    }
    return fail(error, QStringLiteral("A unique destination name could not be allocated.")),
           QString();
}

bool AppleFileCopyReceiver::isValidComponent(const QString& name)
{
    if (name.isEmpty() || name == QStringLiteral(".") ||
            name == QStringLiteral("..") || name.size() > 255 ||
            name.contains('/') || name.contains('\\') || name.contains('\0')) {
        return false;
    }
#ifdef Q_OS_WIN
    if (name.contains(QRegularExpression(QStringLiteral("[<>:\"|?*]"))) ||
            name.endsWith(' ') || name.endsWith('.')) {
        return false;
    }
    const QString base = name.section('.', 0, 0).toUpper();
    static const QSet<QString> reserved = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
        QStringLiteral("NUL"), QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9"),
    };
    if (reserved.contains(base)) return false;
#endif
    return true;
}

QString AppleFileCopyReceiver::uniqueName(const QString& name, int suffix)
{
    const QFileInfo info(name);
    const QString extension = info.suffix();
    const QString extensionText = extension.isEmpty()
            ? QString() : QStringLiteral(".") + extension;
    QString stem = extension.isEmpty()
            ? name : name.left(name.size() - extensionText.size());
    const QString suffixText = QStringLiteral(" %1").arg(suffix);
    const int maximumStem = qMax(0, 255 - suffixText.size() -
                                         extensionText.size());
    stem.truncate(maximumStem);
    return stem + suffixText + extensionText;
}

bool AppleFileCopyReceiver::removeCreatedRoot(const QString& path)
{
    const QFileInfo info(path);
    if (path.isEmpty() || (!info.exists() && !info.isSymLink())) return true;
    if (info.isSymLink() || info.isFile()) return QFile::remove(path);
    return QDir(path).removeRecursively();
}

void AppleFileCopyReceiver::abort()
{
    if (m_CurrentItem.has_value() && m_CurrentItem->file) {
        m_CurrentItem->file->close();
    }
    m_CurrentItem.reset();
    removeCreatedRoot(m_RootPath);
    m_RootPath.clear();
    m_Valid = false;
}
