#pragma once

#include "applefiledrag.h"

#include <QString>

#include <atomic>
#include <functional>
#include <memory>

struct SDL_Window;

enum class AppleMacRemoteFileDragResult
{
    Dropped,
    Cancelled,
    Failed,
};

// Exports Mac-originated items through AppKit's native promised-file drag
// contract. Finder supplies the final path only after the user drops, so file
// contents remain remote until NSFilePromiseProvider requests them.
class AppleMacRemoteFileDragSource
{
public:
    using Materialize = std::function<bool(
            const QString& sourcePath,
            const QString& destinationPath,
            const std::atomic_bool& cancelled,
            QString* completedPath,
            QString* error)>;
    using Finished = std::function<void(
            AppleMacRemoteFileDragResult result,
            const QString& error)>;

    explicit AppleMacRemoteFileDragSource(SDL_Window* window);
    ~AppleMacRemoteFileDragSource();

    AppleMacRemoteFileDragSource(
            const AppleMacRemoteFileDragSource&) = delete;
    AppleMacRemoteFileDragSource& operator=(
            const AppleMacRemoteFileDragSource&) = delete;

    bool isValid() const;
    bool isDragging() const;
    bool leftButtonDown() const;
    bool pointerInsideWindow() const;
    bool begin(const AppleRemoteFileDrag& drag,
               const void* nativeEvent,
               Materialize materialize,
               Finished finished,
               QString* error = nullptr);

private:
    class Impl;
    std::shared_ptr<Impl> m_Impl;
};

#ifdef APPLE_FILE_DRAG_TESTS
bool testAppleMacPromisedFileAdapter(QString* error);
#endif
