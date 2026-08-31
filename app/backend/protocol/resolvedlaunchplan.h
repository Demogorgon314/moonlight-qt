#pragma once

#include "protocoltypes.h"

#include <QString>

#include <atomic>

class ResolvedLaunchPlan
{
public:
    virtual ~ResolvedLaunchPlan() = default;

    const ConnectionIdentity& connection() const { return m_Connection; }
    const QString& endpoint() const { return m_Endpoint; }
    const QString& mode() const { return m_Mode; }
    quint64 revision() const { return m_Revision; }

    ResolvedLaunchPlan(const ResolvedLaunchPlan&) = delete;
    ResolvedLaunchPlan& operator=(const ResolvedLaunchPlan&) = delete;

    bool consume();

protected:
    ResolvedLaunchPlan(ConnectionIdentity connection,
                       QString endpoint,
                       QString mode,
                       quint64 revision);

private:
    ConnectionIdentity m_Connection;
    QString m_Endpoint;
    QString m_Mode;
    quint64 m_Revision;
    std::atomic_bool m_Consumed{false};
};

