#include "backend/protocol/protocoltypes.h"
#include "backend/protocol/resolvedlaunchplan.h"
#include "streaming/streamsession.h"

#include <QCoreApplication>
#include <QDebug>

#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        qCritical() << message;
        std::exit(1);
    }
}

class TestLaunchPlan final : public ResolvedLaunchPlan
{
public:
    TestLaunchPlan(ConnectionIdentity identity, quint64 revision)
        : ResolvedLaunchPlan(std::move(identity),
                             QStringLiteral("host.example:47984"),
                             QStringLiteral("test"),
                             revision)
    {
    }
};

class TestSession final : public StreamSession
{
public:
    int initializeCalls = 0;
    int startCalls = 0;
    int interruptCalls = 0;
    int shouldExitCalls = 0;

    void finishForTest(int result)
    {
        finishSession(result);
    }

    void readyForTest()
    {
        publishReadyForDeletion();
    }

protected:
    bool initializeSession(QQuickWindow*) override
    {
        ++initializeCalls;
        return true;
    }

    void startSession() override
    {
        ++startCalls;
        setRunning();
    }

    void interruptSession() override
    {
        ++interruptCalls;
    }

    void setShouldExitSession(bool) override
    {
        ++shouldExitCalls;
    }
};

class FailingTestSession final : public StreamSession
{
protected:
    bool initializeSession(QQuickWindow*) override { return false; }
    void startSession() override {}
    void interruptSession() override {}
    void setShouldExitSession(bool) override {}
};

void testConnectionIdentity()
{
    const ConnectionIdentity moonlight(ProtocolKind::Moonlight, QStringLiteral("same-host"));
    const ConnectionIdentity apple(ProtocolKind::AppleScreenSharing, QStringLiteral("same-host"));

    require(moonlight != apple, "protocol must be part of connection identity");
    require(moonlight.toString() == QStringLiteral("moonlight:same-host"),
            "Moonlight identity must use a stable protocol prefix");
    require(apple.toString() == QStringLiteral("apple-screen-sharing:same-host"),
            "Apple identity must use a stable protocol prefix");

    bool parsed = false;
    require(ConnectionIdentity::fromString(moonlight.toString(), &parsed) == moonlight && parsed,
            "connection identity must round-trip");
    ConnectionIdentity::fromString(QStringLiteral("unknown:value"), &parsed);
    require(!parsed, "unknown protocol prefix must be rejected");
    ConnectionIdentity::fromString(QStringLiteral("moonlight:"), &parsed);
    require(!parsed, "empty stable identity must be rejected");
}

void testResolvedLaunchPlan()
{
    TestLaunchPlan plan(ConnectionIdentity(ProtocolKind::Moonlight,
                                           QStringLiteral("host-id")),
                        42);
    require(plan.endpoint() == QStringLiteral("host.example:47984"),
            "resolved endpoint must be immutable plan data");
    require(plan.mode() == QStringLiteral("test"), "resolved mode must be retained");
    require(plan.revision() == 42, "resolved revision must be retained");
    require(plan.consume(), "first plan consumption must succeed");
    require(!plan.consume(), "launch plan must be single-use");
}

void testStreamSessionLifecycle()
{
    TestSession session;
    int finalResults = 0;
    int readySignals = 0;
    int lastResult = 0;
    QObject::connect(&session, &StreamSession::sessionFinished,
                     [&](int result) {
                         ++finalResults;
                         lastResult = result;
                     });
    QObject::connect(&session, &StreamSession::readyForDeletion,
                     [&]() { ++readySignals; });

    require(session.initialize(nullptr), "session initialization must succeed");
    require(session.initialize(nullptr), "repeated initialization must be harmless");
    require(session.initializeCalls == 1, "implementation must initialize exactly once");
    require(session.state() == StreamSession::State::Initialized,
            "session must expose initialized state");

    session.start();
    require(session.startCalls == 1, "session implementation must start exactly once");
    require(StreamSession::active() == &session,
            "started session must hold the process-wide active lease");
    require(session.state() == StreamSession::State::Running,
            "session must enter running state");

    session.interrupt();
    session.interrupt();
    require(session.interruptCalls == 1, "interrupt must be idempotent");

    session.finishForTest(17);
    session.finishForTest(99);
    require(finalResults == 1 && lastResult == 17,
            "session must publish exactly one final result");

    session.readyForTest();
    session.readyForTest();
    require(readySignals == 1, "ready-for-deletion must be published exactly once");
    require(StreamSession::active() == nullptr,
            "active lease must be released only after full cleanup");
    require(session.state() == StreamSession::State::ReadyForDeletion,
            "session must expose terminal deletion state");
}

void testFailedInitializationCleanup()
{
    FailingTestSession session;
    int finalResults = 0;
    int readySignals = 0;
    QObject::connect(&session, &StreamSession::sessionFinished,
                     [&](int result) {
                         require(result == 0, "failed preflight must use the neutral failure result");
                         ++finalResults;
                     });
    QObject::connect(&session, &StreamSession::readyForDeletion,
                     [&]() { ++readySignals; });

    require(!session.initialize(nullptr), "failed preflight must be reported to the caller");
    require(session.state() == StreamSession::State::FailedToInitialize,
            "failed preflight must have an observable state before deferred cleanup");
    QCoreApplication::processEvents();
    require(finalResults == 1,
            "failed preflight must publish exactly one terminal result");
    require(readySignals == 1,
            "failed preflight must eventually release its catalog-owned session");
    require(session.state() == StreamSession::State::ReadyForDeletion,
            "failed preflight must reach deletion readiness");
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    std::fprintf(stderr, "testConnectionIdentity\n");
    testConnectionIdentity();
    std::fprintf(stderr, "testResolvedLaunchPlan\n");
    testResolvedLaunchPlan();
    std::fprintf(stderr, "testStreamSessionLifecycle\n");
    testStreamSessionLifecycle();
    std::fprintf(stderr, "testFailedInitializationCleanup\n");
    testFailedInitializationCleanup();
    std::fprintf(stderr, "multiprotocol architecture tests passed\n");
    qInfo() << "multiprotocol architecture tests passed";
    return 0;
}
