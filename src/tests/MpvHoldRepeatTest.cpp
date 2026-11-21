// Held-key seek coalescing: product tests against the REAL MpvController +
// REAL mpv + the REAL YouTube Player.qml key handlers.
//
// Owner decision (2026-11-20): the playback-speed ramp was an unrequested
// feature and was removed. Held LEFT/RIGHT now coalesce into periodic exact
// seeks (one accumulated seek per ~400 ms) on every key path; no playback
// speed is ever changed and no `Set property: speed` is ever issued.
//
// The defect this suite guards (#124): the remote's default directional keys
// reach QML through InputManager::eventFilter's return-false path, carrying
// whatever autorepeat the OS/EGLFS supplies — absent on some remotes,
// mislabeled as isAutoRepeat=false fresh presses on others, variable-rate
// where present. MpvController owns the LEFT/RIGHT hold: the first keydown
// feeds SeekHoldPolicy, a private 400 ms + 100 ms timer synthesizes every
// repeat tick, further keydowns for a held key are ignored, and sendKeyUp ends
// the hold. These trials prove the coalesced shape is identical across remote
// behaviors (absent / mislabeled / variable-rate / clean / press-release
// pairs) and that no speed command ever reaches mpv.
//
// Observation contract (probed on mpv 0.41, --msg-level=all=v):
//   [t][d][cplayer] Run command: seek, ... target="S" ...
//   [t][v][cplayer] Set property: speed=V   (must NEVER appear)
//   [t][d][cplayer] Run command: keypress, ...
//
// QSKips when mpv/ffmpeg is unavailable (CI lanes without a media stack stay
// green) or when the per-instance IPC socket path exceeds sun_path (macOS
// long-TMPDIR; the seeks are observed over IPC-connected commands).
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QThread>
#include <QRegularExpression>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickView>
#include <QQuickItem>
#include <QKeyEvent>
#include <QTest>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QColor>
#include <QUrl>
#include <cstdio>

#include "AppCore.h"
#include "player/MpvController.h"
#include "util/MpvLocator.h"

namespace {

QString g_fixture;          // path to the generated clip
QString g_appRoot;          // worktree root (scripts/ + modules/ live here)

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
    // msg-level=v is the observation seam: mpv logs every IPC set_property /
    // seek / keypress command with an mpv-clock timestamp into --log-file.
    app["mpv_video_args"] = QStringLiteral("--vo=null --ao=null --hwdec=no --msg-level=all=v");
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
        const QList<QByteArray> f = s.mid(close + 2).split(' ');
        if (f.size() > 1 && f[1].toLongLong() == self) return true;
    }
    return false;
}

// ── mpv log timeline (the observation contract) ────────────────────────────
struct SpeedEv { double t; double v; };
struct Timeline {
    QList<SpeedEv> speeds;
    QList<double> seekTimes;
    QList<double> seekTargets;
    QStringList keypressLines;
};

Timeline parseMpvLog(const QString &path) {
    Timeline tl;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return tl;
    const QRegularExpression tsRe(QStringLiteral("^\\[\\s*([0-9]+\\.[0-9]+)\\]"));
    const QRegularExpression speedRe(QStringLiteral("Set property: speed=([0-9.]+)"));
    const QRegularExpression targetRe(QStringLiteral("target=\\\"(-?[0-9.]+)\\\""));
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine());
        const auto tm = tsRe.match(line);
        if (!tm.hasMatch()) continue;
        const double t = tm.captured(1).toDouble();
        if (line.contains(QLatin1String("Set property: speed="))) {
            const auto sm = speedRe.match(line);
            if (sm.hasMatch()) tl.speeds.append({t, sm.captured(1).toDouble()});
        } else if (line.contains(QLatin1String("Run command: seek"))) {
            tl.seekTimes.append(t);
            const auto gm = targetRe.match(line);
            tl.seekTargets.append(gm.hasMatch() ? gm.captured(1).toDouble() : 0.0);
        } else if (line.contains(QLatin1String("Run command: keypress"))) {
            tl.keypressLines.append(line);
        }
    }
    return tl;
}

QString timelineBrief(const Timeline &tl) {
    QString s = QStringLiteral("speeds=[");
    for (const auto &e : tl.speeds)
        s += QStringLiteral("%1@%2s ").arg(e.v).arg(e.t, 0, 'f', 3);
    s += QStringLiteral("] seeks=[");
    for (int i = 0; i < tl.seekTimes.size(); ++i)
        s += QStringLiteral("%1@%2s ").arg(tl.seekTargets.value(i)).arg(tl.seekTimes.at(i), 0, 'f', 3);
    s += QStringLiteral("] keypresses=%1").arg(tl.keypressLines.size());
    return s;
}

// The contract: a held RIGHT produces coalesced forward seeks (one tap plus at
// least one flush, bounded well below the 10/s machine-gun) and never a speed
// command. Returns an empty string when the timeline matches.
QString coalescedForwardIssue(const Timeline &tl) {
    if (!tl.speeds.isEmpty())
        return QStringLiteral("speed commands issued (the ramp is supposed to be gone)");
    if (tl.seekTimes.size() < 2)
        return QStringLiteral("expected the tap seek plus at least one coalesced flush");
    if (tl.seekTimes.size() > 20)
        return QStringLiteral("seek machine-gun: holds are not being coalesced");
    for (double v : tl.seekTargets) {
        if (!(v > 0.0))
            return QStringLiteral("a non-positive (non-forward) seek appeared");
    }
    return QString();
}

// Deterministic per-trial jitter (reproducible evidence, no qrand entropy).
struct Lcg {
    quint32 state;
    explicit Lcg(quint32 seed) : state(seed ? seed : 1) {}
    int next(int lo, int hi) {
        state = state * 1103515245u + 12345u;
        return lo + int((state >> 16) % quint32(hi - lo + 1));
    }
};

// ── real-controller rig (no QML): direct sendKey driving ───────────────────
struct Rig {
    QTemporaryDir data;
    AppCore *core = nullptr;
    MpvController *mpv = nullptr;
    int positions = 0;
    bool launch() {
        if (!data.isValid() || !writeConfig(data.path())) return false;
        core = new AppCore(g_appRoot, data.path());
        mpv = new MpvController(g_appRoot, data.path(), core, nullptr);
        QObject::connect(mpv, &MpvController::positionChanged, mpv, [this](int){ ++positions; });
        mpv->loadAndPlay(g_fixture, 0.0f, 0, -2, {}, {}, false, -1,
                         0.0f, QString(), false, QString(), false, {},
                         0.0f, false, {});
        return spinUntil([this]{ return positions >= 2; }, 6000);
    }
    ~Rig() {
        if (mpv) {
            mpv->forceStop();
            spinUntil([]{ return !anyMpvChild(); }, 3000);
        }
        delete mpv;
        delete core;
    }
};

// ── QML stubs (mirrors youtube-seek-smoke; the warm cache points at the
//    local fixture so the REAL Player.qml launches REAL mpv on it) ─────────
class StubAppCore : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE QVariant get_setting(const QString &, const QString &key) const {
        if (key == QLatin1String("resume_playback")) return QStringLiteral("Never");
        if (key == QLatin1String("playback_resolution")) return QStringLiteral("480p");
        if (key.startsWith(QLatin1String("sponsor_"))) return QStringLiteral("off");
        return QVariant();
    }
    Q_INVOKABLE void note_state(const QString &, const QString &) {}
    Q_INVOKABLE bool telemetryEnabled() const { return true; }
};

class StubRoot : public QObject {
    Q_OBJECT
    Q_PROPERTY(double sh READ sh CONSTANT)
    Q_PROPERTY(double sw READ sw CONSTANT)
    Q_PROPERTY(QString globalFont READ globalFont CONSTANT)
    Q_PROPERTY(QColor surfaceColor READ surfaceColorRead CONSTANT)
    Q_PROPERTY(QColor primaryColor READ primaryColorRead CONSTANT)
    Q_PROPERTY(QColor secondaryColor READ secondaryColorRead CONSTANT)
    Q_PROPERTY(QColor tertiaryColor READ tertiaryColorRead CONSTANT)
    Q_PROPERTY(QColor accentColor READ accentColorRead CONSTANT)
    Q_PROPERTY(QVariantMap hints READ hintsRead CONSTANT)
public:
    double sh() const { return 480; }
    double sw() const { return 640; }
    QString globalFont() const { return QStringLiteral("VCR OSD Mono"); }
    QColor surfaceColorRead() const { return QColor("#101018"); }
    QColor primaryColorRead() const { return QColor("#e6e6e6"); }
    QColor secondaryColorRead() const { return QColor("#8a8a9a"); }
    QColor tertiaryColorRead() const { return QColor("#5a5a6a"); }
    QColor accentColorRead() const { return QColor("#ffcc00"); }
    QVariantMap hintsRead() const {
        QVariantMap m;
        m["back"] = QStringLiteral("BACK");
        m["navigate"] = QStringLiteral("NAV");
        m["browse"] = QStringLiteral("BROWSE");
        m["select"] = QStringLiteral("SELECT");
        return m;
    }
};

class StubModule : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString moduleId READ moduleId CONSTANT)
    Q_PROPERTY(QString moduleName READ moduleName CONSTANT)
    Q_PROPERTY(QString moduleIcon READ moduleIcon CONSTANT)
public:
    QString moduleId() const { return QStringLiteral("com.240mp.youtube"); }
    QString moduleName() const { return QStringLiteral("YouTube"); }
    QString moduleIcon() const { return QString(); }
};

class StubYoutube : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE QString cachedPlaybackUrl(const QString &, const QString &) const {
        return g_fixture;   // local fixture: no network, fully muxed
    }
    Q_INVOKABLE QString cachedPlaybackAudioUrl(const QString &, const QString &) const {
        return QString();
    }
    Q_INVOKABLE bool cachedPlaybackSingle(const QString &, const QString &) const { return true; }
    Q_INVOKABLE QVariantMap getSavedPosition(const QString &) const {
        return QVariantMap{{QStringLiteral("pos"), 0}};
    }
    Q_INVOKABLE QString ytdlFormatForResolution(const QString &) const { return QString(); }
    Q_INVOKABLE QVariantMap buildPlaybackEdl(const QString &, const QString &, const QVariantMap &) const { return {}; }
    Q_INVOKABLE void requestSponsorSegments(const QString &) {}
    Q_INVOKABLE void resolvePlayback(const QString &, const QString &) {}
    Q_INVOKABLE void invalidateCachedUrl(const QString &, const QString &) {}
    Q_INVOKABLE void savePosition(const QString &, int, const QString &, const QString &) {}
    Q_INVOKABLE bool hasSponsorSegments(const QString &) const { return false; }
    Q_INVOKABLE bool sponsorFetchInFlight(const QString &) const { return false; }
    // Items.qml (G5 list-nav probe) menu sources.
    Q_INVOKABLE QVariantMap check_subscriptions() const {
        return QVariantMap{{QStringLiteral("ok"), true}, {QStringLiteral("fileExists"), true}};
    }
    Q_INVOKABLE QVariantMap check_playlists() const {
        return QVariantMap{{QStringLiteral("ok"), true}, {QStringLiteral("fileExists"), true}};
    }
    Q_INVOKABLE QVariantMap check_signin() const {
        return QVariantMap{{QStringLiteral("signedIn"), false}};
    }
    Q_INVOKABLE QVariantList getWatchLater() const {
        return QVariantList{QStringLiteral("wl1")};
    }
    Q_INVOKABLE QVariantList getHistory() const { return {}; }
signals:
    void playbackUrlReady(const QString &);
    void sponsorReady(const QString &);
    void playbackResolveFailed(const QString &videoId, const QString &reason);
};

// Raw-path trial modes — the remote behaviors + the ideal canary.
enum class Mode { Absent, Mislabeled, Variable, Clean, PairStream };
const char *modeName(Mode m) {
    switch (m) {
    case Mode::Absent:     return "absent";
    case Mode::Mislabeled: return "mislabeled";
    case Mode::Variable:   return "variable";
    case Mode::Clean:      return "clean";
    case Mode::PairStream: return "pairstream";
    }
    return "?";
}

} // namespace

class MpvHoldRepeatTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        g_appRoot = QString::fromUtf8(MP240_REPO_ROOT);
        const QString mpv = mpvbin::locate();
        if (mpv.isEmpty())
            QSKIP("mpv not available on this host — hold-repeat tests skipped");
        QTemporaryDir *gen = new QTemporaryDir();   // lives for the whole run
        if (!gen->isValid()) QSKIP("cannot create temp dir for fixture");
        g_fixture = gen->path() + QStringLiteral("/fixture.mp4");
        QProcess ff;
        // 600 s, not 60 s: a coalesced forward hold walks hundreds of seconds,
        // and a short clip would hit EOF mid-trial and truncate the timeline.
        ff.start(QStringLiteral("ffmpeg"),
                 {"-y", "-f", "lavfi", "-i", "testsrc=duration=600:size=320x240:rate=10",
                  "-f", "lavfi", "-i", "sine=frequency=440:duration=600",
                  "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac",
                  "-shortest", g_fixture});
        if (!ff.waitForFinished(120000) || ff.exitCode() != 0 || !QFile::exists(g_fixture))
            QSKIP("ffmpeg could not generate the test fixture — skipped");
        // sun_path guard (macOS long TMPDIR): without IPC the seek commands
        // are dropped pre-connect and there is nothing to observe.
        QTemporaryDir probe;
        if (probe.isValid() && writeConfig(probe.path())) {
            AppCore core(g_appRoot, probe.path());
            MpvController ctl(g_appRoot, probe.path(), &core, nullptr);
            if (ctl.socketPath().isEmpty())
                QSKIP("per-instance IPC socket path exceeds sun_path — skipped");
        }
    }

    // G1 — MISLABELED repeats: 30 fresh-flagged RIGHT presses at 10 Hz while
    // physically held. Fixed: one tap seek, then the internal timer coalesces
    // into periodic flushes. Pre-fix: every press is a fresh tap (31 seeks) —
    // the seek machine-gun the owner saw.
    void mislabeledFreshPressesCoalesceOnce() {
        Rig rig;
        QVERIFY(rig.launch());
        rig.mpv->sendKey(QStringLiteral("RIGHT"), false);
        for (int i = 0; i < 30; ++i) {
            spinFor(100);
            rig.mpv->sendKey(QStringLiteral("RIGHT"), false);   // mislabeled
        }
        spinFor(1200);
        rig.mpv->sendKeyUp(QStringLiteral("RIGHT"));
        spinFor(300);
        const Timeline tl = parseMpvLog(rig.mpv->logFilePath());
        std::printf("  [mislabeled-direct] %s\n", qPrintable(timelineBrief(tl)));
        const QString issue = coalescedForwardIssue(tl);
        QVERIFY2(issue.isEmpty(), qPrintable(issue));
    }

    // G1 — ABSENT autorepeat: one press, a 4 s physical hold, release. Fixed:
    // the internal timer still coalesces. Pre-fix: no repeats ever arrive, so
    // only the tap seek.
    void absentAutoRepeatStillCoalesces() {
        Rig rig;
        QVERIFY(rig.launch());
        rig.mpv->sendKey(QStringLiteral("RIGHT"), false);
        spinFor(4000);
        rig.mpv->sendKeyUp(QStringLiteral("RIGHT"));
        spinFor(300);
        const Timeline tl = parseMpvLog(rig.mpv->logFilePath());
        std::printf("  [absent-direct] %s\n", qPrintable(timelineBrief(tl)));
        const QString issue = coalescedForwardIssue(tl);
        QVERIFY2(issue.isEmpty(), qPrintable(issue));
    }

    // G1 — VARIABLE/FAST rate: flagged repeats at jittered 30-60 ms. Fixed: the
    // OS stream is ignored while held, so the coalesce cadence is unchanged.
    void variableRateCoalescingIsTimerBased() {
        Rig rig;
        QVERIFY(rig.launch());
        Lcg rng(0x5eed);
        rig.mpv->sendKey(QStringLiteral("RIGHT"), false);
        QElapsedTimer wall; wall.start();
        qint64 nextDue = rng.next(30, 60);
        while (wall.elapsed() < 3200) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            if (wall.elapsed() >= nextDue) {
                rig.mpv->sendKey(QStringLiteral("RIGHT"), true);
                nextDue = wall.elapsed() + rng.next(30, 60);
            }
            QThread::msleep(2);
        }
        rig.mpv->sendKeyUp(QStringLiteral("RIGHT"));
        spinFor(300);
        const Timeline tl = parseMpvLog(rig.mpv->logFilePath());
        std::printf("  [variable-direct] %s\n", qPrintable(timelineBrief(tl)));
        const QString issue = coalescedForwardIssue(tl);
        QVERIFY2(issue.isEmpty(), qPrintable(issue));
    }

    // Canary: a GENUINE release (gap > grace window) ends the hold; a later
    // keydown re-arms it fresh. With #133's 250 ms grace, we need gaps > 250 ms.
    void keyUpEndsHoldAndRearms() {
        Rig rig;
        QVERIFY(rig.launch());
        for (int round = 0; round < 2; ++round) {
            rig.mpv->sendKey(QStringLiteral("RIGHT"), false);
            for (int i = 0; i < 6; ++i) {
                spinFor(100);
                rig.mpv->sendKey(QStringLiteral("RIGHT"), true);
            }
            rig.mpv->sendKeyUp(QStringLiteral("RIGHT"));
            spinFor(300);   // > 250 ms grace: genuine release, hold ends
        }
        spinFor(200);
        const Timeline tl = parseMpvLog(rig.mpv->logFilePath());
        std::printf("  [rearm-direct] %s\n", qPrintable(timelineBrief(tl)));
        QVERIFY(tl.speeds.isEmpty());
        QVERIFY2(tl.seekTimes.size() >= 2, "each round must seek at least once");
        for (double v : tl.seekTargets)
            QVERIFY2(v > 0.0, "a non-forward seek appeared");
    }

    // PairStream: the owner's remote autorepeats as discrete press/release
    // pairs. A keyup between every repeat must NOT end the hold — the coalesced
    // cadence is unchanged and only stops after the pair stream ends.
    void pairStreamHoldCoalescesDeterministically() {
        Rig rig;
        QVERIFY(rig.launch());
        rig.mpv->sendKey(QStringLiteral("RIGHT"), false);
        QElapsedTimer wall; wall.start();
        while (wall.elapsed() < 4000) {
            spinFor(120);
            rig.mpv->sendKey(QStringLiteral("RIGHT"), true);
            rig.mpv->sendKeyUp(QStringLiteral("RIGHT"));
        }
        spinFor(400);
        const Timeline tl = parseMpvLog(rig.mpv->logFilePath());
        std::printf("  [pairstream-direct] %s\n", qPrintable(timelineBrief(tl)));
        const QString issue = coalescedForwardIssue(tl);
        QVERIFY2(issue.isEmpty(), qPrintable(issue));
    }

    // Canary: LEFT hold still coalesces into ~400 ms flushes of negative seeks;
    // the RIGHT change must not disturb the rewind.
    void leftHoldCoalescingUnchanged() {
        Rig rig;
        QVERIFY(rig.launch());
        rig.mpv->sendKey(QStringLiteral("LEFT"), false);
        for (int i = 0; i < 12; ++i) {
            spinFor(100);
            rig.mpv->sendKey(QStringLiteral("LEFT"), true);
        }
        rig.mpv->sendKeyUp(QStringLiteral("LEFT"));
        spinFor(300);
        const Timeline tl = parseMpvLog(rig.mpv->logFilePath());
        std::printf("  [left-direct] %s\n", qPrintable(timelineBrief(tl)));
        QVERIFY(tl.speeds.isEmpty());
        QVERIFY2(tl.seekTimes.size() >= 3, "LEFT hold did not produce tap + flush seeks");
        double total = 0.0;
        for (double v : tl.seekTargets) {
            QVERIFY2(v < 0.0, "LEFT hold produced a non-negative seek");
            total += v;
        }
        QVERIFY2(total <= -90.0 && total >= -140.0,
                 qPrintable(QStringLiteral("LEFT hold total %1 s outside the coalesced band").arg(total)));
    }

    // Canary: stop() mid-hold strands no timer — no seek command may appear
    // after the stop (this proves the hold timer is torn down with the session).
    void stopMidHoldStrandsNothing() {
        Rig rig;
        QVERIFY(rig.launch());
        rig.mpv->sendKey(QStringLiteral("RIGHT"), false);
        spinFor(100);
        rig.mpv->sendKey(QStringLiteral("RIGHT"), true);
        spinFor(500);
        rig.mpv->stop();
        spinFor(200);
        const Timeline atStop = parseMpvLog(rig.mpv->logFilePath());
        spinFor(1200);
        const Timeline after = parseMpvLog(rig.mpv->logFilePath());
        std::printf("  [stop-direct] at-stop: %s | after: %s\n",
                    qPrintable(timelineBrief(atStop)), qPrintable(timelineBrief(after)));
        QCOMPARE(after.speeds.size(), atStop.speeds.size());
        QCOMPARE(after.seekTimes.size(), atStop.seekTimes.size());
    }

    // Canary: non-shaped keys are untouched — UP/ENTER taps pass through as
    // keypress, repeats drop, no seek side effects.
    void verticalAndOtherKeysUnchanged() {
        Rig rig;
        QVERIFY(rig.launch());
        rig.mpv->sendKey(QStringLiteral("UP"), false);
        for (int i = 0; i < 3; ++i) rig.mpv->sendKey(QStringLiteral("UP"), true);
        rig.mpv->sendKey(QStringLiteral("ENTER"), false);
        rig.mpv->sendKey(QStringLiteral("SPACE"), true);
        rig.mpv->sendKeyUp(QStringLiteral("UP"));
        spinFor(400);
        const Timeline tl = parseMpvLog(rig.mpv->logFilePath());
        std::printf("  [vertical-direct] %s\n", qPrintable(timelineBrief(tl)));
        QVERIFY(tl.speeds.isEmpty());
        QVERIFY(tl.seekTimes.isEmpty());
        QCOMPARE(tl.keypressLines.size(), 2);
        QVERIFY(tl.keypressLines.at(0).contains(QLatin1String("UP")));
        QVERIFY(tl.keypressLines.at(1).contains(QLatin1String("ENTER")));
    }

    // G5 — navigation is untouched: held UP/DOWN through the RAW path in the
    // real YouTube Items.qml menu still advances one row per event with wrap
    // (Keys.onUp/DownPressed fire on repeats). No MpvController is constructed
    // here — the shaping is player-scoped by construction; this proves the raw
    // held-key list contract empirically.
    void listHoldScrollUnchanged() {
        QQmlEngine engine;
        engine.addImportPath(g_appRoot + "/views");
        StubAppCore stubCore; StubRoot root; StubModule module; StubYoutube yt;
        auto *ctx = engine.rootContext();
        ctx->setContextProperty("appCore", QVariant::fromValue(&stubCore));
        ctx->setContextProperty("root", QVariant::fromValue(&root));
        ctx->setContextProperty("moduleRoot", QVariant::fromValue(&module));
        ctx->setContextProperty("youtubeBackend", QVariant::fromValue(&yt));
        QQmlComponent comp(&engine, QUrl::fromLocalFile(
            g_appRoot + "/modules/youtube/views/Items.qml"));
        QVERIFY2(comp.status() != QQmlComponent::Error, qPrintable(comp.errorString()));
        QObject *obj = comp.create(engine.rootContext());
        QVERIFY(obj);
        QQuickView window;
        window.resize(640, 480);
        auto *qi = qobject_cast<QQuickItem *>(obj);
        QVERIFY(qi);
        qi->setParentItem(window.contentItem());
        qi->setWidth(640);
        qi->setHeight(480);
        qi->setFocus(true);
        window.show();
        QVERIFY(spinUntil([&]{
            return QQmlProperty::read(obj, "items").toList().size() == 4;
        }, 3000));
        // The ListView child owns active focus (focus: true inside the scope).
        auto currentIndex = [&]() -> int {
            const auto kids = qi->childItems();
            for (QQuickItem *k : kids) {
                const QVariant v = QQmlProperty::read(k, "currentIndex");
                if (v.isValid()) return v.toInt();
            }
            return -1;
        };
        QCOMPARE(currentIndex(), 0);
        auto post = [&](int qtKey, bool repeat) {
            QKeyEvent ev(QEvent::KeyPress, qtKey, Qt::NoModifier, 0, 0, 0,
                         QString(), repeat);
            QCoreApplication::sendEvent(&window, &ev);
            QCoreApplication::processEvents();
        };
        // 4 rows: held DOWN = press + 7 repeats -> 1,2,3,0,1,2,3,0 (wrap).
        post(Qt::Key_Down, false);
        QCOMPARE(currentIndex(), 1);
        const int expectedDown[7] = {2, 3, 0, 1, 2, 3, 0};
        for (int i = 0; i < 7; ++i) {
            post(Qt::Key_Down, true);
            QCOMPARE(currentIndex(), expectedDown[i]);
        }
        // Held UP wraps backwards: 3,2,1.
        post(Qt::Key_Up, false);
        QCOMPARE(currentIndex(), 3);
        post(Qt::Key_Up, true);
        QCOMPARE(currentIndex(), 2);
        post(Qt::Key_Up, true);
        QCOMPARE(currentIndex(), 1);
        delete qi;
    }

    // G2/G3 — the RAW remote path through the REAL Player.qml. QKeyEvents are
    // posted straight at the QQuickWindow (eventFilter return-false downstream;
    // no InputManager synthesis, no debug key channel). Every remote behavior
    // must produce the same coalesced forward seeks and never a speed command.
    void rawRemotePathTrials() {
        struct Arm { Mode mode; int trials; };
        const QList<Arm> arms = {{Mode::Absent, 7}, {Mode::Mislabeled, 7},
                                 {Mode::Variable, 7}, {Mode::Clean, 3},
                                 {Mode::PairStream, 7}};

        QJsonArray evidence;
        int trialNo = 0;
        for (const Arm &arm : arms) {
            for (int i = 0; i < arm.trials; ++i) {
                ++trialNo;
                // ── per-trial rig: fresh data dir, controller, QML view, mpv ──
                QTemporaryDir data;
                QVERIFY(data.isValid());
                QVERIFY(writeConfig(data.path()));
                AppCore core(g_appRoot, data.path());
                MpvController mpv(g_appRoot, data.path(), &core, nullptr);
                if (mpv.socketPath().isEmpty())
                    QSKIP("per-instance IPC socket path exceeds sun_path — skipped");
                int positions = 0;
                QObject::connect(&mpv, &MpvController::positionChanged, &mpv, [&](int){ ++positions; });

                QQmlEngine engine;
                engine.addImportPath(g_appRoot + "/views");
                StubAppCore stubCore; StubRoot root; StubModule module; StubYoutube yt;
                auto *ctx = engine.rootContext();
                ctx->setContextProperty("appCore", QVariant::fromValue(&stubCore));
                ctx->setContextProperty("root", QVariant::fromValue(&root));
                ctx->setContextProperty("moduleRoot", QVariant::fromValue(&module));
                ctx->setContextProperty("youtubeBackend", QVariant::fromValue(&yt));
                ctx->setContextProperty("mpvController", QVariant::fromValue(&mpv));
                QVariantMap item;
                item["url"] = g_fixture;
                item["videoId"] = QStringLiteral("holdvid");
                item["title"] = QStringLiteral("HOLD");
                QVariantMap params;
                params["item"] = item;
                params["playlistItems"] = QVariantList{};
                params["playlistIndex"] = -1;
                QQmlComponent comp(&engine, QUrl::fromLocalFile(
                    g_appRoot + "/modules/youtube/views/Player.qml"));
                QVERIFY2(comp.status() != QQmlComponent::Error,
                         qPrintable(comp.errorString()));
                QObject *obj = comp.beginCreate(engine.rootContext());
                QVERIFY(obj);
                comp.setInitialProperties(obj, {{QStringLiteral("navParams"), params}});
                comp.completeCreate();
                QQuickView window;
                window.resize(640, 480);
                auto *qi = qobject_cast<QQuickItem *>(obj);
                QVERIFY(qi);
                qi->setParentItem(window.contentItem());
                qi->setWidth(640);
                qi->setHeight(480);
                qi->setFocus(true);
                window.show();
                const bool playing = spinUntil([&]{
                    return positions >= 2
                        && QQmlProperty::read(obj, "launchDone").toBool();
                }, 8000);
                QVERIFY2(playing, "fixture playback did not start through Player.qml");
                QVERIFY(QQmlProperty::read(obj, "errorMessage").toString().isEmpty());
                QVERIFY(!QQmlProperty::read(obj, "overlayVisible").toBool());

                // ── drive the hold through the RAW path ──
                auto post = [&](QEvent::Type type, bool repeat) {
                    QKeyEvent ev(type, Qt::Key_Right, Qt::NoModifier, 0, 0, 0,
                                 QString(), repeat);
                    QCoreApplication::sendEvent(&window, &ev);
                };
                Lcg rng(quint32(trialNo) * 7919u + quint32(arm.mode));
                const int holdMs = 4000;
                QElapsedTimer wall; wall.start();
                post(QEvent::KeyPress, false);   // the physical press
                qint64 nextDue = -1;
                if (arm.mode == Mode::Mislabeled) nextDue = rng.next(80, 120);
                else if (arm.mode == Mode::Variable) nextDue = rng.next(30, 60);
                else if (arm.mode == Mode::Clean) nextDue = 100;
                else if (arm.mode == Mode::PairStream) nextDue = rng.next(100, 140);
                while (wall.elapsed() < holdMs) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
                    if (nextDue >= 0 && wall.elapsed() >= nextDue) {
                        if (arm.mode == Mode::PairStream) {
                            // The owner's remote autorepeats as discrete
                            // press/release PAIRS. A keyup between every repeat.
                            post(QEvent::KeyPress, true);
                            post(QEvent::KeyRelease, false);
                            nextDue = wall.elapsed() + rng.next(100, 140);
                        } else {
                            // Mislabeled remotes send FRESH presses; the others
                            // send flagged repeats. Absent sends nothing at all.
                            post(QEvent::KeyPress, arm.mode != Mode::Mislabeled);
                            if (arm.mode == Mode::Mislabeled) nextDue = wall.elapsed() + rng.next(80, 120);
                            else if (arm.mode == Mode::Variable) nextDue = wall.elapsed() + rng.next(30, 60);
                            else nextDue = wall.elapsed() + 100;
                        }
                    }
                    QThread::msleep(2);
                }
                post(QEvent::KeyRelease, false);
                spinFor(300);

                const Timeline tl = parseMpvLog(mpv.logFilePath());
                const QString brief = timelineBrief(tl);
                std::printf("  [raw %s #%d] %s\n", modeName(arm.mode), trialNo,
                            qPrintable(brief));

                const QString issue = coalescedForwardIssue(tl);

                QJsonObject row;
                row["trial"] = trialNo;
                row["mode"] = QString::fromLatin1(modeName(arm.mode));
                row["hold_ms"] = holdMs;
                row["pass"] = issue.isEmpty();
                QJsonArray sp;
                for (const auto &e : tl.speeds)
                    sp.append(QJsonObject{{"t", e.t}, {"v", e.v}});
                row["speeds"] = sp;
                QJsonArray sk;
                for (int s = 0; s < tl.seekTimes.size(); ++s)
                    sk.append(QJsonObject{{"t", tl.seekTimes.at(s)}, {"target", tl.seekTargets.value(s)}});
                row["seeks"] = sk;
                evidence.append(row);

                // ── teardown: view destruction key-ups, then reap the child ──
                delete qi;
                mpv.forceStop();
                QVERIFY(spinUntil([]{ return !anyMpvChild(); }, 3000));
                QVERIFY2(issue.isEmpty(), qPrintable(QStringLiteral(
                    "raw %1 trial #%2: %3 — got %4")
                    .arg(modeName(arm.mode)).arg(trialNo).arg(issue).arg(brief)));
            }
        }
        // Evidence file: per-trial speed/seek timelines for the card + PR.
        QString evPath = qEnvironmentVariable("MP240_HOLD_EVIDENCE");
        if (evPath.isEmpty())
            evPath = QDir::tempPath() + QStringLiteral("/hold-coalesce-evidence.json");
        QFile ev(evPath);
        if (ev.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            ev.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented));
            ev.close();
        }
        std::printf("  evidence: %s (%d trials)\n", qPrintable(evPath), trialNo);
    }
};

QTEST_MAIN(MpvHoldRepeatTest)
#include "MpvHoldRepeatTest.moc"
