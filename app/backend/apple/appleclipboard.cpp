#include "appleclipboard.h"

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QSet>

#include <memory>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

QString appleTypeForMime(const QString& format)
{
    if (format.compare(QStringLiteral("text/plain"),
                       Qt::CaseInsensitive) == 0) {
        return QStringLiteral("public.utf8-plain-text");
    }
    if (format.compare(QStringLiteral("text/html"),
                       Qt::CaseInsensitive) == 0) {
        return QStringLiteral("public.html");
    }
    if (format.compare(QStringLiteral("text/rtf"),
                       Qt::CaseInsensitive) == 0 ||
            format.compare(QStringLiteral("application/rtf"),
                           Qt::CaseInsensitive) == 0) {
        return QStringLiteral("public.rtf");
    }
    if (format.compare(QStringLiteral("image/png"),
                       Qt::CaseInsensitive) == 0) {
        return QStringLiteral("public.png");
    }
    if (format.compare(QStringLiteral("image/tiff"),
                       Qt::CaseInsensitive) == 0) {
        return QStringLiteral("public.tiff");
    }
    return format;
}

QByteArray pngForMimeData(const QMimeData* mimeData)
{
    if (mimeData == nullptr || !mimeData->hasImage()) {
        return {};
    }
    const QImage image = qvariant_cast<QImage>(mimeData->imageData());
    if (image.isNull()) {
        return {};
    }
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
        return {};
    }
    return png;
}

#ifndef Q_OS_DARWIN
QString mimeForAppleType(const AppleClipboardFlavor& flavor)
{
    if (flavor.type == QStringLiteral("public.utf8-plain-text")) {
        return QStringLiteral("text/plain");
    }
    if (flavor.type == QStringLiteral("public.html")) {
        return QStringLiteral("text/html");
    }
    if (flavor.type == QStringLiteral("public.rtf")) {
        return QStringLiteral("text/rtf");
    }
    if (flavor.type == QStringLiteral("public.png")) {
        return QStringLiteral("image/png");
    }
    if (flavor.type == QStringLiteral("public.tiff")) {
        return QStringLiteral("image/tiff");
    }
    if (!flavor.type.isEmpty()) {
        return flavor.type;
    }
    for (const AppleClipboardTypeAlias& alias : flavor.aliases) {
        if (alias.tagClass == QStringLiteral("public.mime-type")) {
            return alias.preferredTag;
        }
    }
    return {};
}

std::unique_ptr<QMimeData> mimeDataFromArchive(
        const AppleClipboardArchive& archive)
{
    auto mimeData = std::make_unique<QMimeData>();
    QSet<QString> installed;
    for (const AppleClipboardItem& item : archive.items) {
        for (const AppleClipboardFlavor& flavor : item.flavors) {
            const QString format = mimeForAppleType(flavor);
            if (format.isEmpty() || installed.contains(format) ||
                    !AppleClipboard::isSynchronizableType(format)) {
                continue;
            }
            installed.insert(format);
            if (format == QStringLiteral("text/plain")) {
                mimeData->setText(QString::fromUtf8(flavor.value));
            }
            else if (format == QStringLiteral("text/html")) {
                mimeData->setHtml(QString::fromUtf8(flavor.value));
            }
            else {
                mimeData->setData(format, flavor.value);
            }
            if (format == QStringLiteral("image/png")) {
                QImage image;
                if (image.loadFromData(flavor.value, "PNG")) {
                    mimeData->setImageData(image);
                }
            }
        }
    }
    if (installed.isEmpty()) {
        return {};
    }
    return mimeData;
}
#endif

} // namespace

namespace AppleClipboard {

bool isSynchronizableType(const QString& type)
{
    if (type.isEmpty() || type.startsWith(
                QStringLiteral("dyn."), Qt::CaseInsensitive)) {
        return false;
    }
    static const QStringList excluded = {
        QStringLiteral("com.apple.pasteboardpeeker.lowercasetext"),
        QStringLiteral("com.apple.pasteboardpeeker.uppercasetext"),
        QStringLiteral("public.file-url"),
        QStringLiteral("text/uri-list"),
        QStringLiteral("com.apple.pasteboard.NSFilePromiseID"),
        QStringLiteral("com.apple.pasteboard.NSFilePromiseContent"),
        QStringLiteral("com.apple.pasteboard.promised-file-url"),
        QStringLiteral("com.apple.pasteboard.promised-file-content-type"),
        QStringLiteral("com.ilm.openexr-image"),
        QStringLiteral("application/x-qt-image"),
    };
    for (const QString& candidate : excluded) {
        if (type.compare(candidate, Qt::CaseInsensitive) == 0) {
            return false;
        }
    }
    return true;
}

std::optional<AppleClipboardArchive> archiveFromMimeData(
        const QMimeData* mimeData)
{
    if (mimeData == nullptr) {
        return std::nullopt;
    }
    if (AppleLocalClipboardTracker::containsFiles(mimeData)) {
        return std::nullopt;
    }
    AppleClipboardItem item;
    QSet<QString> capturedTypes;
    for (const QString& format : mimeData->formats()) {
        const QString type = appleTypeForMime(format);
        if (!isSynchronizableType(type) || capturedTypes.contains(type)) {
            continue;
        }
        QByteArray value;
        if (type == QStringLiteral("public.utf8-plain-text")) {
            value = mimeData->text().toUtf8();
        }
        else if (type == QStringLiteral("public.html")) {
            value = mimeData->html().toUtf8();
        }
        else {
            value = mimeData->data(format);
        }
        item.flavors.append({
            type,
            {{QStringLiteral("public.mime-type"), format}},
            value,
            0,
        });
        capturedTypes.insert(type);
    }
    if (!capturedTypes.contains(QStringLiteral("public.png"))) {
        const QByteArray png = pngForMimeData(mimeData);
        if (!png.isEmpty()) {
            item.flavors.append({
                QStringLiteral("public.png"),
                {{QStringLiteral("public.mime-type"),
                  QStringLiteral("image/png")}},
                png,
                0,
            });
        }
    }
    if (item.flavors.isEmpty()) {
        return std::nullopt;
    }
    return AppleClipboardArchive{{item}};
}

#ifndef Q_OS_DARWIN
std::optional<AppleClipboardArchive> readSystemArchive()
{
    const QClipboard* clipboard = QGuiApplication::clipboard();
    return archiveFromMimeData(
            clipboard != nullptr ? clipboard->mimeData() : nullptr);
}

quint64 systemRevision()
{
#ifdef Q_OS_WIN
    return GetClipboardSequenceNumber();
#else
    static quint64 revision = 0;
    static const auto connection = QObject::connect(
            QGuiApplication::clipboard(), &QClipboard::dataChanged,
            QGuiApplication::instance(), [] { ++revision; });
    Q_UNUSED(connection);
    return revision;
#endif
}

bool writeSystemArchive(const AppleClipboardArchive& archive)
{
    QClipboard* clipboard = QGuiApplication::clipboard();
    std::unique_ptr<QMimeData> mimeData = mimeDataFromArchive(archive);
    if (clipboard == nullptr || mimeData == nullptr) {
        return false;
    }
    clipboard->setMimeData(mimeData.release());
    return true;
}
#endif

} // namespace AppleClipboard
