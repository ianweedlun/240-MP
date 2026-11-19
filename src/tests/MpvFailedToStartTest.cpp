// R2 follow-up (issue #110): QProcess::FailedToStart after a SUCCESSFUL
// mpvbin::locate() must emit a synthetic playbackEnded("failed").
//
// The defect: Qt emits errorOccurred(FailedToStart) — and NO finished() — when
// the child cannot be spawned (exec-format error, missing libs, ENOENT race
// between locate and start). MpvController connected only finished(), so the
// Player view waited for a playbackEnded that never came: stuck UI, no error
// path, no goBack. The fix mirrors the existing missing-bin / handoff-refusal
// bail-outs: tear down, then a deferred synthetic end with reason "failed".
//
// Test seam (no product test hooks): the REAL locator is pointed at an
// executable-but-invalid binary. locate() tier 1 is <applicationDirPath>/mpv
// (asserted absent here), tier 2 is QStandardPaths::findExecutable("mpv") over
// PATH — so a private PATH directory holding a chmod +x garbage file named
// "mpv" is returned by locate() and then fails execve (ENOEXEC) in start().
// That is the exact defect shape: locate() succeeds, spawn fails.
//
// The core cases need NO mpv installed, so every CI lane runs this gate for
// real. Only the relaunch-after-failure case needs real mpv + ffmpeg and
// QSKIPs where they are absent. KNOWN-BAD: against the pre-fix controller the
// failedToStart cases fail bounded (spinUntil timeout, never an infinite hang).
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QThread>
#include <QTest>
#include <QString>
#include <QStringList>

#include "AppCore.h"
#include "player/MpvController.h"
#include "util/MpvLocator.h"

namespace {

QString g_appRoot;      // worktree root (scripts/ live here)
QString g_fixture;      // generated 8 s clip (empty when mpv/ffmpeg absent)

// Spin the event loop until pred() is true or ms elapse. Returns pred()'s value.
template <typename Pred>
bool spinUntil(Pred pred, int ms) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (pred()) return true;
        QThread::msleep(2);
    }
    QCoreApplication::processEvents();
    return pred();
}

// Spin the event loop for the FULL duration (no predicate): lets any stray
// deferred signal (double emission, late finished()) surface before asserting.
void spinFor(int ms) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    }
    QCoreApplication::processEvents();
}

bool writeConfig(const QString &dataRoot) {
    QJsonObject app;
    app["mpv_video_args"] = QStringLiteral("--vo=null --ao=null --hwdec=no");
    app["screensaver_timeout"] = QStringLiteral("OFF");
    app["auto_fit"] = QStringLiteral("Off");
    QJsonObject root; root["app"] = app;
    QFile f(dataRoot + QStringLiteral("/config.json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    f.close();
    return true;
}

QString socketPath() {
    return QDir::tempPath() + QStringLiteral("/240mp-mpv.sock");
}

// True if ANY child of this test process named "mpv" still exists in /proc —
// live or zombie. A FailedToStart bail must leave nothing behind (Linux-only).
bool anyMpvChild() {
#ifndef Q_OS_LINUX
    return false;
#else
    const qint64 self = QCoreApplication::applicationPid();
    QDir proc(QStringLiteral("/proc"));
    const QStringList entries = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &e : entries) {
        bool ok = false;
        const qint64 pid = e.toLongLong(&ok);
        if (!ok) continue;
        QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
        if (!comm.open(QIODevice::ReadOnly)) continue;
        if (comm.readAll().trimmed() != QByteArrayLiteral("mpv")) continue;
        QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
        if (!stat.open(QIODevice::ReadOnly)) continue;
        const QByteArray s = stat.readAll();
        const int close = s.lastIndexOf(')');
        if (close < 0) continue;
        const QList<QByteArray> f = s.mid(close + 2).split(' ');
        if (f.size() > 1 && f[1].toLongLong() == self) return true;
    }
    return false;
#endif
}

// Capture every playbackEnded emission with its full payload.
struct EndedSpy : public QObject {
    Q_OBJECT
public:
    struct End { int pos; int dur; QString reason; };
    QList<End> ends;
public slots:
    void onEnded(int pos, int dur, const QString &reason) {
        ends.append({pos, dur, reason});
    }
};

// Scoped PATH override: restores the previous value even when a QVERIFY
// aborts the test function mid-body.
struct PathGuard {
    QByteArray old;
    explicit PathGuard(const QString &newPath) : old(qgetenv("PATH")) {
        qputenv("PATH", newPath.toUtf8());
    }
    ~PathGuard() { qputenv("PATH", old); }
};

// An executable-but-invalid "mpv": locate()'s isExecutable() check passes,
// execve() fails with ENOEXEC -> QProcess::FailedToStart, no finished().
QString makeBadMpv(const QString &dir) {
    const QString p = dir + QStringLiteral("/mpv");
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    f.write("240mp failed-to-start fixture: executable bit set, not a valid binary\n");
    f.close();
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                     QFile::ReadGroup | QFile::ExeGroup |
                     QFile::ReadOther | QFile::ExeOther);
    return p;
}

// The seam is only valid if locate() actually returns the planted binary:
// no sibling mpv may shadow tier 1, and PATH must drive tier 2.
bool seamValid(const QString &expected) {
    return !QFileInfo(QCoreApplication::applicationDirPath() + "/mpv").exists()
        && mpvbin::locate() == expected;
}

} // namespace

class MpvFailedToStartTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        g_appRoot = QString::fromUtf8(MP240_REPO_ROOT);
        // Fixture for the relaunch case only — the core gate runs without mpv.
        if (mpvbin::locate().isEmpty()
            || QStandardPaths::findExecutable("ffmpeg").isEmpty())
            return;
        QTemporaryDir *gen = new QTemporaryDir();   // lives for the whole run
        if (!gen->isValid()) return;
        const QString fixture = gen->path() + QStringLiteral("/fixture.mp4");
        QProcess ff;
        ff.start(QStringLiteral("ffmpeg"),
                 {"-y", "-f", "lavfi", "-i", "testsrc=duration=8:size=320x240:rate=10",
                  "-f", "lavfi", "-i", "sine=frequency=440:duration=8",
                  "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac",
                  "-shortest", fixture});
        if (ff.waitForFinished(30000) && ff.exitCode() == 0 && QFile::exists(fixture))
            g_fixture = fixture;
    }

    // CORE GATE: a spawn failure after a successful locate() emits EXACTLY ONE
    // playbackEnded(0, 0, "failed"), bounded, with no hang, no socket litter,
    // no child left behind, and no late duplicate. x10: every bail must reset
    // the controller state for the next launch. Pre-fix this fails bounded at
    // the spinUntil (the signal never comes) — the KNOWN-BAD proof.
    void failedToStartEmitsExactlyOneFailed() {
        QTemporaryDir data; QVERIFY(data.isValid());
        QVERIFY(writeConfig(data.path()));
        AppCore core(g_appRoot, data.path());
        MpvController mpv(g_appRoot, data.path(), &core, nullptr);
        EndedSpy spy;
        connect(&mpv, &MpvController::playbackEnded, &spy, &EndedSpy::onEnded);

        for (int i = 0; i < 10; ++i) {
            spy.ends.clear();
            QTemporaryDir binDir; QVERIFY(binDir.isValid());
            const QString bad = makeBadMpv(binDir.path());
            QVERIFY(!bad.isEmpty());
            PathGuard guard(binDir.path());
            QVERIFY2(seamValid(bad),
                     "locator seam invalid: sibling mpv shadows tier 1 or PATH tier missed");

            mpv.loadAndPlay(QStringLiteral("/nonexistent/240mp-failedtostart.mp4"),
                            0.0f, 0, -2, {}, {}, false, -1,
                            0.0f, QString(), false, QString(), false, {},
                            0.0f, false, {});
            QElapsedTimer t; t.start();
            const bool ended = spinUntil([&]{ return !spy.ends.isEmpty(); }, 5000);
            QVERIFY2(ended, qPrintable(QStringLiteral(
                "iteration %1: FailedToStart produced no playbackEnded — view waits forever").arg(i)));
            QVERIFY2(t.elapsed() < 5000, "synthetic end not bounded");
            QCOMPARE(spy.ends.size(), 1);
            QCOMPARE(spy.ends.at(0).reason, QStringLiteral("failed"));
            QCOMPARE(spy.ends.at(0).pos, 0);
            QCOMPARE(spy.ends.at(0).dur, 0);
            // No late duplicate (finished() must not follow FailedToStart,
            // and the deferred singleShot fires exactly once).
            spinFor(300);
            QCOMPARE(spy.ends.size(), 1);
            QVERIFY2(!QFile::exists(socketPath()), "socket litter after FailedToStart bail");
            QVERIFY2(!anyMpvChild(), "live/zombie mpv child after FailedToStart bail");
        }
    }

    // RECOVERY: after a FailedToStart bail the controller is not wedged — a
    // relaunch with a working mpv plays normally and ends on demand. Proves
    // the teardown (process null-out, timers, IPC) left a reusable object.
    void failedToStartThenRelaunchPlays() {
        if (g_fixture.isEmpty())
            QSKIP("mpv/ffmpeg not available — relaunch-after-failure case skipped");
        QTemporaryDir data; QVERIFY(data.isValid());
        QVERIFY(writeConfig(data.path()));
        AppCore core(g_appRoot, data.path());
        MpvController mpv(g_appRoot, data.path(), &core, nullptr);
        EndedSpy spy;
        connect(&mpv, &MpvController::playbackEnded, &spy, &EndedSpy::onEnded);
        int positions = 0;
        connect(&mpv, &MpvController::positionChanged, &mpv, [&](int){ ++positions; });

        {
            QTemporaryDir binDir; QVERIFY(binDir.isValid());
            const QString bad = makeBadMpv(binDir.path());
            QVERIFY(!bad.isEmpty());
            PathGuard guard(binDir.path());
            QVERIFY(seamValid(bad));
            mpv.loadAndPlay(g_fixture, 0.0f, 0, -2, {}, {}, false, -1,
                            0.0f, QString(), false, QString(), false, {},
                            0.0f, false, {});
            QVERIFY(spinUntil([&]{ return !spy.ends.isEmpty(); }, 5000));
            QCOMPARE(spy.ends.size(), 1);
            QCOMPARE(spy.ends.at(0).reason, QStringLiteral("failed"));
        }   // PathGuard restores the real PATH here

        QVERIFY2(!mpvbin::locate().isEmpty(), "real mpv vanished after PATH restore");
        mpv.loadAndPlay(g_fixture, 0.0f, 0, -2, {}, {}, false, -1,
                        0.0f, QString(), false, QString(), false, {},
                        0.0f, false, {});
        QVERIFY2(spinUntil([&]{ return positions >= 2; }, 5000),
                 "relaunch after FailedToStart did not play — controller wedged");
        QCOMPARE(spy.ends.size(), 1);   // still only the synthetic failed end
        mpv.forceStop();
        QVERIFY(spinUntil([&]{ return spy.ends.size() >= 2; }, 3000));
        QCOMPARE(spy.ends.size(), 2);
        QCOMPARE(spy.ends.at(1).reason, QStringLiteral("stopped"));
        QVERIFY(spinUntil([&]{ return !anyMpvChild(); }, 2000));
    }

    // CONTROL: the pre-existing missing-bin bail (locate() empty) must keep
    // its deferred synthetic playbackEnded("stopped") — the fix mirrors that
    // family's emission shape and must not disturb it.
    void missingBinStillStopped() {
        QTemporaryDir data; QVERIFY(data.isValid());
        QVERIFY(writeConfig(data.path()));
        AppCore core(g_appRoot, data.path());
        MpvController mpv(g_appRoot, data.path(), &core, nullptr);
        EndedSpy spy;
        connect(&mpv, &MpvController::playbackEnded, &spy, &EndedSpy::onEnded);

        QTemporaryDir emptyDir; QVERIFY(emptyDir.isValid());
        PathGuard guard(emptyDir.path());   // no mpv anywhere on PATH
        QVERIFY2(mpvbin::locate().isEmpty(),
                 "locator seam invalid: mpv still locatable with empty PATH");
        mpv.loadAndPlay(QStringLiteral("/nonexistent/240mp-missingbin.mp4"),
                        0.0f, 0, -2, {}, {}, false, -1,
                        0.0f, QString(), false, QString(), false, {},
                        0.0f, false, {});
        QVERIFY(spinUntil([&]{ return !spy.ends.isEmpty(); }, 5000));
        spinFor(300);
        QCOMPARE(spy.ends.size(), 1);
        QCOMPARE(spy.ends.at(0).reason, QStringLiteral("stopped"));
        QCOMPARE(spy.ends.at(0).pos, 0);
        QCOMPARE(spy.ends.at(0).dur, 0);
    }
};

QTEST_MAIN(MpvFailedToStartTest)
#include "MpvFailedToStartTest.moc"
