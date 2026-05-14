#include "clipboard_mac.h"

#include <QStringList>

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace {

static QString nsStringToQString(NSString* string)
{
    if (string == nil) {
        return QString();
    }

    return QString::fromUtf8([string UTF8String]);
}

static bool encodeImageAsPng(NSImage* image, QByteArray& outPng)
{
    if (image == nil) {
        return false;
    }

    NSBitmapImageRep* bitmapRep = nil;
    CGImageRef cgImage = [image CGImageForProposedRect:nullptr context:nil hints:nil];
    if (cgImage != nullptr) {
        bitmapRep = [[NSBitmapImageRep alloc] initWithCGImage:cgImage];
    }

    if (bitmapRep == nil) {
        NSData* tiffData = [image TIFFRepresentation];
        if (tiffData != nil) {
            bitmapRep = [NSBitmapImageRep imageRepWithData:tiffData];
        }
    }

    if (bitmapRep == nil) {
        return false;
    }

    NSData* pngData = [bitmapRep representationUsingType:NSPNGFileType properties:@{}];
    if (pngData == nil || pngData.length == 0) {
        return false;
    }

    outPng = QByteArray(static_cast<const char*>(pngData.bytes), static_cast<int>(pngData.length));
    return true;
}

static UTType* utTypeForIdentifier(NSString* type)
{
    if (type == nil || type.length == 0) {
        return nil;
    }

    return [UTType typeWithIdentifier:type];
}

static bool typeConformsTo(NSString* type, UTType* expectedType)
{
    if (type == nil || expectedType == nil) {
        return false;
    }

    UTType* utType = utTypeForIdentifier(type);
    return utType != nil && [utType conformsToType:expectedType];
}

static bool isHtmlType(NSString* type)
{
    return [type isEqualToString:NSPasteboardTypeHTML]
            || typeConformsTo(type, UTTypeHTML);
}

static bool isFileUrlType(NSString* type)
{
    return [type isEqualToString:NSPasteboardTypeFileURL]
            || typeConformsTo(type, UTTypeFileURL);
}

static bool isImageLikeType(NSString* type)
{
    if (type == nil) {
        return false;
    }

    if (typeConformsTo(type, UTTypeImage)
            || isFileUrlType(type)
            || isHtmlType(type)) {
        return true;
    }

    NSString* lower = [type lowercaseString];
    return [lower containsString:@"image"];
}

static QString describeTypeFlags(NSString* type)
{
    QStringList flags;
    if (typeConformsTo(type, UTTypeImage)) {
        flags.append(QStringLiteral("image"));
    }
    if (isFileUrlType(type)) {
        flags.append(QStringLiteral("file-url"));
    }
    if (isHtmlType(type)) {
        flags.append(QStringLiteral("html"));
    }

    if (flags.isEmpty()) {
        return QString();
    }

    return QStringLiteral(" (%1)").arg(flags.join(QStringLiteral("/")));
}

static QString summarizePasteboard(NSPasteboard* pasteboard, bool* outHadImageLikeData)
{
    if (outHadImageLikeData != nullptr) {
        *outHadImageLikeData = false;
    }

    NSArray<NSPasteboardItem*>* items = [pasteboard pasteboardItems];
    if (items == nil || items.count == 0) {
        return QStringLiteral("items=0");
    }

    QStringList parts;
    for (NSUInteger itemIndex = 0; itemIndex < items.count; ++itemIndex) {
        NSPasteboardItem* item = items[itemIndex];
        NSMutableArray<NSString*>* typeNames = [NSMutableArray array];
        for (NSString* type in item.types) {
            if (outHadImageLikeData != nullptr && isImageLikeType(type)) {
                *outHadImageLikeData = true;
            }
            const QString typeSummary = nsStringToQString(type) + describeTypeFlags(type);
            [typeNames addObject:[NSString stringWithUTF8String:typeSummary.toUtf8().constData()]];
        }

        parts.append(QStringLiteral("item[%1]=%2")
                     .arg(static_cast<qulonglong>(itemIndex))
                     .arg(nsStringToQString([typeNames componentsJoinedByString:@", "])));
    }

    return parts.join(QStringLiteral("; "));
}

static bool tryEncodeImageData(NSData* data,
                               const QString& sourceDescription,
                               QByteArray& outPng,
                               QString* outSourceDescription)
{
    if (data == nil || data.length == 0) {
        return false;
    }

    NSImage* image = [[NSImage alloc] initWithData:data];
    if (image == nil) {
        return false;
    }

    if (!encodeImageAsPng(image, outPng)) {
        return false;
    }

    if (outSourceDescription != nullptr) {
        *outSourceDescription = sourceDescription;
    }
    return true;
}

static bool tryEncodeImageFromFileUrl(NSString* urlString,
                                      const QString& sourceDescription,
                                      QByteArray& outPng,
                                      QString* outSourceDescription)
{
    if (urlString == nil || urlString.length == 0) {
        return false;
    }

    NSURL* url = [NSURL URLWithString:urlString];
    if (url == nil || !url.isFileURL) {
        return false;
    }

    NSImage* image = [[NSImage alloc] initWithContentsOfURL:url];
    if (image == nil) {
        return false;
    }

    if (!encodeImageAsPng(image, outPng)) {
        return false;
    }

    if (outSourceDescription != nullptr) {
        *outSourceDescription = sourceDescription;
    }
    return true;
}

static bool tryEncodeImageFromHtml(NSString* html,
                                   QByteArray& outPng,
                                   QString* outSourceDescription)
{
    if (html == nil || html.length == 0) {
        return false;
    }

    NSError* regexError = nil;
    NSRegularExpression* imgRegex = [NSRegularExpression regularExpressionWithPattern:@"<img[^>]+src\\s*=\\s*['\"]([^'\"]+)['\"]"
                                                                              options:NSRegularExpressionCaseInsensitive
                                                                                error:&regexError];
    if (imgRegex == nil || regexError != nil) {
        return false;
    }

    NSArray<NSTextCheckingResult*>* matches = [imgRegex matchesInString:html
                                                                 options:0
                                                                   range:NSMakeRange(0, html.length)];
    for (NSTextCheckingResult* match in matches) {
        if (match.numberOfRanges < 2) {
            continue;
        }

        NSString* src = [html substringWithRange:[match rangeAtIndex:1]];
        if (src.length == 0) {
            continue;
        }

        NSString* lowerSrc = [src lowercaseString];
        NSRange base64Marker = [lowerSrc rangeOfString:@";base64,"];
        if ([lowerSrc hasPrefix:@"data:image/"] && base64Marker.location != NSNotFound) {
            NSString* base64Part = [src substringFromIndex:(base64Marker.location + base64Marker.length)];
            NSData* data = [[NSData alloc] initWithBase64EncodedString:base64Part options:NSDataBase64DecodingIgnoreUnknownCharacters];
            if (tryEncodeImageData(data, QStringLiteral("NSPasteboard public.html data-url"), outPng, outSourceDescription)) {
                return true;
            }
            continue;
        }

        if (tryEncodeImageFromFileUrl(src,
                                      QStringLiteral("NSPasteboard public.html file-url"),
                                      outPng,
                                      outSourceDescription)) {
            return true;
        }
    }

    return false;
}

static QString buildHtmlImageTag(const QByteArray& png)
{
    if (png.isEmpty()) {
        return QString();
    }

    const QByteArray base64 = png.toBase64();
    if (base64.isEmpty()) {
        return QString();
    }

    return QStringLiteral("<img src=\"data:image/png;base64,%1\" />")
            .arg(QString::fromLatin1(base64));
}

static bool tryReadItemForTypes(NSPasteboardItem* item,
                                NSArray<NSString*>* orderedTypes,
                                QByteArray& outPng,
                                QString* outSourceDescription)
{
    for (NSString* type in orderedTypes) {
        if (![item.types containsObject:type]) {
            continue;
        }

        if (isFileUrlType(type)) {
            NSString* urlString = [item stringForType:type];
            if (tryEncodeImageFromFileUrl(urlString,
                                          QStringLiteral("NSPasteboard %1").arg(nsStringToQString(type)),
                                          outPng,
                                          outSourceDescription)) {
                return true;
            }
            continue;
        }

        if (isHtmlType(type)) {
            NSString* html = [item stringForType:type];
            if (tryEncodeImageFromHtml(html, outPng, outSourceDescription)) {
                return true;
            }
            continue;
        }

        NSData* data = [item dataForType:type];
        if (tryEncodeImageData(data,
                               QStringLiteral("NSPasteboard %1").arg(nsStringToQString(type)),
                               outPng,
                               outSourceDescription)) {
            return true;
        }
    }

    return false;
}

} // namespace

namespace MacClipboard {

bool readImageAsPng(QByteArray& outPng,
                    QString* outSourceDescription,
                    QString* outFormatsSummary,
                    bool* outHadImageLikeData)
{
    outPng.clear();
    if (outSourceDescription != nullptr) {
        outSourceDescription->clear();
    }
    if (outFormatsSummary != nullptr) {
        outFormatsSummary->clear();
    }
    if (outHadImageLikeData != nullptr) {
        *outHadImageLikeData = false;
    }

    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    if (pasteboard == nil) {
        return false;
    }

    bool hadImageLikeData = false;
    QString formatsSummary = summarizePasteboard(pasteboard, &hadImageLikeData);
    if (outFormatsSummary != nullptr) {
        *outFormatsSummary = formatsSummary;
    }
    if (outHadImageLikeData != nullptr) {
        *outHadImageLikeData = hadImageLikeData;
    }

    if ([NSImage canInitWithPasteboard:pasteboard]) {
        NSImage* image = [[NSImage alloc] initWithPasteboard:pasteboard];
        if (image != nil && encodeImageAsPng(image, outPng)) {
            if (outSourceDescription != nullptr) {
                *outSourceDescription = QStringLiteral("NSImage initWithPasteboard");
            }
            return true;
        }
    }

    NSArray<NSPasteboardItem*>* items = [pasteboard pasteboardItems];
    if (items == nil || items.count == 0) {
        return false;
    }

    NSArray<NSString*>* prioritizedTypes = @[
        NSPasteboardTypePNG,
        NSPasteboardTypeTIFF,
        NSPasteboardTypeJPEG,
        NSPasteboardTypeGIF,
        @"public.heic",
        @"public.heif",
        @"public.webp",
        NSPasteboardTypeFileURL,
        NSPasteboardTypeHTML,
    ];

    for (NSPasteboardItem* item in items) {
        if (tryReadItemForTypes(item, prioritizedTypes, outPng, outSourceDescription)) {
            return true;
        }

        for (NSString* type in item.types) {
            if ([prioritizedTypes containsObject:type] || !isImageLikeType(type)) {
                continue;
            }

            NSData* data = [item dataForType:type];
            if (tryEncodeImageData(data,
                                   QStringLiteral("NSPasteboard %1").arg(nsStringToQString(type)),
                                   outPng,
                                   outSourceDescription)) {
                return true;
            }
        }
    }

    return false;
}

bool writeImageFromPng(const QByteArray& png, QString* outWriteDescription)
{
    if (outWriteDescription != nullptr) {
        outWriteDescription->clear();
    }

    if (png.isEmpty()) {
        return false;
    }

    NSData* pngData = [NSData dataWithBytes:png.constData() length:static_cast<NSUInteger>(png.size())];
    if (pngData == nil || pngData.length == 0) {
        return false;
    }

    NSImage* image = [[NSImage alloc] initWithData:pngData];
    if (image == nil) {
        return false;
    }

    NSData* tiffData = [image TIFFRepresentation];

    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    if (pasteboard == nil) {
        return false;
    }

    NSString* htmlImageTag = nil;
    const QString htmlImage = buildHtmlImageTag(png);
    if (!htmlImage.isEmpty()) {
        htmlImageTag = [NSString stringWithUTF8String:htmlImage.toUtf8().constData()];
    }

    NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
    bool wroteAny = [item setData:pngData forType:NSPasteboardTypePNG];
    if (tiffData != nil && tiffData.length > 0) {
        wroteAny = [item setData:tiffData forType:NSPasteboardTypeTIFF] || wroteAny;
    }
    if (htmlImageTag != nil && htmlImageTag.length > 0) {
        wroteAny = [item setString:htmlImageTag forType:NSPasteboardTypeHTML] || wroteAny;
    }

    if (!wroteAny) {
        return false;
    }

    [pasteboard clearContents];
    wroteAny = [pasteboard writeObjects:@[item]];

    if (wroteAny && outWriteDescription != nullptr) {
        QStringList writtenTypes { QStringLiteral("public.png") };
        if (tiffData != nil && tiffData.length > 0) {
            writtenTypes.append(QStringLiteral("public.tiff"));
        }
        if (htmlImageTag != nil && htmlImageTag.length > 0) {
            writtenTypes.append(QStringLiteral("public.html"));
        }
        *outWriteDescription = writtenTypes.join(QStringLiteral(" + "));
    }

    return wroteAny;
}

int changeCount()
{
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    if (pasteboard == nil) {
        return -1;
    }

    return static_cast<int>(pasteboard.changeCount);
}

} // namespace MacClipboard