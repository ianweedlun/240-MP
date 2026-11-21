// Unit tests for the async mixer process seam (T1 P-5A): dedicated-thread
// QProcess execution, latest-wins coalescing, bounded timeouts, failure
// semantics that preserve the cached state, and the <16 ms caller-thread
// contract. A fake child executable (fake-mixer-child) stands in for
// amixer/wpctl, so no audio hardware is needed. Run under ASan/UBSan with
// ASAN_OPTIONS=detect_leaks=1 — clean shutdown is part of the contract.
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <functional>

#include "audio/MixerProcessRunner.h"

using namespace audio;

namespace {

const QString kFakeChild = QStringLiteral(FAKE_MIXER_CHILD_PATH);

// Synthetic payload format ("LEVEL 42 MUTED 0") — the seam is backend-agnostic;
// amixer/wpctl parsing belongs to the backend tests, not here.
QString payload(int level, bool muted) {
    return QStringLiteral("LEVEL %1 MUTED %2").arg(level).arg(muted ? 1 : 0);
}

std::function<std::optional<MixerState>(const QString &)> levelParser() {
    return [](const QString &out) -> std::optional<MixerState> {
        const QStringList parts = out.simplified().split(QLatin1Char(' '));
        if (parts.size() != 4 || parts[0] != QLatin1String("LEVEL") ||
            parts[2] != QLatin1String("MUTED"))
            return std::nullopt;
        bool okLevel = false;
        bool okMute = false;
        const int level = parts[1].toInt(&okLevel);
        const int muted = parts[3].toInt(&okMute);
        if (!okLevel || !okMute || level < 0 || level > 100 || (muted != 0 && muted != 1))
            return std::nullopt;
        return MixerState{level, muted == 1};
    };
}

MixerProcessRequest fakeRequest(int delayMs, int exitCode, const QString &out,
                                const QString &tag = QStringLiteral("req"),
                                int timeoutMs = 1000) {
    MixerProcessRequest r;
    r.program = kFakeChild;
    r.arguments = {QString::number(delayMs), QString::number(exitCode), out};
    r.tag = tag;
    r.timeoutMs = timeoutMs;
    return r;
}

MixerProcessResult resultAt(const QSignalSpy &spy, int i) {
    return spy.at(i).at(0).value<MixerProcessResult>();
}

double msSince(const QElapsedTimer &t) { return double(t.nsecsElapsed()) / 1e6; }

} // namespace

class MixerProcessRunnerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // The contract constants and the seeded cache: accessors return cached
    // values only, and "observed" is distinct from "assumed".
    void defaultsAndCachedSeed() {
        MixerProcessRequest defaults;
        QCOMPARE(defaults.timeoutMs, 1000); // historical amixer/wpctl budget

        MixerProcessRunner runner;
        QCOMPARE(runner.cachedLevel(), 100); // historical backend default
        QCOMPARE(runner.cachedMuted(), false);
        QCOMPARE(runner.hasHardwareState(), false);
        QCOMPARE(runner.startedCount(), quint64(0));
        QCOMPARE(runner.isBusy(), false);

        runner.setCachedState(42, true);
        QCOMPARE(runner.cachedLevel(), 42);
        QCOMPARE(runner.cachedMuted(), true);
        QCOMPARE(runner.hasHardwareState(), false); // seeding is not observing
        runner.setCachedState(500, false);          // clamped to the contract range
        QCOMPARE(runner.cachedLevel(), 100);
    }

    // Slow success: a 200 ms child never blocks submit()/reads, the cache only
    // moves when the parsed result lands, and every callback is on the owner
    // thread.
    void slowSuccessUpdatesCache() {
        MixerProcessRunner runner;
        runner.setCachedState(60, true);
        QSignalSpy startedSpy(&runner, &MixerProcessRunner::requestStarted);
        QSignalSpy finishedSpy(&runner, &MixerProcessRunner::finished);
        QSignalSpy stateSpy(&runner, &MixerProcessRunner::stateChanged);

        QThread *deliveryThread = nullptr;
        connect(&runner, &MixerProcessRunner::finished, &runner,
                [&](const MixerProcessResult &) { deliveryThread = QThread::currentThread(); });
        QThread *parserThread = nullptr;
        MixerProcessRequest req = fakeRequest(200, 0, payload(42, false), QStringLiteral("slow"));
        req.parser = [&](const QString &out) {
            parserThread = QThread::currentThread();
            return levelParser()(out);
        };

        QElapsedTimer t;
        t.start();
        runner.submit(req);
        const double submitMs = msSince(t);
        QVERIFY2(submitMs < 16.0, qPrintable(QStringLiteral("submit took %1 ms").arg(submitMs)));

        // While the 200 ms child is in flight, reads stay cached and instant.
        t.restart();
        QCOMPARE(runner.cachedLevel(), 60);
        QCOMPARE(runner.cachedMuted(), true);
        QVERIFY(msSince(t) < 16.0);
        QCOMPARE(runner.hasHardwareState(), false);

        QVERIFY(QTest::qWaitFor([&] { return finishedSpy.count() == 1; }, 3000));
        const MixerProcessResult r = resultAt(finishedSpy, 0);
        QCOMPARE(r.status, MixerProcessResult::Status::Success);
        QCOMPARE(r.tag, QStringLiteral("slow"));
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.stdOut, payload(42, false));
        QVERIFY(r.diagnostic.isEmpty());
        QCOMPARE(runner.cachedLevel(), 42);
        QCOMPARE(runner.cachedMuted(), false);
        QCOMPARE(runner.hasHardwareState(), true);
        QCOMPARE(stateSpy.count(), 1);
        QCOMPARE(stateSpy.at(0).at(0).toInt(), 42);
        QCOMPARE(stateSpy.at(0).at(1).toBool(), false);
        QCOMPARE(startedSpy.count(), 1);
        QCOMPARE(runner.startedCount(), quint64(1));
        QCOMPARE(runner.isBusy(), false);
        // Marshalling contract: parser and finished() both ran on THIS thread.
        QCOMPARE(deliveryThread, QThread::currentThread());
        QCOMPARE(parserThread, QThread::currentThread());
    }

    // Timeout: budget enforced off the caller thread, cache untouched, queue
    // still alive afterwards.
    void timeoutKeepsCacheAndQueueAlive() {
        MixerProcessRunner runner;
        runner.setCachedState(55, true);
        QSignalSpy finishedSpy(&runner, &MixerProcessRunner::finished);
        QSignalSpy stateSpy(&runner, &MixerProcessRunner::stateChanged);

        MixerProcessRequest req = fakeRequest(5000, 0, payload(10, false),
                                              QStringLiteral("hang"), 150);
        req.parser = levelParser();
        QElapsedTimer t;
        t.start();
        runner.submit(req);
        QVERIFY(msSince(t) < 16.0);
        QVERIFY(QTest::qWaitFor([&] { return finishedSpy.count() == 1; }, 3000));
        const double elapsed = msSince(t);
        QVERIFY2(elapsed < 1500.0, qPrintable(QStringLiteral("timeout took %1 ms").arg(elapsed)));

        const MixerProcessResult r = resultAt(finishedSpy, 0);
        QCOMPARE(r.status, MixerProcessResult::Status::TimedOut);
        QVERIFY(!r.diagnostic.isEmpty());
        QCOMPARE(runner.cachedLevel(), 55); // last known good survives
        QCOMPARE(runner.cachedMuted(), true);
        QCOMPARE(runner.hasHardwareState(), false);
        QCOMPARE(stateSpy.count(), 0);
        QCOMPARE(runner.isBusy(), false);

        // The queue is not wedged: a good request completes right after.
        MixerProcessRequest good = fakeRequest(0, 0, payload(10, false), QStringLiteral("after"));
        good.parser = levelParser();
        runner.submit(good);
        QVERIFY(QTest::qWaitFor([&] { return finishedSpy.count() == 2; }, 3000));
        QCOMPARE(resultAt(finishedSpy, 1).status, MixerProcessResult::Status::Success);
        QCOMPARE(runner.cachedLevel(), 10);
        QCOMPARE(runner.hasHardwareState(), true);
    }

    // The default budget really is the historical 1000 ms, enforced on the
    // worker thread (a 5 s child must not hold anything for 5 s).
    void defaultTimeoutBudgetIs1000ms() {
        MixerProcessRunner runner;
        QSignalSpy finishedSpy(&runner, &MixerProcessRunner::finished);
        MixerProcessRequest req = fakeRequest(5000, 0, payload(10, false), QStringLiteral("hang"));
        req.parser = levelParser();
        QElapsedTimer t;
        t.start();
        runner.submit(req);
        QVERIFY(QTest::qWaitFor([&] { return finishedSpy.count() == 1; }, 4000));
        const double elapsed = msSince(t);
        QVERIFY(elapsed >= 900.0);  // waited out the budget...
        QVERIFY(elapsed < 2500.0);  // ...but not the child's 5 s
        QCOMPARE(resultAt(finishedSpy, 0).status, MixerProcessResult::Status::TimedOut);
        QCOMPARE(runner.cachedLevel(), 100); // untouched default cache
    }

    // Nonzero exit: failure surfaced, cache intact, queue alive.
    void nonZeroExitKeepsCacheAndQueueAlive() {
        MixerProcessRunner runner;
        runner.setCachedState(55, true);
        QSignalSpy finishedSpy(&runner, &MixerProcessRunner::finished);
        QSignalSpy stateSpy(&runner, &MixerProcessRunner::stateChanged);

        MixerProcessRequest req = fakeRequest(0, 3, payload(10, false), QStringLiteral("fail"));
        req.parser = levelParser();
        runner.submit(req);
        QVERIFY(QTest::qWaitFor([&] { return finishedSpy.count() == 1; }, 3000));
        const MixerProcessResult r = resultAt(finishedSpy, 0);
        QCOMPARE(r.status, MixerProcessResult::Status::NonZeroExit);
        QCOMPARE(r.exitCode, 3);
        QVERIFY(!r.diagnostic.isEmpty());
        QCOMPARE(runner.cachedLevel(), 55);
        QCOMPARE(runner.cachedMuted(), true);
        QCOMPARE(stateSpy.count(), 0);

        MixerProcessRequest good = fakeRequest(0, 0, payload(10, false), QStringLiteral("after"));
        good.parser = levelParser();
        runner.submit(good);
        QVERIFY(QTest::qWaitFor([&] { return finishedSpy.count() == 2; }, 3000));
        QCOMPARE(resultAt(finishedSpy, 1).status, MixerProcessResult::Status::Success);
        QCOMPARE(runner.cachedLevel(), 10);
    }

    // Garbage output with exit 0: unparseable is a failure, cache intact.
    void garbageOutputKeepsCacheAndQueueAlive() {
        MixerProcessRunner runner;
        runner.setCachedState(55, true);
        QSignalSpy finishedSpy(&runner, &MixerProcessRunner::finished);
        QSignalSpy stateSpy(&runner, &MixerProcessRunner::stateChanged);

        MixerProcessRequest req =
            fakeRequest(0, 0, QStringLiteral("total garbage, not a mixer line"),
                      QStringLiteral("garbage"));
        req.parser = levelParser();
        runner.submit(req);
        QVERIFY(QTest::qWaitFor([&] { return finishedSpy.count() == 1; }, 3000));
        const MixerProcessResult r = resultAt(finishedSpy, 0);
        QCOMPARE(r.status, MixerProcessResult::Status::UnparseableOutput);
        QCOMPARE(r.exitCode, 0);
        QVERIFY(!r.diagnostic.isEmpty());
        QCOMPARE(runner.cachedLevel(), 55);
        QCOMPARE(runner.cachedMuted(), true);
        QCOMPARE(runner.hasHardwareState(), false);
        QCOMPARE(stateSpy.count(), 0);

        MixerProcessRequest good = fakeRequest(0, 0, payload(10, false), QStringLiteral("after"));
        good.parser = levelParser();
        runner.submit(good);
        QVERIFY(QTest::qWaitFor([&] { return finishedSpy.count() == 2; }, 3000));
        QCOMPARE(resultAt(finishedSpy, 1).status, MixerProcessResult::Status::Success);
        QCOMPARE(runner.cachedLevel(), 10);
    }

    // Missing executable: spawn failure surfaced, cache intact, queue alive.
    void missingBinaryKeepsCacheAndQueueAlive() {
        MixerProcessRunner runner;
        runner.setCachedState(55, true);
        QSignalSpy finishedSpy(&runner, &MixerProcessRunner::finished);

        MixerProcessRequest req;
        req.program = QStringLiteral("240mp-no-such-mixer-binary-%1")
                          .arg(QCoreApplication::applicationPid());
        req.tag = QStringLiteral("missing");
        req.parser = levelParser();
        QElapsedTimer t;
        t.start();
        runner.submit(req);
        QVERIFY(msSince(t) < 16.0);
        QVERIFY(QTest::qWaitFor([&] { return finishedSpy.count() == 1; }, 3000));
        const MixerProcessResult r = resultAt(finishedSpy, 0);
        QCOMPARE(r.status, MixerProcessResult::Status::SpawnFailed);
        QVERIFY(!r.diagnostic.isEmpty());
        QCOMPARE(runner.cachedLevel(), 55);
        QCOMPARE(runner.cachedMuted(), true);

        MixerProcessRequest good = fakeRequest(0, 0, payload(10, false), QStringLiteral("after"));
        good.parser = levelParser();
        runner.submit(good);
        QVERIFY(QTest::qWaitFor([&] { return finishedSpy.count() == 2; }, 3000));
        QCOMPARE(resultAt(finishedSpy, 1).status, MixerProcessResult::Status::Success);
        QCOMPARE(runner.cachedLevel(), 10);
    }

    // Rapid-fire coalescing: 50 submits while a child runs collapse into at
    // most one trailing request; the LAST value wins and nothing is lost from
    // the accounting (every submit either starts or is coalesced).
    void rapidFireCoalescingLatestWins() {
        MixerProcessRunner runner;
        QSignalSpy startedSpy(&runner, &MixerProcessRunner::requestStarted);
        QSignalSpy finishedSpy(&runner, &MixerProcessRunner::finished);

        MixerProcessRequest first = fakeRequest(300, 0, payload(1, false), QStringLiteral("v1"));
        first.parser = levelParser();
        runner.submit(first);
        QVERIFY(QTest::qWaitFor([&] { return startedSpy.count() == 1; }, 2000));

        // Key-repeat burst while the first child is in flight.
        QElapsedTimer t;
        double maxSubmitMs = 0.0;
        for (int v = 2; v <= 50; ++v) {
            MixerProcessRequest req =
                fakeRequest(300, 0, payload(v, false), QStringLiteral("v%1").arg(v));
            req.parser = levelParser();
            t.restart();
            runner.submit(req);
            maxSubmitMs = qMax(maxSubmitMs, msSince(t));
        }
        QVERIFY2(maxSubmitMs < 16.0,
                 qPrintable(QStringLiteral("burst submit took %1 ms").arg(maxSubmitMs)));

        // The trailing request is the last value, and it always runs.
        QVERIFY(QTest::qWaitFor([&] { return runner.cachedLevel() == 50; }, 5000));
        QVERIFY(QTest::qWaitFor([&] { return !runner.isBusy(); }, 2000));
        const quint64 started = runner.startedCount();
        QVERIFY2(started >= 1 && started <= 2,
                 qPrintable(QStringLiteral("started %1 children").arg(started)));
        QCOMPARE(started + runner.coalescedCount(), quint64(50)); // conservation
        QCOMPARE(quint64(finishedSpy.count()), started);
        const MixerProcessResult last = resultAt(finishedSpy, finishedSpy.count() - 1);
        QCOMPARE(last.status, MixerProcessResult::Status::Success);
        QCOMPARE(last.stdOut, payload(50, false));
        QCOMPARE(runner.cachedMuted(), false);
    }

    // The acceptance timing assertion: with a 200 ms child in flight, every
    // submit()/cached-read call on the caller thread returns well under 16 ms.
    void callerThreadCallsStayUnder16ms() {
        MixerProcessRunner runner;
        QSignalSpy finishedSpy(&runner, &MixerProcessRunner::finished);
        MixerProcessRequest req = fakeRequest(200, 0, payload(7, false), QStringLiteral("load"));
        req.parser = levelParser();
        runner.submit(req);

        QElapsedTimer t;
        double maxSubmitMs = 0.0;
        double maxReadMs = 0.0;
        for (int i = 0; i < 200; ++i) {
            t.restart();
            runner.submit(req); // coalesces; must never spawn on this thread
            maxSubmitMs = qMax(maxSubmitMs, msSince(t));
            t.restart();
            const int level = runner.cachedLevel();
            const bool muted = runner.cachedMuted();
            Q_UNUSED(level);
            Q_UNUSED(muted);
            maxReadMs = qMax(maxReadMs, msSince(t));
        }
        qDebug() << "caller-thread max submit:" << maxSubmitMs << "ms, max read:" << maxReadMs
                 << "ms (200 ms child in flight)";
        QVERIFY2(maxSubmitMs < 16.0,
                 qPrintable(QStringLiteral("submit took %1 ms").arg(maxSubmitMs)));
        QVERIFY2(maxReadMs < 16.0,
                 qPrintable(QStringLiteral("cached read took %1 ms").arg(maxReadMs)));

        QVERIFY(QTest::qWaitFor([&] { return runner.cachedLevel() == 7; }, 5000));
        // The trailing coalesced child may still be running after the first
        // result lands; wait for ALL started children to report back.
        QVERIFY(QTest::qWaitFor(
            [&] { return !runner.isBusy() && quint64(finishedSpy.count()) == runner.startedCount(); },
            3000));
        QVERIFY(runner.startedCount() <= 2);
        QCOMPARE(quint64(finishedSpy.count()), runner.startedCount());
    }

    // Serial execution preserves order; parser-less requests (control probes,
    // mutation echoes) deliver raw output without touching the cache.
    void serialOrderAndNoParserLeavesCache() {
        MixerProcessRunner runner;
        QSignalSpy finishedSpy(&runner, &MixerProcessRunner::finished);
        runner.submit(fakeRequest(30, 0, QStringLiteral("probe-output"), QStringLiteral("A")));
        QVERIFY(QTest::qWaitFor([&] { return finishedSpy.count() == 1; }, 3000));
        runner.submit(fakeRequest(0, 0, QStringLiteral("echo-output"), QStringLiteral("B")));
        QVERIFY(QTest::qWaitFor([&] { return finishedSpy.count() == 2; }, 3000));
        QCOMPARE(resultAt(finishedSpy, 0).tag, QStringLiteral("A"));
        QCOMPARE(resultAt(finishedSpy, 0).stdOut, QStringLiteral("probe-output"));
        QCOMPARE(resultAt(finishedSpy, 1).tag, QStringLiteral("B"));
        QCOMPARE(runner.cachedLevel(), 100); // no parser -> no cache movement
        QCOMPARE(runner.hasHardwareState(), false);
    }

    // Clean shutdown: destroying the runner with a 5 s child in flight and a
    // queued trailing request is bounded and leaks nothing (LSan checks at
    // process exit).
    void shutdownWhileBusyIsBounded() {
        QElapsedTimer t;
        t.start();
        {
            MixerProcessRunner runner;
            runner.submit(fakeRequest(5000, 0, payload(9, false), QStringLiteral("hang")));
            QVERIFY(QTest::qWaitFor([&] { return runner.isBusy(); }, 2000));
            runner.submit(fakeRequest(0, 0, payload(9, false), QStringLiteral("trailing")));
        } // destructor: kill + join + discard pending
        const double teardownMs = msSince(t);
        QVERIFY2(teardownMs < 1000.0,
                 qPrintable(QStringLiteral("teardown took %1 ms").arg(teardownMs)));
    }
};

QTEST_GUILESS_MAIN(MixerProcessRunnerTest)
#include "MixerProcessRunnerTest.moc"