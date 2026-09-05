#include "applescreensharingsession.h"
#include "applescreensharingsession_p.h"

#include "appleinputsourceplatform.h"

#include "applefiledrag.h"
#include "applekeyboardmapper.h"
#include "applewindowskeyboardhook_p.h"
#ifdef Q_OS_DARWIN
#include "applemacinputbridge.h"
#endif
#include "settings/streamingpreferences.h"
#include "streaming/localstreamruntime.h"

#include "SDL.h"

#include <QClipboard>
#include <QDebug>
#include <QGuiApplication>
#include <QMutexLocker>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <chrono>
#include <utility>

using AppleScreenSharingSessionPrivate::steadyNanoseconds;

namespace {

quint32 currentMicroseconds()
{
    return static_cast<quint32>(
            SDL_GetPerformanceCounter() * 1000000ULL / SDL_GetPerformanceFrequency());
}

quint8 appleButtonForSdl(quint8 button)
{
    switch (button) {
    case SDL_BUTTON_LEFT: return 1 << 0;
    case SDL_BUTTON_RIGHT: return 1 << 1;
    case SDL_BUTTON_MIDDLE: return 1 << 2;
    default: return 0;
    }
}

void recordMaximum(std::atomic<quint64>& value, quint64 candidate)
{
    quint64 previous = value.load(std::memory_order_relaxed);
    while (previous < candidate &&
           !value.compare_exchange_weak(
                   previous, candidate,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
    }
}

} // namespace

QList<AppleOutboundControl> AppleScreenSharingSession::takePendingControls()
{
    QMutexLocker locker(&m_InputMutex);
    QList<AppleOutboundControl> result = std::move(m_PendingControls);
    m_PendingControls.clear();
    return result;
}


void AppleScreenSharingSession::handleMouseMotion(
        int x, int y, int displayIndex)
{
    m_LastMouseX = x;
    m_LastMouseY = y;
    m_LastMouseDisplayIndex = displayIndex;
    queuePointer(x, y, 0, 0, displayIndex);
    // Motion events are inside the stream window. A pending remote drag must
    // remain inert so ordinary Finder rearrangement never starts a download.
    activateRemoteFileDragIfEligible(true);
}

void AppleScreenSharingSession::handleMouseButton(
        bool down,
        quint8 button,
        int clickCount,
        int x,
        int y,
        int displayIndex,
        SDL_Window* eventWindow)
{
    if (button == 0) {
        return;
    }
    if (down) {
        if (button == 1 && m_RemoteFileDragInputState != nullptr) {
            const AppleRemoteFileDragInputTransition transition =
                    m_RemoteFileDragInputState->localLeftButtonChanged(
                            true, m_MouseButtons);
            m_MouseButtons = transition.buttons;
        }
        else {
            m_MouseButtons |= button;
        }
    }
    else {
        if (button == 1 && m_RemoteFileDragGate != nullptr &&
                m_RemoteFileDragGate->hasPending()) {
            int width = 0;
            int height = 0;
            if (eventWindow != nullptr) {
                SDL_GetWindowSize(eventWindow, &width, &height);
            }
            const bool inside = eventWindow != nullptr &&
                    x >= 0 && y >= 0 && x < width && y < height;
            activateRemoteFileDragIfEligible(inside);
            if (inside) {
                m_RemoteFileDragGate->clear();
            }
        }
        if (button == 1 && m_RemoteFileDragInputState != nullptr) {
            const AppleRemoteFileDragInputTransition transition =
                    m_RemoteFileDragInputState->localLeftButtonChanged(
                            false, m_MouseButtons);
            m_MouseButtons = transition.buttons;
            if (!transition.forwardToRemote) {
                m_LastMouseX = x;
                m_LastMouseY = y;
                m_LastMouseDisplayIndex = displayIndex;
                return;
            }
        }
        else {
            m_MouseButtons &= ~button;
        }
    }
    m_LastMouseX = x;
    m_LastMouseY = y;
    m_LastMouseDisplayIndex = displayIndex;
    queuePointer(x, y, clickCount, 0, displayIndex);
}


void AppleScreenSharingSession::queueScroll(
        int windowX,
        int windowY,
        qint32 deltaX,
        qint32 deltaY,
        double preciseDeltaX,
        double preciseDeltaY,
        bool flipped,
        int displayIndex,
        const AppleScrollWheelEvent* nativeEvent)
{
    if (m_Observing.load()) {
        return;
    }
    const auto point = remotePoint(windowX, windowY, displayIndex);
    if (!point.has_value()) {
        return;
    }
    std::optional<quint32> displayId;
    if (m_DisplayCount > 1 &&
            displayIndex >= 0 && displayIndex < m_MediaDisplayIds.size()) {
        displayId = m_MediaDisplayIds.at(displayIndex);
    }

    QMutexLocker locker(&m_InputMutex);
    if (displayId != m_SelectedInputDisplayId) {
        AppleOutboundControl selection;
        selection.kind = AppleOutboundControl::Kind::Message;
        selection.queuedAtNanoseconds = steadyNanoseconds();
        selection.message = displayId.has_value()
                ? AppleMediaWire::selectDisplay(*displayId)
                : AppleMediaWire::selectCombinedDisplays();
        m_PendingControls.append(std::move(selection));
        m_SelectedInputDisplayId = displayId;
    }
    AppleOutboundControl input;
    input.kind = AppleOutboundControl::Kind::Message;
    input.queuedAtNanoseconds = steadyNanoseconds();
    const AppleScrollWheelEvent scrollEvent = nativeEvent != nullptr
            ? *nativeEvent
            : AppleMediaWire::scrollWheelDeltas(
                    deltaX,
                    deltaY,
                    preciseDeltaX,
                    preciseDeltaY,
                    flipped,
                    ++m_ScrollEventCount,
                    m_ScrollSpeedMultiplier);
    input.message = AppleMediaWire::scrollWheelEvent(
            scrollEvent, point->first, point->second);
    m_PendingControls.append(std::move(input));
    recordMaximum(m_MaxPendingControlDepth,
                  static_cast<quint64>(m_PendingControls.size()));
}

void AppleScreenSharingSession::queuePointer(
        int windowX,
        int windowY,
        int clickCount,
        quint8 extraButtons,
        int displayIndex)
{
    if (m_Observing.load()) {
        return;
    }
    queuePointerFrame(
            windowX,
            windowY,
            m_MouseButtons | extraButtons,
            clickCount,
            displayIndex,
            clickCount == 0 && extraButtons == 0
                    ? AppleOutboundControl::Coalescing::PointerMotion
                    : AppleOutboundControl::Coalescing::None);
}

void AppleScreenSharingSession::queueFileDragPointer(
        int windowX,
        int windowY,
        int displayIndex,
        bool pressed,
        bool moving)
{
    if (m_Observing.load()) return;
    queuePointerFrame(
            windowX,
            windowY,
            pressed ? static_cast<quint8>(1) : static_cast<quint8>(0),
            1,
            displayIndex,
            moving ? AppleOutboundControl::Coalescing::FileDragMotion
                   : AppleOutboundControl::Coalescing::None);
}

void AppleScreenSharingSession::queuePointerFrame(
        int windowX,
        int windowY,
        quint8 buttons,
        int clickCount,
        int displayIndex,
        AppleOutboundControl::Coalescing coalescing)
{
    const auto point = remotePoint(windowX, windowY, displayIndex);
    if (!point.has_value()) {
        return;
    }
    const quint32 now = currentMicroseconds();
    const quint32 delta = now - m_PreviousInputTimestamp;
    m_PreviousInputTimestamp = now;

    std::optional<quint32> displayId;
    if (m_DisplayCount > 1 &&
            displayIndex >= 0 && displayIndex < m_MediaDisplayIds.size()) {
        displayId = m_MediaDisplayIds.at(displayIndex);
    }

    QMutexLocker locker(&m_InputMutex);
    if (displayId != m_SelectedInputDisplayId) {
        AppleOutboundControl selection;
        selection.kind = AppleOutboundControl::Kind::Message;
        selection.queuedAtNanoseconds = steadyNanoseconds();
        selection.message = displayId.has_value()
                ? AppleMediaWire::selectDisplay(*displayId)
                : AppleMediaWire::selectCombinedDisplays();
        m_PendingControls.append(std::move(selection));
        m_SelectedInputDisplayId = displayId;
    }
    AppleOutboundControl input;
    input.kind = AppleOutboundControl::Kind::Input;
    input.queuedAtNanoseconds = steadyNanoseconds();
    input.timestampDeltaMicroseconds = delta;
    input.coalescing = coalescing;
    input.input = AppleMediaWire::pointerEvent(
            buttons,
            point->first,
            point->second,
            clickCount,
            delta);
    if (input.coalescing != AppleOutboundControl::Coalescing::None &&
            !m_PendingControls.isEmpty() &&
            m_PendingControls.last().coalescing == input.coalescing) {
        const quint32 accumulatedDelta =
                m_PendingControls.last().timestampDeltaMicroseconds + delta;
        input.timestampDeltaMicroseconds = accumulatedDelta;
        input.input = AppleMediaWire::pointerEvent(
                buttons,
                point->first,
                point->second,
                clickCount,
                accumulatedDelta);
        m_PendingControls.last() = std::move(input);
        m_PointerMotionsCoalesced.fetch_add(1, std::memory_order_relaxed);
    }
    else {
        m_PendingControls.append(std::move(input));
    }
    recordMaximum(m_MaxPendingControlDepth,
                  static_cast<quint64>(m_PendingControls.size()));
}

void AppleScreenSharingSession::queueKey(
        bool isDown,
        int sdlKeycode,
        int sdlScancode,
        int sdlModifiers,
        bool systemKeyCaptureRequested)
{
    if (m_Observing.load() || m_KeyboardMapper == nullptr) {
        return;
    }
    int platformGuiModifiers = KMOD_NONE;
#ifdef Q_OS_WIN
    const bool leftWinDown =
            (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0;
    const bool rightWinDown =
            (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
    if (leftWinDown) {
        platformGuiModifiers |= KMOD_LGUI;
    }
    if (rightWinDown) {
        platformGuiModifiers |= KMOD_RGUI;
    }
    if (sdlScancode == SDL_SCANCODE_C ||
            sdlScancode == SDL_SCANCODE_V ||
            platformGuiModifiers != KMOD_NONE) {
        qInfo().nospace()
                << "[DEBUG-APPLE-WIN-GUI-STATE] scan=" << sdlScancode
                << " sdl=0x" << Qt::hex << sdlModifiers
                << " native=0x" << platformGuiModifiers << Qt::dec
                << " capture=" << systemKeyCaptureRequested;
    }
#endif
    const QList<AppleRemoteKeyEvent> keys =
            m_KeyboardMapper->updateWithModifiers(
            isDown,
            sdlKeycode,
            sdlScancode,
            sdlModifiers,
            platformGuiModifiers,
            systemKeyCaptureRequested);
    for (const AppleRemoteKeyEvent& key : keys) {
        queueRemoteKey(key);
    }
}

void AppleScreenSharingSession::queueRemoteKey(
        const AppleRemoteKeyEvent& key)
{
    const quint32 now = currentMicroseconds();
    const quint32 delta = now - m_PreviousInputTimestamp;
    m_PreviousInputTimestamp = now;
    AppleOutboundControl outbound;
    outbound.kind = AppleOutboundControl::Kind::Input;
    outbound.timestampDeltaMicroseconds = delta;
    outbound.input = AppleMediaWire::keyEvent(
                key.isDown,
                key.symbol,
                delta,
                key.keyboardType,
                key.keyCode,
                m_KeyEventSubtype.load());
    queueControl(std::move(outbound));
}

void AppleScreenSharingSession::releaseAllKeys()
{
#ifdef Q_OS_DARWIN
    if (m_AppleMacInputBridge != nullptr) {
        m_AppleMacInputBridge->releasePressedModifiers();
    }
#endif
    if (m_KeyboardMapper == nullptr) {
        return;
    }
    const QList<AppleRemoteKeyEvent> releases =
            m_KeyboardMapper->releaseAll();
    if (releases.isEmpty()) {
        return;
    }
    qInfo() << "Apple Screen Sharing releasing"
            << releases.size() << "pressed remote key(s)";
    if (m_Observing.load()) {
        return;
    }
    for (const AppleRemoteKeyEvent& key : releases) {
        queueRemoteKey(key);
    }
}


bool AppleScreenSharingSession::systemKeyCaptureRequestedForWindow(
        quint32 windowId) const
{
    SDL_Window* primary = m_Runtime != nullptr
            ? m_Runtime->streamWindow() : nullptr;
    if (primary != nullptr && SDL_GetWindowID(primary) == windowId) {
        // A Win key can move Windows focus before the queued SDL event is
        // consumed. The event's window ID plus our requested grab is the stable
        // ownership signal; focus loss separately releases every remote key.
        return m_PrimaryKeyboardGrabActive;
    }
    if (m_SecondaryWindow != nullptr &&
            SDL_GetWindowID(m_SecondaryWindow) == windowId) {
        return m_SecondaryKeyboardGrabActive;
    }
    return false;
}

void AppleScreenSharingSession::queueControl(AppleOutboundControl control)
{
    if (control.queuedAtNanoseconds == 0) {
        control.queuedAtNanoseconds = steadyNanoseconds();
    }
    QMutexLocker locker(&m_InputMutex);
    m_PendingControls.append(std::move(control));
    recordMaximum(m_MaxPendingControlDepth,
                  static_cast<quint64>(m_PendingControls.size()));
}

std::optional<QPair<quint16, quint16>> AppleScreenSharingSession::remotePoint(
        int windowX,
        int windowY,
        int displayIndex) const
{
    const AppleCanvas canvas = displayIndex == 1
            ? m_SecondaryFrames.canvas() : m_PrimaryFrames.canvas();
    SDL_Window* window = displayIndex == 1
            ? m_SecondaryWindow : m_Runtime->streamWindow();
    if (!canvas.isUsable() || window == nullptr) {
        return std::nullopt;
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    const double scale = qMin(static_cast<double>(width) / canvas.width,
                              static_cast<double>(height) / canvas.height);
    const double contentWidth = canvas.width * scale;
    const double contentHeight = canvas.height * scale;
    const double left = (width - contentWidth) / 2.0;
    const double top = (height - contentHeight) / 2.0;
    if (windowX < left || windowY < top ||
            windowX >= left + contentWidth || windowY >= top + contentHeight) {
        return std::nullopt;
    }
    const int x = qBound(0, static_cast<int>((windowX - left) / scale),
                         canvas.width - 1);
    const int y = qBound(0, static_cast<int>((windowY - top) / scale),
                         canvas.height - 1);
    return QPair<quint16, quint16>(
            static_cast<quint16>(qBound(0, x, 65535)),
            static_cast<quint16>(qBound(0, y, 65535)));
}

void AppleScreenSharingSession::pollSdlEvents()
{
    const auto displayIndexForWindow = [this](quint32 windowId) {
        return m_SecondaryWindow != nullptr &&
                SDL_GetWindowID(m_SecondaryWindow) == windowId ? 1 : 0;
    };
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
#ifdef Q_OS_WIN
        bool winKeyDown = false;
        bool rightWinKey = false;
        quint32 winKeyWindowId = 0;
        if (m_WindowsKeyboardHook != nullptr &&
                m_WindowsKeyboardHook->decodeEvent(
                        event, &winKeyDown, &rightWinKey,
                        &winKeyWindowId)) {
            qInfo().nospace()
                    << "[DEBUG-APPLE-WIN-HOOK] "
                    << (winKeyDown ? "down" : "up")
                    << " " << (rightWinKey ? "right" : "left")
                    << " window=" << winKeyWindowId;
            queueKey(
                    winKeyDown,
                    rightWinKey ? SDLK_RGUI : SDLK_LGUI,
                    rightWinKey ? SDL_SCANCODE_RGUI : SDL_SCANCODE_LGUI,
                    rightWinKey ? KMOD_RGUI : KMOD_LGUI,
                    systemKeyCaptureRequestedForWindow(winKeyWindowId));
            continue;
        }
#endif
        switch (event.type) {
        case SDL_QUIT:
            interrupt();
            break;
#if SDL_VERSION_ATLEAST(2, 0, 5)
        case SDL_DROPBEGIN:
            m_PendingLocalDropPaths.clear();
            SDL_GetMouseState(&m_LastMouseX, &m_LastMouseY);
            break;
        case SDL_DROPFILE:
            if (event.drop.file != nullptr) {
                m_PendingLocalDropPaths.append(
                        QString::fromUtf8(event.drop.file));
                SDL_free(event.drop.file);
            }
            break;
        case SDL_DROPCOMPLETE: {
#ifdef Q_OS_WIN
            if (!m_WindowsFileDropTargets.empty()) {
                m_PendingLocalDropPaths.clear();
                break;
            }
#endif
            if (m_PendingLocalDropPaths.isEmpty()) break;
            QList<QByteArray> messages;
            QString error;
            if (!m_FileTransferService->beginLocalDrop(
                        std::exchange(m_PendingLocalDropPaths, {}),
                        &messages,
                        &error)) {
                qWarning().noquote()
                        << "Apple local file transfer could not start:"
                        << error;
                addLaunchWarning(error);
                break;
            }
            for (QByteArray& message : messages) {
                AppleOutboundControl outbound;
                outbound.kind = AppleOutboundControl::Kind::Message;
                outbound.message = std::move(message);
                queueControl(std::move(outbound));
            }
            // SDL publishes the file list only after the native drop has
            // concluded. Mirror Swift's concluded-before-begin-completes path:
            // advertise the drag first, then synthesize the remote press and
            // release in that exact control-queue order so Finder asks for the
            // destination with its type-30 response.
            SDL_GetMouseState(&m_LastMouseX, &m_LastMouseY);
            const int displayIndex = displayIndexForWindow(
                    event.drop.windowID);
            // Finder rejects file-drag pointer frames whose click count is
            // zero. Native Apple viewers retain one through both down and up.
            queuePointer(m_LastMouseX, m_LastMouseY, 1, 1, displayIndex);
            queuePointer(m_LastMouseX, m_LastMouseY, 1, 0, displayIndex);
            qInfo() << "Apple local file drag advertised to the Mac";
            break;
        }
#endif
        case SDL_MOUSEMOTION:
#ifdef Q_OS_DARWIN
            if (m_AppleMacInputBridge != nullptr) {
                break;
            }
#endif
            handleMouseMotion(
                    event.motion.x,
                    event.motion.y,
                    displayIndexForWindow(event.motion.windowID));
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
#ifdef Q_OS_DARWIN
            if (m_AppleMacInputBridge != nullptr) {
                break;
            }
#endif
            const quint8 button = appleButtonForSdl(event.button.button);
            SDL_Window* eventWindow = event.button.windowID == 0
                    ? nullptr
                    : SDL_GetWindowFromID(event.button.windowID);
            handleMouseButton(
                    event.type == SDL_MOUSEBUTTONDOWN,
                    button,
                    event.button.clicks,
                    event.button.x,
                    event.button.y,
                    displayIndexForWindow(event.button.windowID),
                    eventWindow);
            break;
        }
        case SDL_MOUSEWHEEL: {
#ifdef Q_OS_DARWIN
            if (m_AppleMacInputBridge != nullptr) {
                break;
            }
#endif
            const int displayIndex = displayIndexForWindow(
                    event.wheel.windowID);
            m_LastMouseX = event.wheel.mouseX;
            m_LastMouseY = event.wheel.mouseY;
            if (m_NativePrecisionScrollSupported.load()) {
                queueScroll(
                        m_LastMouseX,
                        m_LastMouseY,
                        event.wheel.x,
                        event.wheel.y,
                        event.wheel.preciseX,
                        event.wheel.preciseY,
                        event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED,
                        displayIndex);
                break;
            }
            quint8 wheel = 0;
            if (event.wheel.y > 0) wheel |= 1 << 3;
            if (event.wheel.y < 0) wheel |= 1 << 4;
            if (event.wheel.x > 0) wheel |= 1 << 6;
            if (event.wheel.x < 0) wheel |= 1 << 5;
            if (wheel != 0) {
                queuePointer(m_LastMouseX, m_LastMouseY, 0, wheel,
                             displayIndex);
                queuePointer(m_LastMouseX, m_LastMouseY, 0, 0,
                             displayIndex);
            }
            break;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
#ifdef Q_OS_WIN
            if (m_InputSourceMonitor != nullptr) {
                m_InputSourceMonitor->refresh();
            }
#endif
#ifdef Q_OS_WIN
            if (event.key.keysym.sym == SDLK_r &&
                    (event.key.keysym.mod &
                     (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) ==
                            (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                    showWindowsRemoteMenu();
                }
                break;
            }
            if (event.key.keysym.sym == SDLK_i &&
                    (event.key.keysym.mod &
                     (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) ==
                            (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                    toggleKeyboardInputSourceSharing();
                }
                break;
            }
#endif
            if (event.key.keysym.sym == SDLK_s &&
                    (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) ==
                            (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                    togglePerformanceOverlay();
                }
                break;
            }
            if (event.key.keysym.sym == SDLK_o &&
                    (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) ==
                            (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                    toggleControlMode();
                }
                break;
            }
            if (event.key.keysym.sym == SDLK_m &&
                    (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) ==
                            (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                    toggleAudioMute();
                }
                break;
            }
            if (event.key.keysym.sym == SDLK_c &&
                    (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) ==
                            (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                    toggleClipboardSharing();
                }
                break;
            }
            if (event.key.keysym.sym == SDLK_g &&
                    (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) ==
                            (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                    requestRemoteClipboard();
                }
                break;
            }
            if (event.key.keysym.sym == SDLK_v &&
                    (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) ==
                            (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                    sendLocalClipboard();
                }
                break;
            }
            if (event.key.keysym.sym == SDLK_p &&
                    (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) ==
                            (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
                        m_ActiveFileTransferSessionId != 0 &&
                        m_FileTransferService->setPaused(
                                m_ActiveFileTransferSessionId,
                                !m_ActiveFileTransferPaused)) {
                    m_ActiveFileTransferPaused =
                            !m_ActiveFileTransferPaused;
                }
                break;
            }
            if (event.key.keysym.sym == SDLK_x &&
                    (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) ==
                            (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
                        m_ActiveFileTransferSessionId != 0) {
                    m_FileTransferService->cancel(
                            m_ActiveFileTransferSessionId);
                }
                break;
            }
#ifdef Q_OS_DARWIN
            // AppleMacInputBridge owns keyboard delivery on the SDL content
            // view. Ignore SDL's duplicate keyboard events on macOS.
            if (m_AppleMacInputBridge != nullptr) {
                break;
            }
#endif
            if (event.type == SDL_KEYUP || event.key.repeat == 0) {
                queueKey(event.type == SDL_KEYDOWN,
                         event.key.keysym.sym,
                         event.key.keysym.scancode,
                         event.key.keysym.mod,
                         systemKeyCaptureRequestedForWindow(
                                 event.key.windowID));
            }
            break;
        }
        case SDL_WINDOWEVENT: {
            const int displayIndex = displayIndexForWindow(event.window.windowID);
            SDL_Window* changedWindow = displayIndex == 1
                    ? m_SecondaryWindow : m_Runtime->streamWindow();
            if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                if (displayIndex == 0) {
                    interrupt();
                }
                else if (changedWindow != nullptr) {
                    SDL_HideWindow(changedWindow);
                }
                break;
            }
            if (event.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                setWindowMiniaturized(displayIndex, true);
#ifdef Q_OS_WIN
                if (displayIndex == 0) syncWindowsRemoteMenuButton();
#endif
            }
            else if (event.window.event == SDL_WINDOWEVENT_RESTORED) {
                setWindowMiniaturized(displayIndex, false);
#ifdef Q_OS_WIN
                if (displayIndex == 0) syncWindowsRemoteMenuButton();
#endif
            }
            if (event.window.event == SDL_WINDOWEVENT_MOVED ||
                    event.window.event == SDL_WINDOWEVENT_RESTORED ||
                    event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                captureWindowGeometry(
                        changedWindow,
                        displayIndex == 1 ? AppleWindowRole::Secondary
                                          : AppleWindowRole::Primary);
#ifdef Q_OS_WIN
                if (displayIndex == 0) syncWindowsRemoteMenuButton();
#endif
            }
            if (event.window.event == SDL_WINDOWEVENT_ENTER ||
                    event.window.event == SDL_WINDOWEVENT_MOVED ||
                    event.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) {
                refreshRemoteCursor(changedWindow, false);
            }
            if (event.window.event == SDL_WINDOWEVENT_LEAVE) {
                activateRemoteFileDragIfEligible(false);
            }
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                updateKeyboardGrabState(changedWindow);
                // Match the native client: copying normally occurs while the
                // stream window is inactive, so becoming key is the reliable
                // point to sample the current pasteboard. QClipboard's native
                // notification can be missed while SDL owns the foreground
                // window and must remain only the fast path.
                setClipboardWindowFocused(event.window.windowID, true);
            }
            else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                setClipboardWindowFocused(event.window.windowID, false);
                // System shortcuts can move focus before SDL delivers their
                // key-up events. Match both native iScreenSharing and the
                // Moonlight input path by releasing the exact remote keys now.
                releaseAllKeys();
#if defined(Q_OS_DARWIN) || defined(Q_OS_WIN)
                // Native file drags may move focus before their asynchronous
                // type-32 notification arrives. Their completion path owns the
                // remote pointer release after cancellation is queued.
#else
                if (m_MouseButtons != 0) {
                    m_MouseButtons = 0;
                    queuePointer(m_LastMouseX, m_LastMouseY, 0, 0,
                                 displayIndex);
                }
#endif
            }
            switch (event.window.event) {
            case SDL_WINDOWEVENT_EXPOSED:
            case SDL_WINDOWEVENT_SHOWN:
            case SDL_WINDOWEVENT_RESTORED:
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                updateKeyboardGrabState(changedWindow);
                m_PresentationNeeded.store(true);
                if (displayIndex == 1) {
                    m_SecondaryPresentationNeeded.store(true);
                }
                else if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    scheduleDynamicResolution(
                            changedWindow,
                            event.window.data1,
                            event.window.data2);
                }
                break;
            default:
                break;
            }
            break;
        }
        default:
            break;
        }
    }
}


void AppleScreenSharingSession::updateKeyboardGrabState(SDL_Window* window)
{
    if (window == nullptr) {
        return;
    }
    bool shouldGrab = m_CaptureSystemKeysMode !=
            static_cast<int>(StreamingPreferences::CSK_OFF);
    if (shouldGrab && m_CaptureSystemKeysMode ==
            static_cast<int>(StreamingPreferences::CSK_FULLSCREEN) &&
            (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0) {
        shouldGrab = false;
    }

    bool* active = nullptr;
    const bool isPrimary = m_Runtime != nullptr &&
            window == m_Runtime->streamWindow();
    if (isPrimary) {
        active = &m_PrimaryKeyboardGrabActive;
    }
    else if (window == m_SecondaryWindow) {
        active = &m_SecondaryKeyboardGrabActive;
    }
    if (active == nullptr) {
        return;
    }

    const bool wasActive = *active;
#if SDL_VERSION_ATLEAST(2, 0, 15)
    // Reapply on focus gain because some SDL platform adapters ignore a grab
    // requested while the stream window is still hidden.
    SDL_SetWindowKeyboardGrab(window, shouldGrab ? SDL_TRUE : SDL_FALSE);
#else
    shouldGrab = false;
#endif
    *active = shouldGrab;
#ifdef Q_OS_WIN
    if (m_WindowsKeyboardHook != nullptr) {
        m_WindowsKeyboardHook->update(
                m_Runtime != nullptr ? m_Runtime->streamWindow() : nullptr,
                m_PrimaryKeyboardGrabActive,
                m_SecondaryWindow,
                m_SecondaryKeyboardGrabActive);
    }
#endif
    if (wasActive == shouldGrab) {
        return;
    }
    qInfo().nospace()
            << "Apple Screen Sharing system-key capture "
            << (shouldGrab ? "enabled" : "disabled")
            << " for " << (isPrimary ? "primary" : "secondary")
            << " window";
}
