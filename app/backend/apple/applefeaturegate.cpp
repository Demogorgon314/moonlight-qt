#include "applefeaturegate.h"

#include <QSettings>
#include <QtGlobal>

bool AppleFeatureGate::isRuntimeEnabled()
{
    if (qEnvironmentVariableIsSet("MOONLIGHT_APPLE_SCREEN_SHARING_RUNTIME")) {
        return qEnvironmentVariableIntValue("MOONLIGHT_APPLE_SCREEN_SHARING_RUNTIME") == 1;
    }

    QSettings settings;
    return settings.value(QStringLiteral("appleScreenSharing/runtimeEnabled"), false).toBool();
}

