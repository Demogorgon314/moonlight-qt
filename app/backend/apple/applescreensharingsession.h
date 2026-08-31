#pragma once

#include "appleconnectionstore.h"
#include "streaming/streamsession.h"

#include <atomic>

class AppleScreenSharingSession final : public StreamSession
{
    Q_OBJECT

public:
    AppleScreenSharingSession(AppleSavedConnection connection,
                              QObject* parent = nullptr);
    ~AppleScreenSharingSession() override;

    // Invoked on the owning Qt thread after the worker has stopped all network work.
    void complete(bool success, const QString& error);

protected:
    bool initializeSession(QQuickWindow* qtWindow) override;
    void startSession() override;
    void interruptSession() override;
    void setShouldExitSession(bool quitHostActivity) override;

private:
    AppleSavedConnection m_Connection;
    std::atomic_bool m_Cancelled{false};
};
