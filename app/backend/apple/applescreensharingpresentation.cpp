#include "applescreensharingsession.h"
#include "applescreensharingsession_p.h"

#include <QElapsedTimer>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
#include <QMutexLocker>
#include <QPainter>
#include <QStringList>

#include "SDL.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

using AppleScreenSharingSessionPrivate::calculateIntervalStatistics;
using AppleScreenSharingSessionPrivate::IntervalStatistics;
using AppleScreenSharingSessionPrivate::PerformanceReportIntervalMs;
using AppleScreenSharingSessionPrivate::steadyNanoseconds;

namespace {

constexpr int PerformanceOverlayLineCount = 9;
constexpr int PerformanceOverlayReservedCharacters = 96;

QImage renderMoonlightPerformanceOverlay(
        const QList<ApplePerformanceOverlayTextRun>& runs,
        int outputWidth)
{
    if (runs.isEmpty() || outputWidth <= 0) {
        return {};
    }

    static const QString moonlightFontFamily = []() {
        const int fontId = QFontDatabase::addApplicationFont(
                QStringLiteral(":/data/ModeSeven.ttf"));
        const QStringList families =
                QFontDatabase::applicationFontFamilies(fontId);
        return families.isEmpty()
                ? QStringLiteral("Consolas") : families.first();
    }();
    constexpr int padding = 4;
    const int availableWidth = qMax(1, outputWidth - padding * 2);

    const auto fontForRun = [](
            const ApplePerformanceOverlayTextRun& run,
            double scale) {
        QFont font(moonlightFontFamily);
        font.setStyleHint(QFont::Monospace);
        font.setPixelSize(qMax(8, qRound(run.pixelSize * scale)));
        font.setBold(run.bold);
        return font;
    };
    const auto measure = [&runs, &fontForRun](
            double scale, int* width, int* ascent, int* descent) {
        *width = 0;
        *ascent = 0;
        *descent = 0;
        for (const ApplePerformanceOverlayTextRun& run : runs) {
            const QFontMetrics metrics(fontForRun(run, scale));
            *width += metrics.horizontalAdvance(run.text);
            *ascent = qMax(*ascent, metrics.ascent());
            *descent = qMax(*descent, metrics.descent());
        }
    };

    int contentWidth = 0;
    int maximumAscent = 0;
    int maximumDescent = 0;
    measure(1.0, &contentWidth, &maximumAscent, &maximumDescent);
    double scale = contentWidth > availableWidth
            ? static_cast<double>(availableWidth) / contentWidth
            : 1.0;
    measure(scale, &contentWidth, &maximumAscent, &maximumDescent);
    while (contentWidth > availableWidth && scale > 0.45) {
        scale *= 0.97;
        measure(scale, &contentWidth, &maximumAscent, &maximumDescent);
    }

    QImage image(qMin(outputWidth, contentWidth + padding * 2),
                 maximumAscent + maximumDescent + padding * 2,
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(0, 0, 0, 102));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(QColor(189, 249, 231));
    int x = padding;
    const int baseline = padding + maximumAscent;
    for (const ApplePerformanceOverlayTextRun& run : runs) {
        const QFont font = fontForRun(run, scale);
        painter.setFont(font);
        painter.drawText(x, baseline, run.text);
        x += QFontMetrics(font).horizontalAdvance(run.text);
    }
    painter.end();
    return image;
}

} // namespace

void AppleScreenSharingSession::queueDecodedFrames(QList<AppleDecodedTile> frames,
                                                   int displayIndex)
{
    QMutexLocker locker(&m_FrameMutex);
    bool acceptedBatch = false;
    for (AppleDecodedTile& frame : frames) {
        if (frame.isValid()) {
            if (displayIndex == 1) {
                m_SecondaryLatestFrames.insert(
                        frame.tileIndex, std::move(frame));
            }
            else {
                m_LatestFrames.insert(frame.tileIndex, std::move(frame));
            }
            acceptedBatch = true;
        }
    }
    if (acceptedBatch) {
        if (displayIndex == 1) {
            ++m_SecondaryPendingFrameBatches;
            m_SecondaryPresentationNeeded.store(true);
        }
        else {
            ++m_PendingFrameBatches;
        }
    }
}


void AppleScreenSharingSession::applyCanvas(const AppleCanvas& canvas)
{
    if (!canvas.isUsable()) {
        return;
    }
    {
        QMutexLocker locker(&m_FrameMutex);
        m_Canvas = canvas;
        m_LatestFrames.clear();
        m_TileHeights.clear();
        m_PendingFrameBatches = 0;
        m_AwaitingPresentationBatches = 0;
        m_AwaitingDecodeSubmissions.clear();
    }
    {
        QMutexLocker locker(&m_PerformanceMutex);
        m_PerformanceMetrics.canvasSize = QSize(canvas.width, canvas.height);
    }
    requestPerformanceOverlayUpdate();
    m_PresentationNeeded.store(true);
    qInfo().nospace() << "Apple Screen Sharing canvas="
                      << canvas.width << "x" << canvas.height
                      << " tiles=" << canvas.tileCount;
}

void AppleScreenSharingSession::updatePerformanceStatistics(
        const QString& summary,
        const ApplePerformanceOverlayMetrics& metrics)
{
    {
        QMutexLocker locker(&m_PerformanceMutex);
        m_PerformanceMediaSummary = summary;
        m_PerformanceMetrics.canvasSize = metrics.canvasSize;
        m_PerformanceMetrics.receivedFramesPerSecond =
                metrics.receivedFramesPerSecond;
        m_PerformanceMetrics.decodedFramesPerSecond =
                metrics.decodedFramesPerSecond;
        m_PerformanceMetrics.networkMegabitsPerSecond =
                metrics.networkMegabitsPerSecond;
        m_PerformanceMetrics.decodeMilliseconds =
                metrics.decodeMilliseconds;
        m_PerformanceMetrics.decoderBackend = metrics.decoderBackend;
        m_PerformanceMetrics.hasMediaSample = metrics.hasMediaSample;
    }
    requestPerformanceOverlayUpdate();
}

void AppleScreenSharingSession::updateSecondaryPerformanceStatistics(
        const QString& summary)
{
    {
        QMutexLocker locker(&m_PerformanceMutex);
        m_SecondaryPerformanceSummary = summary;
    }
    requestPerformanceOverlayUpdate();
}

void AppleScreenSharingSession::updateAudioStatistics(const QString& summary)
{
    {
        QMutexLocker locker(&m_PerformanceMutex);
        m_AudioSummary = summary;
    }
    requestPerformanceOverlayUpdate();
}

void AppleScreenSharingSession::requestPerformanceOverlayUpdate()
{
    if (!m_PerformanceOverlayVisible.load()) {
        return;
    }
    m_PerformanceOverlayUpdateNeeded.store(true);
    m_PresentationNeeded.store(true);
}

void AppleScreenSharingSession::togglePerformanceOverlay()
{
    const bool visible = !m_PerformanceOverlayVisible.load();
    m_PerformanceOverlayVisible.store(visible);
    // Clearing and texture upload run on the presentation thread together with
    // the native render call, so the stream hotkey cannot race the immediate
    // context from SDL's GUI-thread event pump.
    m_PerformanceOverlayUpdateNeeded.store(true);
    m_PresentationNeeded.store(true);
    qInfo() << "Apple performance overlay"
            << (visible ? "enabled" : "disabled")
            << "for this stream";
}


void AppleScreenSharingSession::updatePerformanceOverlayTexture()
{
    if (m_VideoRenderer == nullptr) {
        return;
    }
    if (!m_PerformanceOverlayVisible.load()) {
        m_VideoRenderer->clearOverlay();
        m_PerformanceOverlaySize = {};
        m_PresentationNeeded.store(true);
        return;
    }
    QElapsedTimer updateTimer;
    updateTimer.start();

    QString mediaSummary;
    QString secondaryMediaSummary;
    QString presentationSummary;
    QString controlSummary;
    QString audioSummary;
    QString fileTransferSummary;
    ApplePerformanceOverlayMetrics performanceMetrics;
    {
        QMutexLocker locker(&m_PerformanceMutex);
        mediaSummary = m_PerformanceMediaSummary;
        secondaryMediaSummary = m_SecondaryPerformanceSummary;
        presentationSummary = m_PerformancePresentationSummary;
        controlSummary = m_ControlSummary;
        audioSummary = m_AudioSummary;
        fileTransferSummary = m_FileTransferSummary;
        performanceMetrics = m_PerformanceMetrics;
    }
    if (!performanceMetrics.canvasSize.isValid()) {
        QMutexLocker locker(&m_FrameMutex);
        performanceMetrics.canvasSize = QSize(m_Canvas.width, m_Canvas.height);
    }

    const bool moonlightStyle = m_PerformanceOverlayStyle ==
            ApplePerformanceOverlayStyle::Moonlight;
    QList<ApplePerformanceOverlayTextRun> moonlightRuns;
    QStringList lines;
    if (moonlightStyle) {
        moonlightRuns = appleMoonlightPerformanceRuns(performanceMetrics);
    }
    else {
        if (!controlSummary.isEmpty()) {
            lines.append(controlSummary);
        }
        if (!mediaSummary.isEmpty()) {
            lines.append(mediaSummary.split('\n', Qt::SkipEmptyParts));
        }
        if (!secondaryMediaSummary.isEmpty()) {
            lines.append(secondaryMediaSummary);
        }
        if (!audioSummary.isEmpty()) {
            lines.append(audioSummary);
        }
        if (!fileTransferSummary.isEmpty()) {
            lines.append(fileTransferSummary);
        }
        if (!presentationSummary.isEmpty()) {
            lines.append(presentationSummary.split(
                    '\n', Qt::SkipEmptyParts));
        }
        if (lines.isEmpty()) {
            lines.append(QStringLiteral(
                    "APPLE HIGH PERFORMANCE   Measuring..."));
        }
        while (lines.size() < PerformanceOverlayLineCount) {
            lines.append(QString());
        }
    }

    int outputWidth = 0;
    int outputHeight = 0;
    if (!m_VideoRenderer->outputSize(&outputWidth, &outputHeight) ||
            outputWidth <= 0 || outputHeight <= 0) {
        return;
    }

    QImage image;
    if (moonlightStyle) {
        image = renderMoonlightPerformanceOverlay(
                moonlightRuns, outputWidth);
    }
    else {
        QFont font(QStringLiteral("Consolas"));
        font.setPixelSize(qBound(14, outputHeight / 54, 24));
        font.setStyleHint(QFont::Monospace);
        const QFontMetrics metrics(font);
        const int horizontalPadding = qMax(14, metrics.height());
        const int verticalPadding = qMax(8, metrics.height() / 2);
        const int lineSpacing = qMax(2, metrics.height() / 6);
        int widestLine = 0;
        for (const QString& line : std::as_const(lines)) {
            widestLine = qMax(widestLine,
                              metrics.horizontalAdvance(line));
        }
        const int reservedContentWidth = metrics.horizontalAdvance(
                QString(PerformanceOverlayReservedCharacters,
                        QLatin1Char('M')));
        const int requestedImageWidth =
                qMax(widestLine, reservedContentWidth) +
                horizontalPadding * 2;
        const int imageWidth = qBound(
                1,
                qMax(requestedImageWidth,
                     m_PerformanceOverlaySize.first),
                qMax(1, outputWidth));
        const int lineCount = lines.size();
        const int imageHeight = qMin(
                outputHeight,
                verticalPadding * 2 + metrics.height() * lineCount +
                        lineSpacing * qMax(0, lineCount - 1));
        image = QImage(imageWidth, imageHeight,
                       QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(7, 11, 16, 218));
        painter.drawRoundedRect(image.rect(), 8, 8);
        painter.setBrush(QColor(0, 231, 196, 255));
        painter.drawRoundedRect(QRect(0, 0, 5, imageHeight), 2, 2);
        painter.setFont(font);
        int baseline = verticalPadding + metrics.ascent();
        for (int index = 0; index < lines.size(); ++index) {
            painter.setPen(index == 0
                                   ? QColor(126, 255, 229)
                                   : QColor(242, 246, 250));
            painter.drawText(horizontalPadding, baseline, lines.at(index));
            baseline += metrics.height() + lineSpacing;
        }
        painter.end();
    }
    if (image.isNull()) {
        return;
    }

    QString overlayError;
    if (!m_VideoRenderer->uploadOverlay(image, &overlayError)) {
        qWarning().nospace()
                << "Apple High Performance overlay upload failed: "
                << overlayError;
    }
    else {
        m_PerformanceOverlaySize = {image.width(), image.height()};
        m_PresentationNeeded.store(true);
    }
    m_MaxOverlayUpdateMilliseconds = qMax(
            m_MaxOverlayUpdateMilliseconds,
            updateTimer.nsecsElapsed() / 1000000.0);
}


void AppleScreenSharingSession::renderLatestFrames()
{
    if (m_VideoRenderer == nullptr) {
        return;
    }
    const quint64 renderLoopAtNanoseconds = steadyNanoseconds();
    if (m_LastRenderLoopAtNanoseconds != 0 &&
            renderLoopAtNanoseconds >= m_LastRenderLoopAtNanoseconds) {
        m_MaxRenderLoopGapMilliseconds = qMax(
                m_MaxRenderLoopGapMilliseconds,
                (renderLoopAtNanoseconds - m_LastRenderLoopAtNanoseconds) /
                        1000000.0);
    }
    m_LastRenderLoopAtNanoseconds = renderLoopAtNanoseconds;
    if (m_PerformanceOverlayUpdateNeeded.exchange(false)) {
        updatePerformanceOverlayTexture();
    }
    QHash<int, AppleDecodedTile> frames;
    quint64 pendingFrameBatches = 0;
    AppleCanvas canvas;
    {
        QMutexLocker locker(&m_FrameMutex);
        canvas = m_Canvas;
        frames = std::move(m_LatestFrames);
        m_LatestFrames.clear();
        pendingFrameBatches = m_PendingFrameBatches;
        m_PendingFrameBatches = 0;
    }
    if (!canvas.isUsable()) {
        return;
    }
    if (pendingFrameBatches > 0) {
        m_AwaitingPresentationBatches += pendingFrameBatches;
    }
    for (auto iterator = frames.begin(); iterator != frames.end(); ++iterator) {
        AppleDecodedTile& frame = iterator.value();
        if (frame.isValid()) {
            QString uploadError;
            if (!m_VideoRenderer ||
                    !m_VideoRenderer->upload(frame, &uploadError)) {
                qWarning().nospace()
                        << "Apple High Performance tile upload failed: "
                        << uploadError;
                continue;
            }
            m_TextureSizes.insert(
                    frame.tileIndex, {frame.width, frame.height});
            m_TextureFormats.insert(
                    frame.tileIndex, static_cast<quint32>(
                            frame.pixelFormat));
            m_TileHeights.insert(frame.tileIndex, frame.height);
            m_AwaitingDecodeSubmissions.insert(
                    frame.tileIndex, frame.decodeSubmittedAtNanoseconds);
            m_PresentationNeeded.store(true);
            continue;
        }
        qWarning().nospace()
                << "Apple High Performance ignored non-4:4:4 decoded tile "
                << frame.tileIndex;
    }

    // Clear the request before rendering so a frame or window event arriving
    // during the render remains set for the next iteration.
    if (!m_PresentationNeeded.exchange(false)) {
        return;
    }

    const int fallbackHeight = (canvas.height + canvas.tileCount - 1) /
            canvas.tileCount;
    QList<int> tileHeights;
    tileHeights.reserve(canvas.tileCount);
    for (int tile = 0; tile < canvas.tileCount; ++tile) {
        tileHeights.append(m_TileHeights.value(tile, fallbackHeight));
    }
    QString renderError;
    QElapsedTimer renderTimer;
    renderTimer.start();
    const AppleVideoRenderer::RenderResult renderResult =
            m_VideoRenderer->render(
                    canvas,
                    tileHeights,
                    &renderError);
    m_RenderCallDurations.append(renderTimer.nsecsElapsed() / 1000000.0);
    if (renderResult == AppleVideoRenderer::RenderResult::Busy) {
        ++m_PresentationBusyCount;
        m_PresentationNeeded.store(true);
        return;
    }
    if (renderResult == AppleVideoRenderer::RenderResult::Failed) {
        qWarning().nospace()
                << "Apple High Performance 4:4:4 render failed: "
                << renderError;
        m_AwaitingPresentationBatches = 0;
        m_AwaitingDecodeSubmissions.clear();
        return;
    }
    ++m_PresentationCount;
    m_PresentedTileUpdates += static_cast<quint64>(
            m_AwaitingDecodeSubmissions.size());
    const quint64 presentationNow = SDL_GetTicks64();
    if (m_AwaitingPresentationBatches > 0) {
        ++m_DisplayedFrameBatches;
        m_DroppedFrameBatches += m_AwaitingPresentationBatches - 1;
        if (m_LastDisplayedFrameAt != 0 &&
                presentationNow >= m_LastDisplayedFrameAt) {
            m_DisplayFrameIntervals.append(
                    static_cast<double>(presentationNow -
                                        m_LastDisplayedFrameAt));
        }
        m_LastDisplayedFrameAt = presentationNow;
        const quint64 displayedAtNanoseconds = steadyNanoseconds();
        for (quint64 decodeSubmittedAtNanoseconds :
             std::as_const(m_AwaitingDecodeSubmissions)) {
            if (decodeSubmittedAtNanoseconds != 0 &&
                    displayedAtNanoseconds >=
                            decodeSubmittedAtNanoseconds) {
                m_SubmitToDisplayLatencies.append(
                        (displayedAtNanoseconds -
                         decodeSubmittedAtNanoseconds) / 1000000.0);
            }
        }
    }
    m_AwaitingPresentationBatches = 0;
    m_AwaitingDecodeSubmissions.clear();
    if (m_PresentationWindowStartedAt == 0) {
        m_PresentationWindowStartedAt = presentationNow;
    }
    else if (presentationNow - m_PresentationWindowStartedAt >=
             static_cast<quint64>(PerformanceReportIntervalMs)) {
        const double seconds = qMax<quint64>(
                1, presentationNow - m_PresentationWindowStartedAt) / 1000.0;
        const IntervalStatistics displayCadence =
                calculateIntervalStatistics(m_DisplayFrameIntervals);
        const IntervalStatistics submitToDisplay =
                calculateIntervalStatistics(m_SubmitToDisplayLatencies);
        const IntervalStatistics renderCalls =
                calculateIntervalStatistics(m_RenderCallDurations);
        const QString presentationSummary = QStringLiteral(
                "DISPLAY %1 FPS   VSYNC %2 Hz   TILE UPDATES %3/s   COALESCED %4\n"
                "FRAME TIME %5 ms avg   %6 p95   JITTER %7 ms\n"
                        "DECODE TO PRESENT %8 ms avg   %9 p95\n"
                "PRESENT CALL %10 ms avg   %11 p95   BUSY %12")
                .arg(m_DisplayedFrameBatches / seconds, 0, 'f', 1)
                .arg(m_PresentationCount / seconds, 0, 'f', 1)
                .arg(m_PresentedTileUpdates / seconds, 0, 'f', 1)
                .arg(m_DroppedFrameBatches)
                .arg(displayCadence.average, 0, 'f', 1)
                .arg(displayCadence.percentile95, 0, 'f', 1)
                .arg(displayCadence.jitter, 0, 'f', 1)
                .arg(submitToDisplay.average, 0, 'f', 1)
                .arg(submitToDisplay.percentile95, 0, 'f', 1)
                .arg(renderCalls.average, 0, 'f', 2)
                .arg(renderCalls.percentile95, 0, 'f', 2)
                .arg(m_PresentationBusyCount);
        {
            QMutexLocker locker(&m_PerformanceMutex);
            m_PerformancePresentationSummary = presentationSummary;
            m_PerformanceMetrics.presentedFramesPerSecond =
                    m_DisplayedFrameBatches / seconds;
            m_PerformanceMetrics.renderMilliseconds = renderCalls.average;
            m_PerformanceMetrics.hasPresentationSample = true;
        }
        qInfo().nospace()
                << "Apple High Performance presentation: "
                << QString::number(m_PresentationCount / seconds, 'f', 1)
                << " presents/s, "
                << QString::number(m_PresentedTileUpdates / seconds, 'f', 1)
                << " tile updates/s, displayed="
                << QString::number(m_DisplayedFrameBatches / seconds, 'f', 1)
                << " fps, coalesced=" << m_DroppedFrameBatches
                << ", frame interval avg/p95/jitter="
                << QString::number(displayCadence.average, 'f', 1) << "/"
                << QString::number(displayCadence.percentile95, 'f', 1) << "/"
                << QString::number(displayCadence.jitter, 'f', 1)
                << " ms, decode-to-present avg/p95="
                << QString::number(submitToDisplay.average, 'f', 1) << "/"
                << QString::number(submitToDisplay.percentile95, 'f', 1)
                << " ms, present-call avg/p95="
                << QString::number(renderCalls.average, 'f', 2) << "/"
                << QString::number(renderCalls.percentile95, 'f', 2)
                << " ms, busy=" << m_PresentationBusyCount
                << ", render-loop max="
                << QString::number(m_MaxRenderLoopGapMilliseconds, 'f', 2)
                << " ms, overlay-update max="
                << QString::number(m_MaxOverlayUpdateMilliseconds, 'f', 2)
                << " ms";
        m_PresentationWindowStartedAt = presentationNow;
        m_PresentationCount = 0;
        m_PresentedTileUpdates = 0;
        m_DisplayedFrameBatches = 0;
        m_DroppedFrameBatches = 0;
        m_PresentationBusyCount = 0;
        m_DisplayFrameIntervals.clear();
        m_SubmitToDisplayLatencies.clear();
        m_RenderCallDurations.clear();
        m_MaxRenderLoopGapMilliseconds = 0.0;
        m_MaxOverlayUpdateMilliseconds = 0.0;
        requestPerformanceOverlayUpdate();
    }
}


void AppleScreenSharingSession::renderSecondaryFrames()
{
    if (m_SecondaryVideoRenderer == nullptr) {
        return;
    }
    QHash<int, AppleDecodedTile> frames;
    AppleCanvas canvas;
    {
        QMutexLocker locker(&m_FrameMutex);
        canvas = m_SecondaryCanvas;
        frames = std::move(m_SecondaryLatestFrames);
        m_SecondaryLatestFrames.clear();
        m_SecondaryPendingFrameBatches = 0;
    }
    if (!canvas.isUsable()) {
        return;
    }
    for (auto iterator = frames.begin(); iterator != frames.end(); ++iterator) {
        AppleDecodedTile& frame = iterator.value();
        if (!frame.isValid()) {
            continue;
        }
        QString uploadError;
        if (m_SecondaryVideoRenderer->upload(frame, &uploadError)) {
            m_SecondaryTileHeights.insert(frame.tileIndex, frame.height);
            m_SecondaryPresentationNeeded.store(true);
        }
        else {
            qWarning().nospace()
                    << "Apple High Performance display 2 upload failed: "
                    << uploadError;
        }
    }
    if (!m_SecondaryPresentationNeeded.exchange(false)) {
        return;
    }
    const int fallbackHeight = (canvas.height + canvas.tileCount - 1) /
            canvas.tileCount;
    QList<int> tileHeights;
    tileHeights.reserve(canvas.tileCount);
    for (int tile = 0; tile < canvas.tileCount; ++tile) {
        tileHeights.append(m_SecondaryTileHeights.value(
                tile, fallbackHeight));
    }
    QString renderError;
    const AppleVideoRenderer::RenderResult result =
            m_SecondaryVideoRenderer->render(
                    canvas,
                    tileHeights,
                    &renderError);
    if (result == AppleVideoRenderer::RenderResult::Busy) {
        m_SecondaryPresentationNeeded.store(true);
    }
    else if (result == AppleVideoRenderer::RenderResult::Failed) {
        qWarning().nospace()
                << "Apple High Performance display 2 render failed: "
                << renderError;
    }
}

