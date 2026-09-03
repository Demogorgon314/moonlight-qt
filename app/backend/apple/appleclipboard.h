#pragma once

#include "applecontrolfeatures.h"

#include <QString>

#include <optional>

class QMimeData;

namespace AppleClipboard {

// Converts Qt's single-item clipboard representation into the native archive
// model. macOS system clipboard access uses NSPasteboard directly instead.
std::optional<AppleClipboardArchive> archiveFromMimeData(
        const QMimeData* mimeData);

// Reads or replaces the process-wide system clipboard. File-transfer-only and
// transient pasteboard flavors are filtered at the individual flavor boundary.
std::optional<AppleClipboardArchive> readSystemArchive();
bool writeSystemArchive(const AppleClipboardArchive& archive);

bool isSynchronizableType(const QString& type);

#ifdef Q_OS_DARWIN
// Testable native seams. The opaque value must be an NSPasteboard*.
std::optional<AppleClipboardArchive> archiveFromNativePasteboard(
        const void* pasteboard);
bool writeNativePasteboard(const AppleClipboardArchive& archive,
                           void* pasteboard);
#endif

} // namespace AppleClipboard
