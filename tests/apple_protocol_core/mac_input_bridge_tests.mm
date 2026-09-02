#include "backend/apple/applemacinputbridge.h"

#import <AppKit/AppKit.h>

#include <SDL.h>
#include <SDL_syswm.h>

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
