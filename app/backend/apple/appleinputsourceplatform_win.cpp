#include "appleinputsourceplatform.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>

#include <qt_windows.h>

#include <array>
#include <utility>

namespace {

QString identifierForLayout(HKL layout)
{
    const LCID locale = MAKELCID(LOWORD(
            reinterpret_cast<quintptr>(layout)), SORT_DEFAULT);
    std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> localeName{};
    if (LCIDToLocaleName(locale, localeName.data(), localeName.size(), 0) == 0) {
        return {};
    }
    return appleInputSourceIdentifierForWindowsLocale(
            QString::fromWCharArray(localeName.data()));
}

class AppleWindowsInputSourceMonitor final
    : public AppleLocalInputSourceMonitor,
      public QAbstractNativeEventFilter
{
public:
    explicit AppleWindowsInputSourceMonitor(ChangeCallback callback)
        : m_Callback(std::move(callback)),
          m_CurrentIdentifier(identifierForLayout(GetKeyboardLayout(0)))
    {
        if (QCoreApplication::instance() != nullptr) {
            QCoreApplication::instance()->installNativeEventFilter(this);
            m_Installed = true;
        }
    }

    ~AppleWindowsInputSourceMonitor() override
    {
        if (m_Installed && QCoreApplication::instance() != nullptr) {
            QCoreApplication::instance()->removeNativeEventFilter(this);
        }
    }

    bool isValid() const override
    {
        return m_Installed;
    }

    QString currentIdentifier() const override
    {
        return m_CurrentIdentifier;
    }

    void refresh() override
    {
        refreshLayout(GetKeyboardLayout(0));
    }

    bool nativeEventFilter(const QByteArray&, void* message, qintptr*) override
    {
        const MSG* nativeMessage = static_cast<const MSG*>(message);
        if (nativeMessage == nullptr ||
                nativeMessage->message != WM_INPUTLANGCHANGE) {
            return false;
        }
        refreshLayout(reinterpret_cast<HKL>(nativeMessage->lParam));
        return false;
    }

private:
    void refreshLayout(HKL layout)
    {
        const QString identifier = identifierForLayout(layout);
        if (identifier == m_CurrentIdentifier) {
            return;
        }
        m_CurrentIdentifier = identifier;
        if (m_Callback) {
            m_Callback(identifier);
        }
    }
    ChangeCallback m_Callback;
    QString m_CurrentIdentifier;
    bool m_Installed = false;
};

} // namespace

std::unique_ptr<AppleLocalInputSourceMonitor>
createAppleLocalInputSourceMonitor(
        AppleLocalInputSourceMonitor::ChangeCallback callback)
{
    return std::make_unique<AppleWindowsInputSourceMonitor>(
            std::move(callback));
}
