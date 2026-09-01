#pragma once

#include <QList>
#include <QRect>
#include <QSettings>
#include <QSize>
#include <QString>

#include <optional>

enum class AppleWindowRole
{
    Primary,
    Secondary,
};

namespace AppleWindowPlacement {

QRect constrainToVisibleDisplays(const QRect& geometry,
                                 const QList<QRect>& usableDisplayBounds,
                                 const QSize& minimumSize = QSize(320, 240));

}

/** Device-local SDL window geometry; intentionally excluded from configuration sync. */
class AppleWindowPlacementStore
{
public:
    explicit AppleWindowPlacementStore(const QString& settingsPath = QString());

    std::optional<QRect> load(AppleWindowRole role) const;
    bool save(AppleWindowRole role, const QRect& geometry);
    std::optional<QSize> loadViewport(const QString& connectionId,
                                      int displayIndex = 0) const;
    bool saveViewport(const QString& connectionId,
                      int displayIndex,
                      const QSize& size);

private:
    QSettings m_Settings;
};
