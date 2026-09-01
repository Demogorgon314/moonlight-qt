#include "applekeyboardmapper.h"

#include "SDL.h"

namespace {

constexpr quint32 X11Backspace = 0xff08;
constexpr quint32 X11Tab = 0xff09;
constexpr quint32 X11Return = 0xff0d;
constexpr quint32 X11Pause = 0xff13;
constexpr quint32 X11ScrollLock = 0xff14;
constexpr quint32 X11Escape = 0xff1b;
constexpr quint32 X11Home = 0xff50;
constexpr quint32 X11Left = 0xff51;
constexpr quint32 X11Up = 0xff52;
constexpr quint32 X11Right = 0xff53;
constexpr quint32 X11Down = 0xff54;
constexpr quint32 X11PageUp = 0xff55;
constexpr quint32 X11PageDown = 0xff56;
constexpr quint32 X11End = 0xff57;
constexpr quint32 X11Print = 0xff61;
constexpr quint32 X11Insert = 0xff63;
constexpr quint32 X11Menu = 0xff67;
constexpr quint32 X11NumLock = 0xff7f;
constexpr quint32 X11KpEnter = 0xff8d;
constexpr quint32 X11KpMultiply = 0xffaa;
constexpr quint32 X11KpAdd = 0xffab;
constexpr quint32 X11KpSeparator = 0xffac;
constexpr quint32 X11KpSubtract = 0xffad;
constexpr quint32 X11KpDecimal = 0xffae;
constexpr quint32 X11KpDivide = 0xffaf;
constexpr quint32 X11Kp0 = 0xffb0;
constexpr quint32 X11KpEqual = 0xffbd;
constexpr quint32 X11F1 = 0xffbe;
constexpr quint32 X11Delete = 0xffff;
constexpr quint32 X11ShiftLeft = 0xffe1;
constexpr quint32 X11ShiftRight = 0xffe2;
constexpr quint32 X11ControlLeft = 0xffe3;
constexpr quint32 X11ControlRight = 0xffe4;
constexpr quint32 X11CapsLock = 0xffe5;
constexpr quint32 X11OptionLeft = 0xffe9;
constexpr quint32 X11OptionRight = 0xffea;
constexpr quint32 X11CommandLeft = 0xffeb;
constexpr quint32 X11CommandRight = 0xffec;

bool isSystemScancode(int scancode)
{
    return scancode == SDL_SCANCODE_LGUI ||
            scancode == SDL_SCANCODE_RGUI;
}

int keyIdentity(int keycode, int scancode)
{
    // SDL normally supplies a non-zero physical scancode. Retain usable state
    // for synthesized events too, without making all UNKNOWN keys collide.
    return scancode != SDL_SCANCODE_UNKNOWN
            ? scancode : -(keycode + 1);
}

quint32 keySymbolForSdl(int keycode, int scancode, bool swapAltAndCommand)
{
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) {
        return X11F1 + static_cast<quint32>(scancode - SDL_SCANCODE_F1);
    }
    if (scancode >= SDL_SCANCODE_F13 && scancode <= SDL_SCANCODE_F24) {
        return X11F1 + 12 +
                static_cast<quint32>(scancode - SDL_SCANCODE_F13);
    }
    switch (scancode) {
    case SDL_SCANCODE_LSHIFT: return X11ShiftLeft;
    case SDL_SCANCODE_RSHIFT: return X11ShiftRight;
    case SDL_SCANCODE_LCTRL: return X11ControlLeft;
    case SDL_SCANCODE_RCTRL: return X11ControlRight;
    case SDL_SCANCODE_CAPSLOCK: return X11CapsLock;
    case SDL_SCANCODE_LALT:
        return swapAltAndCommand ? X11CommandLeft : X11OptionLeft;
    case SDL_SCANCODE_RALT:
        return swapAltAndCommand ? X11CommandRight : X11OptionRight;
    case SDL_SCANCODE_LGUI:
        return swapAltAndCommand ? X11OptionLeft : X11CommandLeft;
    case SDL_SCANCODE_RGUI:
        return swapAltAndCommand ? X11OptionRight : X11CommandRight;
    default:
        break;
    }

    switch (keycode) {
    case SDLK_BACKSPACE: return X11Backspace;
    case SDLK_TAB: return X11Tab;
    case SDLK_RETURN:
    case SDLK_RETURN2: return X11Return;
    case SDLK_ESCAPE: return X11Escape;
    case SDLK_PAUSE: return X11Pause;
    case SDLK_SCROLLLOCK: return X11ScrollLock;
    case SDLK_PRINTSCREEN: return X11Print;
    case SDLK_HOME: return X11Home;
    case SDLK_LEFT: return X11Left;
    case SDLK_UP: return X11Up;
    case SDLK_RIGHT: return X11Right;
    case SDLK_DOWN: return X11Down;
    case SDLK_PAGEUP: return X11PageUp;
    case SDLK_PAGEDOWN: return X11PageDown;
    case SDLK_END: return X11End;
    case SDLK_INSERT: return X11Insert;
    case SDLK_DELETE: return X11Delete;
    case SDLK_APPLICATION: return X11Menu;
    case SDLK_NUMLOCKCLEAR: return X11NumLock;
    case SDLK_KP_ENTER: return X11KpEnter;
    case SDLK_KP_MULTIPLY: return X11KpMultiply;
    case SDLK_KP_PLUS: return X11KpAdd;
    case SDLK_KP_COMMA: return X11KpSeparator;
    case SDLK_KP_MINUS: return X11KpSubtract;
    case SDLK_KP_PERIOD: return X11KpDecimal;
    case SDLK_KP_DIVIDE: return X11KpDivide;
    case SDLK_KP_0: return X11Kp0;
    case SDLK_KP_1: return X11Kp0 + 1;
    case SDLK_KP_2: return X11Kp0 + 2;
    case SDLK_KP_3: return X11Kp0 + 3;
    case SDLK_KP_4: return X11Kp0 + 4;
    case SDLK_KP_5: return X11Kp0 + 5;
    case SDLK_KP_6: return X11Kp0 + 6;
    case SDLK_KP_7: return X11Kp0 + 7;
    case SDLK_KP_8: return X11Kp0 + 8;
    case SDLK_KP_9: return X11Kp0 + 9;
    case SDLK_KP_EQUALS: return X11KpEqual;
    default:
        // SDL keycodes for printable layout-dependent keys are Unicode scalar
        // values. Apple Screen Sharing accepts the same scalar as its keysym.
        if (keycode >= 0x20 && keycode <= 0x10ffff) {
            return static_cast<quint32>(keycode);
        }
        return 0;
    }
}

quint16 appleVirtualKeyCodeForSdl(int scancode, bool swapAltAndCommand)
{
    // Apple virtual key codes are semantic for modifiers. Swapping Alt and Win
    // therefore swaps both the keysym and this key code, rather than preserving
    // the local physical scan position.
    switch (scancode) {
    case SDL_SCANCODE_A: return 0;
    case SDL_SCANCODE_S: return 1;
    case SDL_SCANCODE_D: return 2;
    case SDL_SCANCODE_F: return 3;
    case SDL_SCANCODE_H: return 4;
    case SDL_SCANCODE_G: return 5;
    case SDL_SCANCODE_Z: return 6;
    case SDL_SCANCODE_X: return 7;
    case SDL_SCANCODE_C: return 8;
    case SDL_SCANCODE_V: return 9;
    case SDL_SCANCODE_NONUSBACKSLASH: return 10;
    case SDL_SCANCODE_B: return 11;
    case SDL_SCANCODE_Q: return 12;
    case SDL_SCANCODE_W: return 13;
    case SDL_SCANCODE_E: return 14;
    case SDL_SCANCODE_R: return 15;
    case SDL_SCANCODE_Y: return 16;
    case SDL_SCANCODE_T: return 17;
    case SDL_SCANCODE_1: return 18;
    case SDL_SCANCODE_2: return 19;
    case SDL_SCANCODE_3: return 20;
    case SDL_SCANCODE_4: return 21;
    case SDL_SCANCODE_6: return 22;
    case SDL_SCANCODE_5: return 23;
    case SDL_SCANCODE_EQUALS: return 24;
    case SDL_SCANCODE_9: return 25;
    case SDL_SCANCODE_7: return 26;
    case SDL_SCANCODE_MINUS: return 27;
    case SDL_SCANCODE_8: return 28;
    case SDL_SCANCODE_0: return 29;
    case SDL_SCANCODE_RIGHTBRACKET: return 30;
    case SDL_SCANCODE_O: return 31;
    case SDL_SCANCODE_U: return 32;
    case SDL_SCANCODE_LEFTBRACKET: return 33;
    case SDL_SCANCODE_I: return 34;
    case SDL_SCANCODE_P: return 35;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_RETURN2: return 36;
    case SDL_SCANCODE_L: return 37;
    case SDL_SCANCODE_J: return 38;
    case SDL_SCANCODE_APOSTROPHE: return 39;
    case SDL_SCANCODE_K: return 40;
    case SDL_SCANCODE_SEMICOLON: return 41;
    case SDL_SCANCODE_BACKSLASH:
    case SDL_SCANCODE_NONUSHASH: return 42;
    case SDL_SCANCODE_COMMA: return 43;
    case SDL_SCANCODE_SLASH: return 44;
    case SDL_SCANCODE_N: return 45;
    case SDL_SCANCODE_M: return 46;
    case SDL_SCANCODE_PERIOD: return 47;
    case SDL_SCANCODE_TAB: return 48;
    case SDL_SCANCODE_SPACE: return 49;
    case SDL_SCANCODE_GRAVE: return 50;
    case SDL_SCANCODE_BACKSPACE: return 51;
    case SDL_SCANCODE_ESCAPE: return 53;
    case SDL_SCANCODE_RGUI: return swapAltAndCommand ? 61 : 54;
    case SDL_SCANCODE_LGUI: return swapAltAndCommand ? 58 : 55;
    case SDL_SCANCODE_LSHIFT: return 56;
    case SDL_SCANCODE_CAPSLOCK: return 57;
    case SDL_SCANCODE_LALT: return swapAltAndCommand ? 55 : 58;
    case SDL_SCANCODE_LCTRL: return 59;
    case SDL_SCANCODE_RSHIFT: return 60;
    case SDL_SCANCODE_RALT: return swapAltAndCommand ? 54 : 61;
    case SDL_SCANCODE_RCTRL: return 62;
    case SDL_SCANCODE_F17: return 64;
    case SDL_SCANCODE_KP_PERIOD: return 65;
    case SDL_SCANCODE_KP_MULTIPLY: return 67;
    case SDL_SCANCODE_KP_PLUS: return 69;
    case SDL_SCANCODE_NUMLOCKCLEAR:
    case SDL_SCANCODE_CLEAR: return 71;
    case SDL_SCANCODE_VOLUMEUP: return 72;
    case SDL_SCANCODE_VOLUMEDOWN: return 73;
    case SDL_SCANCODE_MUTE: return 74;
    case SDL_SCANCODE_KP_DIVIDE: return 75;
    case SDL_SCANCODE_KP_ENTER: return 76;
    case SDL_SCANCODE_KP_MINUS: return 78;
    case SDL_SCANCODE_F18: return 79;
    case SDL_SCANCODE_F19: return 80;
    case SDL_SCANCODE_KP_EQUALS: return 81;
    case SDL_SCANCODE_KP_0: return 82;
    case SDL_SCANCODE_KP_1: return 83;
    case SDL_SCANCODE_KP_2: return 84;
    case SDL_SCANCODE_KP_3: return 85;
    case SDL_SCANCODE_KP_4: return 86;
    case SDL_SCANCODE_KP_5: return 87;
    case SDL_SCANCODE_KP_6: return 88;
    case SDL_SCANCODE_KP_7: return 89;
    case SDL_SCANCODE_F20: return 90;
    case SDL_SCANCODE_KP_8: return 91;
    case SDL_SCANCODE_KP_9: return 92;
    case SDL_SCANCODE_F5: return 96;
    case SDL_SCANCODE_F6: return 97;
    case SDL_SCANCODE_F7: return 98;
    case SDL_SCANCODE_F3: return 99;
    case SDL_SCANCODE_F8: return 100;
    case SDL_SCANCODE_F9: return 101;
    case SDL_SCANCODE_F11: return 103;
    case SDL_SCANCODE_F13: return 105;
    case SDL_SCANCODE_F16: return 106;
    case SDL_SCANCODE_F14: return 107;
    case SDL_SCANCODE_F10: return 109;
    case SDL_SCANCODE_F12: return 111;
    case SDL_SCANCODE_F15: return 113;
    case SDL_SCANCODE_INSERT:
    case SDL_SCANCODE_HELP: return 114;
    case SDL_SCANCODE_HOME: return 115;
    case SDL_SCANCODE_PAGEUP: return 116;
    case SDL_SCANCODE_DELETE: return 117;
    case SDL_SCANCODE_F4: return 118;
    case SDL_SCANCODE_END: return 119;
    case SDL_SCANCODE_F2: return 120;
    case SDL_SCANCODE_PAGEDOWN: return 121;
    case SDL_SCANCODE_F1: return 122;
    case SDL_SCANCODE_LEFT: return 123;
    case SDL_SCANCODE_RIGHT: return 124;
    case SDL_SCANCODE_DOWN: return 125;
    case SDL_SCANCODE_UP: return 126;
    default: return 0;
    }
}

} // namespace

AppleKeyboardMapper::AppleKeyboardMapper(bool swapAltAndCommand,
                                         quint16 keyboardType)
    : m_SwapAltAndCommand(swapAltAndCommand),
      m_KeyboardType(keyboardType)
{
}

std::optional<AppleRemoteKeyEvent> AppleKeyboardMapper::update(
        bool isDown,
        int sdlKeycode,
        int sdlScancode,
        bool systemKeyCaptureRequested)
{
    const int identity = keyIdentity(sdlKeycode, sdlScancode);
    const auto pressed = m_PressedKeys.constFind(identity);
    if (!isDown && pressed != m_PressedKeys.cend()) {
        const PressedKey key = pressed.value();
        m_PressedKeys.remove(identity);
        m_PressOrder.removeAll(identity);
        if (sdlScancode == m_SynthesizedGuiScancode) {
            m_SynthesizedGuiScancode = SDL_SCANCODE_UNKNOWN;
        }
        return AppleRemoteKeyEvent{
            false, key.symbol, m_KeyboardType, key.keyCode,
        };
    }

    if (!isDown || pressed != m_PressedKeys.cend() ||
            (isSystemScancode(sdlScancode) &&
             !systemKeyCaptureRequested)) {
        return std::nullopt;
    }

    const quint32 symbol = keySymbolForSdl(
            sdlKeycode, sdlScancode, m_SwapAltAndCommand);
    if (symbol == 0) {
        return std::nullopt;
    }
    const quint16 keyCode = appleVirtualKeyCodeForSdl(
            sdlScancode, m_SwapAltAndCommand);
    m_PressedKeys.insert(identity, PressedKey{symbol, keyCode});
    m_PressOrder.append(identity);
    return AppleRemoteKeyEvent{
        true, symbol, m_KeyboardType, keyCode,
    };
}

QList<AppleRemoteKeyEvent> AppleKeyboardMapper::updateWithModifiers(
        bool isDown,
        int sdlKeycode,
        int sdlScancode,
        int sdlModifiers,
        int platformGuiModifiers,
        bool systemKeyCaptureRequested)
{
    QList<AppleRemoteKeyEvent> events;

    const bool isGuiKey = sdlScancode == SDL_SCANCODE_LGUI ||
            sdlScancode == SDL_SCANCODE_RGUI;
    if (!isGuiKey) {
        const int effectiveGuiModifiers =
                (sdlModifiers | platformGuiModifiers) & KMOD_GUI;
        const bool guiIsDown = effectiveGuiModifiers != 0;
        const bool guiIsTracked =
                m_PressedKeys.contains(keyIdentity(
                        SDLK_LGUI, SDL_SCANCODE_LGUI)) ||
                m_PressedKeys.contains(keyIdentity(
                        SDLK_RGUI, SDL_SCANCODE_RGUI));

        // On Windows, system-key capture can preserve KMOD_GUI on the
        // following character while SDL never delivers the Win key itself.
        // Apple expects the modifier as its own input record, matching the
        // native client's flagsChanged event before keyDown.
        if (guiIsDown && !guiIsTracked && systemKeyCaptureRequested) {
            const bool rightOnly =
                    (effectiveGuiModifiers & KMOD_RGUI) != 0 &&
                    (effectiveGuiModifiers & KMOD_LGUI) == 0;
            const int guiKeycode = rightOnly ? SDLK_RGUI : SDLK_LGUI;
            const int guiScancode = rightOnly
                    ? SDL_SCANCODE_RGUI : SDL_SCANCODE_LGUI;
            const std::optional<AppleRemoteKeyEvent> command = update(
                    true, guiKeycode, guiScancode, true);
            if (command.has_value()) {
                events.append(*command);
                m_SynthesizedGuiScancode = guiScancode;
            }
        }
        else if (!guiIsDown &&
                 m_SynthesizedGuiScancode != SDL_SCANCODE_UNKNOWN) {
            const int guiScancode = m_SynthesizedGuiScancode;
            const int guiKeycode = guiScancode == SDL_SCANCODE_RGUI
                    ? SDLK_RGUI : SDLK_LGUI;
            const std::optional<AppleRemoteKeyEvent> command = update(
                    false, guiKeycode, guiScancode, true);
            if (command.has_value()) {
                events.append(*command);
            }
            m_SynthesizedGuiScancode = SDL_SCANCODE_UNKNOWN;
        }
    }

    const std::optional<AppleRemoteKeyEvent> key = update(
            isDown, sdlKeycode, sdlScancode,
            systemKeyCaptureRequested);
    if (key.has_value()) {
        events.append(*key);
    }
    return events;
}

QList<AppleRemoteKeyEvent> AppleKeyboardMapper::releaseAll()
{
    QList<AppleRemoteKeyEvent> releases;
    releases.reserve(m_PressOrder.size());
    for (auto it = m_PressOrder.crbegin(); it != m_PressOrder.crend(); ++it) {
        const auto pressed = m_PressedKeys.constFind(*it);
        if (pressed == m_PressedKeys.cend()) {
            continue;
        }
        releases.append(AppleRemoteKeyEvent{
            false,
            pressed->symbol,
            m_KeyboardType,
            pressed->keyCode,
        });
    }
    m_PressedKeys.clear();
    m_PressOrder.clear();
    m_SynthesizedGuiScancode = SDL_SCANCODE_UNKNOWN;
    return releases;
}

int AppleKeyboardMapper::pressedKeyCount() const
{
    return m_PressedKeys.size();
}
