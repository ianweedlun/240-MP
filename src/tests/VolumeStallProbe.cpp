// P-5A delayed-mixer UI-stall regression instrument (torture T1, checked-in).
//
// Promoted from the T1 lab harness (scratch torture-t1/t1_core.cpp,
// scenarioVolumeUi) into the repo's test tree. Drives the REAL InputManager
// file channel (/tmp/240mp_key.in), the REAL VolumeManager and the REAL
// ALSA/PipeWire mixer classes, offscreen Qt, no audio hardware: the mixer
// executables on PATH are scratch stubs whose delay/mode the orchestrator
// (run_volume_stall_case.py) controls via TORTURE_* env vars.
//
// Scenarios:
//   volume-alsa / volume-pipewire  — timing run: constructor probe, 3 direct
//       Volume Down presses, then a file-channel "voldown" injection while a
//       5 ms UI heartbeat measures the peak event-loop gap. Volume DOWN on
//       purpose: PipeWire starts at cached level 100, so Volume Up is a clamp
//       no-op that never reaches the mixer.
//   volume-noop  — same shape with the NoopVolumeMixer injected through
//       VolumeManager's two-arg constructor (requires TORTURE_FORCE_NOOP=1).
//       Green control: proves the measurement rig itself never stalls when
//       the mixer is instant, isolating any red to the hardware mixers.
//   fault-alsa / fault-pipewire  — robustness run: 3 Volume Down presses, a
//       mute toggle and one more press against a failing mixer (nonzero exit
//       / garbage output / missing executable, chosen by the orchestrator).
//       Must complete bounded, crash-free, with sane cached state.
//
// Like the ROB-07 contract probe, this binary always exits 0 when it ran;
// the pass/fail assertions live in the orchestrator. One machine-readable
// RESULT JSON line per run carries the measurements (see README-volume-stall.md).

#include <QGuiApplication>
#include <QQuickWindow>
#include <QTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QDebug>
#include <SDL.h>
#include <algorithm>
#include <functional>
#include <numeric>
#include <vector>

#include "AppCore.h"
#include "audio/VolumeManager.h"
#include "audio/VolumeKeyMap.h"
#include "audio/NoopVolumeMixer.h"
#include "input/InputManager.h"

namespace {

constexpr qint64 kTargetMs = 16;
const QString kKeyChannelPath = QStringLiteral("/tmp/240mp_key.in");

void writeFile(const QString &path, const QByteArray &body)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        qFatal("cannot write %s", qPrintable(path));
    f.write(body);
    f.close();
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

int mixerLogLines(const QString &path)
{
    if (path.isEmpty()) return 0;
    QFile log(path);
    if (!log.open(QIODevice::ReadOnly)) return 0;
    int lines = 0;
    while (!log.atEnd()) { log.readLine(); ++lines; }
    return lines;
}

QJsonObject readJson(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

struct RunInput
{
    QString scenario;      // volume-alsa | volume-pipewire | volume-noop | fault-*
    QString backend;       // alsa | pipewire | none
    bool faultMode = false;
    bool forceNoop = false;
    QString repoRoot;
    QString dataRoot;
    QString mixerLog;
    QString snapshot;
    int stubDelayMs = 0;
    QString stubMode;
};

int runScenario(const RunInput &in)
{
    QDir().mkpath(in.dataRoot);
    writeFile(in.dataRoot + QStringLiteral("/config.json"),
              R"({"app":{"color_scheme":"Video 1"},"modules":{}})");
    QFile::remove(in.dataRoot + QStringLiteral("/volume.json"));
    QFile::remove(kKeyChannelPath);

    // ── Constructor probe: ALSA scans `amixer scontrols` here, so a slow
    // mixer stalls app startup before any key is even pressed.
    QElapsedTimer constructClock;
    constructClock.start();
    VolumeManager volume(in.dataRoot,
                         in.forceNoop ? std::unique_ptr<audio::VolumeMixer>(
                                            std::make_unique<audio::NoopVolumeMixer>())
                                      : nullptr);
    const qint64 constructMs = constructClock.elapsed();
    const int callsAfterConstructor = mixerLogLines(in.mixerLog);

    AppCore core(in.repoRoot, in.dataRoot);
    InputManager input(in.dataRoot, &core);
    QQuickWindow window;
    window.resize(640, 480);
    input.setTargetWindow(&window);
    input.setVolumeManager(&volume);
    window.installEventFilter(&input);
    window.show();
    window.requestActivate();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    // Track what the UI actually sees: every volumeChanged emission is one
    // settled press. The injected file-channel key must add one emission on
    // top of the direct presses, or the injection never reached the mixer.
    int volumeChangedSignals = 0;
    int lastSignalLevel = -1;
    bool lastSignalMuted = false;
    QObject::connect(&volume, &VolumeManager::volumeChanged, &volume,
                     [&](int level, bool muted) {
        ++volumeChangedSignals;
        lastSignalLevel = level;
        lastSignalMuted = muted;
    });

    // ── Direct presses: the exact call the InputManager eventFilter makes
    // for a hardware Volume Down key (audio::applyVolumeKey, VolumeKeyMap.h).
    std::vector<qint64> directTimes;
    for (int i = 0; i < 3; ++i) {
        QElapsedTimer one;
        one.start();
        audio::applyVolumeKey(volume, audio::VolumeKey::Down);
        directTimes.push_back(one.elapsed());
    }
    const int callsAfterDirect = mixerLogLines(in.mixerLog);
    const int postDirectLevel = lastSignalLevel;

    qint64 baselineMaxGap = 0, injectionMaxGap = 0;
    bool injectedKeyDelivered = false;
    int extraPresses = 0;

    if (in.faultMode) {
        // Mute toggle exercises the setMuted confirm-read path (ALSA: set +
        // sget), then one more level press. No heartbeat: fault runs assert
        // bounded completion and sane state, not the stall gate.
        QElapsedTimer muteClock;
        muteClock.start();
        // Use the return value, NOT volume.muted(): a muted() read here would
        // spawn another mixer call and pollute the phase-boundary counts.
        const bool muteResult = volume.toggleMute();
        const qint64 muteMs = muteClock.elapsed();
        QElapsedTimer one;
        one.start();
        audio::applyVolumeKey(volume, audio::VolumeKey::Down);
        directTimes.push_back(one.elapsed());
        extraPresses = 2;
        QJsonObject faultExtra{
            {QStringLiteral("mute_toggle_ms"), muteMs},
            {QStringLiteral("mute_toggle_result"), muteResult}
        };
        const int callsAfterExtra = mixerLogLines(in.mixerLog);
        const int finalLevel = volume.level();
        const bool finalMuted = volume.muted();
        const int callsTotal = mixerLogLines(in.mixerLog);
        const QJsonObject persisted = readJson(in.dataRoot + QStringLiteral("/volume.json"));
        const qint64 directTotal = std::accumulate(directTimes.begin(), directTimes.end(), qint64(0));
        const qint64 maxDirect = *std::max_element(directTimes.begin(), directTimes.end());
        QJsonArray directRows;
        for (qint64 v : directTimes) directRows.append(v);
        const bool saneState = finalLevel >= 0 && finalLevel <= 100;
        QJsonObject out{
            {QStringLiteral("scenario"), QStringLiteral("P-5A")},
            {QStringLiteral("case"), in.scenario},
            {QStringLiteral("snapshot"), in.snapshot},
            {QStringLiteral("backend_requested"), in.backend},
            {QStringLiteral("backend_selected"), volume.backendName()},
            {QStringLiteral("stub_delay_ms"), in.stubDelayMs},
            {QStringLiteral("stub_mode"), in.stubMode},
            {QStringLiteral("constructor_ms"), constructMs},
            {QStringLiteral("direct_key_times_ms"), directRows},
            {QStringLiteral("direct_key_mean_ms"), directTimes.empty() ? 0.0 : double(directTotal) / directTimes.size()},
            {QStringLiteral("direct_key_max_ms"), maxDirect},
            {QStringLiteral("volume_changed_signals"), volumeChangedSignals},
            {QStringLiteral("final_level"), finalLevel},
            {QStringLiteral("final_muted"), finalMuted},
            {QStringLiteral("persisted_level"), persisted.contains(QStringLiteral("level"))
                ? QJsonValue(persisted.value(QStringLiteral("level")).toInt()) : QJsonValue()},
            {QStringLiteral("persisted_muted"), persisted.contains(QStringLiteral("muted"))
                ? QJsonValue(persisted.value(QStringLiteral("muted")).toBool()) : QJsonValue()},
            {QStringLiteral("mixer_calls_after_constructor"), callsAfterConstructor},
            {QStringLiteral("mixer_calls_after_direct"), callsAfterDirect},
            {QStringLiteral("mixer_calls_after_presses"), callsAfterExtra},
            {QStringLiteral("mixer_calls_total"), callsTotal},
            {QStringLiteral("per_press_direct"), (callsAfterDirect - callsAfterConstructor) / 3},
            {QStringLiteral("extra_presses"), extraPresses},
            {QStringLiteral("fault_extra"), faultExtra},
            {QStringLiteral("target_ms"), kTargetMs},
            {QStringLiteral("pass"), saneState},
            {QStringLiteral("anchor"), in.backend == QLatin1String("alsa")
                ? QStringLiteral("src/audio/ALSAVolumeMixer.cpp:42-47 (runAmixer synchronous); VolumeManager.cpp:37-47,49-60,93-105 (7 spawns/press)")
                : QStringLiteral("src/audio/PipeWireVolumeMixer.cpp:10-15 (run synchronous),17-25 (setLevel caches m_level at :22 before success check)")}
        };
        QTextStream(stdout) << "RESULT " << QJsonDocument(out).toJson(QJsonDocument::Compact) << Qt::endl;
        QFile::remove(kKeyChannelPath);
        return 0;
    }

    // ── Heartbeat + file-channel injection (timing runs). Shape preserved
    // from the lab harness so numbers stay comparable with the recorded
    // 3970174 baseline: 5 ms heartbeat, 300 ms baseline window, one
    // "voldown" line written to the real debug key channel, 2500 ms total.
    // The channel polls every 200 ms (InputManager::startDebugKeyChannel),
    // so the injection gap includes up to one poll interval of alignment.
    QElapsedTimer clock;
    clock.start();
    qint64 lastBeat = 0;
    bool injected = false;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        const qint64 now = clock.elapsed();
        const qint64 gap = now - lastBeat;
        if (lastBeat > 0) {
            if (injected) injectionMaxGap = std::max(injectionMaxGap, gap);
            else baselineMaxGap = std::max(baselineMaxGap, gap);
        }
        lastBeat = now;
    });
    heartbeat.start();
    QTimer::singleShot(300, [&] {
        injected = true;
        writeFile(kKeyChannelPath, "voldown\n");
    });
    waitUntil([] { return false; }, 2500);
    heartbeat.stop();
    const int callsAfterPresses = mixerLogLines(in.mixerLog);
    extraPresses = 1;
    injectedKeyDelivered = volumeChangedSignals >= 4 && lastSignalLevel != postDirectLevel;

    const int finalLevel = volume.level();
    const bool finalMuted = volume.muted();
    const int callsTotal = mixerLogLines(in.mixerLog);
    const QJsonObject persisted = readJson(in.dataRoot + QStringLiteral("/volume.json"));

    const qint64 directTotal = std::accumulate(directTimes.begin(), directTimes.end(), qint64(0));
    const qint64 maxDirect = *std::max_element(directTimes.begin(), directTimes.end());
    QJsonArray directRows;
    for (qint64 v : directTimes) directRows.append(v);
    const bool pass = constructMs < kTargetMs && maxDirect < kTargetMs
                      && injectionMaxGap < kTargetMs && injectedKeyDelivered;
    QJsonObject out{
        {QStringLiteral("scenario"), QStringLiteral("P-5A")},
        {QStringLiteral("case"), in.scenario},
        {QStringLiteral("snapshot"), in.snapshot},
        {QStringLiteral("backend_requested"), in.backend},
        {QStringLiteral("backend_selected"), volume.backendName()},
        {QStringLiteral("stub_delay_ms"), in.stubDelayMs},
        {QStringLiteral("stub_mode"), in.stubMode},
        {QStringLiteral("force_noop"), in.forceNoop},
        {QStringLiteral("constructor_ms"), constructMs},
        {QStringLiteral("direct_key_times_ms"), directRows},
        {QStringLiteral("direct_key_mean_ms"), directTimes.empty() ? 0.0 : double(directTotal) / directTimes.size()},
        {QStringLiteral("direct_key_max_ms"), maxDirect},
        {QStringLiteral("baseline_peak_heartbeat_gap_ms"), baselineMaxGap},
        {QStringLiteral("injection_peak_heartbeat_gap_ms"), injectionMaxGap},
        {QStringLiteral("injected_key_delivered"), injectedKeyDelivered},
        {QStringLiteral("volume_changed_signals"), volumeChangedSignals},
        {QStringLiteral("post_direct_level"), postDirectLevel},
        {QStringLiteral("final_level"), finalLevel},
        {QStringLiteral("final_muted"), finalMuted},
        {QStringLiteral("persisted_level"), persisted.contains(QStringLiteral("level"))
            ? QJsonValue(persisted.value(QStringLiteral("level")).toInt()) : QJsonValue()},
        {QStringLiteral("persisted_muted"), persisted.contains(QStringLiteral("muted"))
            ? QJsonValue(persisted.value(QStringLiteral("muted")).toBool()) : QJsonValue()},
        {QStringLiteral("mixer_calls_after_constructor"), callsAfterConstructor},
        {QStringLiteral("mixer_calls_after_direct"), callsAfterDirect},
        {QStringLiteral("mixer_calls_after_presses"), callsAfterPresses},
        {QStringLiteral("mixer_calls_total"), callsTotal},
        {QStringLiteral("per_press_direct"), (callsAfterDirect - callsAfterConstructor) / 3},
        {QStringLiteral("per_press_extra"), callsAfterPresses - callsAfterDirect},
        {QStringLiteral("extra_presses"), extraPresses},
        {QStringLiteral("target_ms"), kTargetMs},
        {QStringLiteral("pass"), pass},
        {QStringLiteral("anchor"), in.backend == QLatin1String("alsa")
            ? QStringLiteral("src/audio/ALSAVolumeMixer.cpp:42-47 (runAmixer synchronous); VolumeManager.cpp:37-47,49-60,93-105 (7 spawns/press)")
            : QStringLiteral("src/audio/PipeWireVolumeMixer.cpp:10-15 (run synchronous),17-25 (setLevel caches m_level at :22 before success check)")}
    };
    QTextStream(stdout) << "RESULT " << QJsonDocument(out).toJson(QJsonDocument::Compact) << Qt::endl;
    QFile::remove(kKeyChannelPath);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("SDL_VIDEODRIVER", "dummy");
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("240-MP-Volume-Stall-Probe"));

    const QString scenario = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    RunInput in;
    in.scenario = scenario;
    if (scenario == QLatin1String("volume-alsa")) in.backend = QStringLiteral("alsa");
    else if (scenario == QLatin1String("volume-pipewire")) in.backend = QStringLiteral("pipewire");
    else if (scenario == QLatin1String("volume-noop")) { in.backend = QStringLiteral("none"); in.forceNoop = true; }
    else if (scenario == QLatin1String("fault-alsa")) { in.backend = QStringLiteral("alsa"); in.faultMode = true; }
    else if (scenario == QLatin1String("fault-pipewire")) { in.backend = QStringLiteral("pipewire"); in.faultMode = true; }
    else {
        QTextStream(stderr) << "usage: volume-stall-probe <volume-alsa|volume-pipewire|volume-noop|fault-alsa|fault-pipewire>\n";
        return 64;
    }
    if (in.forceNoop && qEnvironmentVariable("TORTURE_FORCE_NOOP") != QLatin1String("1")) {
        QTextStream(stderr) << "volume-noop requires TORTURE_FORCE_NOOP=1 (orchestrator-set)\n";
        return 64;
    }
    in.repoRoot = QStringLiteral(MP240_REPO_ROOT);
    in.mixerLog = qEnvironmentVariable("TORTURE_MIXER_LOG");
    in.snapshot = qEnvironmentVariable("TORTURE_SNAPSHOT", QStringLiteral("unknown"));
    in.stubDelayMs = qEnvironmentVariable("TORTURE_DELAY_MS", QStringLiteral("0")).toInt();
    in.stubMode = qEnvironmentVariable("TORTURE_STUB_MODE", QStringLiteral("normal"));
    QString dataRoot = qEnvironmentVariable("TORTURE_DATA_ROOT");
    QTemporaryDir fallbackDir;
    if (dataRoot.isEmpty()) {
        if (!fallbackDir.isValid()) return 70;
        dataRoot = fallbackDir.path() + QStringLiteral("/data");
    }
    in.dataRoot = dataRoot;
    return runScenario(in);
}