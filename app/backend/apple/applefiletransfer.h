#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

struct AppleFileTransferRequest
{
    quint32 sessionId = 0;
    QString destinationPath;
};

enum class AppleFileTransferControl : quint16
{
    Pause = 3,
    Resume = 4,
    Stop = 5,
};

enum class AppleFileCopyItemType : quint8
{
    File = 1,
    Directory = 2,
    SymbolicLink = 3,
};

struct AppleFileCopyUtcDateTime
{
    quint64 seconds = 0;
    quint16 fraction = 0;

    bool operator==(const AppleFileCopyUtcDateTime& other) const
    {
        return seconds == other.seconds && fraction == other.fraction;
    }
};

struct AppleFileCopyExtendedAttribute
{
    QString name;
    QByteArray value;

    bool operator==(const AppleFileCopyExtendedAttribute& other) const
    {
        return name == other.name && value == other.value;
    }
};

struct AppleFileCopySummary
{
    bool rootIsFile = false;
    quint64 logicalBytes = 0;
    quint64 physicalBytes = 0;
    quint64 fileCount = 0;
    quint64 folderCount = 0;
};

struct AppleFileCopyItemMetadata
{
    AppleFileCopyItemType type = AppleFileCopyItemType::File;
    quint8 userAccess = 0;
    QByteArray finderInfo = QByteArray(32, '\0');
    quint64 resourceForkSize = 0;
    quint64 dataForkSize = 0;
    AppleFileCopyUtcDateTime creationDate;
    AppleFileCopyUtcDateTime contentModificationDate;
    AppleFileCopyUtcDateTime attributeModificationDate;
    AppleFileCopyUtcDateTime accessDate;
    AppleFileCopyUtcDateTime backupDate;
    quint16 nodeFlags = 0;
    quint16 level = 0;
    quint16 mode = 0;
    quint32 textEncodingHint = 0;
    QString name;
    QByteArray symbolicLinkTarget;
    QList<AppleFileCopyExtendedAttribute> extendedAttributes;
};

struct AppleFileCopyReceiverFrame
{
    enum class Kind
    {
        Summary,
        Item,
        Data,
        CompressedData,
        Done,
    };

    quint32 sessionId = 0;
    Kind kind = Kind::Done;
    AppleFileCopySummary summary;
    AppleFileCopyItemMetadata item;
    QByteArray data;
    int uncompressedLength = 0;
    qint32 status = 0;
};

struct AppleRemoteFileDrag
{
    quint32 sessionId = 0;
    QStringList sourcePaths;
    QByteArray imagePng;
};

struct AppleFileTransferResponse
{
    enum class Kind
    {
        Completed,
        Progress,
    };

    Kind kind = Kind::Completed;
    quint32 sessionId = 0;
    quint16 errorCode = 0;
    QString name;
    double fraction = 0.0;
};

class AppleFileTransferProtocol
{
public:
    static constexpr int MaximumDropPayloadLength = 0x70'0000;
    static constexpr int MaximumRemoteDragPayloadLength = 0x63ff'fff;
    static constexpr int MaximumDestinationPathLength = 0x3ff;
    static constexpr int MaximumRemoteSourcePathLength = 20'000;
    static constexpr int MaximumCompletionNameLength = 0x3ff;
    static constexpr int MaximumFileCopyBodyLength = 4 * 1024 * 1024;
    static constexpr int MaximumRecordPlaintextLength = 60'000;
    static constexpr int MaximumDataBlockLength = 65'536;

    static QByteArray beginDrop(const QList<QUrl>& fileUrls,
                                quint32 sessionId,
                                QString* error = nullptr);
    static QByteArray cancelDrop(quint32 sessionId,
                                 QString* error = nullptr);
    static bool parseFileRequest(const QByteArray& message,
                                 AppleFileTransferRequest* request,
                                 QString* error = nullptr);
    static QByteArray startFileReceive(quint32 sessionId,
                                       const QString& destinationPath,
                                       QString* error = nullptr);
    static QByteArray startFileSend(quint32 sessionId,
                                    const QString& sourcePath,
                                    QString* error = nullptr);
    static QByteArray control(quint32 sessionId,
                              AppleFileTransferControl action,
                              QString* error = nullptr);
    static QByteArray completion(quint32 sessionId,
                                 quint16 errorCode,
                                 const QString& name,
                                 QString* error = nullptr);
    static QByteArray progress(quint32 sessionId,
                               double fraction,
                               QString* error = nullptr);

    static QByteArray senderSummary(quint32 sessionId,
                                    const AppleFileCopySummary& summary,
                                    QString* error = nullptr);
    static QByteArray senderItem(quint32 sessionId,
                                 const AppleFileCopyItemMetadata& item,
                                 QString* error = nullptr);
    static QByteArray senderData(quint32 sessionId,
                                 const QByteArray& data,
                                 QString* error = nullptr);
    static QByteArray senderCompressedData(quint32 sessionId,
                                           int uncompressedLength,
                                           const QByteArray& data,
                                           QString* error = nullptr);
    static QByteArray senderDone(quint32 sessionId,
                                 qint32 status,
                                 QString* error = nullptr);

    static bool parseResponse(const QByteArray& message,
                              AppleFileTransferResponse* response,
                              QString* error = nullptr);
    static bool parseReceiverFrame(const QByteArray& message,
                                   AppleFileCopyReceiverFrame* frame,
                                   QString* error = nullptr);
    static bool parseRemoteDrag(
            const QByteArray& message,
            std::optional<AppleRemoteFileDrag>* drag,
            QString* error = nullptr);
    static bool expectedStreamMessageLength(const QByteArray& fragment,
                                            int* length,
                                            QString* error = nullptr);
    static QList<QByteArray> fragments(const QByteArray& message);
};

// Reassembles the logical file-copy messages split across encrypted records.
// A control record cannot contain bytes from two logical messages.
class AppleFileTransferReassembler
{
public:
    bool receive(const QByteArray& fragment,
                 std::optional<QByteArray>* message,
                 QString* error = nullptr);
    void reset();

private:
    QByteArray m_Buffer;
    int m_ExpectedLength = 0;
};
