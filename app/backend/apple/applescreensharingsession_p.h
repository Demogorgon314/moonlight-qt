#pragma once

#include "applevideodecoder.h"

#include "SDL.h"

#include <QString>
#include <QVector>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include "SDL_syswm.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>

namespace AppleScreenSharingSessionPrivate {

inline constexpr qint64 PerformanceReportIntervalMs = 1000;
inline constexpr int RealtimeMediaPollTimeoutMs = 1;
inline constexpr int MaximumReconnectAttempts = 3;

inline QString appleVideoDecoderBackendName(AppleVideoDecoderBackend backend)
{
    switch (backend) {
    case AppleVideoDecoderBackend::D3D11va:
        return QStringLiteral("D3D11VA");
    case AppleVideoDecoderBackend::VideoToolbox:
        return QStringLiteral("VideoToolbox");
    case AppleVideoDecoderBackend::Software:
    default:
        return QStringLiteral("software");
    }
}

struct IntervalStatistics
{
    double average = 0.0;
    double percentile95 = 0.0;
    double jitter = 0.0;
};

inline IntervalStatistics calculateIntervalStatistics(
        const QVector<double>& intervals)
{
    IntervalStatistics statistics;
    if (intervals.isEmpty()) {
        return statistics;
    }

    QVector<double> sorted = intervals;
    double sum = 0.0;
    for (double interval : sorted) {
        sum += interval;
    }
    statistics.average = sum / sorted.size();
    std::sort(sorted.begin(), sorted.end());
    const int percentileIndex = qBound(
            0,
            static_cast<int>(std::ceil(sorted.size() * 0.95)) - 1,
            sorted.size() - 1);
    statistics.percentile95 = sorted.at(percentileIndex);

    double squaredDeviation = 0.0;
    for (double interval : intervals) {
        const double deviation = interval - statistics.average;
        squaredDeviation += deviation * deviation;
    }
    statistics.jitter = std::sqrt(squaredDeviation / intervals.size());
    return statistics;
}

inline quint64 steadyNanoseconds()
{
    return static_cast<quint64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline double cursorDpiScale(SDL_Window* window)
{
#ifdef Q_OS_WIN
    if (window != nullptr) {
        SDL_SysWMinfo windowInfo = {};
        SDL_VERSION(&windowInfo.version);
        if (SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
                windowInfo.subsystem == SDL_SYSWM_WINDOWS &&
                windowInfo.info.win.window != nullptr) {
            const UINT dpi = GetDpiForWindow(windowInfo.info.win.window);
            if (dpi > 0) {
                return static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI;
            }
        }
    }
#elif defined(Q_OS_DARWIN)
    // Cocoa SDL window sizes and cursor surfaces are already expressed in
    // logical points. Applying the Metal drawable scale here halves dynamic
    // resolution requests and doubles the visible cursor on Retina displays.
    Q_UNUSED(window)
    return 1.0;
#else
    Q_UNUSED(window)
#endif
    return 1.0;
}

#ifdef Q_OS_WIN
inline HWND nativeHandleForWindow(SDL_Window* window)
{
    if (window == nullptr) {
        return nullptr;
    }
    SDL_SysWMinfo windowInfo = {};
    SDL_VERSION(&windowInfo.version);
    if (SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
            windowInfo.subsystem == SDL_SYSWM_WINDOWS &&
            windowInfo.info.win.window != nullptr) {
        return windowInfo.info.win.window;
    }
    return nullptr;
}

inline bool nativeHandleMatchesWindow(SDL_Window* window, HWND nativeHandle)
{
    return nativeHandle != nullptr &&
            nativeHandleForWindow(window) == nativeHandle;
}
#endif

} // namespace AppleScreenSharingSessionPrivate
