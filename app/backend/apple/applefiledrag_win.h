#pragma once

#include "applefiledrag.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>

// Installs an OLE drop target on the SDL stream HWND. Unlike SDL_DROPFILE,
// OLE exposes the file list and hover coordinates at DragEnter/DragOver time,
// which Finder needs in order to choose the actual remote drop target.
class AppleWindowsFileDropTarget
{
public:
    AppleWindowsFileDropTarget(
            void* nativeWindow,
            int displayIndex,
            std::shared_ptr<AppleLocalFileDragLifecycle> lifecycle);
    ~AppleWindowsFileDropTarget();

    AppleWindowsFileDropTarget(const AppleWindowsFileDropTarget&) = delete;
    AppleWindowsFileDropTarget& operator=(
            const AppleWindowsFileDropTarget&) = delete;

    bool isValid() const;
    QString errorString() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_Impl;
};

enum class AppleWindowsRemoteFileDragResult
{
    Dropped,
    Cancelled,
    Failed,
};

// Exports Mac-originated items through the Shell's asynchronous virtual-file
// promise formats. Explorer chooses the destination, then reads each content
// stream on its extraction worker without blocking the stream-window thread.
class AppleWindowsRemoteFileDragSource
{
public:
    using Materialize =
            std::function<bool(const std::atomic_bool& cancelled,
                               QStringList* paths,
                               QString* error)>;
    using Finished = std::function<void(
            AppleWindowsRemoteFileDragResult result,
            const QString& error)>;

    explicit AppleWindowsRemoteFileDragSource(void* nativeWindow);
    ~AppleWindowsRemoteFileDragSource();

    AppleWindowsRemoteFileDragSource(
            const AppleWindowsRemoteFileDragSource&) = delete;
    AppleWindowsRemoteFileDragSource& operator=(
            const AppleWindowsRemoteFileDragSource&) = delete;

    bool isValid() const;
    bool isDragging() const;
    void addStreamWindow(void* nativeWindow);
    bool leftButtonDown() const;
    bool pointerInsideWindow() const;
    bool begin(
            const AppleRemoteFileDrag& drag,
            Materialize materialize,
            Finished finished,
            QString* error = nullptr);

private:
    class Impl;
    std::shared_ptr<Impl> m_Impl;
};

#ifdef APPLE_FILE_DRAG_TESTS
bool testAppleWindowsPromisedFileDataObject(
        const QString& promisedPath,
        const QString& materializedPath,
        QString* descriptorName,
        QByteArray* contents,
        QString* error);
bool testAppleWindowsPromisedFileMetadataIsLazy(QString* error);
bool testAppleWindowsPromisedFileAsyncCompletionIsReusable(QString* error);
bool testAppleWindowsPromisedFilesDropIntoShellFolders(
        const QString& firstPromisedPath,
        const QString& firstMaterializedPath,
        const QString& firstDestination,
        const QString& secondPromisedPath,
        const QString& secondMaterializedPath,
        const QString& secondDestination,
        QString* error);
#endif
