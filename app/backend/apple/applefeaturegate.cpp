#include "applefeaturegate.h"

#include <QSettings>
#include <QtGlobal>

bool AppleFeatureGate::isRuntimeEnabled()
{
    if (qEnvironmentVariableIsSet("MOONLIGHT_APPLE_SCREEN_SHARING_RUNTIME")) {
        return qEnvironmentVariableIntValue("MOONLIGHT_APPLE_SCREEN_SHARING_RUNTIME") == 1;
    }

    QSettings settings;
    // Reaching this code already means the Apple implementation was explicitly
    // compiled into the product. Make that build usable without requiring a
    // hidden environment variable, while preserving explicit runtime-off values.
    return settings.value(QStringLiteral("appleScreenSharing/runtimeEnabled"), true).toBool();
}
