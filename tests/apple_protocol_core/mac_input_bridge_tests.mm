#include "backend/apple/applemacinputbridge.h"
#include "backend/apple/applefiledrag.h"

#import <AppKit/AppKit.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include <memory>
#include <vector>

@interface MoonlightTestDraggingInfo : NSObject

- (instancetype)initWithPasteboard:(NSPasteboard*)pasteboard
                           location:(NSPoint)location
                     sequenceNumber:(NSInteger)sequenceNumber;
- (NSPasteboard*)draggingPasteboard;
- (NSPoint)draggingLocation;
- (NSInteger)draggingSequenceNumber;
- (void)setDraggingLocation:(NSPoint)location;

@end

@implementation MoonlightTestDraggingInfo {
    NSPasteboard* _pasteboard;
    NSPoint _location;
    NSInteger _sequenceNumber;
}

- (instancetype)initWithPasteboard:(NSPasteboard*)pasteboard
                           location:(NSPoint)location
                     sequenceNumber:(NSInteger)sequenceNumber
{
    self = [super init];
    if (self != nil) {
        _pasteboard = [pasteboard retain];
        _location = location;
        _sequenceNumber = sequenceNumber;
    }
    return self;
}

- (void)dealloc
{
    [_pasteboard release];
    [super dealloc];
}

- (NSPasteboard*)draggingPasteboard { return _pasteboard; }
- (NSPoint)draggingLocation { return _location; }
- (NSInteger)draggingSequenceNumber { return _sequenceNumber; }
- (void)setDraggingLocation:(NSPoint)location { _location = location; }

@end

bool testAppleMacZoomButtonUsesNativeFullscreen()
{
    @autoreleasepool {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            return false;
        }
        SDL_Window* window = SDL_CreateWindow(
                "Apple macOS input bridge test",
                SDL_WINDOWPOS_UNDEFINED,
                SDL_WINDOWPOS_UNDEFINED,
                640,
                360,
                SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_METAL);
        if (window == nullptr) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }

        SDL_SysWMinfo windowInfo = {};
        SDL_VERSION(&windowInfo.version);
        if (!SDL_GetWindowWMInfo(window, &windowInfo) ||
                windowInfo.subsystem != SDL_SYSWM_COCOA ||
                windowInfo.info.cocoa.window == nil) {
            SDL_DestroyWindow(window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }
        NSButton* zoomButton = [windowInfo.info.cocoa.window
                standardWindowButton:NSWindowZoomButton];
        if (zoomButton == nil) {
            SDL_DestroyWindow(window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }
        id const originalTarget = zoomButton.target;
        SEL const originalAction = zoomButton.action;
        bool bridgeValid = false;
        bool usesNativeFullscreen = false;
        {
            AppleMacInputBridge bridge(
                    window,
                    {},
                    {},
                    {},
                    {});
            bridgeValid = bridge.isValid();
            usesNativeFullscreen =
                    zoomButton.target == windowInfo.info.cocoa.window &&
                    zoomButton.action == @selector(toggleFullScreen:);
        }

        const bool restored = zoomButton.target == originalTarget &&
                zoomButton.action == originalAction;
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return bridgeValid && usesNativeFullscreen && restored;
    }
}

bool testAppleMacInputBridgeRoutesRemoteDragBeforePointer()
{
    @autoreleasepool {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return false;
        SDL_Window* window = SDL_CreateWindow(
                "Apple macOS remote drag routing test",
                SDL_WINDOWPOS_UNDEFINED,
                SDL_WINDOWPOS_UNDEFINED,
                640,
                360,
                SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_METAL);
        if (window == nullptr) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }
        SDL_SysWMinfo windowInfo = {};
        SDL_VERSION(&windowInfo.version);
        if (!SDL_GetWindowWMInfo(window, &windowInfo) ||
                windowInfo.subsystem != SDL_SYSWM_COCOA ||
                windowInfo.info.cocoa.window == nil) {
            SDL_DestroyWindow(window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }

        int remoteDragCalls = 0;
        int pointerCalls = 0;
        bool outsideReported = false;
        bool bridgeValid = false;
        {
            AppleMacInputBridge bridge(
                    window,
                    {},
                    [&](const AppleMacPointerEvent&) { ++pointerCalls; },
                    [&](const void*, bool pointerInsideView) {
                        ++remoteDragCalls;
                        outsideReported = !pointerInsideView;
                        return !pointerInsideView;
                    },
                    {});
            bridgeValid = bridge.isValid();
            NSWindow* nativeWindow = windowInfo.info.cocoa.window;
            NSEvent* outsideDrag = [NSEvent
                    mouseEventWithType:NSEventTypeLeftMouseDragged
                              location:NSMakePoint(-20.0, -20.0)
                         modifierFlags:0
                             timestamp:0
                          windowNumber:nativeWindow.windowNumber
                               context:nil
                           eventNumber:1
                            clickCount:1
                              pressure:1.0];
            [nativeWindow.contentView mouseDragged:outsideDrag];
        }

        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return bridgeValid && remoteDragCalls == 1 && outsideReported &&
                pointerCalls == 0;
    }
}

bool testAppleMacInputBridgeRoutesLocalFileDrag()
{
    @autoreleasepool {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return false;
        SDL_Window* window = SDL_CreateWindow(
                "Apple macOS local file drag routing test",
                SDL_WINDOWPOS_UNDEFINED,
                SDL_WINDOWPOS_UNDEFINED,
                640,
                360,
                SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_METAL);
        if (window == nullptr) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }
        SDL_SysWMinfo windowInfo = {};
        SDL_VERSION(&windowInfo.version);
        if (!SDL_GetWindowWMInfo(window, &windowInfo) ||
                windowInfo.subsystem != SDL_SYSWM_COCOA ||
                windowInfo.info.cocoa.window == nil) {
            SDL_DestroyWindow(window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }

        QStringList actions;
        auto lifecycle = std::make_shared<AppleLocalFileDragLifecycle>(
                [](const AppleFileDragPoint&) { return true; },
                [&actions](const QStringList& paths) {
                    actions.append(QStringLiteral("announce:%1")
                                           .arg(paths.first()));
                    return true;
                },
                [&actions](const AppleFileDragPoint& point,
                           AppleLocalFileDragPointerAction action) {
                    QString actionName = QStringLiteral("move");
                    if (action == AppleLocalFileDragPointerAction::Press) {
                        actionName = QStringLiteral("down");
                    }
                    else if (action ==
                             AppleLocalFileDragPointerAction::Release) {
                        actionName = QStringLiteral("up");
                    }
                    actions.append(QStringLiteral("pointer:%1,%2:%3")
                                           .arg(point.x)
                                           .arg(point.y)
                                           .arg(actionName));
                },
                [&actions]() { actions.append(QStringLiteral("cancel")); });
        NSPasteboard* pasteboard = [NSPasteboard pasteboardWithUniqueName];
        [pasteboard clearContents];
        NSURL* fileUrl = [NSURL fileURLWithPath:@"/tmp/local-drag.txt"];
        [pasteboard writeObjects:@[fileUrl]];
        MoonlightTestDraggingInfo* draggingInfo =
                [[MoonlightTestDraggingInfo alloc]
                        initWithPasteboard:pasteboard
                                   location:NSMakePoint(100.0, 120.0)
                             sequenceNumber:41];

        bool bridgeValid = false;
        NSDragOperation entered = NSDragOperationNone;
        NSDragOperation updated = NSDragOperationNone;
        bool prepared = false;
        bool dropped = false;
        {
            AppleMacInputBridge bridge(
                    window, {}, {}, {}, {}, lifecycle, 1);
            bridgeValid = bridge.isValid();
            NSView* view = windowInfo.info.cocoa.window.contentView;
            entered = [view draggingEntered:
                    (id<NSDraggingInfo>)draggingInfo];
            [draggingInfo setDraggingLocation:NSMakePoint(140.0, 100.0)];
            updated = [view draggingUpdated:
                    (id<NSDraggingInfo>)draggingInfo];
            prepared = [view prepareForDragOperation:
                    (id<NSDraggingInfo>)draggingInfo];
            dropped = [view performDragOperation:
                    (id<NSDraggingInfo>)draggingInfo];
        }

        [draggingInfo release];
        [pasteboard releaseGlobally];
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return bridgeValid && entered == NSDragOperationCopy &&
                updated == NSDragOperationCopy && prepared && dropped &&
                actions == QStringList{
                        QStringLiteral("announce:/tmp/local-drag.txt"),
                        QStringLiteral("pointer:100,240:down"),
                        QStringLiteral("pointer:140,260:move"),
                        QStringLiteral("pointer:140,260:up")};
    }
}

bool testAppleMacInputBridgeReleasesModifiersOnFocusLoss()
{
    @autoreleasepool {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return false;
        SDL_Window* window = SDL_CreateWindow(
                "Apple macOS modifier release test",
                SDL_WINDOWPOS_UNDEFINED,
                SDL_WINDOWPOS_UNDEFINED,
                640,
                360,
                SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_METAL);
        if (window == nullptr) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }
        SDL_SysWMinfo windowInfo = {};
        SDL_VERSION(&windowInfo.version);
        if (!SDL_GetWindowWMInfo(window, &windowInfo) ||
                windowInfo.subsystem != SDL_SYSWM_COCOA ||
                windowInfo.info.cocoa.window == nil) {
            SDL_DestroyWindow(window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }

        std::vector<AppleMacKeyEvent> events;
        bool bridgeValid = false;
        {
            AppleMacInputBridge bridge(
                    window,
                    [&events](const AppleMacKeyEvent& event) {
                        events.push_back(event);
                    },
                    {},
                    {},
                    {});
            bridgeValid = bridge.isValid();
            NSWindow* nativeWindow = windowInfo.info.cocoa.window;
            NSView* view = nativeWindow.contentView;
            NSEvent* shiftDown = [NSEvent
                    keyEventWithType:NSEventTypeFlagsChanged
                             location:NSZeroPoint
                        modifierFlags:NSEventModifierFlagShift
                            timestamp:0
                         windowNumber:nativeWindow.windowNumber
                              context:nil
                           characters:@""
          charactersIgnoringModifiers:@""
                            isARepeat:NO
                              keyCode:56];
            [view flagsChanged:shiftDown];
            NSEvent* shiftUp = [NSEvent
                    keyEventWithType:NSEventTypeFlagsChanged
                             location:NSZeroPoint
                        modifierFlags:0
                            timestamp:0
                         windowNumber:nativeWindow.windowNumber
                              context:nil
                           characters:@""
          charactersIgnoringModifiers:@""
                            isARepeat:NO
                              keyCode:56];
            [view flagsChanged:shiftUp];

            [view flagsChanged:shiftDown];
            [view resignFirstResponder];
            bridge.releasePressedModifiers();
            [view flagsChanged:shiftUp];
        }

        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return bridgeValid && events.size() == 4 &&
                events[0].type == AppleMacKeyEvent::Type::Modifier &&
                events[0].modifierDown && events[0].keyCode == 56 &&
                events[1].type == AppleMacKeyEvent::Type::Modifier &&
                !events[1].modifierDown && events[1].keyCode == 56 &&
                events[2].type == AppleMacKeyEvent::Type::Modifier &&
                events[2].modifierDown && events[2].keyCode == 56 &&
                events[3].type == AppleMacKeyEvent::Type::Modifier &&
                !events[3].modifierDown && events[3].keyCode == 56;
    }
}
