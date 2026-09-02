#include "applefiletransferprogress.h"

#include <QBackingStore>
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QExposeEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QResizeEvent>
#include <QScreen>
#include <QUrl>
#include <QWindow>

#include <algorithm>
#include <utility>

namespace {

constexpr int WindowWidth = 440;
constexpr int HeaderHeight = 54;
constexpr int RowHeight = 94;
constexpr int MaximumVisibleRows = 4;
constexpr int MaximumRetainedEntries = 32;

QString translated(const char* source)
{
    return QCoreApplication::translate(
            "AppleFileTransferProgressWindow", source);
}

bool isTerminal(AppleFileTransferProgressState state)
{
    return state == AppleFileTransferProgressState::Completed ||
            state == AppleFileTransferProgressState::Failed ||
            state == AppleFileTransferProgressState::Cancelled;
}

bool canPause(AppleFileTransferProgressState state)
{
    return state == AppleFileTransferProgressState::Sending ||
            state == AppleFileTransferProgressState::Receiving ||
            state == AppleFileTransferProgressState::Paused;
}

QString rateText(double bytesPerSecond)
{
    if (bytesPerSecond <= 0.0) return {};
    if (bytesPerSecond >= 1'000'000'000.0) {
        return translated("%1 GB/s").arg(
                bytesPerSecond / 1'000'000'000.0, 0, 'f', 1);
    }
    if (bytesPerSecond >= 1'000'000.0) {
        return translated("%1 MB/s").arg(
                bytesPerSecond / 1'000'000.0, 0, 'f', 1);
    }
    if (bytesPerSecond >= 1'000.0) {
        return translated("%1 KB/s").arg(
                bytesPerSecond / 1'000.0, 0, 'f', 0);
    }
    return translated("%1 B/s").arg(bytesPerSecond, 0, 'f', 0);
}

QString statusText(const AppleFileTransferProgressEntry& entry)
{
    QString status;
    switch (entry.state) {
    case AppleFileTransferProgressState::WaitingForRemote:
        status = translated("Waiting for the remote Mac…");
        break;
    case AppleFileTransferProgressState::Sending:
        status = translated("Sending…");
        break;
    case AppleFileTransferProgressState::Receiving:
        status = translated("Receiving…");
        break;
    case AppleFileTransferProgressState::Paused:
        status = translated("Paused");
        break;
    case AppleFileTransferProgressState::Completing:
        status = translated("Finishing…");
        break;
    case AppleFileTransferProgressState::Completed:
        status = entry.incoming ? translated("Received")
                                : translated("Sent");
        break;
    case AppleFileTransferProgressState::Failed:
        status = entry.errorText.isEmpty()
                ? translated("Failed")
                : translated("Failed: %1").arg(entry.errorText);
        break;
    case AppleFileTransferProgressState::Cancelled:
        status = translated("Cancelled");
        break;
    }
    if ((entry.state == AppleFileTransferProgressState::Sending ||
         entry.state == AppleFileTransferProgressState::Receiving) &&
            entry.bytesPerSecond > 0.0) {
        status += QStringLiteral(" · ") + rateText(entry.bytesPerSecond);
    }
    return status;
}

QString destinationText(const AppleFileTransferProgressEntry& entry)
{
    return entry.incoming
            ? translated("From %1").arg(entry.remoteName)
            : translated("To %1").arg(entry.remoteName);
}

QPainterPath roundedPath(const QRectF& rectangle, qreal radius)
{
    QPainterPath path;
    path.addRoundedRect(rectangle, radius, radius);
    return path;
}

} // namespace

class AppleFileTransferProgressWindow::Window final : public QWindow
{
public:
    Window(PauseHandler pause, CancelHandler cancel)
        : m_BackingStore(this),
          m_Pause(std::move(pause)),
          m_Cancel(std::move(cancel))
    {
        setTitle(translated("File Transfers"));
        // This needs to be an ordinary top-level window so minimizing leaves
        // an accessible taskbar/Dock entry. Tool windows have no reliable way
        // to restore themselves after minimization on Windows. Customize the
        // chrome to keep only the two management actions that make sense for
        // a fixed-size progress surface.
        setFlags(Qt::Window | Qt::CustomizeWindowHint |
                 Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                 Qt::WindowMinimizeButtonHint |
                 Qt::WindowCloseButtonHint |
                 Qt::WindowStaysOnTopHint);
        setModality(Qt::NonModal);
        resize(WindowWidth, 180);
    }

    void updateEntry(const AppleFileTransferProgressEntry& update)
    {
        if (update.sessionId == 0) return;
        const auto existing = std::find_if(
                m_Entries.begin(), m_Entries.end(),
                [&update](const AppleFileTransferProgressEntry& entry) {
                    return entry.sessionId == update.sessionId;
                });
        const bool isNew = existing == m_Entries.end();
        if (isNew) {
            AppleFileTransferProgressEntry entry = update;
            entry.progress = std::clamp(entry.progress, 0.0, 1.0);
            m_Entries.prepend(std::move(entry));
        }
        else {
            if (!update.name.isEmpty()) existing->name = update.name;
            if (!update.remoteName.isEmpty()) {
                existing->remoteName = update.remoteName;
            }
            if (!update.path.isEmpty() &&
                    update.state != AppleFileTransferProgressState::Failed) {
                existing->path = update.path;
            }
            if (!update.errorText.isEmpty()) {
                existing->errorText = update.errorText;
            }
            existing->incoming = update.incoming;
            existing->state = update.state;
            if (update.hasProgress) {
                existing->progress = std::max(
                        existing->progress,
                        std::clamp(update.progress, 0.0, 1.0));
            }
            if (update.bytesPerSecond > 0.0) {
                existing->bytesPerSecond = update.bytesPerSecond;
            }
            if (isTerminal(update.state)) {
                existing->bytesPerSecond = 0.0;
            }
        }

        while (m_Entries.size() > MaximumRetainedEntries) {
            const auto removable = std::find_if(
                    m_Entries.rbegin(), m_Entries.rend(),
                    [](const AppleFileTransferProgressEntry& entry) {
                        return isTerminal(entry.state);
                    });
            if (removable == m_Entries.rend()) break;
            m_Entries.erase(std::next(removable).base());
        }
        updateWindowSize();
        if (isNew && !isVisible()) {
            positionOnActiveScreen();
            show();
            raise();
        }
        requestUpdate();
    }

    void failActiveEntries(const QString& reason)
    {
        bool changed = false;
        for (AppleFileTransferProgressEntry& entry : m_Entries) {
            if (isTerminal(entry.state)) continue;
            entry.state = AppleFileTransferProgressState::Failed;
            entry.errorText = reason;
            entry.bytesPerSecond = 0.0;
            changed = true;
        }
        if (changed) {
            updateWindowSize();
            requestUpdate();
        }
    }

protected:
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::UpdateRequest) {
            renderNow();
            return true;
        }
        return QWindow::event(event);
    }

    void exposeEvent(QExposeEvent*) override
    {
        if (isExposed()) renderNow();
    }

    void resizeEvent(QResizeEvent* event) override
    {
        m_BackingStore.resize(event->size());
        requestUpdate();
    }

    void closeEvent(QCloseEvent* event) override
    {
        // Closing the progress surface must not cancel active transfers. Keep
        // its entries alive so a later transfer can show the same window with
        // complete history.
        hide();
        event->ignore();
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Escape) {
            hide();
            event->accept();
            return;
        }
        QWindow::keyPressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWindow::mouseReleaseEvent(event);
            return;
        }
        const QPoint point = event->position().toPoint();
        if (m_ClearRect.contains(point)) {
            for (auto iterator = m_Entries.begin();
                 iterator != m_Entries.end();) {
                if (isTerminal(iterator->state)) {
                    iterator = m_Entries.erase(iterator);
                }
                else {
                    ++iterator;
                }
            }
            if (m_Entries.isEmpty()) hide();
            updateWindowSize();
            requestUpdate();
            event->accept();
            return;
        }
        for (const RowHitTargets& targets : std::as_const(m_RowHitTargets)) {
            const auto entry = std::find_if(
                    m_Entries.cbegin(), m_Entries.cend(),
                    [&targets](const AppleFileTransferProgressEntry& value) {
                        return value.sessionId == targets.sessionId;
                    });
            if (entry == m_Entries.cend()) continue;
            if (!targets.pause.isNull() && targets.pause.contains(point)) {
                if (m_Pause) {
                    m_Pause(entry->sessionId,
                            entry->state !=
                                    AppleFileTransferProgressState::Paused);
                }
                event->accept();
                return;
            }
            if (!targets.cancel.isNull() && targets.cancel.contains(point)) {
                if (m_Cancel) m_Cancel(entry->sessionId);
                event->accept();
                return;
            }
            if (!targets.reveal.isNull() && targets.reveal.contains(point)) {
                const QFileInfo info(entry->path);
                if (info.exists()) {
                    const QString directory = info.isDir()
                            ? info.absoluteFilePath()
                            : info.absolutePath();
                    QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
                }
                event->accept();
                return;
            }
        }
        QWindow::mouseReleaseEvent(event);
    }

private:
    struct RowHitTargets
    {
        quint32 sessionId = 0;
        QRect pause;
        QRect cancel;
        QRect reveal;
    };

    QList<AppleFileTransferProgressEntry> visibleEntries() const
    {
        QList<AppleFileTransferProgressEntry> visible;
        for (const AppleFileTransferProgressEntry& entry : m_Entries) {
            if (!isTerminal(entry.state)) visible.append(entry);
            if (visible.size() == MaximumVisibleRows) return visible;
        }
        for (const AppleFileTransferProgressEntry& entry : m_Entries) {
            if (isTerminal(entry.state)) visible.append(entry);
            if (visible.size() == MaximumVisibleRows) break;
        }
        return visible;
    }

    void updateWindowSize()
    {
        const int rowCount = std::max(
                1, static_cast<int>(visibleEntries().size()));
        const int contentHeight = std::clamp(
                HeaderHeight + rowCount * RowHeight, 180, 440);
        if (width() != WindowWidth || height() != contentHeight) {
            resize(WindowWidth, contentHeight);
        }
    }

    void positionOnActiveScreen()
    {
        QScreen* activeScreen = QGuiApplication::screenAt(QCursor::pos());
        if (activeScreen == nullptr) activeScreen = QGuiApplication::primaryScreen();
        if (activeScreen == nullptr) return;
        const QRect available = activeScreen->availableGeometry();
        setPosition(available.right() - width() - 20,
                    available.top() + 20);
    }

    void drawFileIcon(QPainter& painter,
                      const QRectF& bounds,
                      bool incoming,
                      const QColor& accent)
    {
        QColor tile = accent;
        tile.setAlpha(30);
        painter.fillPath(roundedPath(bounds, 10.0), tile);
        painter.setPen(QPen(accent, 1.7));
        const QRectF document = bounds.adjusted(12, 8, -12, -8);
        QPainterPath page;
        page.moveTo(document.left(), document.top());
        page.lineTo(document.right() - 7, document.top());
        page.lineTo(document.right(), document.top() + 7);
        page.lineTo(document.right(), document.bottom());
        page.lineTo(document.left(), document.bottom());
        page.closeSubpath();
        painter.drawPath(page);
        const qreal centerX = bounds.center().x();
        const qreal top = bounds.center().y() - 6;
        const qreal bottom = bounds.center().y() + 7;
        painter.drawLine(QPointF(centerX, incoming ? top : bottom),
                         QPointF(centerX, incoming ? bottom : top));
        const qreal arrowY = incoming ? bottom : top;
        const qreal direction = incoming ? -1.0 : 1.0;
        painter.drawLine(QPointF(centerX, arrowY),
                         QPointF(centerX - 4, arrowY + direction * 4));
        painter.drawLine(QPointF(centerX, arrowY),
                         QPointF(centerX + 4, arrowY + direction * 4));
    }

    void drawRoundButton(QPainter& painter,
                         const QRectF& bounds,
                         const QColor& foreground,
                         const QColor& background)
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(background);
        painter.drawEllipse(bounds);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(foreground, 1.8, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
    }

    void renderNow()
    {
        if (!isExposed()) return;
        const QRect area(QPoint(0, 0), size());
        m_BackingStore.beginPaint(area);
        QPainter painter(m_BackingStore.paintDevice());
        painter.setRenderHint(QPainter::Antialiasing);

        const QPalette palette = QGuiApplication::palette();
        const QColor background = palette.color(QPalette::Window);
        const QColor foreground = palette.color(QPalette::WindowText);
        const QColor secondary = palette.color(QPalette::PlaceholderText);
        const QColor separator = palette.color(QPalette::Midlight);
        const QColor accent = palette.color(QPalette::Highlight);
        QColor buttonBackground = palette.color(QPalette::AlternateBase);
        if (buttonBackground == background) {
            buttonBackground = separator;
            buttonBackground.setAlpha(75);
        }

        painter.fillRect(area, background);
        QFont titleFont = QGuiApplication::font();
        titleFont.setBold(true);
        painter.setFont(titleFont);
        painter.setPen(foreground);
        painter.drawText(QRect(14, 0, width() - 28, HeaderHeight),
                         Qt::AlignCenter, translated("File Transfers"));

        const bool canClear = std::any_of(
                m_Entries.cbegin(), m_Entries.cend(),
                [](const AppleFileTransferProgressEntry& entry) {
                    return isTerminal(entry.state);
                });
        QFont actionFont = QGuiApplication::font();
        actionFont.setPointSizeF(std::max(8.0, actionFont.pointSizeF() - 1.0));
        painter.setFont(actionFont);
        const QString clearText = translated("Clear");
        const int clearWidth = QFontMetrics(actionFont)
                .horizontalAdvance(clearText) + 8;
        m_ClearRect = QRect(width() - 14 - clearWidth, 0,
                            clearWidth, HeaderHeight);
        painter.setPen(canClear ? accent : secondary);
        painter.drawText(m_ClearRect, Qt::AlignCenter, clearText);
        if (!canClear) m_ClearRect = {};

        painter.setPen(QPen(separator, 1));
        painter.drawLine(0, HeaderHeight - 1, width(), HeaderHeight - 1);

        m_RowHitTargets.clear();
        const QList<AppleFileTransferProgressEntry> entries = visibleEntries();
        if (entries.isEmpty()) {
            painter.setFont(QGuiApplication::font());
            painter.setPen(secondary);
            painter.drawText(QRect(20, HeaderHeight, width() - 40,
                                   height() - HeaderHeight),
                             Qt::AlignCenter | Qt::TextWordWrap,
                             translated("No File Transfers\n"
                                        "Drag a file onto the remote display to send it."));
        }

        for (int index = 0; index < entries.size(); ++index) {
            const AppleFileTransferProgressEntry& entry = entries.at(index);
            const int top = HeaderHeight + index * RowHeight;
            if (index > 0) {
                painter.setPen(QPen(separator, 1));
                painter.drawLine(72, top, width(), top);
            }
            drawFileIcon(painter, QRectF(14, top + 25, 44, 44),
                         entry.incoming, accent);

            RowHitTargets targets;
            targets.sessionId = entry.sessionId;
            const bool terminal = isTerminal(entry.state);
            const QFileInfo pathInfo(entry.path);
            const int actionWidth = terminal ? 38 : 70;
            const int textLeft = 70;
            const int textRight = width() - 14 - actionWidth;

            QFont nameFont = QGuiApplication::font();
            nameFont.setBold(true);
            painter.setFont(nameFont);
            painter.setPen(foreground);
            painter.drawText(QRect(textLeft, top + 10,
                                   textRight - textLeft, 21),
                             Qt::AlignVCenter,
                             QFontMetrics(nameFont).elidedText(
                                     entry.name, Qt::ElideMiddle,
                                     textRight - textLeft));

            QFont detailFont = QGuiApplication::font();
            detailFont.setPointSizeF(
                    std::max(8.0, detailFont.pointSizeF() - 1.0));
            painter.setFont(detailFont);
            painter.setPen(secondary);
            painter.drawText(QRect(textLeft, top + 31,
                                   textRight - textLeft, 18),
                             Qt::AlignVCenter,
                             QFontMetrics(detailFont).elidedText(
                                     destinationText(entry), Qt::ElideRight,
                                     textRight - textLeft));

            const QColor statusColor =
                    entry.state == AppleFileTransferProgressState::Failed
                    ? QColor(214, 54, 59) : secondary;
            painter.setPen(statusColor);
            const QString status = statusText(entry);
            painter.drawText(QRect(textLeft, top + 49,
                                   textRight - textLeft, 18),
                             Qt::AlignVCenter,
                             QFontMetrics(detailFont).elidedText(
                                     status, Qt::ElideRight,
                                     textRight - textLeft));

            if (!terminal &&
                    entry.state !=
                            AppleFileTransferProgressState::WaitingForRemote) {
                const QRectF track(textLeft, top + 75,
                                   textRight - textLeft, 4);
                QColor trackColor = separator;
                trackColor.setAlpha(115);
                painter.fillPath(roundedPath(track, 2), trackColor);
                const qreal progressWidth = track.width() *
                        std::clamp(entry.progress, 0.0, 1.0);
                if (progressWidth > 0.0) {
                    painter.fillPath(roundedPath(
                            QRectF(track.left(), track.top(),
                                   progressWidth, track.height()), 2), accent);
                }
            }

            if (terminal) {
                if (pathInfo.exists()) {
                    targets.reveal = QRect(width() - 47, top + 32, 28, 28);
                    drawRoundButton(painter, targets.reveal,
                                    secondary, buttonBackground);
                    const QPointF center = targets.reveal.center();
                    painter.drawEllipse(QRectF(center.x() - 6,
                                               center.y() - 6, 10, 10));
                    painter.drawLine(QPointF(center.x() + 2,
                                             center.y() + 2),
                                     QPointF(center.x() + 7,
                                             center.y() + 7));
                }
            }
            else {
                if (canPause(entry.state)) {
                    targets.pause = QRect(width() - 78, top + 32, 28, 28);
                    drawRoundButton(painter, targets.pause,
                                    secondary, buttonBackground);
                    const QPointF center = targets.pause.center();
                    if (entry.state == AppleFileTransferProgressState::Paused) {
                        QPainterPath play;
                        play.moveTo(center.x() - 3, center.y() - 6);
                        play.lineTo(center.x() + 6, center.y());
                        play.lineTo(center.x() - 3, center.y() + 6);
                        play.closeSubpath();
                        painter.fillPath(play, secondary);
                    }
                    else {
                        painter.drawLine(QPointF(center.x() - 3,
                                                 center.y() - 5),
                                         QPointF(center.x() - 3,
                                                 center.y() + 5));
                        painter.drawLine(QPointF(center.x() + 3,
                                                 center.y() - 5),
                                         QPointF(center.x() + 3,
                                                 center.y() + 5));
                    }
                }
                targets.cancel = QRect(width() - 43, top + 32, 28, 28);
                drawRoundButton(painter, targets.cancel,
                                secondary, buttonBackground);
                const QPointF center = targets.cancel.center();
                painter.drawLine(QPointF(center.x() - 5, center.y() - 5),
                                 QPointF(center.x() + 5, center.y() + 5));
                painter.drawLine(QPointF(center.x() + 5, center.y() - 5),
                                 QPointF(center.x() - 5, center.y() + 5));
            }
            m_RowHitTargets.append(targets);
        }

        painter.end();
        m_BackingStore.endPaint();
        m_BackingStore.flush(area);
    }

    QBackingStore m_BackingStore;
    PauseHandler m_Pause;
    CancelHandler m_Cancel;
    QList<AppleFileTransferProgressEntry> m_Entries;
    QList<RowHitTargets> m_RowHitTargets;
    QRect m_ClearRect;
};

AppleFileTransferProgressWindow::AppleFileTransferProgressWindow(
        PauseHandler pause,
        CancelHandler cancel)
    : m_Window(std::make_unique<Window>(
              std::move(pause), std::move(cancel)))
{
}

AppleFileTransferProgressWindow::~AppleFileTransferProgressWindow() = default;

void AppleFileTransferProgressWindow::update(
        const AppleFileTransferProgressEntry& entry)
{
    m_Window->updateEntry(entry);
}

void AppleFileTransferProgressWindow::failActive(const QString& reason)
{
    m_Window->failActiveEntries(reason);
}

void AppleFileTransferProgressWindow::close()
{
    if (m_Window) m_Window->hide();
}
