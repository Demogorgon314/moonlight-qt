#include "applemacinputbridge.h"
#include "applefiledrag.h"

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
                 AppleMacInputBridge::RemoteDragCallback remoteDrag,
                 AppleMacInputBridge::CloseCallback close,
                 std::shared_ptr<AppleLocalFileDragLifecycle> localFileDrag,
                 int localFileDragDisplayIndex)
        : keyCallback(std::move(key)),
          pointerCallback(std::move(pointer)),
          remoteDragCallback(std::move(remoteDrag)),
          closeCallback(std::move(close)),
          localFileDragLifecycle(std::move(localFileDrag)),
          displayIndex(localFileDragDisplayIndex)
    {
    }

    ~InputContext()
    {
        if (localFileDragLifecycle != nullptr) {
            localFileDragLifecycle->cancel();
        }
        [lastLeftDragEvent release];
        lastLeftDragEvent = nil;
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

    bool beginRemoteDrag(NSView* view, NSEvent* event)
    {
        if (view == nil || event == nil ||
                event.type != NSEventTypeLeftMouseDragged) {
            return false;
        }
        [lastLeftDragEvent release];
        lastLeftDragEvent = [event retain];
        if (!remoteDragCallback) return false;
        const NSPoint point = [view convertPoint:event.locationInWindow
                                        fromView:nil];
        const bool began = remoteDragCallback(
                event, NSPointInRect(point, view.visibleRect));
        if (began) {
            [lastLeftDragEvent release];
            lastLeftDragEvent = nil;
        }
        return began;
    }

    void clearLeftDragEvent()
    {
        [lastLeftDragEvent release];
        lastLeftDragEvent = nil;
    }

    void repostRemoteDragEvent()
    {
        NSEvent* event = NSApp.currentEvent;
        if (event.type != NSEventTypeLeftMouseDragged) {
            event = lastLeftDragEvent;
        }
        if (event != nil) [NSApp postEvent:event atStart:NO];
    }

    bool enterLocalFileDrag(NSView* view, id<NSDraggingInfo> sender)
    {
        if (localFileDragLifecycle == nullptr || view == nil || sender == nil) {
            return false;
        }
        const QStringList paths = localFilePaths(sender.draggingPasteboard);
        if (paths.isEmpty()) {
            return false;
        }
        const NSInteger sequenceNumber = sender.draggingSequenceNumber;
        const quintptr identity = sequenceNumber == 0
                ? 1 : static_cast<quintptr>(sequenceNumber);
        return localFileDragLifecycle->enter(
                identity, paths, localFileDragPoint(view, sender));
    }

    bool moveLocalFileDrag(NSView* view, id<NSDraggingInfo> sender)
    {
        return localFileDragLifecycle != nullptr && view != nil &&
                sender != nil && localFileDragLifecycle->move(
                        localFileDragPoint(view, sender));
    }

    void leaveLocalFileDrag()
    {
        if (localFileDragLifecycle != nullptr) {
            localFileDragLifecycle->leave();
        }
    }

    bool dropLocalFileDrag(NSView* view, id<NSDraggingInfo> sender)
    {
        return localFileDragLifecycle != nullptr && view != nil &&
                sender != nil && localFileDragLifecycle->drop(
                        localFileDragPoint(view, sender));
    }

    void endLocalFileDrag()
    {
        if (localFileDragLifecycle != nullptr) {
            localFileDragLifecycle->cancel();
        }
    }

    bool hasActiveLocalFileDrag() const
    {
        return localFileDragLifecycle != nullptr &&
                localFileDragLifecycle->isActive();
    }

    AppleMacInputBridge::KeyCallback keyCallback;
    AppleMacInputBridge::PointerCallback pointerCallback;
    AppleMacInputBridge::RemoteDragCallback remoteDragCallback;
    AppleMacInputBridge::CloseCallback closeCallback;
    std::shared_ptr<AppleLocalFileDragLifecycle> localFileDragLifecycle;
    int displayIndex = 0;
    std::unordered_set<unsigned short> activeModifierKeyCodes;
    NSEvent* lastLeftDragEvent = nil;

private:
    static QStringList localFilePaths(NSPasteboard* pasteboard)
    {
        if (pasteboard == nil) {
            return {};
        }
        NSArray* urls = [pasteboard
                readObjectsForClasses:@[[NSURL class]]
                              options:@{
                                  NSPasteboardURLReadingFileURLsOnlyKey: @YES,
                              }];
        QStringList paths;
        for (NSURL* url in urls) {
            if (url.isFileURL && url.path != nil) {
                paths.append(QString::fromUtf8(url.path.UTF8String));
            }
        }
        return paths;
    }

    AppleFileDragPoint localFileDragPoint(
            NSView* view,
            id<NSDraggingInfo> sender) const
    {
        const NSPoint point = [view convertPoint:sender.draggingLocation
                                         fromView:nil];
        return {
            static_cast<int>(point.x),
            static_cast<int>(view.bounds.size.height - point.y),
            displayIndex,
        };
    }
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

void bridgeMouseDragged(id view, SEL, NSEvent* event)
{
    if (InputContext* context = inputContext(view)) {
        NSView* inputView = static_cast<NSView*>(view);
        if (context->beginRemoteDrag(inputView, event)) return;
        context->sendPointer(
                inputView, event, AppleMacPointerEvent::Type::Motion);
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
        if (event.buttonNumber == 0) context->clearLeftDragEvent();
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

NSDragOperation bridgeDraggingEntered(
        id view, SEL, id<NSDraggingInfo> sender)
{
    InputContext* context = inputContext(view);
    return context != nullptr && context->enterLocalFileDrag(view, sender)
            ? NSDragOperationCopy : NSDragOperationNone;
}

NSDragOperation bridgeDraggingUpdated(
        id view, SEL, id<NSDraggingInfo> sender)
{
    InputContext* context = inputContext(view);
    return context != nullptr && context->moveLocalFileDrag(view, sender)
            ? NSDragOperationCopy : NSDragOperationNone;
}

void bridgeDraggingExited(id view, SEL, id<NSDraggingInfo>)
{
    if (InputContext* context = inputContext(view)) {
        context->leaveLocalFileDrag();
    }
}

BOOL bridgePrepareForDragOperation(id view, SEL, id<NSDraggingInfo>)
{
    InputContext* context = inputContext(view);
    return context != nullptr && context->hasActiveLocalFileDrag();
}

BOOL bridgePerformDragOperation(
        id view, SEL, id<NSDraggingInfo> sender)
{
    InputContext* context = inputContext(view);
    return context != nullptr && context->dropLocalFileDrag(view, sender);
}

void bridgeDraggingEnded(id view, SEL, id<NSDraggingInfo>)
{
    if (InputContext* context = inputContext(view)) {
        context->endLocalFileDrag();
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
                         reinterpret_cast<IMP>(bridgeMouseDragged)) ||
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
            !addOverride(subclass, original, @selector(draggingEntered:),
                         reinterpret_cast<IMP>(bridgeDraggingEntered)) ||
            !addOverride(subclass, original, @selector(draggingUpdated:),
                         reinterpret_cast<IMP>(bridgeDraggingUpdated)) ||
            !addOverride(subclass, original, @selector(draggingExited:),
                         reinterpret_cast<IMP>(bridgeDraggingExited)) ||
            !addOverride(subclass, original,
                         @selector(prepareForDragOperation:),
                         reinterpret_cast<IMP>(
                                 bridgePrepareForDragOperation)) ||
            !addOverride(subclass, original,
                         @selector(performDragOperation:),
                         reinterpret_cast<IMP>(bridgePerformDragOperation)) ||
            !class_addMethod(
                    subclass,
                    @selector(draggingEnded:),
                    reinterpret_cast<IMP>(bridgeDraggingEnded),
                    "v@:@") ||
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
    NSArray<NSPasteboardType>* originalDraggedTypes = nil;
    NSTrackingArea* trackingArea = nil;
    NSButton* closeButton = nil;
    id originalCloseTarget = nil;
    SEL originalCloseAction = nullptr;
    NSButton* zoomButton = nil;
    id originalZoomTarget = nil;
    SEL originalZoomAction = nullptr;
    Class originalClass = Nil;
    bool valid = false;
};

AppleMacInputBridge::AppleMacInputBridge(
        SDL_Window* window,
        KeyCallback keyCallback,
        PointerCallback pointerCallback,
        RemoteDragCallback remoteDragCallback,
        CloseCallback closeCallback,
        std::shared_ptr<AppleLocalFileDragLifecycle> localFileDragLifecycle,
        int displayIndex)
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
                std::move(remoteDragCallback),
                std::move(closeCallback),
                std::move(localFileDragLifecycle),
                displayIndex);
        d->view = [view retain];
        d->originalClass = originalClass;
        d->originalDraggedTypes = [view.registeredDraggedTypes copy];
        NSMutableArray<NSPasteboardType>* draggedTypes =
                [NSMutableArray arrayWithArray:
                        d->originalDraggedTypes ?: @[]];
        if (![draggedTypes containsObject:NSPasteboardTypeFileURL]) {
            [draggedTypes addObject:NSPasteboardTypeFileURL];
        }
        [view registerForDraggedTypes:draggedTypes];
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
        d->zoomButton = [[view.window
                standardWindowButton:NSWindowZoomButton] retain];
        if (d->zoomButton != nil) {
            d->originalZoomTarget = [d->zoomButton.target retain];
            d->originalZoomAction = d->zoomButton.action;
            // SDL's FULLSCREEN_DESKTOP path explicitly hides the macOS menu
            // bar. Let AppKit own the transition so the menu bar and Dock keep
            // their native edge-reveal behavior, matching the Swift client.
            d->zoomButton.target = view.window;
            d->zoomButton.action = @selector(toggleFullScreen:);
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
            [d->view unregisterDraggedTypes];
            if (d->originalDraggedTypes.count > 0) {
                [d->view registerForDraggedTypes:d->originalDraggedTypes];
            }
            [d->originalDraggedTypes release];
            d->originalDraggedTypes = nil;
            if (d->zoomButton != nil) {
                d->zoomButton.target = d->originalZoomTarget;
                d->zoomButton.action = d->originalZoomAction;
                [d->originalZoomTarget release];
                d->originalZoomTarget = nil;
                [d->zoomButton release];
                d->zoomButton = nil;
            }
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

void AppleMacInputBridge::repostRemoteDragEvent()
{
    @autoreleasepool {
        if (d->context) d->context->repostRemoteDragEvent();
    }
}
