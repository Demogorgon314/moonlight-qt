#pragma once

#include <QString>

// Opens the platform-native directory picker without requiring QApplication.
// ownerWindow is an HWND on Windows and may be null on other platforms.
QString chooseAppleFileTransferDirectory(const QString& title,
                                         const QString& initialDirectory,
                                         void* ownerWindow = nullptr);
