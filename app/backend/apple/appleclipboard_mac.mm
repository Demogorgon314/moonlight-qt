#include "appleclipboard.h"

#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>

#include <QHash>

#include <limits>

namespace {

const NSArray<NSString*>* tagClasses()
{
    static NSArray<NSString*>* values = [[NSArray alloc] initWithObjects:
            @"com.apple.ostype",
            @"com.apple.nspboard-type",
            @"public.filename-extension",
            @"public.mime-type",
            nil];
    return values;
}

QString qtString(NSString* value)
{
    return value != nil
            ? QString::fromUtf8(value.UTF8String) : QString();
}

NSString* nsString(const QString& value)
{
    return [NSString stringWithUTF8String:value.toUtf8().constData()];
}

QList<AppleClipboardTypeAlias> aliasesForType(NSString* type)
{
    QList<AppleClipboardTypeAlias> aliases;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    for (NSString* tagClass in tagClasses()) {
        CFStringRef tag = UTTypeCopyPreferredTagWithClass(
                reinterpret_cast<CFStringRef>(type),
                reinterpret_cast<CFStringRef>(tagClass));
        if (tag == nullptr) {
            continue;
        }
        aliases.append({qtString(tagClass),
                        qtString(reinterpret_cast<NSString*>(tag))});
        CFRelease(tag);
    }
#pragma clang diagnostic pop
    return aliases;
}

QList<NSString*> identifiersForFlavor(
        const AppleClipboardFlavor& flavor)
{
    QList<NSString*> identifiers;
    NSString* type = nsString(flavor.type);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (type.length > 0 && UTTypeIsDeclared(
                reinterpret_cast<CFStringRef>(type))) {
        identifiers.append(type);
        return identifiers;
    }

    QHash<QString, QString> aliases;
    for (const AppleClipboardTypeAlias& alias : flavor.aliases) {
        aliases[alias.tagClass] = alias.preferredTag;
    }
    for (NSString* tagClass in tagClasses()) {
        const QString tag = aliases.value(qtString(tagClass));
        if (tag.isEmpty()) {
            continue;
        }
        CFStringRef identifier = UTTypeCreatePreferredIdentifierForTag(
                reinterpret_cast<CFStringRef>(tagClass),
                reinterpret_cast<CFStringRef>(nsString(tag)),
                nullptr);
        if (identifier == nullptr) {
            continue;
        }
        NSString* value = reinterpret_cast<NSString*>(identifier);
        bool alreadyPresent = false;
        for (NSString* existing : identifiers) {
            if ([existing isEqualToString:value]) {
                alreadyPresent = true;
                break;
            }
        }
        if (!alreadyPresent) {
            identifiers.append([[value copy] autorelease]);
        }
        CFRelease(identifier);
    }
#pragma clang diagnostic pop
    if (identifiers.isEmpty() && type.length > 0) {
        identifiers.append(type);
    }
    return identifiers;
}

} // namespace

namespace AppleClipboard {

std::optional<AppleClipboardArchive> archiveFromNativePasteboard(
        const void* opaquePasteboard)
{
    NSPasteboard* pasteboard = reinterpret_cast<NSPasteboard*>(
            const_cast<void*>(opaquePasteboard));
    if (pasteboard == nil) {
        return std::nullopt;
    }
    AppleClipboardArchive archive;
    for (NSPasteboardItem* pasteboardItem in pasteboard.pasteboardItems) {
        AppleClipboardItem item;
        for (NSPasteboardType nativeType in pasteboardItem.types) {
            const QString type = qtString(nativeType);
            if (!isSynchronizableType(type)) {
                continue;
            }
            NSData* data = [pasteboardItem dataForType:nativeType];
            if (data == nil || data.length >
                    static_cast<NSUInteger>(std::numeric_limits<int>::max())) {
                continue;
            }
            item.flavors.append({
                type,
                aliasesForType(nativeType),
                QByteArray(static_cast<const char*>(data.bytes),
                           static_cast<int>(data.length)),
                0,
            });
        }
        if (!item.flavors.isEmpty()) {
            archive.items.append(std::move(item));
        }
    }
    return archive.isEmpty()
            ? std::nullopt
            : std::optional<AppleClipboardArchive>(std::move(archive));
}

bool writeNativePasteboard(const AppleClipboardArchive& archive,
                           void* opaquePasteboard)
{
    NSPasteboard* pasteboard = reinterpret_cast<NSPasteboard*>(
            opaquePasteboard);
    if (pasteboard == nil) {
        return false;
    }
    NSMutableArray<NSPasteboardItem*>* pasteboardItems =
            [NSMutableArray array];
    for (const AppleClipboardItem& archiveItem : archive.items) {
        NSPasteboardItem* item = [[[NSPasteboardItem alloc] init] autorelease];
        bool wroteFlavor = false;
        for (const AppleClipboardFlavor& flavor : archiveItem.flavors) {
            const QList<NSString*> identifiers =
                    identifiersForFlavor(flavor);
            NSData* data = [NSData dataWithBytes:flavor.value.constData()
                                          length:flavor.value.size()];
            for (NSString* identifier : identifiers) {
                if (!isSynchronizableType(qtString(identifier))) {
                    continue;
                }
                wroteFlavor = [item setData:data forType:identifier] ||
                        wroteFlavor;
            }
        }
        if (wroteFlavor) {
            [pasteboardItems addObject:item];
        }
    }
    if (pasteboardItems.count == 0) {
        return false;
    }
    [pasteboard clearContents];
    return [pasteboard writeObjects:pasteboardItems];
}

std::optional<AppleClipboardArchive> readSystemArchive()
{
    return archiveFromNativePasteboard([NSPasteboard generalPasteboard]);
}

quint64 systemRevision()
{
    return static_cast<quint64>([NSPasteboard generalPasteboard].changeCount);
}

bool writeSystemArchive(const AppleClipboardArchive& archive)
{
    return writeNativePasteboard(
            archive, [NSPasteboard generalPasteboard]);
}

} // namespace AppleClipboard
