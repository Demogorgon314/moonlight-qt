#pragma once

#ifdef Q_OS_WIN

#include "applescreensharingsession_p.h"

#include "SDL.h"

#include <QDebug>

#include <qt_windows.h>

// SDL's Windows system-key grab is implemented with WH_KEYBOARD_LL. With the
// SDL3 backend used through sdl2-compat, the hook can suppress LWIN/RWIN before
// their SDL keyboard events reach this session. Install after SDL updates its
// grab so this session can forward Command explicitly while still preventing
// the local Start menu from opening.
class AppleWindowsKeyboardHook
{
public:
    AppleWindowsKeyboardHook()
        : m_EventType(SDL_RegisterEvents(1))
    {
    }

    ~AppleWindowsKeyboardHook()
    {
        stop();
    }

    void update(SDL_Window* primary,
                bool primaryCapture,
                SDL_Window* secondary,
                bool secondaryCapture)
    {
        stop();
        m_Primary = targetForWindow(primary, primaryCapture);
        m_Secondary = targetForWindow(secondary, secondaryCapture);
        if (m_EventType == InvalidEventType ||
                (!m_Primary.capture && !m_Secondary.capture)) {
            return;
        }

        s_ActiveHook = this;
        m_Hook = SetWindowsHookExW(
                WH_KEYBOARD_LL,
                &AppleWindowsKeyboardHook::keyboardHookProc,
                GetModuleHandleW(nullptr),
                0);
        if (m_Hook == nullptr) {
            s_ActiveHook = nullptr;
            qWarning().nospace()
                    << "[DEBUG-APPLE-WIN-HOOK] installation failed error="
                    << GetLastError();
            return;
        }
        qInfo().nospace()
                << "[DEBUG-APPLE-WIN-HOOK] installed primary="
                << m_Primary.windowId << "/" << m_Primary.capture
                << " secondary=" << m_Secondary.windowId
                << "/" << m_Secondary.capture;
    }

    bool decodeEvent(const SDL_Event& event,
                     bool* isDown,
                     bool* isRight,
                     quint32* windowId) const
    {
        if (event.type != m_EventType || isDown == nullptr ||
                isRight == nullptr || windowId == nullptr) {
            return false;
        }
        *isDown = (event.user.code & DownFlag) != 0;
        *isRight = (event.user.code & RightFlag) != 0;
        *windowId = event.user.windowID;
        return true;
    }

private:
    struct WindowTarget
    {
        HWND nativeHandle = nullptr;
        quint32 windowId = 0;
        bool capture = false;
    };

    static constexpr Uint32 InvalidEventType = static_cast<Uint32>(-1);
    static constexpr Sint32 DownFlag = 1 << 0;
    static constexpr Sint32 RightFlag = 1 << 1;

    static WindowTarget targetForWindow(SDL_Window* window, bool capture)
    {
        WindowTarget target;
        target.nativeHandle =
                AppleScreenSharingSessionPrivate::nativeHandleForWindow(window);
        target.windowId = window != nullptr ? SDL_GetWindowID(window) : 0;
        target.capture = capture && target.nativeHandle != nullptr &&
                target.windowId != 0;
        return target;
    }

    const WindowTarget* capturedTarget(HWND foreground) const
    {
        if (m_Primary.capture && m_Primary.nativeHandle == foreground) {
            return &m_Primary;
        }
        if (m_Secondary.capture && m_Secondary.nativeHandle == foreground) {
            return &m_Secondary;
        }
        return nullptr;
    }

    static LRESULT CALLBACK keyboardHookProc(
            int code, WPARAM message, LPARAM data)
    {
        AppleWindowsKeyboardHook* hook = s_ActiveHook;
        if (code == HC_ACTION && hook != nullptr) {
            const auto* keyboard =
                    reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            const bool isWin = keyboard->vkCode == VK_LWIN ||
                    keyboard->vkCode == VK_RWIN;
            const bool isDown = message == WM_KEYDOWN ||
                    message == WM_SYSKEYDOWN;
            const bool isUp = message == WM_KEYUP ||
                    message == WM_SYSKEYUP;
            const WindowTarget* target =
                    hook->capturedTarget(GetForegroundWindow());
            if (isWin && (isDown || isUp) && target != nullptr) {
                SDL_Event event = {};
                event.type = hook->m_EventType;
                event.user.timestamp = SDL_GetTicks();
                event.user.windowID = target->windowId;
                event.user.code = (isDown ? DownFlag : 0) |
                        (keyboard->vkCode == VK_RWIN ? RightFlag : 0);
                if (SDL_PushEvent(&event) == 1) {
                    return 1;
                }
            }
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    void stop()
    {
        if (m_Hook != nullptr) {
            UnhookWindowsHookEx(m_Hook);
            m_Hook = nullptr;
        }
        if (s_ActiveHook == this) {
            s_ActiveHook = nullptr;
        }
    }

    inline static AppleWindowsKeyboardHook* s_ActiveHook = nullptr;
    Uint32 m_EventType = InvalidEventType;
    HHOOK m_Hook = nullptr;
    WindowTarget m_Primary;
    WindowTarget m_Secondary;
};
#endif
