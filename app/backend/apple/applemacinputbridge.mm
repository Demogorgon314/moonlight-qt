#include "applemacinputbridge.h"

#import <AppKit/AppKit.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <QDebug>

#include <SDL.h>
#include <SDL_syswm.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <unordered_set>
#include <utility>

namespace {

const void* InputContextKey = &InputContextKey;

std::u32string unicodeScalars(NSString* string)
{
    std::u32string result;
    if (string == nil) {
        return result;
    }
    for (NSUInteger index = 0; index < string.length; ++index) {
        const unichar first = [string characterAtIndex:index];
        if (CFStringIsSurrogateHighCharacter(first) &&
                index + 1 < string.length) {
            const unichar second = [string characterAtIndex:index + 1];
            if (CFStringIsSurrogateLowCharacter(second)) {
                result.push_back(CFStringGetLongCharacterForSurrogatePair(
                        first, second));
                ++index;
                continue;
            }
        }
        result.push_back(first);
    }
    return result;
}

NSEventModifierFlags modifierFlagForKeyCode(unsigned short keyCode)
{
    switch (keyCode) {
    case 56:
    case 60: return NSEventModifierFlagShift;
    case 59:
    case 62: return NSEventModifierFlagControl;
    case 58:
    case 61: return NSEventModifierFlagOption;
    case 55:
    case 54: return NSEventModifierFlagCommand;
    case 57: return NSEventModifierFlagCapsLock;
    default: return 0;
    }
}

struct InputContext
{
    InputContext(AppleMacInputBridge::KeyCallback key,
                 AppleMacInputBridge::PointerCallback pointer,
                 AppleMacInputBridge::CloseCallback close)
        : keyCallback(std::move(key)),
          pointerCallback(std::move(pointer)),
          closeCallback(std::move(close))
    {
    }

    void sendKey(NSEvent* event)
    {
        if (!keyCallback || event == nil) {
            return;
        }
        AppleMacKeyEvent nativeEvent;
        nativeEvent.keyCode = event.keyCode;
        nativeEvent.keyboardType = static_cast<unsigned short>(
                CGEventGetIntegerValueField(
                        event.CGEvent, kCGKeyboardEventKeyboardType));
        nativeEvent.shiftDown =
                (event.modifierFlags & NSEventModifierFlagShift) != 0;
        nativeEvent.controlDown =
                (event.modifierFlags & NSEventModifierFlagControl) != 0;
        nativeEvent.optionDown =
                (event.modifierFlags & NSEventModifierFlagOption) != 0;
        nativeEvent.commandDown =
                (event.modifierFlags & NSEventModifierFlagCommand) != 0;
        nativeEvent.controlEventObserved =
                activeModifierKeyCodes.count(59) != 0 ||
                activeModifierKeyCodes.count(62) != 0;
        nativeEvent.optionEventObserved =
                activeModifierKeyCodes.count(58) != 0 ||
                activeModifierKeyCodes.count(61) != 0;
        nativeEvent.commandEventObserved =
                activeModifierKeyCodes.count(55) != 0 ||
                activeModifierKeyCodes.count(54) != 0;
        nativeEvent.characters = unicodeScalars(event.characters);
        nativeEvent.charactersIgnoringModifiers =
                unicodeScalars(event.charactersIgnoringModifiers);

        if (event.type == NSEventTypeFlagsChanged) {
            const auto active = activeModifierKeyCodes.find(event.keyCode);
            if (active != activeModifierKeyCodes.end()) {
                nativeEvent.modifierDown = false;
                activeModifierKeyCodes.erase(active);
            }
            else {
                const NSEventModifierFlags flag =
                        modifierFlagForKeyCode(event.keyCode);
                if (flag == 0 || (event.modifierFlags & flag) == 0) {
                    return;
                }
                nativeEvent.modifierDown = true;
                activeModifierKeyCodes.insert(event.keyCode);
            }
            nativeEvent.type = AppleMacKeyEvent::Type::Modifier;
        }
        else {
            nativeEvent.type = event.type == NSEventTypeKeyDown
                    ? AppleMacKeyEvent::Type::Down
                    : AppleMacKeyEvent::Type::Up;
        }
        keyCallback(nativeEvent);
    }

    void sendPointer(NSView* view,
                     NSEvent* event,
                     AppleMacPointerEvent::Type type)
    {
        if (!pointerCallback || view == nil || event == nil) {
            return;
        }
        const NSPoint point = [view convertPoint:event.locationInWindow
                                        fromView:nil];
        const NSRect bounds = view.bounds;
        AppleMacPointerEvent nativeEvent;
        nativeEvent.type = type;
        nativeEvent.x = std::clamp(
                static_cast<int>(point.x),
                0,
                std::max(0, static_cast<int>(bounds.size.width) - 1));
        nativeEvent.y = std::clamp(
                static_cast<int>(bounds.size.height - point.y),
                0,
                std::max(0, static_cast<int>(bounds.size.height) - 1));
        if (type != AppleMacPointerEvent::Type::Scroll) {
            nativeEvent.clickCount = static_cast<int>(event.clickCount);
        }
        if (type == AppleMacPointerEvent::Type::ButtonDown ||
                type == AppleMacPointerEvent::Type::ButtonUp) {
            nativeEvent.buttonNumber =
                    static_cast<unsigned char>(event.buttonNumber);
        }
        if (type == AppleMacPointerEvent::Type::Scroll) {
            nativeEvent.deltaX = static_cast<int>(event.deltaX);
            nativeEvent.deltaY = static_cast<int>(event.deltaY);
            nativeEvent.preciseDeltaX = event.hasPreciseScrollingDeltas
                    ? event.scrollingDeltaX : event.deltaX;
            nativeEvent.preciseDeltaY = event.hasPreciseScrollingDeltas
                    ? event.scrollingDeltaY : event.deltaY;
            nativeEvent.scrollingDirectionInverted =
                    event.isDirectionInvertedFromDevice;
            if (event.CGEvent != nullptr) {
                nativeEvent.hasNativeScrollEvent = true;
                nativeEvent.nativeScrollEvent =
                        appleMacScrollWheelEventFromCGEvent(event.CGEvent);
            }
        }
        pointerCallback(nativeEvent);
    }

    AppleMacInputBridge::KeyCallback keyCallback;
    AppleMacInputBridge::PointerCallback pointerCallback;
    AppleMacInputBridge::CloseCallback closeCallback;
    std::unordered_set<unsigned short> activeModifierKeyCodes;
};

InputContext* inputContext(id view)
{
    NSValue* value = objc_getAssociatedObject(view, InputContextKey);
    return value != nil
            ? static_cast<InputContext*>(value.pointerValue)
            : nullptr;
}

void bridgeKeyDown(id view, SEL, NSEvent* event)
{
    if (InputContext* context = inputContext(view)) {
        context->sendKey(event);
    }
}

void bridgeKeyUp(id view, SEL, NSEvent* event)
{
    if (InputContext* context = inputContext(view)) {
        context->sendKey(event);
    }
}

void bridgeFlagsChanged(id view, SEL, NSEvent* event)
{
    if (InputContext* context = inputContext(view)) {
        context->sendKey(event);
    }
}

BOOL bridgePerformKeyEquivalent(id view, SEL, NSEvent* event)
{
    InputContext* context = inputContext(view);
    if (context != nullptr && event.type == NSEventTypeKeyDown) {
        context->sendKey(event);
        return YES;
    }
    return NO;
}

BOOL bridgeAcceptsFirstResponder(id, SEL)
{
    return YES;
}

BOOL bridgeAcceptsFirstMouse(id, SEL, NSEvent*)
{
    return YES;
}

void bridgeMouseMoved(id view, SEL, NSEvent* event)
{
    if (InputContext* context = inputContext(view)) {
        context->sendPointer(
                view, event, AppleMacPointerEvent::Type::Motion);
    }
}

void bridgeMouseDown(id view, SEL, NSEvent* event)
{
    NSView* inputView = static_cast<NSView*>(view);
    [inputView.window makeFirstResponder:inputView];
    if (InputContext* context = inputContext(view)) {
        context->sendPointer(
                view, event, AppleMacPointerEvent::Type::ButtonDown);
    }
}

void bridgeMouseUp(id view, SEL, NSEvent* event)
{
    if (InputContext* context = inputContext(view)) {
        context->sendPointer(
                view, event, AppleMacPointerEvent::Type::ButtonUp);
    }
}

void bridgeScrollWheel(id view, SEL, NSEvent* event)
{
    if (InputContext* context = inputContext(view)) {
        context->sendPointer(
                view, event, AppleMacPointerEvent::Type::Scroll);
    }
}

void bridgeCloseWindow(id view, SEL, id)
{
    if (InputContext* context = inputContext(view);
            context != nullptr && context->closeCallback) {
        context->closeCallback();
    }
}

bool addOverride(Class subclass, Class original, SEL selector, IMP function)
{
    Method method = class_getInstanceMethod(original, selector);
    return method != nullptr && class_addMethod(
            subclass, selector, function, method_getTypeEncoding(method));
}

Class inputSubclassForClass(Class original)
{
    std::string name = "MoonlightAppleMacInput_";
    for (const char* character = class_getName(original);
         character != nullptr && *character != '\0'; ++character) {
        name.push_back(std::isalnum(static_cast<unsigned char>(*character))
                ? *character : '_');
    }
    if (Class existing = objc_getClass(name.c_str())) {
        return class_getSuperclass(existing) == original ? existing : Nil;
    }
    Class subclass = objc_allocateClassPair(original, name.c_str(), 0);
    if (subclass == Nil ||
            !addOverride(subclass, original, @selector(keyDown:),
                         reinterpret_cast<IMP>(bridgeKeyDown)) ||
            !addOverride(subclass, original, @selector(keyUp:),
                         reinterpret_cast<IMP>(bridgeKeyUp)) ||
            !addOverride(subclass, original, @selector(flagsChanged:),
                         reinterpret_cast<IMP>(bridgeFlagsChanged)) ||
            !addOverride(subclass, original, @selector(performKeyEquivalent:),
                         reinterpret_cast<IMP>(bridgePerformKeyEquivalent)) ||
            !addOverride(subclass, original, @selector(acceptsFirstResponder),
                         reinterpret_cast<IMP>(bridgeAcceptsFirstResponder)) ||
            !addOverride(subclass, original, @selector(acceptsFirstMouse:),
                         reinterpret_cast<IMP>(bridgeAcceptsFirstMouse)) ||
            !addOverride(subclass, original, @selector(mouseMoved:),
                         reinterpret_cast<IMP>(bridgeMouseMoved)) ||
            !addOverride(subclass, original, @selector(mouseDragged:),
                         reinterpret_cast<IMP>(bridgeMouseMoved)) ||
            !addOverride(subclass, original, @selector(rightMouseDragged:),
                         reinterpret_cast<IMP>(bridgeMouseMoved)) ||
            !addOverride(subclass, original, @selector(otherMouseDragged:),
                         reinterpret_cast<IMP>(bridgeMouseMoved)) ||
            !addOverride(subclass, original, @selector(mouseDown:),
                         reinterpret_cast<IMP>(bridgeMouseDown)) ||
            !addOverride(subclass, original, @selector(rightMouseDown:),
                         reinterpret_cast<IMP>(bridgeMouseDown)) ||
            !addOverride(subclass, original, @selector(otherMouseDown:),
                         reinterpret_cast<IMP>(bridgeMouseDown)) ||
            !addOverride(subclass, original, @selector(mouseUp:),
                         reinterpret_cast<IMP>(bridgeMouseUp)) ||
            !addOverride(subclass, original, @selector(rightMouseUp:),
                         reinterpret_cast<IMP>(bridgeMouseUp)) ||
            !addOverride(subclass, original, @selector(otherMouseUp:),
                         reinterpret_cast<IMP>(bridgeMouseUp)) ||
            !addOverride(subclass, original, @selector(scrollWheel:),
                         reinterpret_cast<IMP>(bridgeScrollWheel)) ||
            !class_addMethod(
                    subclass,
                    sel_registerName("moonlightCloseWindow:"),
                    reinterpret_cast<IMP>(bridgeCloseWindow),
                    "v@:@")) {
        if (subclass != Nil) {
            objc_disposeClassPair(subclass);
        }
        return Nil;
    }
    objc_registerClassPair(subclass);
    return subclass;
}

NSView* nativeViewForSdlWindow(SDL_Window* window)
{
    if (window == nullptr) {
        return nil;
    }
    SDL_SysWMinfo windowInfo = {};
    SDL_VERSION(&windowInfo.version);
    if (!SDL_GetWindowWMInfo(window, &windowInfo) ||
            windowInfo.subsystem != SDL_SYSWM_COCOA ||
            windowInfo.info.cocoa.window == nil) {
        return nil;
    }
    return windowInfo.info.cocoa.window.contentView;
}

} // namespace

AppleScrollWheelEvent appleMacScrollWheelEventFromCGEvent(
        const void* opaqueCgEvent)
{
    AppleScrollWheelEvent event;
    if (opaqueCgEvent == nullptr) {
        return event;
    }
    const CGEventRef cgEvent = reinterpret_cast<CGEventRef>(
            const_cast<void*>(opaqueCgEvent));
    const auto field = [cgEvent](CGEventField name) {
        return CGEventGetIntegerValueField(cgEvent, name);
    };
    const auto clampInt16 = [](int64_t value) {
        return static_cast<qint16>(std::clamp<int64_t>(
                value,
                std::numeric_limits<qint16>::min(),
                std::numeric_limits<qint16>::max()));
    };
    const auto clampInt32 = [](int64_t value) {
        return static_cast<qint32>(std::clamp<int64_t>(
                value,
                std::numeric_limits<qint32>::min(),
                std::numeric_limits<qint32>::max()));
    };

    // These fields intentionally mirror RemoteVideoCanvasNSView in the Swift
    // client. macOS already supplies natural direction, acceleration, gesture
    // phase, and momentum; reconstructing them from NSEvent deltas loses data.
    event.deltaX = clampInt16(field(kCGScrollWheelEventDeltaAxis2));
    event.deltaY = clampInt16(field(kCGScrollWheelEventDeltaAxis1));
    event.deltaZ = clampInt16(field(kCGScrollWheelEventDeltaAxis3));
    event.fixedDeltaX = clampInt32(
            field(kCGScrollWheelEventFixedPtDeltaAxis2));
    event.fixedDeltaY = clampInt32(
            field(kCGScrollWheelEventFixedPtDeltaAxis1));
    event.fixedDeltaZ = clampInt32(
            field(kCGScrollWheelEventFixedPtDeltaAxis3));
    event.pointDeltaX = clampInt32(
            field(kCGScrollWheelEventPointDeltaAxis2));
    event.pointDeltaY = clampInt32(
            field(kCGScrollWheelEventPointDeltaAxis1));
    event.pointDeltaZ = clampInt32(
            field(kCGScrollWheelEventPointDeltaAxis3));
    event.scrollPhase = static_cast<quint32>(
            field(kCGScrollWheelEventScrollPhase));
    event.momentumPhase = static_cast<quint32>(
            field(kCGScrollWheelEventMomentumPhase));
    event.scrollCount = static_cast<quint32>(
            field(kCGScrollWheelEventScrollCount));
    event.flags = static_cast<quint32>(CGEventGetFlags(cgEvent));
    return event;
}

struct AppleMacInputBridge::Private
{
    std::unique_ptr<InputContext> context;
    NSView* view = nil;
    NSTrackingArea* trackingArea = nil;
    NSButton* closeButton = nil;
    id originalCloseTarget = nil;
    SEL originalCloseAction = nullptr;
    Class originalClass = Nil;
    bool valid = false;
};

AppleMacInputBridge::AppleMacInputBridge(
        SDL_Window* window,
        KeyCallback keyCallback,
        PointerCallback pointerCallback,
        CloseCallback closeCallback)
    : d(std::make_unique<Private>())
{
    @autoreleasepool {
        NSView* view = nativeViewForSdlWindow(window);
        if (view == nil) {
            qWarning() << "Apple macOS input adapter could not find the SDL content view";
            return;
        }
        Class originalClass = object_getClass(view);
        Class inputClass = inputSubclassForClass(originalClass);
        if (inputClass == Nil) {
            qWarning() << "Apple macOS input adapter could not create its NSView class";
            return;
        }

        d->context = std::make_unique<InputContext>(
                std::move(keyCallback),
                std::move(pointerCallback),
                std::move(closeCallback));
        d->view = [view retain];
        d->originalClass = originalClass;
        objc_setAssociatedObject(
                view,
                InputContextKey,
                [NSValue valueWithPointer:d->context.get()],
                OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        object_setClass(view, inputClass);
        d->trackingArea = [[NSTrackingArea alloc]
                initWithRect:NSZeroRect
                     options:NSTrackingActiveInKeyWindow |
                             NSTrackingInVisibleRect |
                             NSTrackingMouseMoved |
                             NSTrackingEnabledDuringMouseDrag
                       owner:view
                    userInfo:nil];
        [view addTrackingArea:d->trackingArea];
        d->closeButton = [[view.window
                standardWindowButton:NSWindowCloseButton] retain];
        if (d->closeButton != nil) {
            d->originalCloseTarget = [d->closeButton.target retain];
            d->originalCloseAction = d->closeButton.action;
            d->closeButton.target = view;
            d->closeButton.action =
                    sel_registerName("moonlightCloseWindow:");
        }
        [view.window setAcceptsMouseMovedEvents:YES];
        [view.window makeFirstResponder:view];
        d->valid = true;
    }
}

AppleMacInputBridge::~AppleMacInputBridge()
{
    @autoreleasepool {
        if (d->view != nil) {
            if (d->closeButton != nil) {
                d->closeButton.target = d->originalCloseTarget;
                d->closeButton.action = d->originalCloseAction;
                [d->originalCloseTarget release];
                d->originalCloseTarget = nil;
                [d->closeButton release];
                d->closeButton = nil;
            }
            if (d->trackingArea != nil) {
                [d->view removeTrackingArea:d->trackingArea];
                [d->trackingArea release];
                d->trackingArea = nil;
            }
            if (d->originalClass != Nil) {
                object_setClass(d->view, d->originalClass);
            }
            objc_setAssociatedObject(
                    d->view, InputContextKey, nil,
                    OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [d->view release];
            d->view = nil;
        }
        d->context.reset();
        d->valid = false;
    }
}

bool AppleMacInputBridge::isValid() const
{
    return d->valid;
}
