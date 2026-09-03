#include "appleinputsourceplatform.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

#include <utility>

namespace {

QString currentInputSourceIdentifier()
{
    TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
    if (source == nullptr) {
        return {};
    }
    CFStringRef identifier = static_cast<CFStringRef>(
            TISGetInputSourceProperty(source, kTISPropertyInputSourceID));
    QString result;
    if (identifier != nullptr) {
        result = QString::fromCFString(identifier);
    }
    CFRelease(source);
    return result;
}

class AppleMacInputSourceMonitor final : public AppleLocalInputSourceMonitor
{
public:
    explicit AppleMacInputSourceMonitor(ChangeCallback callback)
        : m_Callback(std::move(callback)),
          m_CurrentIdentifier(currentInputSourceIdentifier())
    {
        NSDistributedNotificationCenter* center =
                [NSDistributedNotificationCenter defaultCenter];
        AppleMacInputSourceMonitor* monitor = this;
        m_Observer = [center
                addObserverForName:(__bridge NSString*)
                                           kTISNotifySelectedKeyboardInputSourceChanged
                           object:nil
                            queue:[NSOperationQueue mainQueue]
                       usingBlock:^(NSNotification*) {
                           monitor->refresh();
                       }];
    }

    ~AppleMacInputSourceMonitor() override
    {
        if (m_Observer != nil) {
            [[NSDistributedNotificationCenter defaultCenter]
                    removeObserver:m_Observer];
            m_Observer = nil;
        }
    }

    bool isValid() const override
    {
        return m_Observer != nil && !m_CurrentIdentifier.isEmpty();
    }

    QString currentIdentifier() const override
    {
        return m_CurrentIdentifier;
    }

    void refresh() override
    {
        const QString identifier = currentInputSourceIdentifier();
        if (identifier.isEmpty() || identifier == m_CurrentIdentifier) {
            return;
        }
        m_CurrentIdentifier = identifier;
        if (m_Callback) {
            m_Callback(identifier);
        }
    }

private:
    ChangeCallback m_Callback;
    QString m_CurrentIdentifier;
    id m_Observer = nil;
};

} // namespace

std::unique_ptr<AppleLocalInputSourceMonitor>
createAppleLocalInputSourceMonitor(
        AppleLocalInputSourceMonitor::ChangeCallback callback)
{
    return std::make_unique<AppleMacInputSourceMonitor>(std::move(callback));
}
