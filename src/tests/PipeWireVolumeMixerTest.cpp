// PipeWire async backend contract (T1 P-5A). Drives the REAL PipeWireVolumeMixer
// against the checked-in compiled mixer-stub copied onto PATH as `wpctl`, so no
// audio hardware is needed. Verifies the acceptance the stall harness measures
// end-to-end, isolated at the backend: no public call blocks >16 ms with a slow
// child, cached reads never spawn, clamp no-ops spawn nothing, rapid repeats
// coalesce (no pileup), and every failure mode rolls back to the last confirmed
// state without wedging the queue. Run under ASan/UBSan with detect_leaks=1.
#include <QTest>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include <functional>

#include "audio/PipeWireVolumeMixer.h"

using namespace audio;

namespace {

bool waitUntil(const std::function<bool()> &pred, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return pred();
}

int logLines(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    return int(f.readAll().count('\n'));
}

// "level muted" text written by the stub.
QPair<int, bool> stubState(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return { -1, false };
    const QStringList parts = QString::fromUtf8(f.readAll()).trimmed().split(' ');
    if (parts.size() < 2)
        return { -1, false };
    return { parts[0].toInt(), parts[1] != QLatin1String("0") };
}

} // namespace

class PipeWireVolumeMixerTest : public QObject {
    Q_OBJECT

    // Scratch rig: a bin dir holding the stub as `wpctl`, placed alone on PATH
    // so the backend's child resolves to the stub (or is absent). Env selects
    // delay / fault mode / log / state exactly like the stall orchestrator.
    struct Rig {
        QTemporaryDir dir;
        QString bin, log, state;
        bool ok = false;
        Rig(int delayMs, const char *mode = "normal", bool withWpctl = true)
        {
            bin = dir.path() + QStringLiteral("/bin");
            QDir().mkpath(bin);
            log = dir.path() + QStringLiteral("/mixer.log");
            state = dir.path() + QStringLiteral("/state.txt");
            if (withWpctl) {
                ok = QFile::copy(QStringLiteral(MIXER_STUB_PATH), bin + QStringLiteral("/wpctl"));
                QFile::setPermissions(bin + QStringLiteral("/wpctl"),
                                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            } else {
                ok = true; // intentionally empty bin -> SpawnFailed
            }
            qputenv("PATH", bin.toUtf8());
            qputenv("TORTURE_DELAY_MS", QByteArray::number(delayMs));
            qputenv("TORTURE_MIXER_LOG", log.toUtf8());
            qputenv("TORTURE_STUB_STATE", state.toUtf8());
            qputenv("TORTURE_STUB_MODE", mode);
        }
    };

    QString m_origPath;

    // All submitted work has run and every completion was processed, stable
    // across a quiet window. The runner is async in BOTH directions: right
    // after submit() the worker may not have registered the child yet
    // (isBusy() still false, startedCount() still 0), and a mutation's
    // completion arms a refresh on the owner thread only when events are
    // pumped. So settle = continuous event pumping + startedCount stability
    // + !isBusy for a full quiet window. startedCount counts start attempts
    // (incl. SpawnFailed), so the zero-guard also terminates fault runs.
    bool waitSettle(PipeWireVolumeMixer &m, int timeoutMs = 6000)
    {
        QElapsedTimer t;
        t.start();
        quint64 lastStarted = m.startedCount();
        QElapsedTimer quiet;
        quiet.start();
        while (t.elapsed() < timeoutMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            const quint64 started = m.startedCount();
            if (started == 0 || m.isBusy() || started != lastStarted) {
                lastStarted = started;
                quiet.restart();
            } else if (quiet.elapsed() >= 100) {
                return true;
            }
            QThread::msleep(1);
        }
        return false;
    }

private Q_SLOTS:
    void initTestCase() { m_origPath = QString::fromUtf8(qgetenv("PATH")); }
    void cleanup() { qputenv("PATH", m_origPath.toUtf8()); }
    void cleanupTestCase() { qputenv("PATH", m_origPath.toUtf8()); }

    // Construction spawns nothing and keeps the historical cached initial state.
    void constructionSilentAndInstant()
    {
        Rig rig(200);
        QVERIFY(rig.ok);
        QElapsedTimer t;
        t.start();
        PipeWireVolumeMixer m;
        QVERIFY(t.elapsed() < 16);
        QCOMPARE(m.level(), 100);
        QCOMPARE(m.isMuted(), false);
        QCOMPARE(logLines(rig.log), 0);
        QCOMPARE(m.startedCount(), quint64(0));
    }

    // Cached reads never spawn, even with a slow child available.
    void cachedReadsNeverSpawn()
    {
        Rig rig(200);
        PipeWireVolumeMixer m;
        for (int i = 0; i < 50; ++i) {
            QCOMPARE(m.level(), 100);
            QCOMPARE(m.isMuted(), false);
        }
        QCOMPARE(logLines(rig.log), 0);
        QCOMPARE(m.startedCount(), quint64(0));
    }

    // The core stall gate: every public call returns <16 ms while a 200 ms
    // child is in flight.
    void publicCallsUnder16WithSlowChild()
    {
        Rig rig(200);
        PipeWireVolumeMixer m;
        QElapsedTimer t;
        t.start();
        QVERIFY(m.setLevel(85));
        QVERIFY(t.elapsed() < 16);
        // Child is now sleeping 200 ms on the worker thread; reads and further
        // mutations must not block on it.
        for (int i = 0; i < 5; ++i) {
            t.restart();
            m.level();
            m.isMuted();
            QVERIFY(t.elapsed() < 16);
        }
        t.restart();
        QVERIFY(m.setMuted(true));
        QVERIFY(t.elapsed() < 16);
        t.restart();
        QVERIFY(m.setLevel(70));
        QVERIFY(t.elapsed() < 16);
        QVERIFY(waitSettle(m));
        QCOMPARE(m.level(), 70);
    }

    // Step down/up and clamps settle to the requested value; a genuine no-op at
    // the clamp spawns nothing.
    void stepAndClampSemantics()
    {
        Rig rig(0);
        PipeWireVolumeMixer m;
        // Clamp-at-max no-op: cache already 100.
        QVERIFY(m.setLevel(150));
        QCOMPARE(m.level(), 100);
        QCOMPARE(logLines(rig.log), 0);
        // Step down.
        QVERIFY(m.setLevel(85));
        QVERIFY(waitSettle(m));
        QCOMPARE(m.level(), 85);
        QCOMPARE(stubState(rig.state).first, 85);
        QCOMPARE(logLines(rig.log), 2); // set-volume + get-volume refresh
        // Step up.
        QVERIFY(m.setLevel(90));
        QVERIFY(waitSettle(m));
        QCOMPARE(m.level(), 90);
        // Clamp at 0.
        QVERIFY(m.setLevel(-5));
        QVERIFY(waitSettle(m));
        QCOMPARE(m.level(), 0);
        QCOMPARE(stubState(rig.state).first, 0);
        const int before = logLines(rig.log);
        QVERIFY(m.setLevel(0)); // no-op at 0
        QCOMPARE(logLines(rig.log), before);
        // Back to max, then no-op at max.
        QVERIFY(m.setLevel(100));
        QVERIFY(waitSettle(m));
        QCOMPARE(m.level(), 100);
        const int beforeMax = logLines(rig.log);
        QVERIFY(m.setLevel(100));
        QCOMPARE(logLines(rig.log), beforeMax);
    }

    // Mute toggle is a single invocation (no read-back), matches pre-fix, and a
    // repeated no-op mute spawns nothing.
    void muteToggleSemantics()
    {
        Rig rig(0);
        PipeWireVolumeMixer m;
        QSignalSpy spy(&m, &PipeWireVolumeMixer::stateChanged);
        QVERIFY(m.setMuted(true));
        QVERIFY(waitSettle(m));
        QCOMPARE(m.isMuted(), true);
        QCOMPARE(stubState(rig.state).second, true);
        QCOMPARE(logLines(rig.log), 1); // set-mute only
        QVERIFY(m.setMuted(false));
        QVERIFY(waitSettle(m));
        QCOMPARE(m.isMuted(), false);
        QCOMPARE(logLines(rig.log), 2);
        QVERIFY(m.setMuted(false)); // no-op
        QCOMPARE(logLines(rig.log), 2);
        QVERIFY(spy.count() >= 2);
    }

    // Mute survives a subsequent level change (refresh confirms level only).
    void muteSurvivesLevelChange()
    {
        Rig rig(0);
        PipeWireVolumeMixer m;
        QVERIFY(m.setMuted(true));
        QVERIFY(waitSettle(m));
        QVERIFY(m.setLevel(80));
        QVERIFY(waitSettle(m));
        QCOMPARE(m.level(), 80);
        QCOMPARE(m.isMuted(), true);
        QCOMPARE(stubState(rig.state), qMakePair(80, true));
    }

    // Rapid key repeats coalesce latest-wins: no pileup, last value wins, and
    // every started child is accounted for in the argv log.
    void rapidBurstCoalesces()
    {
        Rig rig(50);
        PipeWireVolumeMixer m;
        const int n = 20;
        int last = 100;
        for (int i = 0; i < n; ++i) {
            last = 100 - (i + 1) * 5; // 95,90,...,5
            QVERIFY(m.setLevel(last));
        }
        QVERIFY(waitSettle(m));
        QCOMPARE(m.level(), last);
        QCOMPARE(stubState(rig.state).first, last);
        // No pileup: at most a couple of mutations plus one trailing refresh.
        QVERIFY(m.startedCount() <= 4);
        QVERIFY(m.coalescedCount() > 0);
        // Conservation law: 20 mutation submits + exactly one re-armed refresh
        // (only the last mutation completes with seq == m_seq). Exact start
        // counts are scheduler-nondeterministic; the sum is not.
        QCOMPARE(m.startedCount() + m.coalescedCount(), quint64(21));
        // Every started child logged exactly one argv line.
        QCOMPARE(quint64(logLines(rig.log)), m.startedCount());
    }

    // Nonzero exit rolls back to the last confirmed state; queue stays usable.
    void exit1RollsBack()
    {
        Rig rig(20, "exit1");
        PipeWireVolumeMixer m;
        QSignalSpy spy(&m, &PipeWireVolumeMixer::stateChanged);
        QVERIFY(m.setLevel(85));
        QCOMPARE(m.level(), 85); // optimistic
        QVERIFY(waitSettle(m));
        QCOMPARE(m.level(), 100); // rolled back to confirmed initial
        QCOMPARE(logLines(rig.log), 1); // mutation only, no refresh on failure
        QVERIFY(spy.count() >= 1);
        // Still usable: another request is accepted and also rolls back cleanly.
        QVERIFY(m.setLevel(90));
        QVERIFY(waitSettle(m));
        QCOMPARE(m.level(), 100);
    }

    // Unparseable echo (exit 0) is treated as failure and rolls back.
    void garbageRollsBack()
    {
        Rig rig(20, "garbage");
        PipeWireVolumeMixer m;
        QVERIFY(m.setLevel(85));
        QVERIFY(waitSettle(m));
        QCOMPARE(m.level(), 100);
        QCOMPARE(logLines(rig.log), 1);
    }

    // Missing executable: SpawnFailed rolls back, nothing spawns, no wedge.
    void missingBinaryRollsBack()
    {
        Rig rig(20, "normal", /*withWpctl=*/false);
        PipeWireVolumeMixer m;
        QVERIFY(m.setLevel(85));
        QVERIFY(waitSettle(m));
        QCOMPARE(m.level(), 100);
        QCOMPARE(logLines(rig.log), 0);
        QElapsedTimer t;
        t.start();
        QVERIFY(m.setLevel(90));
        QVERIFY(t.elapsed() < 16);
        QVERIFY(waitSettle(m));
        QCOMPARE(m.level(), 100);
    }

    // A child slower than the 1000 ms budget is killed (TimedOut) and the cache
    // rolls back; the queue survives for the next request.
    void timeoutRollsBack()
    {
        Rig rig(1500);
        PipeWireVolumeMixer m;
        QElapsedTimer t;
        t.start();
        QVERIFY(m.setLevel(85));
        QVERIFY(t.elapsed() < 16); // submit does not wait
        QVERIFY(waitSettle(m, 3000));
        QCOMPARE(m.level(), 100); // TimedOut -> rollback
        QVERIFY(t.elapsed() < 2500); // bounded by the 1000 ms budget, not 1500
    }

    // A refresh armed by an older mutation must not resurrect its value over a
    // newer press submitted while the read was in flight.
    void staleRefreshNeverResurrects()
    {
        Rig rig(100);
        PipeWireVolumeMixer m;
        QVERIFY(m.setLevel(85));
        // Wait until the mutation finished and its refresh is in flight (2 lines).
        QVERIFY(waitUntil([&] { return logLines(rig.log) >= 2; }, 3000));
        QVERIFY(m.setLevel(70)); // newer mutation while the 85-refresh sleeps
        QVERIFY(waitSettle(m));
        QCOMPARE(m.level(), 70);
        QCOMPARE(stubState(rig.state).first, 70);
    }
};

QTEST_MAIN(PipeWireVolumeMixerTest)
#include "PipeWireVolumeMixerTest.moc"
