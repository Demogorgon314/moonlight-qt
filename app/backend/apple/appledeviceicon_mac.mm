#include "appledeviceinfo.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <QBuffer>
#include <QCache>
#include <QImage>

namespace {

UTType* declaredDeviceType(const QString& model,
                           const std::optional<qint32>& colorIndex)
{
    QString tag = model;
    if (colorIndex.has_value()) {
        tag += QStringLiteral("@ECOLOR=%1").arg(*colorIndex);
    }
    UTType* type = [UTType
            typeWithTag:tag.toNSString()
            tagClass:@"com.apple.device-model-code"
            conformingToType:nil];
    return type != nil && type.declared ? type : nil;
}

QString pngDataUrl(NSImage* image)
{
    if (image == nil) {
        return {};
    }
    NSBitmapImageRep* representation = [NSBitmapImageRep
            imageRepWithData:image.TIFFRepresentation];
    NSData* png = [representation
            representationUsingType:NSBitmapImageFileTypePNG
            properties:@{}];
    if (png == nil) {
        return {};
    }
    QImage qtImage = QImage::fromData(
            static_cast<const uchar*>(png.bytes),
            static_cast<int>(png.length), "PNG");
    if (qtImage.isNull()) {
        return {};
    }
    if (qtImage.width() > 256 || qtImage.height() > 256) {
        qtImage = qtImage.scaled(
                256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly) || !qtImage.save(&buffer, "PNG")) {
        return {};
    }
    return QStringLiteral("data:image/png;base64,%1")
            .arg(QString::fromLatin1(encoded.toBase64()));
}

} // namespace

QString AppleDeviceInfo::iconSource(const AppleRemoteDeviceInfo& deviceInfo)
{
    if (!deviceInfo.isValid()) {
        return {};
    }
    const QString model = deviceInfo.modelIdentifier.trimmed();
    const std::optional<qint32> colorIndex =
            deviceInfo.enclosureColorIndex();
    const QString cacheKey = QStringLiteral("%1|%2")
            .arg(model,
                 colorIndex.has_value() ? QString::number(*colorIndex)
                                        : QStringLiteral("-"));
    static QCache<QString, QString> cache(25);
    if (const QString* cached = cache.object(cacheKey)) {
        return *cached;
    }

    @autoreleasepool {
        UTType* type = declaredDeviceType(model, colorIndex);
        if (type == nil && colorIndex.has_value()) {
            type = declaredDeviceType(model, std::nullopt);
        }
        if (type == nil) {
            return {};
        }
        const QString source = pngDataUrl(
                [[NSWorkspace sharedWorkspace] iconForContentType:type]);
        if (!source.isEmpty()) {
            cache.insert(cacheKey, new QString(source));
        }
        return source;
    }
}
