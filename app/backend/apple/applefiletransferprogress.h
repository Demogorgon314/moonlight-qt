#pragma once

#include <QString>
#include <QtGlobal>

#include <functional>
#include <memory>

enum class AppleFileTransferProgressState
{
    WaitingForRemote,
    Sending,
    Receiving,
    Paused,
    Completing,
    Completed,
    Failed,
    Cancelled,
};

struct AppleFileTransferProgressEntry
{
    quint32 sessionId = 0;
    QString name;
    QString remoteName;
    QString path;
    QString errorText;
    AppleFileTransferProgressState state =
            AppleFileTransferProgressState::WaitingForRemote;
    double progress = 0.0;
    double bytesPerSecond = 0.0;
    bool incoming = false;
    bool hasProgress = false;
};

// A compact, modeless transfer window shared by Windows and macOS. The
// platform directory chooser remains native, while progress presentation uses
// Qt Gui so both clients expose the same observable transfer controls.
class AppleFileTransferProgressWindow
{
public:
    using PauseHandler = std::function<void(quint32 sessionId, bool paused)>;
    using CancelHandler = std::function<void(quint32 sessionId)>;

    AppleFileTransferProgressWindow(PauseHandler pause,
                                    CancelHandler cancel);
    ~AppleFileTransferProgressWindow();

    AppleFileTransferProgressWindow(
            const AppleFileTransferProgressWindow&) = delete;
    AppleFileTransferProgressWindow& operator=(
            const AppleFileTransferProgressWindow&) = delete;

    void update(const AppleFileTransferProgressEntry& entry);
    void failActive(const QString& reason);
    void close();

private:
    class Window;
    std::unique_ptr<Window> m_Window;
};
