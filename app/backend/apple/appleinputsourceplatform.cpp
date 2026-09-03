#include "appleinputsourceplatform.h"

QString appleInputSourceIdentifierForWindowsLocale(const QString& localeName)
{
    const QString normalized = localeName.trimmed().toLower();
    if (normalized.startsWith(QStringLiteral("en"))) {
        return QStringLiteral("com.apple.keylayout.ABC");
    }
    if (normalized == QStringLiteral("zh-cn") ||
            normalized == QStringLiteral("zh-sg") ||
            normalized.startsWith(QStringLiteral("zh-hans"))) {
        return QStringLiteral("com.apple.inputmethod.SCIM.ITABC");
    }
    return {};
}
