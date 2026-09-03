#pragma once

#include <QString>

#include <functional>
#include <memory>

class AppleLocalInputSourceMonitor
{
public:
    using ChangeCallback = std::function<void(const QString& identifier)>;

    virtual ~AppleLocalInputSourceMonitor() = default;
    virtual bool isValid() const = 0;
    virtual QString currentIdentifier() const = 0;
    virtual void refresh() = 0;
};

// Returns the closest stable macOS input-source identifier for a Windows input
// locale. Unknown locales deliberately remain unmapped so the session can
// retain subtype-one key semantics instead of selecting the wrong IME.
QString appleInputSourceIdentifierForWindowsLocale(const QString& localeName);

std::unique_ptr<AppleLocalInputSourceMonitor>
createAppleLocalInputSourceMonitor(
        AppleLocalInputSourceMonitor::ChangeCallback callback);
