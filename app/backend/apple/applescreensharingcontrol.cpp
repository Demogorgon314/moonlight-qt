#include "applescreensharingsession.h"
#include "applescreensharingsession_p.h"

#include "appleclipboard.h"
#include "applefiledrag.h"
#include "applefiletransferdialog.h"
#include "applefiletransferprogress.h"
#include "appleprotocol.h"
#ifdef Q_OS_DARWIN
#include "applefiledrag_mac.h"
#include "applemacinputbridge.h"
#endif
#ifdef Q_OS_WIN
#include "applefiledrag_win.h"
#endif
#include "streaming/localstreamruntime.h"

#include "SDL.h"

#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>
#include <QMutexLocker>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include "SDL_syswm.h"
#endif

#include <chrono>
#include <memory>
#include <utility>

using AppleScreenSharingSessionPrivate::cursorDpiScale;
using AppleScreenSharingSessionPrivate::steadyNanoseconds;
#ifdef Q_OS_WIN
using AppleScreenSharingSessionPrivate::nativeHandleForWindow;
#endif

void AppleScreenSharingSession::ensureLocalFileDragLifecycle()
{
    if (m_LocalFileDragLifecycle != nullptr) return;
    m_LocalFileDragLifecycle =
            std::make_shared<AppleLocalFileDragLifecycle>(
                    [this](const AppleFileDragPoint& point) {
                        return remotePoint(
                                point.x,
                                point.y,
                                point.displayIndex).has_value();
                    },
                    [this](const QStringList& paths) {
                        QList<QByteArray> messages;
                        QString error;
                        if (!m_FileTransferService->beginLocalDrop(
                                    paths, &messages, &error)) {
                            qWarning().noquote()
                                    << "Apple local file drag could not start:"
                                    << error;
                            return false;
                        }
                        for (QByteArray& message : messages) {
                            AppleOutboundControl outbound;
                            outbound.kind = AppleOutboundControl::Kind::Message;
                            outbound.message = std::move(message);
                            queueControl(std::move(outbound));
                        }
                        qInfo() << "Apple local file drag entered the remote display";
                        return true;
                    },
                    [this](const AppleFileDragPoint& point,
                           AppleLocalFileDragPointerAction action) {
                        const bool pressed = action !=
                                AppleLocalFileDragPointerAction::Release;
                        if (!remotePoint(
                                    point.x,
                                    point.y,
                                    point.displayIndex).has_value()) {
                            if (!pressed && m_LocalFileDragPointerActive) {
                                queueFileDragPointer(
                                        m_LastLocalFileDragX,
                                        m_LastLocalFileDragY,
                                        m_LastLocalFileDragDisplayIndex,
                                        false,
                                        false);
                                m_LocalFileDragPointerActive = false;
                            }
                            return;
                        }
                        m_LastLocalFileDragX = point.x;
                        m_LastLocalFileDragY = point.y;
                        m_LastLocalFileDragDisplayIndex = point.displayIndex;
                        m_LocalFileDragPointerActive = pressed;
                        m_LastMouseX = point.x;
                        m_LastMouseY = point.y;
                        m_LastMouseDisplayIndex = point.displayIndex;
                        queueFileDragPointer(
                                point.x,
                                point.y,
                                point.displayIndex,
                                pressed,
                                action ==
                                        AppleLocalFileDragPointerAction::Move);
                    },
                    [this]() {
                        QList<QByteArray> messages;
                        m_FileTransferService->cancelLocalDrop(&messages);
                        for (QByteArray& message : messages) {
                            AppleOutboundControl outbound;
                            outbound.kind = AppleOutboundControl::Kind::Message;
                            outbound.message = std::move(message);
                            queueControl(std::move(outbound));
                        }
                    });
}

#ifdef Q_OS_WIN
void AppleScreenSharingSession::installWindowsFileDropTarget(
        SDL_Window* window,
        int displayIndex)
{
    if (window == nullptr) return;
    ensureLocalFileDragLifecycle();
    void* const nativeWindow = nativeHandleForWindow(window);
    auto target = std::make_unique<AppleWindowsFileDropTarget>(
            nativeWindow,
            displayIndex,
            m_LocalFileDragLifecycle);
    if (!target->isValid()) {
        qWarning().noquote()
                << "Apple native Windows file drop unavailable:"
                << target->errorString();
        return;
    }
    m_WindowsFileDropTargets.push_back(std::move(target));
    if (m_WindowsRemoteFileDragSource != nullptr) {
        m_WindowsRemoteFileDragSource->addStreamWindow(nativeWindow);
    }
    qInfo() << "Apple native Windows file drop target enabled for display"
            << displayIndex + 1;
}
#endif


void AppleScreenSharingSession::applyControlEvents(
        const AppleControlEvents& events)
{
    for (const AppleCursorUpdate& update : events.cursorUpdates) {
        ++m_RemoteCursorUpdateCount;
        m_RemoteCursorStore.apply(update);
    }
    if (!events.cursorUpdates.isEmpty()) {
        refreshRemoteCursor(cursorWindow(), true);
    }
    for (const AppleDisplayLayout& layout : events.displayLayouts) {
        if (m_MediaDisplayIds.isEmpty()) {
            for (const AppleDisplayRect& display : layout.displays) {
                m_MediaDisplayIds.append(display.id);
            }
        }
        m_DisplayLayout = layout;
        qInfo().nospace()
                << "Apple Screen Sharing display layout: "
                << layout.displays.size() << " display(s), backing="
                << layout.backingWidth << "x" << layout.backingHeight;
    }
    if (!events.cursorUpdates.isEmpty()) {
        updateControlSummary();
    }
}

SDL_Window* AppleScreenSharingSession::cursorWindow() const
{
    SDL_Window* primaryWindow = m_Runtime != nullptr
            ? m_Runtime->streamWindow() : nullptr;
    SDL_Window* focusedWindow = SDL_GetMouseFocus();
    if (focusedWindow != nullptr &&
            (focusedWindow == primaryWindow ||
             focusedWindow == m_SecondaryWindow)) {
        return focusedWindow;
    }
    return primaryWindow != nullptr ? primaryWindow : m_SecondaryWindow;
}

void AppleScreenSharingSession::refreshRemoteCursor(
        SDL_Window* window,
        bool force)
{
    const std::optional<AppleCursorImage> source =
            m_RemoteCursorStore.selectedImage();
    if (!source.has_value()) {
        useDefaultRemoteCursor();
        return;
    }

    const double dpiScale = cursorDpiScale(window);
    if (!force && m_ActiveRemoteCursor != nullptr &&
            qFuzzyCompare(dpiScale, m_ActiveRemoteCursorScale)) {
        return;
    }

    const AppleCursorImage cursorImage = source->scaledForDpi(dpiScale);
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
            const_cast<char*>(cursorImage.rgba.constData()),
            cursorImage.width,
            cursorImage.height,
            32,
            cursorImage.width * 4,
            SDL_PIXELFORMAT_RGBA32);
    SDL_Cursor* cursor = nullptr;
    if (surface != nullptr) {
        cursor = SDL_CreateColorCursor(
                surface,
                cursorImage.hotspotX,
                cursorImage.hotspotY);
        SDL_FreeSurface(surface);
    }
    if (cursor == nullptr) {
        qWarning().nospace()
                << "Failed to create Apple remote cursor: " << SDL_GetError();
        useDefaultRemoteCursor();
        return;
    }

    SDL_Cursor* previous = m_ActiveRemoteCursor;
    SDL_SetCursor(cursor);
    m_ActiveRemoteCursor = cursor;
    m_ActiveRemoteCursorScale = dpiScale;
    SDL_ShowCursor(SDL_ENABLE);
    if (previous != nullptr) {
        SDL_FreeCursor(previous);
    }

    const std::optional<quint32> selectedId = m_RemoteCursorStore.selectedId();
    qInfo().nospace()
            << "Apple remote cursor id="
            << (selectedId.has_value() ? QString::number(*selectedId)
                                       : QStringLiteral("none"))
            << ", points=" << source->width << "x" << source->height
            << ", dpi=" << QString::number(dpiScale, 'f', 2)
            << "x, raster=" << cursorImage.width << "x"
            << cursorImage.height;
}

void AppleScreenSharingSession::useDefaultRemoteCursor()
{
    SDL_Cursor* previous = m_ActiveRemoteCursor;
    m_ActiveRemoteCursor = nullptr;
    m_ActiveRemoteCursorScale = 0.0;
    if (previous == nullptr) {
        return;
    }
    SDL_SetCursor(SDL_GetDefaultCursor());
    SDL_ShowCursor(SDL_ENABLE);
    SDL_FreeCursor(previous);
}

void AppleScreenSharingSession::applyRemoteClipboardArchive(
        const AppleClipboardArchive& archive,
        bool receivedAutomatically)
{
    if (m_Observing.load() || archive.isEmpty() ||
            (receivedAutomatically &&
             !m_ClipboardAutomaticEligible.load())) {
        return;
    }
    m_ApplyingRemoteClipboard = true;
    m_LocalClipboardTracker.expectRemoteArchive(archive);
    if (!AppleClipboard::writeSystemArchive(archive)) {
        m_LocalClipboardTracker.reset();
        m_ApplyingRemoteClipboard = false;
        qWarning() << "Apple clipboard could not install the remote archive";
        return;
    }
    // Consume the native write immediately. QClipboard::dataChanged may be
    // delivered synchronously or later depending on the platform plugin.
    (void)m_LocalClipboardTracker.dataChanged(
            AppleClipboard::readSystemArchive());
    m_ApplyingRemoteClipboard = false;

    int flavorCount = 0;
    qsizetype byteCount = 0;
    for (const AppleClipboardItem& item : archive.items) {
        flavorCount += item.flavors.size();
        for (const AppleClipboardFlavor& flavor : item.flavors) {
            byteCount += flavor.value.size();
        }
    }
    qInfo().nospace()
            << "Apple clipboard received " << archive.items.size()
            << " item(s), " << flavorCount << " flavor(s), "
            << byteCount << " bytes"
            << (receivedAutomatically ? " automatically" : " manually");
}

void AppleScreenSharingSession::finishNativeRemoteFileDrag(
        quint32 sessionId)
{
    QList<QByteArray> cancelMessages;
    if (m_FileTransferService != nullptr) {
        m_FileTransferService->cancelRemoteDrag(
                sessionId, &cancelMessages);
    }
    for (QByteArray& message : cancelMessages) {
        AppleOutboundControl outbound;
        outbound.kind = AppleOutboundControl::Kind::Message;
        outbound.message = std::move(message);
        queueControl(std::move(outbound));
    }
    if (m_RemoteFileDragInputState != nullptr) {
        const AppleRemoteFileDragInputTransition ended =
                m_RemoteFileDragInputState->nativeDragEnded(m_MouseButtons);
        m_MouseButtons = ended.buttons;
    }
    queueFileDragPointer(
            m_LastMouseX,
            m_LastMouseY,
            m_LastMouseDisplayIndex,
            false,
            false);
}

bool AppleScreenSharingSession::activateRemoteFileDragIfEligible(
        bool pointerInsideStream,
        const void* nativeEvent)
{
    if (m_RemoteFileDragGate == nullptr) return false;
#ifdef Q_OS_DARWIN
    if (m_MacRemoteFileDragSource != nullptr &&
            m_MacRemoteFileDragSource->isValid()) {
        const bool leftButtonDown =
                m_MacRemoteFileDragSource->leftButtonDown();
        if (nativeEvent == nullptr) {
            if (m_RemoteFileDragGate->hasPending() && leftButtonDown &&
                    !pointerInsideStream &&
                    m_AppleMacInputBridge != nullptr) {
                // The type-32 notification arrives asynchronously after the
                // drag crossed the view boundary. Re-run the native event,
                // matching RemoteVideoCanvasNSView in the Swift client.
                m_AppleMacInputBridge->repostRemoteDragEvent();
            }
            return false;
        }
        const std::optional<AppleRemoteFileDrag> drag =
                m_RemoteFileDragGate->takeIfEligible(
                        leftButtonDown, pointerInsideStream);
        if (!drag.has_value()) return false;

        const AppleRemoteFileDragInputTransition began =
                m_RemoteFileDragInputState->nativeDragBegan(m_MouseButtons);
        m_MouseButtons = began.buttons;
        const std::shared_ptr<AppleFileTransferService> service =
                m_FileTransferService;
        const quint32 remoteDragSessionId = drag->sessionId;
        const QPointer<AppleScreenSharingSession> session(this);
        QString startError;
        const bool started = m_MacRemoteFileDragSource->begin(
                *drag,
                nativeEvent,
                [service](const QString& sourcePath,
                          const QString& destinationPath,
                          const std::atomic_bool& nativeCancelled,
                          QString* completedPath,
                          QString* error) {
                    return service != nullptr &&
                            service->materializeRemoteFile(
                                    sourcePath,
                                    destinationPath,
                                    nativeCancelled,
                                    completedPath,
                                    error);
                },
                [session, remoteDragSessionId](
                        AppleMacRemoteFileDragResult result,
                        const QString& error) {
                    if (session == nullptr) return;
                    QMetaObject::invokeMethod(
                            session,
                            [session, remoteDragSessionId, result, error]() {
                                if (session == nullptr) return;
                                session->finishNativeRemoteFileDrag(
                                        remoteDragSessionId);
                                if (result ==
                                        AppleMacRemoteFileDragResult::Dropped) {
                                    qInfo() << "Apple promised files were dropped through Finder";
                                }
                                else if (result ==
                                                 AppleMacRemoteFileDragResult::Failed &&
                                         !error.isEmpty()) {
                                    qWarning().noquote()
                                            << "Apple promised-file drag failed:"
                                            << error;
                                    session->addLaunchWarning(error);
                                }
                            },
                            Qt::QueuedConnection);
                },
                &startError);
        if (started) {
            qInfo() << "Apple remote promised-file drag entered Finder";
            return true;
        }
        const AppleRemoteFileDragInputTransition failed =
                m_RemoteFileDragInputState->nativeDragStartFailed(
                        m_MouseButtons);
        m_MouseButtons = failed.buttons;
        m_RemoteFileDragGate->update(*drag);
        qWarning().noquote()
                << "Apple promised-file drag could not start:"
                << startError;
        return false;
    }
#else
    Q_UNUSED(nativeEvent);
#endif
    bool leftButtonDown = (m_MouseButtons & 1) != 0;
#ifdef Q_OS_WIN
    if (m_WindowsRemoteFileDragSource != nullptr &&
            m_WindowsRemoteFileDragSource->isValid()) {
        // SDL can lose its button state as soon as the cursor leaves the
        // client area. The async type-32 promise must use physical Windows
        // state and the actual HWND client rectangle instead.
        leftButtonDown = m_WindowsRemoteFileDragSource->leftButtonDown();
        pointerInsideStream =
                m_WindowsRemoteFileDragSource->pointerInsideWindow();
    }
#endif
    const std::optional<AppleRemoteFileDrag> drag =
            m_RemoteFileDragGate->takeIfEligible(
                    leftButtonDown, pointerInsideStream);
    if (!drag.has_value()) return false;

    const QPointer<AppleScreenSharingSession> session(this);
    QTimer::singleShot(0, this, [session, drag]() {
        if (session == nullptr || session->m_Cancelled.load()) return;
#ifdef Q_OS_WIN
        if (session->m_WindowsRemoteFileDragSource != nullptr &&
                session->m_WindowsRemoteFileDragSource->isValid()) {
            const QString temporaryRoot = QStandardPaths::writableLocation(
                    QStandardPaths::TempLocation);
            auto staging = std::make_shared<QTemporaryDir>(
                    QDir(temporaryRoot).filePath(
                            QStringLiteral("Moonlight-AppleDrag-XXXXXX")));
            if (!staging->isValid()) {
                const QString error = tr("Couldn’t create temporary storage for the remote drag.");
                qWarning().noquote() << "Apple promised-file drag:" << error;
                session->addLaunchWarning(error);
                return;
            }
            // Explorer may complete the copy asynchronously after DoDragDrop
            // returns, so cleanup is delayed after a successful drop.
            staging->setAutoRemove(false);
            const QString stagingPath = staging->path();
            const std::shared_ptr<AppleFileTransferService> service =
                    session->m_FileTransferService;
            // OLE owns the physical mouse-up once DoDragDrop begins. Transfer
            // ownership before entering its nested message loop.
            const AppleRemoteFileDragInputTransition began =
                    session->m_RemoteFileDragInputState->nativeDragBegan(
                            session->m_MouseButtons);
            session->m_MouseButtons = began.buttons;
            qInfo() << "Apple remote promised-file drag entered Windows Explorer";
            QString startError;
            const bool started = session->m_WindowsRemoteFileDragSource->begin(
                    *drag,
                    [service, drag, staging](
                            const std::atomic_bool& nativeCancelled,
                            QStringList* paths,
                            QString* error) {
                        return service != nullptr &&
                                service->materializeRemoteDrag(
                                        *drag,
                                        staging->path(),
                                        nativeCancelled,
                                        paths,
                                        error);
                    },
                    [session, stagingPath](
                            AppleWindowsRemoteFileDragResult result,
                            const QString& error) {
                        if (session == nullptr) {
                            QDir(stagingPath).removeRecursively();
                            return;
                        }
                        QMetaObject::invokeMethod(
                                session,
                                [session, stagingPath, result, error]() {
                                    if (session == nullptr) {
                                        QDir(stagingPath).removeRecursively();
                                        return;
                                    }
                                    if (result == AppleWindowsRemoteFileDragResult::Failed) {
                                        qWarning().noquote()
                                                << "Apple promised-file drag failed:"
                                                << error;
                                        if (!error.isEmpty()) {
                                            session->addLaunchWarning(error);
                                        }
                                    }
                                    else if (result == AppleWindowsRemoteFileDragResult::Dropped) {
                                        qInfo() << "Apple promised files were dropped through Windows Explorer";
                                    }
                                    const int cleanupDelay =
                                            result == AppleWindowsRemoteFileDragResult::Dropped
                                            ? 10 * 60 * 1000 : 0;
                                    QTimer::singleShot(
                                            cleanupDelay,
                                            QCoreApplication::instance(),
                                            [stagingPath]() {
                                                QDir(stagingPath).removeRecursively();
                                            });
                                },
                                Qt::QueuedConnection);
                    },
                    &startError);
            if (started) {
                if (session != nullptr) {
                    session->finishNativeRemoteFileDrag(drag->sessionId);
                }
                return;
            }
            const AppleRemoteFileDragInputTransition failed =
                    session->m_RemoteFileDragInputState->nativeDragStartFailed(
                            session->m_MouseButtons);
            session->m_MouseButtons = failed.buttons;
            QDir(stagingPath).removeRecursively();
            qWarning().noquote()
                    << "Apple promised-file drag could not start:"
                    << startError;
            return;
        }
#endif
        // Keep a portable fallback for platforms without a native promised-
        // file source. Windows normally takes the OLE branch above.
        QSettings settings;
        const QString fallbackDirectory =
                QStandardPaths::writableLocation(
                        QStandardPaths::DownloadLocation);
        const QString initialDirectory = settings.value(
                QStringLiteral("appleScreenSharing/fileTransferDownloadDirectory"),
                fallbackDirectory).toString();
        void* ownerWindow = nullptr;
#ifdef Q_OS_WIN
        ownerWindow = nativeHandleForWindow(
                session->m_Runtime != nullptr
                        ? session->m_Runtime->streamWindow() : nullptr);
#endif
        const QString destination = chooseAppleFileTransferDirectory(
                tr("Save files from %1").arg(session->m_Connection.displayName),
                initialDirectory,
                ownerWindow);
        if (destination.isEmpty()) {
            qInfo() << "Apple remote file drag was declined after leaving the stream";
            return;
        }
        settings.setValue(
                QStringLiteral("appleScreenSharing/fileTransferDownloadDirectory"),
                destination);
        QString error;
        if (!session->m_FileTransferService->acceptRemoteDrag(
                    *drag, destination, &error)) {
            qWarning().noquote()
                    << "Apple remote file transfer could not start:"
                    << error;
            session->addLaunchWarning(error);
        }
    });
    return true;
}

void AppleScreenSharingSession::applyFileTransferEvents(
        QList<AppleFileTransferEvent> events)
{
    for (const AppleFileTransferEvent& event : std::as_const(events)) {
        if (event.kind == AppleFileTransferEvent::Kind::RemoteDrag) {
            m_RemoteFileDragGate->update(event.remoteDrag);
            if (!event.remoteDrag.sourcePaths.isEmpty()) {
                SDL_Window* window = m_Runtime != nullptr
                        ? m_Runtime->streamWindow() : nullptr;
                int globalX = 0;
                int globalY = 0;
                SDL_GetGlobalMouseState(&globalX, &globalY);
                int windowX = 0;
                int windowY = 0;
                int width = 0;
                int height = 0;
                if (window != nullptr) {
                    SDL_GetWindowPosition(window, &windowX, &windowY);
                    SDL_GetWindowSize(window, &width, &height);
                }
                const bool inside = window != nullptr &&
                        globalX >= windowX && globalY >= windowY &&
                        globalX < windowX + width &&
                        globalY < windowY + height;
                activateRemoteFileDragIfEligible(inside);
            }
            continue;
        }

        const bool incoming = event.direction ==
                AppleFileTransferEvent::Direction::FromRemote;
        if (!m_FileTransferProgressWindow) {
            const QPointer<AppleScreenSharingSession> session(this);
            m_FileTransferProgressWindow =
                    std::make_unique<AppleFileTransferProgressWindow>(
                            [session](quint32 sessionId, bool paused) {
                                if (session != nullptr) {
                                    session->m_FileTransferService->setPaused(
                                            sessionId, paused);
                                }
                            },
                            [session](quint32 sessionId) {
                                if (session != nullptr) {
                                    session->m_FileTransferService->cancel(
                                            sessionId);
                                }
                            });
        }
        if (event.kind == AppleFileTransferEvent::Kind::Started ||
                event.kind == AppleFileTransferEvent::Kind::Progress ||
                event.kind == AppleFileTransferEvent::Kind::Paused) {
            m_ActiveFileTransferSessionId = event.sessionId;
            m_ActiveFileTransferPaused =
                    event.kind == AppleFileTransferEvent::Kind::Paused;
        }
        QString state;
        switch (event.kind) {
        case AppleFileTransferEvent::Kind::Started:
            state = incoming ? tr("receiving") : tr("sending");
            break;
        case AppleFileTransferEvent::Kind::Progress:
            state = tr("%1%2")
                    .arg(qRound(event.progress * 100))
                    .arg(QLatin1Char('%'));
            if (event.bytesPerSecond > 0) {
                state += tr(" · %1 MB/s")
                        .arg(event.bytesPerSecond / 1000000.0, 0, 'f', 1);
            }
            break;
        case AppleFileTransferEvent::Kind::Paused:
            state = tr("paused");
            break;
        case AppleFileTransferEvent::Kind::Completing:
            state = tr("finishing");
            break;
        case AppleFileTransferEvent::Kind::Completed:
            state = tr("completed");
            break;
        case AppleFileTransferEvent::Kind::Failed:
            state = tr("failed");
            break;
        case AppleFileTransferEvent::Kind::Cancelled:
            state = tr("cancelled");
            break;
        case AppleFileTransferEvent::Kind::RemoteDrag:
            break;
        }

        AppleFileTransferProgressEntry progressEntry;
        progressEntry.sessionId = event.sessionId;
        progressEntry.name = event.name;
        progressEntry.remoteName = m_Connection.displayName;
        progressEntry.incoming = incoming;
        progressEntry.path = event.path;
        progressEntry.progress = event.progress;
        progressEntry.bytesPerSecond = event.bytesPerSecond;
        progressEntry.hasProgress =
                event.kind == AppleFileTransferEvent::Kind::Progress ||
                event.kind == AppleFileTransferEvent::Kind::Completed;
        switch (event.kind) {
        case AppleFileTransferEvent::Kind::Started:
            progressEntry.state = incoming
                    ? AppleFileTransferProgressState::Receiving
                    : AppleFileTransferProgressState::Sending;
            break;
        case AppleFileTransferEvent::Kind::Progress:
            progressEntry.state = incoming
                    ? AppleFileTransferProgressState::Receiving
                    : AppleFileTransferProgressState::Sending;
            break;
        case AppleFileTransferEvent::Kind::Paused:
            progressEntry.state = AppleFileTransferProgressState::Paused;
            break;
        case AppleFileTransferEvent::Kind::Completing:
            progressEntry.state = AppleFileTransferProgressState::Completing;
            progressEntry.progress = 1.0;
            progressEntry.hasProgress = true;
            break;
        case AppleFileTransferEvent::Kind::Completed:
            progressEntry.state = AppleFileTransferProgressState::Completed;
            progressEntry.progress = 1.0;
            break;
        case AppleFileTransferEvent::Kind::Failed:
            progressEntry.state = AppleFileTransferProgressState::Failed;
            progressEntry.errorText = event.errorText.isEmpty()
                    ? event.path : event.errorText;
            progressEntry.path.clear();
            break;
        case AppleFileTransferEvent::Kind::Cancelled:
            progressEntry.state = AppleFileTransferProgressState::Cancelled;
            break;
        case AppleFileTransferEvent::Kind::RemoteDrag:
            break;
        }
        m_FileTransferProgressWindow->update(progressEntry);

        const QString summary = tr("FILE %1 %2 · %3")
                .arg(incoming ? QStringLiteral("↓") : QStringLiteral("↑"),
                     event.name,
                     state);
        {
            QMutexLocker locker(&m_PerformanceMutex);
            m_FileTransferSummary = summary;
        }
        qInfo().noquote() << "Apple Screen Sharing" << summary;
        requestPerformanceOverlayUpdate();
        if (event.kind == AppleFileTransferEvent::Kind::Completed ||
                event.kind == AppleFileTransferEvent::Kind::Failed ||
                event.kind == AppleFileTransferEvent::Kind::Cancelled) {
            if (m_ActiveFileTransferSessionId == event.sessionId) {
                m_ActiveFileTransferSessionId = 0;
                m_ActiveFileTransferPaused = false;
            }
            QPointer<AppleScreenSharingSession> guard(this);
            QTimer::singleShot(4000, this, [guard, summary]() {
                if (guard == nullptr) return;
                {
                    QMutexLocker locker(&guard->m_PerformanceMutex);
                    if (guard->m_FileTransferSummary != summary) return;
                    guard->m_FileTransferSummary.clear();
                }
                guard->requestPerformanceOverlayUpdate();
            });
        }
    }
}

void AppleScreenSharingSession::updateControlSummary()
{
    const QString clipboard = !m_ClipboardSupported.load()
            ? QStringLiteral("OFF")
            : m_SharedClipboardSupported.load() &&
                      m_ClipboardSharingEnabled.load()
                    ? QStringLiteral("SHARED")
                    : QStringLiteral("MANUAL");
    const QString summary = QStringLiteral(
            "MODE %1   CLIPBOARD %2   FILES %3   CURSOR %4   DISPLAY %5%6   O:MODE C:SHARE G:GET V:SEND M:MUTE P:PAUSE X:CANCEL")
            .arg(m_Observing.load() ? QStringLiteral("OBSERVE")
                                    : QStringLiteral("CONTROL"))
            .arg(!m_Observing.load() && m_ControlReady.load()
                         ? clipboard : QStringLiteral("OFF"))
            .arg(!m_Observing.load() && m_FileTransferSupported.load()
                         ? QStringLiteral("ON") : QStringLiteral("OFF"))
            .arg(m_RemoteCursorUpdateCount)
            .arg(m_DisplayCount)
            .arg(m_DynamicResolutionEnabled
                         ? QStringLiteral(" DYNAMIC") : QString());
    {
        QMutexLocker locker(&m_PerformanceMutex);
        m_ControlSummary = summary;
    }
    if (m_Runtime && m_Runtime->streamWindow() != nullptr) {
        const QString title = tr("%1 — Apple Screen Sharing [%2]")
                .arg(m_Connection.displayName,
                     m_Observing.load() ? tr("Observe") : tr("Control"));
        SDL_SetWindowTitle(m_Runtime->streamWindow(), title.toUtf8().constData());
    }
#ifdef Q_OS_DARWIN
    if (m_AppleMacInputBridge != nullptr) {
        m_AppleMacInputBridge->updateClipboardMenuState(
                m_ClipboardSupported.load(),
                m_SharedClipboardSupported.load(),
                m_ClipboardSharingEnabled.load(),
                m_ControlReady.load() && !m_Observing.load());
    }
#endif
    requestPerformanceOverlayUpdate();
}

void AppleScreenSharingSession::localClipboardChanged()
{
    if (m_ApplyingRemoteClipboard) {
        return;
    }
    refreshLocalClipboard(false);
}

void AppleScreenSharingSession::refreshLocalClipboard(bool windowFocusGained)
{
    if (!m_ControlReady.load() || m_Observing.load() ||
            !m_SharedClipboardSupported.load() ||
            !m_ClipboardSharingEnabled.load() ||
            !m_ClipboardAutomaticEligible.load()) {
        return;
    }
    const std::optional<AppleClipboardArchive> systemArchive =
            AppleClipboard::readSystemArchive();
    const std::optional<AppleClipboardArchive> archive = windowFocusGained
            ? m_LocalClipboardTracker.windowFocusGained(systemArchive)
            : m_LocalClipboardTracker.dataChanged(systemArchive);
    if (!archive.has_value()) {
        return;
    }
    AppleOutboundControl outbound;
    outbound.kind = AppleOutboundControl::Kind::LocalClipboardArchive;
    outbound.clipboardArchive = *archive;
    queueControl(std::move(outbound));
    qInfo().nospace()
            << "Apple clipboard advertised " << archive->items.size()
            << " item(s) after "
            << (windowFocusGained ? "stream-window focus" : "local change");
}

void AppleScreenSharingSession::setClipboardWindowFocused(
        quint32 windowId,
        bool focused)
{
    if (focused) {
        m_ClipboardFocusedWindows.insert(windowId);
    }
    else {
        m_ClipboardFocusedWindows.remove(windowId);
    }
    updateClipboardAutomaticEligibility(focused);
}

void AppleScreenSharingSession::updateClipboardAutomaticEligibility(
        bool refreshWhenEligible)
{
    const bool eligible = m_SharedClipboardSupported.load() &&
            m_ClipboardSharingEnabled.load() && !m_Observing.load() &&
            !m_ClipboardFocusedWindows.isEmpty();
    const bool changed = m_ClipboardAutomaticEligible.exchange(eligible) !=
            eligible;
    if (changed && m_ControlReady.load()) {
        AppleOutboundControl outbound;
        outbound.kind =
                AppleOutboundControl::Kind::SetClipboardAutomaticEligible;
        outbound.enabled = eligible;
        queueControl(std::move(outbound));
    }
    if (eligible && refreshWhenEligible) {
        refreshLocalClipboard(true);
    }
}

void AppleScreenSharingSession::toggleClipboardSharing()
{
    if (!m_ControlReady.load() || m_Observing.load() ||
            !m_SharedClipboardSupported.load()) {
        return;
    }
    const bool enabled = !m_ClipboardSharingEnabled.load();
    m_ClipboardSharingEnabled.store(enabled);
    m_Connection.sharedClipboardEnabled = enabled;
    emit clipboardSharingChanged(m_Connection.id, enabled);
    AppleOutboundControl outbound;
    outbound.kind = AppleOutboundControl::Kind::SetClipboardSharing;
    outbound.enabled = enabled;
    queueControl(std::move(outbound));
    updateClipboardAutomaticEligibility(enabled);
    updateControlSummary();
    qInfo() << "Apple shared clipboard" << (enabled ? "enabled" : "disabled");
}

void AppleScreenSharingSession::requestRemoteClipboard()
{
    if (!m_ControlReady.load() || m_Observing.load() ||
            !m_ClipboardSupported.load() ||
            (m_SharedClipboardSupported.load() &&
             m_ClipboardSharingEnabled.load())) {
        return;
    }
    AppleOutboundControl outbound;
    outbound.kind = AppleOutboundControl::Kind::RequestRemoteClipboard;
    queueControl(std::move(outbound));
    qInfo() << "Apple clipboard manual receive requested";
}

void AppleScreenSharingSession::sendLocalClipboard()
{
    if (!m_ControlReady.load() || m_Observing.load() ||
            !m_ClipboardSupported.load() ||
            (m_SharedClipboardSupported.load() &&
             m_ClipboardSharingEnabled.load())) {
        return;
    }
    const std::optional<AppleClipboardArchive> archive =
            AppleClipboard::readSystemArchive();
    if (!archive.has_value()) {
        return;
    }
    AppleOutboundControl outbound;
    outbound.kind = AppleOutboundControl::Kind::SendClipboardArchive;
    outbound.clipboardArchive = *archive;
    queueControl(std::move(outbound));
    qInfo() << "Apple clipboard manually sent" << archive->items.size()
            << "item(s)";
}

void AppleScreenSharingSession::toggleControlMode()
{
    const bool observing = !m_Observing.load();
    if (observing) {
        releaseAllKeys();
    }
    m_Observing.store(observing);
    m_FileTransferService->setControlling(!observing);
    updateClipboardAutomaticEligibility(!observing);
    m_MouseButtons = 0;
    AppleOutboundControl outbound;
    outbound.kind = AppleOutboundControl::Kind::SetObserving;
    outbound.observing = observing;
    queueControl(std::move(outbound));
    updateControlSummary();
    if (!observing) {
        localClipboardChanged();
    }
    qInfo() << "Apple Screen Sharing mode changed to"
            << (observing ? "observe" : "control");
}

void AppleScreenSharingSession::toggleAudioMute()
{
    const bool muted = !m_AudioMuted.load();
    m_AudioMuted.store(muted);
    updateControlSummary();
    qInfo() << "Apple Screen Sharing audio"
            << (muted ? "muted" : "unmuted");
}

void AppleScreenSharingSession::scheduleDynamicResolution(
        SDL_Window* window,
        int width,
        int height,
        bool waitsForViewportToSettle)
{
    if (!m_DynamicResolutionEnabled || m_DisplayCount != 1 ||
            m_DynamicResolutionTimer == nullptr) {
        return;
    }
    const QSize size = AppleDynamicResolution::normalizedSizeForDpi(
            width, height, cursorDpiScale(window));
    if (!size.isValid()) {
        return;
    }
    if (size == m_LastRequestedDynamicResolution ||
            (m_Canvas.width == size.width() * 2 &&
             m_Canvas.height == size.height() * 2)) {
        m_PendingDynamicResolution = {};
        m_DynamicResolutionTimer->stop();
        return;
    }
    if (size == m_PendingDynamicResolution &&
            m_DynamicResolutionTimer->isActive()) {
        return;
    }
    m_PendingDynamicResolution = size;
    if (m_LiveResizing) {
        m_DynamicResolutionTimer->stop();
        return;
    }
    const quint64 now = steadyNanoseconds() / 1000000ULL;
    const quint64 earliest = m_LastDynamicResolutionRequestAt == 0
            ? now : m_LastDynamicResolutionRequestAt + 2500;
    const quint64 debounce = waitsForViewportToSettle ? 500 : 0;
    const int interval = static_cast<int>(qMax<quint64>(
            debounce, earliest > now ? earliest - now : 0));
    m_DynamicResolutionTimer->start(interval);
}

void AppleScreenSharingSession::sendPendingDynamicResolution()
{
    if (!m_DynamicResolutionEnabled ||
            !m_PendingDynamicResolution.isValid()) {
        return;
    }
    if (!m_ControlReady.load()) {
        m_DynamicResolutionTimer->start(100);
        return;
    }
    const QSize size = m_PendingDynamicResolution;
    m_PendingDynamicResolution = {};
    const QByteArray message = AppleWire::displayConfiguration({size});
    if (message.isEmpty()) {
        return;
    }
    AppleOutboundControl outbound;
    outbound.kind = AppleOutboundControl::Kind::Message;
    outbound.message = message;
    queueControl(std::move(outbound));
    m_LastRequestedDynamicResolution = size;
    m_LastDynamicResolutionRequestAt = steadyNanoseconds() / 1000000ULL;
    qInfo().nospace() << "Apple Screen Sharing requested dynamic resolution "
                      << size.width() << "x" << size.height();
}
