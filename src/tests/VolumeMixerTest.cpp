// Verifies the app-wide system-volume abstraction (issue #34): the
// VolumeMixer contract, the factory's runtime backend detection, and the
// async ALSA backend (T1 P-5A) end-to-end through the real MixerProcessRunner
// seam. No real audio hardware is needed — on WSL/CI the factory falls back
// to the no-op mixer, and the ALSA behavioural tests drive the compiled
// mixer-stub child (MIXER_STUB_PATH) with the TORTURE_* env knobs, exactly
// like the volume-stall harness does.
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <functional>
#include <memory>

#include "audio/ALSAVolumeMixer.h"
#include "audio/MixerProcessRunner.h"
#include "audio/NoopVolumeMixer.h"
#include "audio/PipeWireVolumeMixer.h"
#include "audio/VolumeMixerFactory.h"

using namespace audio;

namespace {

// Per-test stub environment: state file, argv log, delay/fault/card knobs.
// The runner's children inherit the process env at spawn time, so setting it
// before construction is enough. Restored on destruction so tests stay
// independent.
struct StubEnv {
    QTemporaryDir dir;
    QString statePath;
    QString logPath;
    QString amixerPath;

    explicit StubEnv(int delayMs = 0, const QByteArray &mode = "normal",
                     const QByteArray &card = "combined") {
        statePath = dir.filePath(QStringLiteral("state"));
        logPath = dir.filePath(QStringLiteral("argv.log"));
        QFile f(statePath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            qFatal("volume-mixer-tests: cannot seed stub state file");
        f.write("50 0\n"); // same initial state the stall harness uses
        f.close();
        // The stub dispatches on basename(argv[0]) == "amixer"/"wpctl", so
        // install a copy under that name exactly like the harness orchestrator.
        // MIXER_STUB_PATH is the compiled stub's absolute path (CMake).
        amixerPath = dir.filePath(QStringLiteral("amixer"));
        if (!QFile::copy(QStringLiteral(MIXER_STUB_PATH), amixerPath))
            qFatal("volume-mixer-tests: cannot copy mixer-stub from %s", MIXER_STUB_PATH);
        QFile::setPermissions(amixerPath,
                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                              QFile::ReadGroup | QFile::ExeGroup |
                              QFile::ReadOther | QFile::ExeOther);
        qputenv("TORTURE_DELAY_MS", QByteArray::number(delayMs));
        qputenv("TORTURE_STUB_MODE", mode);
        qputenv("TORTURE_STUB_STATE", statePath.toUtf8());
        qputenv("TORTURE_STUB_CARD", card);
        qputenv("TORTURE_MIXER_LOG", logPath.toUtf8());
    }
    ~StubEnv() {
        qunsetenv("TORTURE_DELAY_MS");
        qunsetenv("TORTURE_STUB_MODE");
        qunsetenv("TORTURE_STUB_STATE");
        qunsetenv("TORTURE_STUB_CARD");
        qunsetenv("TORTURE_MIXER_LOG");
    }
};

QStringList logLines(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QStringList lines;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (!line.isEmpty()) lines << line;
    }
    return lines;
}

void clearLog(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        qFatal("volume-mixer-tests: cannot clear mixer argv log");
}

QString stateFileBody(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll()).trimmed();
}

bool pumpUntil(const std::function<bool()> &predicate, int timeoutMs = 5000) {
    QElapsedTimer clock;
    clock.start();
    while (!predicate()) {
        if (clock.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return true;
}

// Tracks every runner completion so "settled" means every started child has
// reported AND its owner-thread result was processed — not merely "no child
// is running right now" (which is also true in the gap before a spawn).
// The mixer's isIdle() closes the remaining gaps: it is false from submit()
// until the completion handler runs (covering queued-but-unspawned children,
// which startedCount cannot see yet) and false again the moment a handler
// chains the next request. No wall-clock quiet window is needed — the
// predicate is exact, not timing-dependent.
struct SettleWatch {
    quint64 finished = 0;
    MixerProcessResult::Status lastStatus = MixerProcessResult::Status::Success;
    SettleWatch(MixerProcessRunner *runner, const ALSAVolumeMixer *mixer)
        : m_mixer(mixer) {
        QObject::connect(runner, &MixerProcessRunner::finished,
                         runner, [this](const MixerProcessResult &r) {
            ++finished;
            lastStatus = r.status;
        });
    }
    bool settled(MixerProcessRunner *runner) const {
        return m_mixer->isIdle() && finished == runner->startedCount()
            && !runner->isBusy();
    }
private:
    const ALSAVolumeMixer *m_mixer;
};

} // namespace

class VolumeMixerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // setLevel clamps out-of-range values into 0..100.
    void testNoopClampsLevel() {
        NoopVolumeMixer m;
        QVERIFY(m.setLevel(150));
        QCOMPARE(m.level(), 100);
        QVERIFY(m.setLevel(-10));
        QCOMPARE(m.level(), 0);
        QCOMPARE(m.setLevel(42), true);
        QCOMPARE(m.level(), 42);
    }

    // Mute is tracked independently and survives level changes.
    void testNoopMuteState() {
        NoopVolumeMixer m;
        QVERIFY(!m.isMuted());
        QVERIFY(m.setMuted(true));
        QVERIFY(m.isMuted());
        // Turning the volume up is how you un-mute (matches a physical button).
        m.setLevel(80);
        QVERIFY(m.isMuted()); // no-op keeps whatever it was told
    }

    // Every backend reports a distinct, stable name.
    void testBackendNames() {
        QCOMPARE(NoopVolumeMixer().backendName(), QStringLiteral("none"));
        QCOMPARE(PipeWireVolumeMixer().backendName(), QStringLiteral("pipewire"));
        QCOMPARE(ALSAVolumeMixer().backendName(), QStringLiteral("alsa"));
    }

    // The factory always returns a usable mixer and never a null pointer.
    void testFactoryAlwaysSucceeds() {
        auto mixer = makeSystemVolumeMixer();
        QVERIFY(mixer != nullptr);
        // On machines without audio daemons (WSL/CI containers) it falls back to none;
        // on real hardware with ALSA or PipeWire, it selects that backend.
        const QString name = mixer->backendName();
        QVERIFY(name == QStringLiteral("none") ||
                name == QStringLiteral("alsa") ||
                name == QStringLiteral("pipewire"));
    }

    // The factory's choice is stable across repeated calls.
    void testFactoryDeterministic() {
        QCOMPARE(makeSystemVolumeMixer()->backendName(),
                 makeSystemVolumeMixer()->backendName());
    }

    // Auto-detection must land on a control that actually exists: a desktop's
    // 'Master', a Pi's 'PCM'/'Headphone', and a sane fallback when the card
    // exposes nothing we recognise (issue #34 — volume keys were a no-op on
    // hardware whose card has no 'Master').
    void testAlsaControlDetection() {
        // Pi bcm2835 (composite/headphone out).
        const QString pi = QStringLiteral(
            "Simple mixer control 'PCM',0\n"
            "Simple mixer control 'Headphone',0\n");
        QCOMPARE(ALSAVolumeMixer::pickControl(pi), QStringLiteral("PCM"));
        // Desktop card.
        const QString desktop = QStringLiteral(
            "Simple mixer control 'PCM',0\n"
            "Simple mixer control 'Master',0\n");
        QCOMPARE(ALSAVolumeMixer::pickControl(desktop), QStringLiteral("Master"));
        // Only a headphone control (some Pi configs).
        const QString phone = QStringLiteral("Simple mixer control 'Headphone',0\n");
        QCOMPARE(ALSAVolumeMixer::pickControl(phone), QStringLiteral("Headphone"));
        // Nothing usable -> historical default, not an arbitrary control.
        QCOMPARE(ALSAVolumeMixer::pickControl(QStringLiteral("Simple mixer control 'Mic',0\n")),
                 QStringLiteral("Master"));
        QCOMPARE(ALSAVolumeMixer::pickControl(QStringLiteral("")), QStringLiteral("Master"));
    }

    // amixer's `get` output carries the level as "[71%]" — the old regex looked
    // for a leading '%' and never matched, so level() always echoed its cache.
    void testAlsaLevelParsing() {
        QCOMPARE(ALSAVolumeMixer::parseLevelPercent(QStringLiteral(
                     "Front Left: Playback 50 [71%] [-10.00dB] [on]\n"
                     "Front Right: Playback 50 [71%] [-10.00dB] [on]\n")), 71);
        QCOMPARE(ALSAVolumeMixer::parseLevelPercent(QStringLiteral(
                     "Mono: Playback 0 [0%] [-51.00dB] [off]")), 0);
        QCOMPARE(ALSAVolumeMixer::parseLevelPercent(QStringLiteral(
                     "Simple mixer control 'PCM',0")), -1);  // no level -> cache
    }

    // Split-control cards name the elements separately instead of exposing a
    // combined 'Master': `scontrols` then lists 'Master Playback Volume' and
    // 'Master Playback Switch'. The volume half must be picked up by the same
    // detection, and the switch half paired with it — otherwise mute targets a
    // volume element and does nothing.
    void testAlsaSplitControlPairing() {
        const QString split = QStringLiteral(
            "Simple mixer control 'Master Playback Volume',0\n"
            "Simple mixer control 'Master Playback Switch',0\n"
            "Simple mixer control 'Mic',0\n");
        QCOMPARE(ALSAVolumeMixer::pickControl(split), QStringLiteral("Master Playback Volume"));
        QCOMPARE(ALSAVolumeMixer::pickSwitchControl(split, QStringLiteral("Master Playback Volume")),
                 QStringLiteral("Master Playback Switch"));

        // Same pairing when the pinned name is the bare control.
        const QString pcmSplit = QStringLiteral(
            "Simple mixer control 'PCM Playback Volume',0\n"
            "Simple mixer control 'PCM Playback Switch',0\n");
        QCOMPARE(ALSAVolumeMixer::pickSwitchControl(pcmSplit, QStringLiteral("PCM")),
                 QStringLiteral("PCM Playback Switch"));

        // A volume control with no matching switch resolves to nothing, so the
        // caller keeps driving the control it already has.
        const QString volumeOnly = QStringLiteral("Simple mixer control 'PCM Playback Volume',0\n");
        QVERIFY(ALSAVolumeMixer::pickSwitchControl(volumeOnly, QStringLiteral("PCM Playback Volume")).isEmpty());
        QVERIFY(ALSAVolumeMixer::pickSwitchControl(QString(), QStringLiteral("PCM")).isEmpty());
        QVERIFY(ALSAVolumeMixer::pickSwitchControl(pcmSplit, QString()).isEmpty());
    }

    // Playback switch read-back: [on]/[off], plus the "any channel off counts
    // as muted" rule and the no-value case that must not be mistaken for audio
    // that is playing.
    void testAlsaSwitchParsing() {
        bool ok = false;
        QVERIFY(!ALSAVolumeMixer::parseSwitchMuted(QStringLiteral("  Mono: Playback [on]"), &ok));
        QVERIFY(ok);

        QVERIFY(ALSAVolumeMixer::parseSwitchMuted(QStringLiteral("  Mono: Playback [off]"), &ok));
        QVERIFY(ok);

        // Stereo rows, one side muted -> mute is on.
        QVERIFY(ALSAVolumeMixer::parseSwitchMuted(QStringLiteral(
                    "  Front Left: Playback [off]\n"
                    "  Front Right: Playback [on]\n"), &ok));
        QVERIFY(ok);

        // No switch reading at all (capture-only output): unknown, not "off".
        ok = true;
        QVERIFY(!ALSAVolumeMixer::parseSwitchMuted(QStringLiteral(
                    "Simple mixer control 'Mic',0\n"
                    "  Capture channels: Mono\n"
                    "  Mono: Capture [on]\n"), &ok));
        QVERIFY(!ok);
        QVERIFY(!ALSAVolumeMixer::parseSwitchMuted(QString(), &ok));
        QVERIFY(!ok);
    }

    // The project's Pi card, verbatim from the hardware: `amixer scontrols`
    // lists one combined 'PCM' control (pvolume + pswitch), and the split
    // 'PCM Playback Switch' name is only a raw numid — `amixer sget 'PCM
    // Playback Switch'` fails there. Mute must therefore read back from the
    // combined control rather than assume the split pair.
    void testAlsaPiCombinedControlFixture() {
        const QString piScontrols = QStringLiteral("Simple mixer control 'PCM',0\n");
        QCOMPARE(ALSAVolumeMixer::pickControl(piScontrols), QStringLiteral("PCM"));
        QVERIFY(ALSAVolumeMixer::pickSwitchControl(piScontrols, QStringLiteral("PCM")).isEmpty());

        const QString piOn = QStringLiteral(
            "Simple mixer control 'PCM',0\n"
            "  Capabilities: pvolume pvolume-joined pswitch pswitch-joined\n"
            "  Playback channels: Mono\n"
            "  Limits: Playback -10239 - 400\n"
            "  Mono: Playback -2260 [75%] [-22.60dB] [on]\n");
        const QString piOff = QStringLiteral(
            "Simple mixer control 'PCM',0\n"
            "  Capabilities: pvolume pvolume-joined pswitch pswitch-joined\n"
            "  Playback channels: Mono\n"
            "  Limits: Playback -10239 - 400\n"
            "  Mono: Playback -2260 [75%] [-22.60dB] [off]\n");

        bool ok = false;
        QVERIFY(!ALSAVolumeMixer::parseSwitchMuted(piOn, &ok));
        QVERIFY(ok);
        QVERIFY(ALSAVolumeMixer::parseSwitchMuted(piOff, &ok));
        QVERIFY(ok);
        // The same output still yields the level — the [on]/[off] token must not
        // be confused with the "[75%]" reading.
        QCOMPARE(ALSAVolumeMixer::parseLevelPercent(piOff), 75);
    }

    // ---- Async ALSA behaviour (T1 P-5A), real runner + stub child ----------

    // Construction must not block even with a 200 ms mixer: sane cached
    // defaults immediately, hardware observation (and the stateReady /
    // stateChanged signals) afterwards. Startup costs exactly two children:
    // scontrols discovery + one sget (the combined control carries mute too).
    void testAlsaAsyncConstructionNonBlocking() {
        StubEnv env(200);
        MixerProcessRunner runner;
        QElapsedTimer clock;
        clock.start();
        ALSAVolumeMixer mixer(QString(), &runner, env.amixerPath);
        const qint64 ctorMs = clock.elapsed();
        QVERIFY2(ctorMs < 16, qPrintable(QStringLiteral("ctor blocked %1ms").arg(ctorMs)));
        QCOMPARE(mixer.level(), 100); // cached default, no spawn
        QCOMPARE(mixer.isMuted(), false);
        QVERIFY(!mixer.hasHardwareState());

        QSignalSpy readySpy(&mixer, &ALSAVolumeMixer::stateReady);
        QSignalSpy stateSpy(&mixer, &ALSAVolumeMixer::stateChanged);
        QVERIFY(pumpUntil([&] { return mixer.hasHardwareState(); }));
        QCOMPARE(mixer.level(), 50); // stub initial state observed
        QCOMPARE(mixer.isMuted(), false);
        QCOMPARE(readySpy.count(), 1);
        QVERIFY(stateSpy.count() >= 1);
        QCOMPARE(stateSpy.last().at(0).toInt(), 50);
        QCOMPARE(stateSpy.last().at(1).toBool(), false);
        const QStringList lines = logLines(env.logPath);
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines.at(0).endsWith(QStringLiteral("scontrols")));
        QVERIFY(lines.at(1).endsWith(QStringLiteral("sget PCM")));
    }

    // One sset per level change — the echo carries the resulting state, so no
    // get/sset/get round trips. Clamping stays 0..100, same as before.
    void testAlsaSetLevelSingleInvocationAndClamp() {
        StubEnv env(0);
        MixerProcessRunner runner;
        ALSAVolumeMixer mixer(QString(), &runner, env.amixerPath);
        SettleWatch watch(&runner, &mixer);
        QVERIFY(pumpUntil([&] { return mixer.hasHardwareState(); }));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));

        clearLog(env.logPath);
        QVERIFY(mixer.setLevel(150));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QCOMPARE(mixer.level(), 100);
        QStringList lines = logLines(env.logPath);
        QCOMPARE(lines.size(), 1);
        QVERIFY(lines.at(0).endsWith(QStringLiteral("sset PCM 100%")));

        QVERIFY(mixer.setLevel(-7));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QCOMPARE(mixer.level(), 0);
        lines = logLines(env.logPath);
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines.at(1).endsWith(QStringLiteral("sset PCM 0%")));

        QVERIFY(mixer.setLevel(42));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QCOMPARE(mixer.level(), 42);
        lines = logLines(env.logPath);
        QCOMPARE(lines.size(), 3);
        QVERIFY(lines.at(2).endsWith(QStringLiteral("sset PCM 42%")));
        QCOMPARE(stateFileBody(env.statePath), QStringLiteral("42 0"));
    }

    // A level change must not disturb mute: on a combined control the sset
    // echo carries [off], and the cache keeps reporting muted.
    void testAlsaLevelChangePreservesMute() {
        StubEnv env(0);
        MixerProcessRunner runner;
        ALSAVolumeMixer mixer(QString(), &runner, env.amixerPath);
        SettleWatch watch(&runner, &mixer);
        QVERIFY(pumpUntil([&] { return mixer.hasHardwareState(); }));
        QVERIFY(mixer.setMuted(true));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QVERIFY(mixer.isMuted());

        clearLog(env.logPath);
        QVERIFY(mixer.setLevel(60));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QCOMPARE(mixer.level(), 60);
        QVERIFY(mixer.isMuted());
        const QStringList lines = logLines(env.logPath);
        QCOMPARE(lines.size(), 1);
        QVERIFY(lines.at(0).endsWith(QStringLiteral("sset PCM 60%")));
        QCOMPARE(stateFileBody(env.statePath), QStringLiteral("60 1"));
    }

    // Rapid repeats: the backend absorbs the burst into one pending value per
    // dimension, so three presses cost at most two children and the LAST
    // requested value always reaches hardware. Conservation law holds:
    // started + coalesced == submitted (the backend never doubles outstanding).
    void testAlsaRapidPressesCoalesce() {
        StubEnv env(200);
        MixerProcessRunner runner;
        ALSAVolumeMixer mixer(QString(), &runner, env.amixerPath);
        SettleWatch watch(&runner, &mixer);
        QVERIFY(pumpUntil([&] { return mixer.hasHardwareState(); }));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        const int startedBefore = runner.startedCount();

        clearLog(env.logPath);
        QVERIFY(mixer.setLevel(45));
        QVERIFY(mixer.setLevel(40));
        QVERIFY(mixer.setLevel(35));
        QCOMPARE(mixer.level(), 35); // optimistic cache tracks the last request
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QCOMPARE(mixer.level(), 35);
        QCOMPARE(stateFileBody(env.statePath), QStringLiteral("35 0"));
        const int burstChildren = logLines(env.logPath).size();
        QVERIFY(burstChildren >= 1);
        QVERIFY(burstChildren <= 2);
        QCOMPARE(runner.startedCount(), startedBefore + burstChildren);
        QCOMPARE(runner.coalescedCount(), 0); // absorbed before submit, not in the runner
    }

    // Split-control card: mute writes/reads address the SWITCH element, never
    // the volume element; level writes address the volume element and leave
    // the mute flag alone (the split echo carries no switch).
    void testAlsaMuteSplitSwitchTarget() {
        StubEnv env(0, "normal", "split");
        MixerProcessRunner runner;
        ALSAVolumeMixer mixer(QString(), &runner, env.amixerPath);
        SettleWatch watch(&runner, &mixer);
        QVERIFY(pumpUntil([&] { return mixer.hasHardwareState(); }));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QCOMPARE(mixer.level(), 50);
        QVERIFY(!mixer.isMuted());
        // Startup: scontrols + sget volume + sget switch (separate elements).
        QCOMPARE(logLines(env.logPath).size(), 3);

        clearLog(env.logPath);
        QVERIFY(mixer.setMuted(true));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QVERIFY(mixer.isMuted());
        QCOMPARE(mixer.level(), 50);
        QStringList lines = logLines(env.logPath);
        QCOMPARE(lines.size(), 1);
        QVERIFY(lines.at(0).endsWith(QStringLiteral("set PCM Playback Switch mute")));

        QVERIFY(mixer.setMuted(false));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QVERIFY(!mixer.isMuted());
        lines = logLines(env.logPath);
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines.at(1).endsWith(QStringLiteral("set PCM Playback Switch unmute")));

        clearLog(env.logPath);
        QVERIFY(mixer.setLevel(20));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QCOMPARE(mixer.level(), 20);
        QVERIFY(!mixer.isMuted()); // split sset echo has no switch -> cache kept
        lines = logLines(env.logPath);
        QCOMPARE(lines.size(), 1);
        QVERIFY(lines.at(0).endsWith(QStringLiteral("sset PCM Playback Volume 20%")));
        QCOMPARE(stateFileBody(env.statePath), QStringLiteral("20 0"));
    }

    // Combined control (the Pi): mute is driven on the volume control itself,
    // which carries the pswitch, and reconciled from the set echo.
    void testAlsaMuteCombinedTarget() {
        StubEnv env(0);
        MixerProcessRunner runner;
        ALSAVolumeMixer mixer(QString(), &runner, env.amixerPath);
        SettleWatch watch(&runner, &mixer);
        QVERIFY(pumpUntil([&] { return mixer.hasHardwareState(); }));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));

        clearLog(env.logPath);
        QVERIFY(mixer.setMuted(true));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QVERIFY(mixer.isMuted());
        QStringList lines = logLines(env.logPath);
        QCOMPARE(lines.size(), 1);
        QVERIFY(lines.at(0).endsWith(QStringLiteral("set PCM mute")));
        QCOMPARE(stateFileBody(env.statePath), QStringLiteral("50 1"));

        QVERIFY(mixer.setMuted(false));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QVERIFY(!mixer.isMuted());
        lines = logLines(env.logPath);
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines.at(1).endsWith(QStringLiteral("set PCM unmute")));
        QCOMPARE(stateFileBody(env.statePath), QStringLiteral("50 0"));
    }

    // Failing mixer (nonzero exit): discovery still completes, stateReady
    // still fires, cached state stays usable, nothing hangs. Mutations are
    // accepted optimistically (the historical m_level = v behaviour) and the
    // next successful observation would reconcile them.
    void testAlsaFailureKeepsStateUsable() {
        StubEnv env(0, "exit1");
        MixerProcessRunner runner;
        ALSAVolumeMixer mixer(QString(), &runner, env.amixerPath);
        SettleWatch watch(&runner, &mixer);
        QSignalSpy readySpy(&mixer, &ALSAVolumeMixer::stateReady);
        QVERIFY(pumpUntil([&] { return readySpy.count() == 1; }));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QVERIFY(!mixer.hasHardwareState());
        QCOMPARE(mixer.level(), 100); // default still usable
        QCOMPARE(mixer.isMuted(), false);

        QVERIFY(mixer.setLevel(30)); // accepted despite the broken mixer
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QCOMPARE(mixer.level(), 30);
        QCOMPARE(mixer.isMuted(), false);
        QVERIFY(logLines(env.logPath).size() >= 2); // discovery + write attempted
        QCOMPARE(watch.lastStatus, MixerProcessResult::Status::NonZeroExit);
    }

    // Garbage output parses to nothing: the cache keeps its defaults, the
    // backend stays usable, and no state is invented from unparseable text.
    void testAlsaGarbageKeepsDefaults() {
        StubEnv env(0, "garbage");
        MixerProcessRunner runner;
        ALSAVolumeMixer mixer(QString(), &runner, env.amixerPath);
        SettleWatch watch(&runner, &mixer);
        QSignalSpy readySpy(&mixer, &ALSAVolumeMixer::stateReady);
        QVERIFY(pumpUntil([&] { return readySpy.count() == 1; }));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QVERIFY(!mixer.hasHardwareState());
        QCOMPARE(mixer.level(), 100);
        QCOMPARE(mixer.isMuted(), false);

        QVERIFY(mixer.setLevel(20));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QCOMPARE(mixer.level(), 20); // optimistic, usable
        QVERIFY(!mixer.hasHardwareState()); // garbage never counts as observation
    }

    // Missing executable: construction is still instant, discovery reports
    // SpawnFailed and completes, cached state stays usable, no hang.
    void testAlsaMissingBinaryNoHang() {
        StubEnv env(0);
        MixerProcessRunner runner;
        QElapsedTimer clock;
        clock.start();
        ALSAVolumeMixer mixer(QString(), &runner,
                              env.dir.filePath(QStringLiteral("no-such-amixer")));
        QVERIFY(clock.elapsed() < 16);
        SettleWatch watch(&runner, &mixer);
        QSignalSpy readySpy(&mixer, &ALSAVolumeMixer::stateReady);
        QVERIFY(pumpUntil([&] { return readySpy.count() == 1; }));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QCOMPARE(watch.lastStatus, MixerProcessResult::Status::SpawnFailed);
        QVERIFY(!mixer.hasHardwareState());
        QCOMPARE(mixer.level(), 100);
        QVERIFY(mixer.setLevel(40));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QCOMPARE(mixer.level(), 40);
    }

    // A child slower than the 1000 ms budget is killed and reported TimedOut;
    // the cache stays usable and the caller never waits on it.
    void testAlsaTimeoutBounded() {
        StubEnv env(1500); // > default 1000 ms budget
        MixerProcessRunner runner;
        ALSAVolumeMixer mixer(QString(), &runner, env.amixerPath);
        SettleWatch watch(&runner, &mixer);
        QElapsedTimer clock;
        clock.start();
        QVERIFY(pumpUntil([&] { return watch.finished >= 1; }, 3000));
        QVERIFY(clock.elapsed() < 3000);
        QCOMPARE(watch.lastStatus, MixerProcessResult::Status::TimedOut);
        QVERIFY(!mixer.hasHardwareState());
        QCOMPARE(mixer.level(), 100); // cached default still usable
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }, 3000));
    }

    // Persisted-value compatibility: restore() calls setLevel/setMuted right
    // after construction, before discovery completes. The mutations must be
    // held, then flushed in order (level, then mute) against the DISCOVERED
    // control — never a guessed one — and both must reach hardware.
    void testAlsaRestoreFlushOrder() {
        StubEnv env(50);
        MixerProcessRunner runner;
        ALSAVolumeMixer mixer(QString(), &runner, env.amixerPath);
        SettleWatch watch(&runner, &mixer);
        QVERIFY(mixer.setLevel(70));
        QVERIFY(mixer.setMuted(true));
        QCOMPARE(mixer.level(), 70); // optimistic, immediate
        QVERIFY(mixer.isMuted());
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QVERIFY(mixer.hasHardwareState());
        QCOMPARE(mixer.level(), 70);
        QVERIFY(mixer.isMuted());
        QCOMPARE(stateFileBody(env.statePath), QStringLiteral("70 1"));
        const QStringList lines = logLines(env.logPath);
        QCOMPARE(lines.size(), 3); // scontrols + sset + set; echoes observed both
        QVERIFY(lines.at(0).endsWith(QStringLiteral("scontrols")));
        QVERIFY(lines.at(1).endsWith(QStringLiteral("sset PCM 70%")));
        QVERIFY(lines.at(2).endsWith(QStringLiteral("set PCM mute")));
    }

    // A read that was in flight when a newer mutation was accepted must not
    // resurrect the old hardware value over the user's request.
    void testAlsaStaleReadCannotResurrect() {
        StubEnv env(200);
        MixerProcessRunner runner;
        ALSAVolumeMixer mixer(QString(), &runner, env.amixerPath);
        SettleWatch watch(&runner, &mixer);
        // Wait until the initial sget is in flight (scontrols done, sget started).
        QVERIFY(pumpUntil([&] { return runner.startedCount() >= 2; }));
        QSignalSpy stateSpy(&mixer, &ALSAVolumeMixer::stateChanged);
        QVERIFY(mixer.setLevel(80)); // lands while the 50-read is still running
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        QCOMPARE(mixer.level(), 80);
        QCOMPARE(stateFileBody(env.statePath), QStringLiteral("80 0"));
        // Every observation emitted after the press must carry the new value;
        // the late 50-read was discarded by the generation guard.
        QVERIFY(stateSpy.count() >= 1);
        for (const auto &args : stateSpy)
            QCOMPARE(args.at(0).toInt(), 80);
    }

    // level()/isMuted() are pure cache reads: any number of them spawns nothing.
    void testAlsaCachedReadsNeverSpawn() {
        StubEnv env(200);
        MixerProcessRunner runner;
        ALSAVolumeMixer mixer(QString(), &runner, env.amixerPath);
        SettleWatch watch(&runner, &mixer);
        QVERIFY(pumpUntil([&] { return mixer.hasHardwareState(); }));
        QVERIFY(pumpUntil([&] { return watch.settled(&runner); }));
        const int started = runner.startedCount();
        clearLog(env.logPath);
        for (int i = 0; i < 50; ++i) {
            QCOMPARE(mixer.level(), 50);
            QCOMPARE(mixer.isMuted(), false);
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCOMPARE(logLines(env.logPath).size(), 0);
        QCOMPARE(runner.startedCount(), started);
    }
};

QTEST_MAIN(VolumeMixerTest)
#include "VolumeMixerTest.moc"