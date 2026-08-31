#include "localstreamruntime.h"

#include "SDL_compat.h"
#include "settings/streamingpreferences.h"
#include "streamutils.h"
#include "audio/renderers/renderer.h"

#include <QScreen>

LocalStreamRuntime::~LocalStreamRuntime()
{
    clearPcmOutput();
    clearInputContext();
    shutdown();
}

bool LocalStreamRuntime::initialize(QQuickWindow* window,
                                    const LocalStreamRuntimeConfig& config)
{
    if (m_VideoInitialized.load()) {
        return false;
    }

    m_QtWindow = window;
    if (m_QtWindow != nullptr && m_QtWindow->screen() != nullptr) {
        m_ClientDisplayName = m_QtWindow->screen()->name();
    }

    SDL_SetHint(SDL_HINT_TRACKPAD_IS_TOUCH_ONLY,
                config.nativeTouchpadEnabled ? "1" : "0");

#ifdef Q_OS_DARWIN
    if (qEnvironmentVariableIntValue("I_WANT_BUGGY_FULLSCREEN") == 0) {
        bool shouldUseFullScreenSpaces =
                config.windowMode != StreamingPreferences::WM_FULLSCREEN;
        SDL_DisplayMode desktopMode;
        SDL_Rect safeArea;
        for (int displayIndex = 0;
             StreamUtils::getNativeDesktopMode(displayIndex, &desktopMode, &safeArea);
             displayIndex++) {
            if (desktopMode.h == safeArea.h && desktopMode.w == safeArea.w) {
                continue;
            }

            if (config.streamWidth == desktopMode.w && config.streamHeight == desktopMode.h) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Overriding default fullscreen mode for native fullscreen resolution");
                shouldUseFullScreenSpaces = false;
                break;
            }
            if (config.streamWidth == safeArea.w && config.streamHeight == safeArea.h) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Overriding default fullscreen mode for native safe area resolution");
                shouldUseFullScreenSpaces = true;
                break;
            }
        }
        SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES,
                    shouldUseFullScreenSpaces ? "1" : "0");
    }
#endif

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s",
                     SDL_GetError());
        return false;
    }

    m_VideoInitialized.store(true);
    SDL_StopTextInput();
    return true;
}

SDL_Window* LocalStreamRuntime::createStreamWindow(const QString& title,
                                                   int x,
                                                   int y,
                                                   int width,
                                                   int height,
                                                   quint32 defaultFlags)
{
    if (!m_VideoInitialized.load() || m_StreamWindow != nullptr) {
        return nullptr;
    }

    const QByteArray titleUtf8 = title.toUtf8();
    m_StreamWindow = SDL_CreateWindow(titleUtf8.constData(),
                                      x,
                                      y,
                                      width,
                                      height,
                                      static_cast<Uint32>(defaultFlags) |
                                              StreamUtils::getPlatformWindowFlags());
    if (m_StreamWindow == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_CreateWindow() failed with platform flags: %s",
                    SDL_GetError());
        m_StreamWindow = SDL_CreateWindow(titleUtf8.constData(),
                                          x,
                                          y,
                                          width,
                                          height,
                                          static_cast<Uint32>(defaultFlags));
    }

    if (m_StreamWindow == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_CreateWindow() failed: %s",
                     SDL_GetError());
    }
    return m_StreamWindow;
}

void LocalStreamRuntime::destroyStreamWindow()
{
    if (m_StreamWindow != nullptr) {
        SDL_DestroyWindow(m_StreamWindow);
        m_StreamWindow = nullptr;
    }
}

LocalInputContext* LocalStreamRuntime::setInputContext(
        std::unique_ptr<LocalInputContext> inputContext)
{
    Q_ASSERT(m_InputContext == nullptr);
    m_InputContext = std::move(inputContext);
    return m_InputContext.get();
}

void LocalStreamRuntime::clearInputContext()
{
    m_InputContext.reset();
}

IAudioRenderer* LocalStreamRuntime::setPcmOutput(
        std::unique_ptr<IAudioRenderer> pcmOutput)
{
    Q_ASSERT(m_PcmOutput == nullptr);
    m_PcmOutput = std::move(pcmOutput);
    return m_PcmOutput.get();
}

void LocalStreamRuntime::clearPcmOutput()
{
    m_PcmOutput.reset();
}

void LocalStreamRuntime::requestStop()
{
    if (!m_VideoInitialized.load()) {
        return;
    }

    SDL_Event event = {};
    event.type = SDL_QUIT;
    event.quit.timestamp = SDL_GetTicks();
    SDL_PushEvent(&event);
}

void LocalStreamRuntime::shutdown()
{
    destroyStreamWindow();
    if (m_VideoInitialized.exchange(false)) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}
