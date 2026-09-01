#include "applewindowplacement.h"

#include "settings/devicelocalsettings.h"

#include <QtGlobal>

namespace {
constexpr auto LocalStateFileName = "apple-stream-window-placement.ini";
constexpr auto LocalStateVersionKey = "meta/version";
constexpr int LocalStateVersion = 1;

QString geometryKey(AppleWindowRole role)
{
    return role == AppleWindowRole::Primary
            ? QStringLiteral("primary/geometry")
            : QStringLiteral("secondary/geometry");
}

qint64 intersectionArea(const QRect& first, const QRect& second)
{
    const QRect intersection = first.intersected(second);
    return intersection.isValid()
            ? static_cast<qint64>(intersection.width()) * intersection.height()
            : 0;
}
}

QRect AppleWindowPlacement::constrainToVisibleDisplays(
        const QRect& geometry,
        const QList<QRect>& usableDisplayBounds,
        const QSize& minimumSize)
{
    if (!geometry.isValid() || usableDisplayBounds.isEmpty()) {
        return geometry;
    }

    const QRect* selectedDisplay = &usableDisplayBounds.first();
    qint64 largestIntersection = -1;
    for (const QRect& display : usableDisplayBounds) {
        if (!display.isValid()) {
            continue;
        }
        const qint64 area = intersectionArea(geometry, display);
        if (area > largestIntersection) {
            largestIntersection = area;
            selectedDisplay = &display;
        }
    }

    const int maximumWidth = qMax(1, selectedDisplay->width());
    const int maximumHeight = qMax(1, selectedDisplay->height());
    const int minimumWidth = qMin(qMax(1, minimumSize.width()), maximumWidth);
    const int minimumHeight = qMin(qMax(1, minimumSize.height()), maximumHeight);
    const int width = qBound(minimumWidth, geometry.width(), maximumWidth);
    const int height = qBound(minimumHeight, geometry.height(), maximumHeight);
    const int maximumX = selectedDisplay->x() + maximumWidth - width;
    const int maximumY = selectedDisplay->y() + maximumHeight - height;
    const int x = qBound(selectedDisplay->x(), geometry.x(), maximumX);
    const int y = qBound(selectedDisplay->y(), geometry.y(), maximumY);
    return QRect(x, y, width, height);
}

AppleWindowPlacementStore::AppleWindowPlacementStore(const QString& settingsPath)
    : m_Settings(settingsPath.isEmpty()
                         ? DeviceLocalSettings::filePath(
                                   QLatin1String(LocalStateFileName),
                                   "MOONLIGHT_APPLE_WINDOW_PLACEMENT_DIR")
                         : settingsPath,
                 QSettings::IniFormat)
{
}

std::optional<QRect> AppleWindowPlacementStore::load(AppleWindowRole role) const
{
    if (m_Settings.value(QLatin1String(LocalStateVersionKey), 0).toInt() !=
            LocalStateVersion) {
        return std::nullopt;
    }

    const QRect geometry = m_Settings.value(geometryKey(role)).toRect();
    return geometry.isValid() ? std::optional<QRect>(geometry) : std::nullopt;
}

bool AppleWindowPlacementStore::save(AppleWindowRole role,
                                     const QRect& geometry)
{
    if (!geometry.isValid()) {
        return false;
    }

    m_Settings.setValue(QLatin1String(LocalStateVersionKey), LocalStateVersion);
    m_Settings.setValue(geometryKey(role), geometry);
    m_Settings.sync();
    return m_Settings.status() == QSettings::NoError;
}
