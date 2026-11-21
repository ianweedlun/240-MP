// IDLE-01 two-sided stale idle-count regression probe (checked-in).
//
// Contract under test: returning from playback WITHOUT keypresses must not
// fire the screensaver sooner than a full threshold of post-return idle.
// Pre-fix, setMpvActive(false)/setScriptActive(false) unblocked the 1 s tick
// without resetting the frozen pre-playback count: pre-idle to threshold-1,
// play, return keyless => saver fires on the FIRST tick (~1 s at the
// production 60 s threshold). The class fix (feature/idle01-unblock-reset)
// resets the count on the blocking->unblocked edge.
//
// Arms (one per process invocation, fresh IdleTracker each):
//   D  — pre-idle to threshold-1, setMpvActive(true) block, keyless return.
//        Pre-fix: fires on FIRST post-return tick (~0.5 s) => DEFECT.
//        Post-fix: full threshold (~3.4-4 s / 4 ticks) => PASS.
//   S  — arm D via setScriptActive (secondary producer), same shape.
//   H1 — same pre-idle + block, return + ONE real non-auto-repeat KeyPress
//        through the app event-filter path => healthy control, PASS both sides.
//   H2 — resetActivity() baseline (no pre-idle), block, keyless return
//        => healthy control, PASS both sides.
//
// Acceleration: production threshold default 60 s; the gate runs 4 s by
// default (real 1000 ms QTimer ticks, no timer scaling). Override with the
// orchestrator's IDLE01_THRESHOLD_S env or argv[3].
//
// This probe is an instrument: it always exits 0 when it ran (any verdict)
// and writes the measurement JSON; the pass/fail assertions live in
// run_idle_tracker_stale_count_case.py (ROB-07/volume-stall convention).
// The timing verdict below is diagnostic: the 1 s tick is phase-locked to
// construction, not to return, so a healthy count starting at 0 fires on the
// threshold-th tick AFTER return — the first of those ticks lands 0..1000 ms
// after return. Healthy band: latency >= (threshold-1)*1000*0.95 (Qt
// CoarseTimer fires up to 5% early) and >= threshold real ticks (reset
// emissions excluded). Defect band: latency <= 1100 ms and <= 2 real ticks.
// The gap between the bands is INCONCLUSIVE — inspect the phase log.
#include <QGuiApplication>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QTimer>
#include <QThread>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QDebug>
#include <cstdio>
#include <cstdarg>

#include "IdleTracker.h"

// Compiled-rev provenance, set by CMake from IDLE01_SOURCE_ROOT's git HEAD
// (the tree the IdleTracker TU came from — differs from the configure tree
// in two-sided control builds).
#ifndef IDLE01_SOURCE_REV
#define IDLE01_SOURCE_REV "unknown"
#endif

static QElapsedTimer g_clock;
static qint64 g_fireMs = -1;
static int g_idleEmissions = 0;
static int g_zeroEmissions = 0; // idleSecondsChanged with value 0 = a reset, not a tick
static int g_idleEmissionsDuringBlock = 0;
static int g_activityDetections = 0;
static bool g_inBlock = false;

static void logLine(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[T+%7lld ms] ", (long long)g_clock.elapsed());
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

// Continuous event pump for a fixed duration — never a blind sleep.
static void spinMs(qint64 ms)
{
    const qint64 end = g_clock.elapsed() + ms;
    while (g_clock.elapsed() < end) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    }
}

// Pump until predicate true or deadline; returns predicate value.
template <typename Pred>
static bool spinUntil(Pred p, qint64 deadlineMs)
{
    while (g_clock.elapsed() < deadlineMs) {
        if (p()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    }
    return p();
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: idle-tracker-stale-count-probe <D|H1|H2|S> <out.json> [threshold_s]\n");
        return 2;
    }
    const QString arm = QString::fromLatin1(argv[1]);
    const QString outPath = QString::fromLocal8Bit(argv[2]);
    if (arm != "D" && arm != "H1" && arm != "H2" && arm != "S") {
        fprintf(stderr, "unknown arm\n");
        return 2;
    }
    int threshold = 4; // accelerated default; production default is 60 s
    if (argc == 4) {
        bool ok = false;
        threshold = QString::fromLatin1(argv[3]).toInt(&ok);
        if (!ok || threshold < 3) {
            fprintf(stderr, "threshold_s must be an integer >= 3 (bands need a gap)\n");
            return 2;
        }
    }

    QGuiApplication app(argc, argv);
    g_clock.start();

    IdleTracker tracker(threshold);
    tracker.setEnabled(true); // production default is OFF until Main.qml applies setting

    QObject::connect(&tracker, &IdleTracker::idleSecondsChanged, [&]() {
        ++g_idleEmissions;
        if (tracker.idleSeconds() == 0) ++g_zeroEmissions;
        if (g_inBlock) ++g_idleEmissionsDuringBlock;
        logLine("idleSecondsChanged -> %d", tracker.idleSeconds());
    });
    QObject::connect(&tracker, &IdleTracker::activeChanged, [&]() {
        logLine("activeChanged -> active=%d", tracker.isActive());
        if (tracker.isActive() && g_fireMs < 0) g_fireMs = g_clock.elapsed();
    });
    QObject::connect(&tracker, &IdleTracker::activityDetected, [&]() {
        ++g_activityDetections;
        logLine("activityDetected (count now %d)", tracker.idleSeconds());
    });
    QObject::connect(&tracker, &IdleTracker::mpvActiveChanged, [&]() {
        logLine("mpvActiveChanged -> %d", tracker.isMpvActive());
    });
    QObject::connect(&tracker, &IdleTracker::scriptActiveChanged, [&]() {
        logLine("scriptActiveChanged -> %d", tracker.isScriptActive());
    });

    logLine("ARM %s start: threshold=%d s (accelerated from 60), enabled=%d, rev=%s",
            qPrintable(arm), threshold, tracker.isEnabled(), IDLE01_SOURCE_REV);

    QJsonObject result;
    result["arm"] = arm;
    result["source_rev"] = IDLE01_SOURCE_REV;
    result["threshold_s"] = threshold;
    result["acceleration"] = "production default 60 s -> accelerated threshold; real 1000 ms ticks, no timer scaling";

    // --- Phase 1: pre-idle ---
    const int preIdleTarget = (arm == "H2") ? 0 : threshold - 1;
    if (arm == "H2") {
        tracker.resetActivity(); // explicit fresh-idle baseline per card
        logLine("H2: resetActivity() baseline, count=%d", tracker.idleSeconds());
    }
    bool preOk = true;
    if (preIdleTarget > 0) {
        preOk = spinUntil([&]() { return tracker.idleSeconds() >= preIdleTarget; },
                          g_clock.elapsed() + (threshold + 6) * 1000);
        logLine("pre-idle reached: count=%d target=%d ok=%d",
                tracker.idleSeconds(), preIdleTarget, preOk);
    }
    result["pre_idle_target"] = preIdleTarget;
    result["pre_idle_reached"] = preOk;
    result["pre_idle_ms"] = (double)g_clock.elapsed();

    // --- Phase 2: playback block (real time passes, count must freeze) ---
    const qint64 blockStart = g_clock.elapsed();
    g_inBlock = true;
    const int countAtBlock = tracker.idleSeconds();
    if (arm == "S") tracker.setScriptActive(true);
    else            tracker.setMpvActive(true);
    spinMs(3500); // 3.5 real seconds of "playback"
    const int countAfterBlock = tracker.idleSeconds();
    const bool frozen = (countAfterBlock == countAtBlock) && (g_idleEmissionsDuringBlock == 0);
    g_inBlock = false;
    logLine("block done: countAtBlock=%d countAfterBlock=%d emissionsDuringBlock=%d frozen=%d",
            countAtBlock, countAfterBlock, g_idleEmissionsDuringBlock, frozen);
    result["block_start_ms"] = (double)blockStart;
    result["block_duration_ms"] = 3500;
    result["count_at_block"] = countAtBlock;
    result["count_after_block"] = countAfterBlock;
    result["idle_emissions_during_block"] = g_idleEmissionsDuringBlock;
    result["freeze_observed"] = frozen;

    // --- Phase 3: return ---
    const int emissionsBeforeReturn = g_idleEmissions;
    const int zeroEmissionsBeforeReturn = g_zeroEmissions;
    const qint64 tReturn = g_clock.elapsed();
    bool keySent = false;
    if (arm == "S") tracker.setScriptActive(false);
    else            tracker.setMpvActive(false);
    logLine("RETURN (no keys yet): count=%d active=%d", tracker.idleSeconds(), tracker.isActive());

    if (arm == "H1") {
        // One real non-auto-repeat KeyPress through the app delivery path:
        // QGuiApplication::sendEvent -> notify() -> app-wide event filters
        // (IdleTracker installs itself on QCoreApplication::instance()).
        QObject receiver;
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, " ");
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, " ");
        QGuiApplication::sendEvent(&receiver, &press);
        QGuiApplication::sendEvent(&receiver, &release);
        keySent = true;
        logLine("H1: sent real KeyPress+KeyRelease (Space, no auto-repeat); count=%d detections=%d",
                tracker.idleSeconds(), g_activityDetections);
    }
    result["key_sent"] = keySent;
    result["return_ms"] = (double)tReturn;

    // Wait for saver fire (or window expiry). Window: threshold + 4 s.
    const qint64 windowEnd = tReturn + (threshold + 4) * 1000;
    spinUntil([&]() { return g_fireMs >= 0; }, windowEnd);

    const bool fired = g_fireMs >= 0;
    const double latency = fired ? double(g_fireMs - tReturn) : -1.0;
    const int ticksAfterReturn = g_idleEmissions - emissionsBeforeReturn;
    const int realTicksAfterReturn = ticksAfterReturn - (g_zeroEmissions - zeroEmissionsBeforeReturn);
    const double contractMinMs = (threshold - 1) * 1000.0 * 0.95;
    const double defectMaxMs = 1100.0;
    QString verdict;
    if (!preOk || !frozen) {
        verdict = "INCONCLUSIVE"; // instrument sanity failed — debug rig, not product claim
    } else if (!fired) {
        verdict = "INCONCLUSIVE"; // saver never fired in window; unexpected for all arms
    } else if (latency <= defectMaxMs && realTicksAfterReturn <= 2) {
        verdict = "DEFECT"; // first-tick fire from the stale pre-playback count
    } else if (latency >= contractMinMs && realTicksAfterReturn >= threshold) {
        verdict = "PASS"; // full threshold of post-return idle elapsed
    } else {
        verdict = "INCONCLUSIVE"; // between bands — inspect phase/log before claiming
    }
    // activityDetected counts are side-dependent (the post-fix unblock edge
    // emits one itself) and are asserted by the orchestrator, not here.

    logLine("RESULT arm=%s fired=%d latency_ms=%.1f real_ticks_after_return=%d contract_min_ms=%.0f verdict=%s",
            qPrintable(arm), fired, latency, realTicksAfterReturn, contractMinMs, qPrintable(verdict));

    result["fired"] = fired;
    result["fire_latency_ms"] = latency;
    result["ticks_after_return"] = ticksAfterReturn;
    result["real_ticks_after_return"] = realTicksAfterReturn;
    result["contract_min_ms"] = contractMinMs;
    result["activity_detections"] = g_activityDetections;
    result["verdict"] = verdict;

    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly)) {
        fprintf(stderr, "cannot write %s\n", qPrintable(outPath));
        return 3;
    }
    const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Compact);
    out.write(QJsonDocument(result).toJson());
    out.close();
    printf("RESULT %s\n", json.constData());
    fflush(stdout);
    return 0;
}