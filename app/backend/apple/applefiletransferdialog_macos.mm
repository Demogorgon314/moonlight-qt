#include "applefiletransferdialog.h"

#import <AppKit/AppKit.h>

#include <QByteArray>

QString chooseAppleFileTransferDirectory(const QString& title,
                                         const QString& initialDirectory,
                                         void*)
{
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:NO];
        [panel setCanChooseDirectories:YES];
        [panel setAllowsMultipleSelection:NO];
        [panel setCanCreateDirectories:YES];

        const QByteArray titleUtf8 = title.toUtf8();
        [panel setTitle:[NSString stringWithUTF8String:titleUtf8.constData()]];
        const QByteArray directoryUtf8 = initialDirectory.toUtf8();
        if (!directoryUtf8.isEmpty()) {
            NSString* path = [NSString stringWithUTF8String:
                    directoryUtf8.constData()];
            [panel setDirectoryURL:[NSURL fileURLWithPath:path
                                                isDirectory:YES]];
        }
        if ([panel runModal] != NSModalResponseOK ||
                [panel URLs].count == 0) {
            return {};
        }
        const char* selected = [[[[panel URLs] objectAtIndex:0] path]
                fileSystemRepresentation];
        return selected != nullptr ? QString::fromUtf8(selected) : QString();
    }
}
