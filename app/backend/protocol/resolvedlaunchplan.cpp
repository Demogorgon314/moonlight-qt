#include "resolvedlaunchplan.h"

ResolvedLaunchPlan::ResolvedLaunchPlan(ConnectionIdentity connection,
                                       QString endpoint,
                                       QString mode,
                                       quint64 revision)
    : m_Connection(std::move(connection)),
      m_Endpoint(std::move(endpoint)),
      m_Mode(std::move(mode)),
      m_Revision(revision)
{
}

bool ResolvedLaunchPlan::consume()
{
    bool expected = false;
    return m_Consumed.compare_exchange_strong(expected, true);
}

