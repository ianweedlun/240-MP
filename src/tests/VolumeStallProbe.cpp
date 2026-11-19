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
//   edge-alsa / edge-pipewire  — semantics-under-delay run: a 25-press Volume
//       Down repeat burst drives the level to the 0 clamp (ALSA from hardware
//       50, PipeWire from cached 100), 3 more presses prove the clamp is a
//       no-op (no signal, no mixer child), then a file-channel "volmute"
//       injection mutes and a direct toggleMute() unmutes. A 5 ms UI
//       heartbeat gates the whole phase (<16 ms); the orchestrator asserts
//       the exact OSD signal count (one per effective press + the two mute
//       events) and persisted == final.
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

// Async counting boundary (T1 P-5A fix): mixer children run on the runner's
// worker thread, so a phase boundary must wait for the argv log to settle
// before counting — otherwise the count races the spawn. "Settled" = no new
// line for delay + 150 ms (the largest gap between consecutive children is
// one child's runtime plus spawn overhead). Bounded so a missing mixer or a
// stuck stub can never wedge the probe.
void waitLogQuiescent(const QString &mixerLog, int delayMs)
{
    const int quietWindowMs = delayMs + 150;
    QElapsedTimer clock;
    clock.start();
    const qint64 deadlineMs = qMax(4000, delayMs * 15);
    int lastCount = mixerLogLines(mixerLog);
    qint64 lastChange = 0;
    while (clock.elapsed() < deadlineMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
        const int count = mixerLogLines(mixerLog);
        if (count != lastCount) {
            lastCount = count;
            lastChange = clock.elapsed();
        } else if (clock.elapsed() - lastChange >= quietWindowMs) {
            return;
        }
    }
}

QJsonObject readJson(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

struct RunInput
{
    QString scenario;      // volume-alsa | volume-pipewire | volume-noop | fault-* | edge-*
    QString backend;       // alsa | pipewire | none
    bool faultMode = false;
    bool edgeMode = false;
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

    // Async startup contract (T1 P-5A fix): the mixer probes hardware in the
    // background, so measured presses must wait for the first hardware
    // observation — otherwise deltas would be computed against the cached
    // default (100) instead of the real level, breaking the settled ladder.
    // The wait itself pumps the event loop; it is NOT part of any stall gate.
    // Fault runs skip it: a failing mixer never reports hardware state, and
    // their assertions are bounded completion + sane cached state.
    qint64 readyWaitMs = 0;
    if (!in.faultMode) {
        QElapsedTimer readyClock;
        readyClock.start();
        waitUntil([&] { return volume.mixerReady(); }, qMax(2000, in.stubDelayMs * 10));
        readyWaitMs = readyClock.elapsed();
    }
    waitLogQuiescent(in.mixerLog, in.stubDelayMs);
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
    waitLogQuiescent(in.mixerLog, in.stubDelayMs);
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
        waitLogQuiescent(in.mixerLog, in.stubDelayMs);
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
            {QStringLiteral("mixer_calls_direct_total"), callsAfterDirect - callsAfterConstructor},
            {QStringLiteral("per_press_direct"), (callsAfterDirect - callsAfterConstructor) / 3},
            {QStringLiteral("backend_ready"), volume.mixerReady()},
            {QStringLiteral("ready_wait_ms"), readyWaitMs},
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
    waitLogQuiescent(in.mixerLog, in.stubDelayMs);
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
        {QStringLiteral("mixer_calls_direct_total"), callsAfterDirect - callsAfterConstructor},
        {QStringLiteral("per_press_direct"), (callsAfterDirect - callsAfterConstructor) / 3},
        {QStringLiteral("per_press_extra"), callsAfterPresses - callsAfterDirect},
        {QStringLiteral("backend_ready"), volume.mixerReady()},
        {QStringLiteral("ready_wait_ms"), readyWaitMs},
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

// Edge scenario (t_7621e31a verification card): repeat burst → clamp at 0 →
// mute through the REAL file channel → direct unmute, all under the stub
// delay, with the 5 ms heartbeat gating every caller-visible phase. Setup is
// the same real path as the timing runs (AppCore + InputManager eventFilter +
// VolumeManager + hardware mixer); only the press program differs.
int runEdgeScenario(const RunInput &in)
{
    constexpr int kBurstPresses = 25;   // ALSA 50→0 in 10, PipeWire 100→0 in 20
    constexpr int kClampPresses = 3;    // no-op presses at the 0 clamp

    QDir().mkpath(in.dataRoot);
    writeFile(in.dataRoot + QStringLiteral("/config.json"),
              R"({"app":{"color_scheme":"Video 1"},"modules":{}})");
    QFile::remove(in.dataRoot + QStringLiteral("/volume.json"));
    QFile::remove(kKeyChannelPath);

    QElapsedTimer constructClock;
    constructClock.start();
    VolumeManager volume(in.dataRoot, nullptr);
    const qint64 constructMs = constructClock.elapsed();

    // Same startup contract as the timing runs: wait for the first hardware
    // observation so the burst computes deltas against the real level.
    QElapsedTimer readyClock;
    readyClock.start();
    waitUntil([&] { return volume.mixerReady(); }, qMax(2000, in.stubDelayMs * 10));
    const qint64 readyWaitMs = readyClock.elapsed();
    waitLogQuiescent(in.mixerLog, in.stubDelayMs);
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

    int volumeChangedSignals = 0;
    int lastSignalLevel = -1;
    bool lastSignalMuted = false;
    QObject::connect(&volume, &VolumeManager::volumeChanged, &volume,
                     [&](int level, bool muted) {
        ++volumeChangedSignals;
        lastSignalLevel = level;
        lastSignalMuted = muted;
    });

    // Heartbeat across the whole edge phase: baseline window first (rig
    // noise floor), then every gap while presses/injection/persistence run.
    QElapsedTimer clock;
    clock.start();
    qint64 lastBeat = 0;
    qint64 baselineMaxGap = 0, edgeMaxGap = 0;
    bool active = false;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        const qint64 now = clock.elapsed();
        const qint64 gap = now - lastBeat;
        if (lastBeat > 0) {
            if (active) edgeMaxGap = std::max(edgeMaxGap, gap);
            else baselineMaxGap = std::max(baselineMaxGap, gap);
        }
        lastBeat = now;
    });
    heartbeat.start();
    waitUntil([] { return false; }, 300);
    active = true;

    // ── Repeat burst: held-key shape. Optimistic updates make every press
    // instant; latest-wins coalescing collapses the mixer children.
    std::vector<qint64> directTimes;
    for (int i = 0; i < kBurstPresses; ++i) {
        QElapsedTimer one;
        one.start();
        audio::applyVolumeKey(volume, audio::VolumeKey::Down);
        directTimes.push_back(one.elapsed());
    }
    waitLogQuiescent(in.mixerLog, in.stubDelayMs);
    const int callsAfterBurst = mixerLogLines(in.mixerLog);
    const int postBurstLevel = volume.level();
    const int postBurstSignalLevel = lastSignalLevel;

    // ── Clamp proof: presses at 0 must be no-ops — no signal, no child.
    const int signalsBeforeClamp = volumeChangedSignals;
    for (int i = 0; i < kClampPresses; ++i) {
        QElapsedTimer one;
        one.start();
        audio::applyVolumeKey(volume, audio::VolumeKey::Down);
        directTimes.push_back(one.elapsed());
    }
    waitLogQuiescent(in.mixerLog, in.stubDelayMs);
    const int callsAfterClamp = mixerLogLines(in.mixerLog);
    const int clampLevel = volume.level();
    const int clampSignals = volumeChangedSignals - signalsBeforeClamp;

    // ── Mute through the real file channel (200 ms poll alignment), then a
    // direct toggle back. The injection must add exactly one coherent signal.
    const int signalsBeforeMute = volumeChangedSignals;
    writeFile(kKeyChannelPath, "volmute\n");
    const bool muteInjectedDelivered = waitUntil([&] {
        return volumeChangedSignals > signalsBeforeMute && lastSignalMuted;
    }, 2000) && volume.muted();
    QElapsedTimer muteClock;
    muteClock.start();
    const bool muteToggleResult = volume.toggleMute();
    const qint64 muteToggleMs = muteClock.elapsed();

    // Let the debounced persist (250 ms) flush off-thread while the heartbeat
    // keeps gating — persistence must not stall the UI either.
    waitLogQuiescent(in.mixerLog, in.stubDelayMs);
    waitUntil([] { return false; }, 700);
    heartbeat.stop();

    const int finalLevel = volume.level();
    const bool finalMuted = volume.muted();
    const int callsTotal = mixerLogLines(in.mixerLog);
    const QJsonObject persisted = readJson(in.dataRoot + QStringLiteral("/volume.json"));
    const qint64 directTotal = std::accumulate(directTimes.begin(), directTimes.end(), qint64(0));
    const qint64 maxDirect = *std::max_element(directTimes.begin(), directTimes.end());
    QJsonArray directRows;
    for (qint64 v : directTimes) directRows.append(v);
    const bool pass = constructMs < kTargetMs && maxDirect < kTargetMs
                      && edgeMaxGap < kTargetMs && postBurstLevel == 0
                      && clampLevel == 0 && muteInjectedDelivered && !finalMuted;
    QJsonObject out{
        {QStringLiteral("scenario"), QStringLiteral("P-5A-edge")},
        {QStringLiteral("case"), in.scenario},
        {QStringLiteral("snapshot"), in.snapshot},
        {QStringLiteral("backend_requested"), in.backend},
        {QStringLiteral("backend_selected"), volume.backendName()},
        {QStringLiteral("stub_delay_ms"), in.stubDelayMs},
        {QStringLiteral("stub_mode"), in.stubMode},
        {QStringLiteral("constructor_ms"), constructMs},
        {QStringLiteral("burst_presses"), kBurstPresses},
        {QStringLiteral("clamp_presses"), kClampPresses},
        {QStringLiteral("direct_key_times_ms"), directRows},
        {QStringLiteral("direct_key_mean_ms"), directTimes.empty() ? 0.0 : double(directTotal) / directTimes.size()},
        {QStringLiteral("direct_key_max_ms"), maxDirect},
        {QStringLiteral("baseline_peak_heartbeat_gap_ms"), baselineMaxGap},
        {QStringLiteral("edge_peak_heartbeat_gap_ms"), edgeMaxGap},
        {QStringLiteral("post_burst_level"), postBurstLevel},
        {QStringLiteral("post_burst_signal_level"), postBurstSignalLevel},
        {QStringLiteral("clamp_level"), clampLevel},
        {QStringLiteral("clamp_signals"), clampSignals},
        {QStringLiteral("clamp_calls_delta"), callsAfterClamp - callsAfterBurst},
        {QStringLiteral("mute_injected_delivered"), muteInjectedDelivered},
        {QStringLiteral("mute_toggle_ms"), muteToggleMs},
        {QStringLiteral("mute_toggle_result"), muteToggleResult},
        {QStringLiteral("volume_changed_signals"), volumeChangedSignals},
        {QStringLiteral("final_level"), finalLevel},
        {QStringLiteral("final_muted"), finalMuted},
        {QStringLiteral("persisted_level"), persisted.contains(QStringLiteral("level"))
            ? QJsonValue(persisted.value(QStringLiteral("level")).toInt()) : QJsonValue()},
        {QStringLiteral("persisted_muted"), persisted.contains(QStringLiteral("muted"))
            ? QJsonValue(persisted.value(QStringLiteral("muted")).toBool()) : QJsonValue()},
        {QStringLiteral("mixer_calls_after_constructor"), callsAfterConstructor},
        {QStringLiteral("mixer_calls_after_burst"), callsAfterBurst},
        {QStringLiteral("mixer_calls_after_clamp"), callsAfterClamp},
        {QStringLiteral("mixer_calls_total"), callsTotal},
        {QStringLiteral("mixer_calls_burst_total"), callsAfterBurst - callsAfterConstructor},
        {QStringLiteral("backend_ready"), volume.mixerReady()},
        {QStringLiteral("ready_wait_ms"), readyWaitMs},
        {QStringLiteral("target_ms"), kTargetMs},
        {QStringLiteral("pass"), pass},
        {QStringLiteral("anchor"), QStringLiteral("src/audio/VolumeManager.cpp (changeVolume clamp + submitLevel/submitMuted no-op guards); src/input/InputManager.cpp (volmute file channel)")},
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
    else if (scenario == QLatin1String("edge-alsa")) { in.backend = QStringLiteral("alsa"); in.edgeMode = true; }
    else if (scenario == QLatin1String("edge-pipewire")) { in.backend = QStringLiteral("pipewire"); in.edgeMode = true; }
    else {
        QTextStream(stderr) << "usage: volume-stall-probe <volume-alsa|volume-pipewire|volume-noop|fault-alsa|fault-pipewire|edge-alsa|edge-pipewire>\n";
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
    return in.edgeMode ? runEdgeScenario(in) : runScenario(in);
}