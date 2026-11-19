// R-2 product tests against the REAL MpvController + REAL mpv (SPEC-1/2/4).
//
// The defect: a Back pressed before mpv's IPC socket reaches ConnectedState was
// dropped at the sendCommand gate, stranding playback (forever with loop on).
// The fix makes the quit-intent set {ESC, BS, STOP} guaranteed-delivery in
// sendKey: pre-connect -> forceStop() (reap + playbackEnded("stopped"));
// post-connect -> byte-identical keypress (lua semantics preserved).
//
// These are behavioral tests, not source snapshots: they spawn real mpv, drive
// sendKey at a controlled IPC phase, and observe playbackEnded / continued
// playback / child reaping. They QSKIP when mpv or ffmpeg is unavailable so CI
// lanes without a media stack stay green; the lab (mpv present) runs them for
// real. The offscreen QPA platform is set via the CMake test ENVIRONMENT
// property (QTEST_MAIN constructs the application before main body code runs).
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QThread>
#include <QTest>
#include <QString>
#include <QStringList>

#include "AppCore.h"
#include "player/MpvController.h"
#include "util/MpvLocator.h"

namespace {

QString g_fixture;          // path to the generated 8 s clip
QString g_appRoot;          // worktree root (scripts/ live here)

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

// Spin the event loop for the FULL duration (no predicate). spinUntil with an
// always-true pred returns on the first iteration — it is NOT a wait; lua
// binding changes (menu open / saver dismiss) need real elapsed time to land.
void spinFor(int ms) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    }
    QCoreApplication::processEvents();
}

bool writeConfig(const QString &dataRoot, const QString &screensaver) {
    QJsonObject app;
    app["mpv_video_args"] = QStringLiteral("--vo=null --ao=null --hwdec=no");
    app["screensaver_timeout"] = screensaver;
    app["auto_fit"] = QStringLiteral("Off");
    QJsonObject root; root["app"] = app;
    QFile f(dataRoot + QStringLiteral("/config.json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    f.close();
    return true;
}

// The controller's per-instance IPC socket path (T2, issue #111: pid+seq
// token under QDir::tempPath(), published via the getter). SPEC-1 asserts it
// is removed once the child is reaped — no litter for the next launch.
QString socketPath(const MpvController &mpv) {
    return mpv.socketPath();
}

// True if ANY child of this test process named "mpv" still exists in /proc —
// live or zombie. QProcess reaps on finished, so a correctly forceStop'd child
// leaves nothing behind. (Linux-only; the real-mpv tests QSKIP elsewhere.)
bool anyMpvChild() {
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
        // fields after ") ": state ppid ...
        const QList<QByteArray> f = s.mid(close + 2).split(' ');
        if (f.size() > 1 && f[1].toLongLong() == self) return true;
    }
    return false;
}

// Capture playbackEnded reasons emitted by the controller.
struct EndedSpy : public QObject {
    Q_OBJECT
public:
    QStringList reasons;
public slots:
    void onEnded(int, int, const QString &reason) { reasons.append(reason); }
};

} // namespace

class MpvQuitIntentTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        g_appRoot = QString::fromUtf8(MP240_REPO_ROOT);
        const QString mpv = mpvbin::locate();
        if (mpv.isEmpty())
            QSKIP("mpv not available on this host — real-mpv quit-intent tests skipped");
        // Generate a small 8 s clip once (ffmpeg ships with the lab media stack).
        QTemporaryDir *gen = new QTemporaryDir();   // lives for the whole run
        if (!gen->isValid()) QSKIP("cannot create temp dir for fixture");
        g_fixture = gen->path() + QStringLiteral("/fixture.mp4");
        QProcess ff;
        ff.start(QStringLiteral("ffmpeg"),
                 {"-y", "-f", "lavfi", "-i", "testsrc=duration=8:size=320x240:rate=10",
                  "-f", "lavfi", "-i", "sine=frequency=440:duration=8",
                  "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac",
                  "-shortest", g_fixture});
        if (!ff.waitForFinished(30000) || ff.exitCode() != 0 || !QFile::exists(g_fixture))
            QSKIP("ffmpeg could not generate the test fixture — skipped");
    }

    // SPEC-1: synchronous pre-connect invariant. A quit-intent key delivered
    // before IPC connects must reap mpv (forceStop -> playbackEnded("stopped")
    // EXACTLY once), remove the socket, and leave zero live/zombie mpv children.
    // x100 for each of ESC / BS / STOP. Same call stack, no event-loop turn, so
    // the connect timer cannot tick — this tests the invariant, not a window.
    void spec1_preconnectQuitIntent() {
        QTemporaryDir data; QVERIFY(data.isValid());
        QVERIFY(writeConfig(data.path(), QStringLiteral("OFF")));
        AppCore core(g_appRoot, data.path());
        MpvController mpv(g_appRoot, data.path(), &core, nullptr);
        EndedSpy spy;
        connect(&mpv, &MpvController::playbackEnded, &spy, &EndedSpy::onEnded);

        const QStringList keys = {QStringLiteral("ESC"), QStringLiteral("BS"), QStringLiteral("STOP")};
        for (const QString &key : keys) {
            int stopped = 0;
            for (int i = 0; i < 100; ++i) {
                spy.reasons.clear();
                mpv.loadAndPlay(g_fixture, 0.0f, 0, -2, {}, {}, false, -1,
                                0.0f, QString(), false, QString(), false, {},
                                0.0f, false, {});
                mpv.sendKey(key);   // SAME synchronous step: IPC still unconnected
                const bool ended = spinUntil([&]{ return !spy.reasons.isEmpty(); }, 3000);
                QVERIFY2(ended, qPrintable(QStringLiteral("%1 iteration %2: pre-connect quit intent did not reap mpv").arg(key).arg(i)));
                QCOMPARE(spy.reasons.size(), 1);
                QCOMPARE(spy.reasons.at(0), QStringLiteral("stopped"));
                QVERIFY2(!QFile::exists(socketPath(mpv)), "socket litter after forceStop");
                QVERIFY2(spinUntil([&]{ return !anyMpvChild(); }, 2000), "live/zombie mpv child after forceStop");
                ++stopped;
            }
            QCOMPARE(stopped, 100);
        }
    }

    // SPEC-2a: post-connect OSC-menu preservation. Once IPC is connected, ESC is
    // a plain keypress: with the OSC menu open it closes the menu (lua menu-esc)
    // and playback CONTINUES; a second ESC (menu closed) quits. Proves the fix
    // did not turn post-connect Back into a forceStop.
    void spec2_postconnectMenuPreserved() {
        QTemporaryDir data; QVERIFY(data.isValid());
        QVERIFY(writeConfig(data.path(), QStringLiteral("OFF")));
        AppCore core(g_appRoot, data.path());
        MpvController mpv(g_appRoot, data.path(), &core, nullptr);
        EndedSpy spy;
        connect(&mpv, &MpvController::playbackEnded, &spy, &EndedSpy::onEnded);
        int positions = 0;
        connect(&mpv, &MpvController::positionChanged, &mpv, [&](int){ ++positions; });

        mpv.loadAndPlay(g_fixture, 0.0f, 0, -2, {}, {}, false, -1,
                        0.0f, QString(), false, QString(), false, {},
                        0.0f, false, {});
        // Wait for connected playback (time-pos events flow only once connected).
        QVERIFY(spinUntil([&]{ return positions >= 2; }, 5000));
        const int beforeMenu = positions;
        mpv.sendKey(QStringLiteral("UP"));      // open the OSC menu (open_menu_up)
        spinFor(400);   // let toggle_menu's lua callback install forced menu-esc
        mpv.sendKey(QStringLiteral("ESC"));     // menu-esc: closes menu, keeps playing
        // Playback must CONTINUE (more time-pos events), not end.
        const bool stillPlaying = spinUntil([&]{ return positions > beforeMenu + 1; }, 2000);
        QVERIFY(stillPlaying);
        QVERIFY(spy.reasons.isEmpty());
        spinFor(300);   // let the close callback remove forced menu-esc first
        mpv.sendKey(QStringLiteral("ESC"));     // menu closed now: ESC quits
        QVERIFY(spinUntil([&]{ return !spy.reasons.isEmpty(); }, 3000));
        QCOMPARE(spy.reasons.last(), QStringLiteral("stopped"));
    }

    // SPEC-2b: post-connect screensaver preservation. The shipped saver
    // (mpv-screensaver.lua) activates after screensaver_timeout seconds PAUSED
    // and force-binds the dismiss keys: the first ESC only wakes it (consumed,
    // mpv stays alive), the next ESC acts normally and quits. A fix that sent
    // "quit" or forceStop'd post-connect would fail the wake step.
    void spec2_postconnectScreensaverPreserved() {
        QTemporaryDir data; QVERIFY(data.isValid());
        QVERIFY(writeConfig(data.path(), QStringLiteral("3")));   // 3 s paused -> saver
        AppCore core(g_appRoot, data.path());
        MpvController mpv(g_appRoot, data.path(), &core, nullptr);
        EndedSpy spy;
        connect(&mpv, &MpvController::playbackEnded, &spy, &EndedSpy::onEnded);
        int positions = 0;
        connect(&mpv, &MpvController::positionChanged, &mpv, [&](int){ ++positions; });

        mpv.loadAndPlay(g_fixture, 0.0f, 0, -2, {}, {}, false, -1,
                        0.0f, QString(), false, QString(), false, {},
                        0.0f, false, {});
        QVERIFY(spinUntil([&]{ return positions >= 2; }, 5000));
        mpv.sendKey(QStringLiteral("SPACE"));   // pause (mpv default binding)
        const int atPause = positions;
        spinFor(600);
        QVERIFY2(positions <= atPause + 1, "SPACE did not pause playback");
        // pause_monitor ticks 1 s; paused_sec >= 3 activates the saver. The 3 s
        // timeout also leaves a safe wake->resume window: after dismiss the
        // saver cannot re-arm for another 3 s of paused time.
        spinFor(4500);
        QVERIFY(spy.reasons.isEmpty());         // still alive while the saver runs
        const int beforeWake = positions;
        mpv.sendKey(QStringLiteral("ESC"));     // ss-dismiss-ESC: wakes, consumed
        // Real elapsed time: the dismiss lua callback must RUN and remove the
        // forced dismiss bindings before SPACE is dispatched, or SPACE is
        // consumed too (keys bind at dispatch; callbacks run afterwards).
        spinFor(300);
        QVERIFY2(spy.reasons.isEmpty(), "first ESC quit instead of waking the screensaver");
        mpv.sendKey(QStringLiteral("SPACE"));   // resume: proves mpv survived the wake
        QVERIFY2(spinUntil([&]{ return positions > beforeWake; }, 2000),
                 "playback did not resume after screensaver wake");
        mpv.sendKey(QStringLiteral("ESC"));     // saver gone: ESC quits normally
        QVERIFY(spinUntil([&]{ return !spy.reasons.isEmpty(); }, 3000));
        QCOMPARE(spy.reasons.size(), 1);
        QCOMPARE(spy.reasons.last(), QStringLiteral("stopped"));
    }

    // SPEC-4: loop-on infinite strand. With loop enabled a pre-connect Back must
    // still reap mpv promptly; without the fix mpv loops forever and never ends.
    void spec4_loopOnPreconnectBounded() {
        QTemporaryDir data; QVERIFY(data.isValid());
        QVERIFY(writeConfig(data.path(), QStringLiteral("OFF")));
        AppCore core(g_appRoot, data.path());
        MpvController mpv(g_appRoot, data.path(), &core, nullptr);
        EndedSpy spy;
        connect(&mpv, &MpvController::playbackEnded, &spy, &EndedSpy::onEnded);

        for (int i = 0; i < 10; ++i) {
            spy.reasons.clear();
            mpv.loadAndPlay(g_fixture, 0.0f, 0, -2, {}, {}, /*loop=*/true, -1,
                            0.0f, QString(), false, QString(), false, {},
                            0.0f, false, {});
            mpv.sendKey(QStringLiteral("ESC"));
            QElapsedTimer t; t.start();
            const bool ended = spinUntil([&]{ return !spy.reasons.isEmpty(); }, 3000);
            QVERIFY(ended);
            QCOMPARE(spy.reasons.last(), QStringLiteral("stopped"));
            QVERIFY(t.elapsed() < 3000);   // bounded, not an 8 s EOF / infinite loop
            QVERIFY(spinUntil([&]{ return !anyMpvChild(); }, 2000));
        }
    }
};

QTEST_MAIN(MpvQuitIntentTest)
#include "MpvQuitIntentTest.moc"
