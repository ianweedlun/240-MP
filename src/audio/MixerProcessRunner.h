#ifndef MIXERPROCESSRUNNER_H
#define MIXERPROCESSRUNNER_H

#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <optional>

class QThread;

namespace audio {

// ---------------------------------------------------------------------------
// MixerProcessRunner — the shared asynchronous subprocess + state-cache seam
// for the volume mixer backends (T1 P-5A). Replaces the old stack-QProcess +
// waitForFinished(1000) pattern in ALSAVolumeMixer / PipeWireVolumeMixer.
//
// THREADING CONTRACT (read this before implementing a backend on top):
//  * The runner object (the "facade") lives on the thread that constructs it —
//    normally the UI/owner thread. Every public method is thread-safe and
//    non-blocking: submit() and the cached accessors only take a short mutex
//    and post an event; they NEVER start a process, never wait on one, and
//    return in well under a frame budget (unit-tested < 16 ms with a 200 ms
//    child in flight).
//  * All QProcess and QTimer activity happens on a dedicated internal worker
//    thread owned by the runner. There is no waitForStarted()/waitForFinished()
//    on any caller thread; completion is delivered through QProcess signals.
//    (The single bounded wait in the whole class is a post-SIGKILL reap on the
//    worker thread during teardown, so the QProcess dies in NotRunning state.)
//  * finished(), requestStarted() and stateChanged() are emitted on the OWNER
//    thread, marshalled back from the worker. Connect to them directly from
//    the owner thread; no queued-connection gymnastics needed.
//  * A request's parser runs on the OWNER thread too, so it may safely touch
//    backend state. Keep it pure and cheap (regex over stdout).
//
// COALESCING CONTRACT:
//  * Fire-and-forget: submit() enqueues a request and returns. At most ONE
//    child process is in flight and at most ONE trailing request is queued.
//    While a child runs, every new submit() REPLACES the queued trailing
//    request (latest-wins), so held-key repeat can never pile up subprocesses.
//    Intermediate volume steps are intentionally dropped; the last requested
//    value always runs.
//  * Consequence for backends: a refresh read queued behind a mutation can be
//    coalesced away by a newer mutation. If you need a final hardware read,
//    re-arm it from the mutation's finished() handler — after the LAST
//    mutation completes, nothing newer arrives and the read survives.
//  * Execution is serial and results are delivered in start order. The seam
//    does not decide staleness policy: if a late read could resurrect a value
//    the user has already moved past, guard on the result's tag/arguments (or
//    a backend generation counter) before applying it.
//
// CACHING CONTRACT:
//  * cachedLevel()/cachedMuted()/hasHardwareState() are synchronous-looking
//    accessors that ONLY return cached values — they never spawn a process.
//    Defaults match the historical backend caches: level 100, muted false.
//    hasHardwareState() is false until a successful parsed read proves the
//    cache reflects hardware; use it to tell "observed" from "assumed".
//  * Only a successful run whose parser returns a value updates the cache
//    (and emits stateChanged()). Nonzero exit, spawn failure, timeout and
//    unparseable output all leave the last known good cache untouched.
//  * setCachedState() seeds/optimistically updates the cache (clamped 0..100)
//    without emitting stateChanged() — it is not a hardware observation.
//    Callers that surface optimistic values emit their own notifications.
//
// FAILURE + TIMEOUT CONTRACT:
//  * Every request carries a bounded budget (timeoutMs, default 1000 ms — the
//    historical amixer/wpctl budget), enforced by a timer on the worker
//    thread. On expiry the child is SIGKILLed and the result is TimedOut.
//  * finished() is emitted for EVERY terminal outcome with a Status of
//    Success, NonZeroExit, SpawnFailed, TimedOut or UnparseableOutput, plus a
//    human-readable diagnostic; failures are also logged through the
//    "audio.mixer.runner" logging category. No failure mode wedges the queue:
//    the next queued request starts immediately afterwards.
//
// SHUTDOWN CONTRACT:
//  * Destroy the runner on its owner thread (like any QObject). The destructor
//    discards the queued request, kills any in-flight child, joins the worker
//    thread and deletes everything it owns — no leaked QProcess, no detached
//    threads, no callbacks into destroyed objects. Teardown is bounded
//    (SIGKILL + short reap) but is NOT a hot-path call: do not destroy and
//    recreate runners per key press.
// ---------------------------------------------------------------------------

// Cached mixer state. Percentages are 0..100; mute is independent of level.
struct MixerState {
    int level = 100;
    bool muted = false;
};

// One subprocess request. `tag` is a diagnostic label ("sset", "get-volume",
// "scontrols", ...) echoed back in the result so backends can correlate
// completions. `parser` is optional: when set, a successful run's stdout is
// parsed on the owner thread; a value updates the cache, std::nullopt turns
// the result into UnparseableOutput. Without a parser the raw output is simply
// delivered in finished() (e.g. control discovery or mutation echoes).
struct MixerProcessRequest {
    QString program;
    QStringList arguments;
    QString tag;
    int timeoutMs = 1000;
    std::function<std::optional<MixerState>(const QString &stdOut)> parser;
};

// Terminal outcome of one request, delivered on the owner thread.
struct MixerProcessResult {
    enum class Status {
        Success,           // exit 0, normal exit (and parsed, if parser set)
        NonZeroExit,       // nonzero exit code or crash
        SpawnFailed,       // executable missing / not launchable
        TimedOut,          // budget expired; child was killed
        UnparseableOutput, // exit 0 but the parser rejected the output
    };
    QString tag;
    QStringList arguments;
    Status status = Status::Success;
    int exitCode = -1;
    QString stdOut;
    QString stdErr;
    QString diagnostic; // empty on Success, human-readable otherwise
};

class MixerProcessWorker; // internal; lives on the runner's worker thread

class MixerProcessRunner : public QObject {
    Q_OBJECT
public:
    explicit MixerProcessRunner(QObject *parent = nullptr);
    ~MixerProcessRunner() override;

    // Fire-and-forget, latest-wins coalescing (see contract above).
    // Thread-safe, non-blocking, never spawns on the calling thread.
    void submit(const MixerProcessRequest &request);

    // Cached-state accessors: never spawn, never wait on I/O.
    int cachedLevel() const;
    bool cachedMuted() const;
    bool hasHardwareState() const;
    // Seed/optimistic update (clamped 0..100). No signal, not an observation.
    void setCachedState(int level, bool muted);

    // Lock-free diagnostics, mainly for tests and the regression harness.
    quint64 startedCount() const;   // children actually started
    quint64 coalescedCount() const; // queued requests dropped by a newer submit
    bool isBusy() const;            // a child is in flight right now

signals:
    // A child is about to be started for this tag (owner thread).
    void requestStarted(const QString &tag);
    // Every terminal outcome, in start order (owner thread).
    void finished(const audio::MixerProcessResult &result);
    // Only after a successful parsed read updated the cache (owner thread).
    void stateChanged(int level, bool muted);

private:
    friend class MixerProcessWorker;
    void handleResult(const MixerProcessResult &result,
                      const std::function<std::optional<MixerState>(const QString &)> &parser);
    std::optional<MixerProcessRequest> takePending();

    mutable QMutex m_mutex;
    std::optional<MixerProcessRequest> m_pending; // guarded by m_mutex
    bool m_shuttingDown = false;                  // guarded by m_mutex
    int m_level = 100;                            // guarded by m_mutex
    bool m_muted = false;                         // guarded by m_mutex
    bool m_hasHardwareState = false;              // guarded by m_mutex
    std::atomic<quint64> m_startedCount{0};
    std::atomic<quint64> m_coalescedCount{0};
    std::atomic<bool> m_busy{false};
    MixerProcessWorker *m_worker = nullptr; // lives on m_thread
    QThread *m_thread = nullptr;
};

} // namespace audio

Q_DECLARE_METATYPE(audio::MixerProcessResult)

#endif // MIXERPROCESSRUNNER_H