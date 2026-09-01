#include "applefeaturegate.h"

#include <QSettings>
#include <QtGlobal>

bool AppleFeatureGate::isRuntimeEnabled()
{
    if (qEnvironmentVariableIsSet("MOONLIGHT_APPLE_SCREEN_SHARING_RUNTIME")) {
        return qEnvironmentVariableIntValue("MOONLIGHT_APPLE_SCREEN_SHARING_RUNTIME") == 1;
    }

    QSettings settings;
    // Reaching this code means the Apple implementation is present in this
    // supported-platform build. Keep it usable without requiring a hidden
    // environment variable, while preserving explicit runtime-off values.
    return settings.value(QStringLiteral("appleScreenSharing/runtimeEnabled"), true).toBool();
}
