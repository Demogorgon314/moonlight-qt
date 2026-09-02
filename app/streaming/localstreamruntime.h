#pragma once

#include <QPointer>
#include <QQuickWindow>
#include <QString>
#include <QtGlobal>

#include <atomic>
#include <memory>

struct SDL_Window;
class IAudioRenderer;

class LocalInputContext
{
public:
    virtual ~LocalInputContext() = default;
};

struct LocalStreamRuntimeConfig
{
    int streamWidth = 0;
    int streamHeight = 0;
    bool useFullScreenSpaces = true;
    bool nativeTouchpadEnabled = false;
};

class LocalStreamRuntime
{
public:
    LocalStreamRuntime();
    ~LocalStreamRuntime();

    LocalStreamRuntime(const LocalStreamRuntime&) = delete;
    LocalStreamRuntime& operator=(const LocalStreamRuntime&) = delete;

    bool initialize(QQuickWindow* window, const LocalStreamRuntimeConfig& config);
    SDL_Window* createStreamWindow(const QString& title,
                                   int x,
                                   int y,
                                   int width,
                                   int height,
                                   quint32 defaultFlags);
    void destroyStreamWindow();

    LocalInputContext* setInputContext(std::unique_ptr<LocalInputContext> inputContext);
    void clearInputContext();
    IAudioRenderer* setPcmOutput(std::unique_ptr<IAudioRenderer> pcmOutput);
    void clearPcmOutput();

    void requestStop();
    void shutdown();

    QQuickWindow* qtWindow() const { return m_QtWindow.data(); }
    SDL_Window* streamWindow() const { return m_StreamWindow; }
    const QString& clientDisplayName() const { return m_ClientDisplayName; }
    bool isInitialized() const { return m_VideoInitialized.load(); }

private:
    QPointer<QQuickWindow> m_QtWindow;
    SDL_Window* m_StreamWindow = nullptr;
    std::unique_ptr<LocalInputContext> m_InputContext;
    std::unique_ptr<IAudioRenderer> m_PcmOutput;
    QString m_ClientDisplayName;
    std::atomic_bool m_VideoInitialized{false};
};
