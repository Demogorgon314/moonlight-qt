#pragma once

#include "applemediaprotocol.h"

#include <functional>
#include <memory>
#include <string>

struct SDL_Window;
class AppleLocalFileDragLifecycle;

enum class AppleMacClipboardCommand
{
    ToggleSharing,
    Receive,
    Send,
};

struct AppleMacKeyEvent
{
    enum class Type { Down, Up, Modifier };

    Type type = Type::Down;
    unsigned short keyCode = 0;
    unsigned short keyboardType = 0;
    bool modifierDown = false;
    bool shiftDown = false;
    bool controlDown = false;
    bool optionDown = false;
    bool commandDown = false;
    bool isRepeat = false;
    bool controlEventObserved = false;
    bool optionEventObserved = false;
    bool commandEventObserved = false;
    std::u32string characters;
    std::u32string charactersIgnoringModifiers;
};

struct AppleMacPointerEvent
{
    enum class Type { Motion, ButtonDown, ButtonUp, Scroll };

    Type type = Type::Motion;
    int x = 0;
    int y = 0;
    int clickCount = 0;
    unsigned char buttonNumber = 0;
    int deltaX = 0;
    int deltaY = 0;
    double preciseDeltaX = 0.0;
    double preciseDeltaY = 0.0;
    bool scrollingDirectionInverted = false;
    bool hasNativeScrollEvent = false;
    AppleScrollWheelEvent nativeScrollEvent;
};

#ifdef Q_OS_DARWIN
AppleScrollWheelEvent appleMacScrollWheelEventFromCGEvent(
        const void* cgEvent);
#endif

// Owns the macOS input seam for an SDL stream window. The adapter attaches
// directly to SDL's existing NSView so AppKit remains the sole event owner.
class AppleMacInputBridge
{
public:
    using KeyCallback = std::function<void(const AppleMacKeyEvent& event)>;
    using PointerCallback =
            std::function<void(const AppleMacPointerEvent& event)>;
    using RemoteDragCallback =
            std::function<bool(const void* nativeEvent,
                               bool pointerInsideView)>;
    using CloseCallback = std::function<void()>;
    using ClipboardCommandCallback =
            std::function<void(AppleMacClipboardCommand command)>;

    AppleMacInputBridge(SDL_Window* window,
                        KeyCallback keyCallback,
                        PointerCallback pointerCallback,
                        RemoteDragCallback remoteDragCallback,
                        CloseCallback closeCallback,
                        std::shared_ptr<AppleLocalFileDragLifecycle>
                                localFileDragLifecycle = {},
                        int displayIndex = 0,
                        ClipboardCommandCallback clipboardCommandCallback = {});
    ~AppleMacInputBridge();

    AppleMacInputBridge(const AppleMacInputBridge&) = delete;
    AppleMacInputBridge& operator=(const AppleMacInputBridge&) = delete;

    bool isValid() const;
    // Releases every modifier still held by AppKit and forgets their state.
    // Safe to call again when a delayed flagsChanged event arrives.
    void releasePressedModifiers();
    void repostRemoteDragEvent();
    void updateClipboardMenuState(bool clipboardSupported,
                                  bool sharedClipboardSupported,
                                  bool sharingEnabled,
                                  bool controlling);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
