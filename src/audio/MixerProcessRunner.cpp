#include "audio/MixerProcessRunner.h"

#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QThread>
#include <QTimer>

namespace audio {

Q_LOGGING_CATEGORY(lcMixerRunner, "audio.mixer.runner")

namespace {

const char *statusName(MixerProcessResult::Status status) {
    switch (status) {
    case MixerProcessResult::Status::Success: return "success";
    case MixerProcessResult::Status::NonZeroExit: return "nonzero-exit";
    case MixerProcessResult::Status::SpawnFailed: return "spawn-failed";
    case MixerProcessResult::Status::TimedOut: return "timed-out";
    case MixerProcessResult::Status::UnparseableOutput: return "unparseable-output";
    }
    return "unknown";
}

} // namespace

// ---------------------------------------------------------------------------
// Worker: owns the QProcess and the timeout timer. Every method here runs on
// the dedicated worker thread (lambda connections use `this` as context, and
// the facade only ever posts queued invocations). Nothing below may block on
// a caller thread; the sole bounded wait is the post-kill reap in shutdown().
// ---------------------------------------------------------------------------
class MixerProcessWorker : public QObject {
public:
    explicit MixerProcessWorker(MixerProcessRunner *facade)
        : m_facade(facade) {
        m_process = new QProcess(this);
        m_process->setProcessChannelMode(QProcess::SeparateChannels);
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, [this] { onTimeout(); });
        connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
                    onFinished(exitCode, exitStatus);
                });
        connect(m_process, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError error) { onError(error); });
    }

    // Start the queued request unless a child is already in flight. Posted by
    // every submit() and re-run after every terminal outcome, so the queue can
    // never wedge: any pending request is guaranteed a pump.
    void maybeStart() {
        if (m_running || m_shutDown || !m_facade) return;
        const std::optional<MixerProcessRequest> req = m_facade->takePending();
        if (!req.has_value()) return;
        m_current = *req;
        m_timedOut = false;
        m_running = true;
        m_facade->m_busy.store(true, std::memory_order_relaxed);
        m_facade->m_startedCount.fetch_add(1, std::memory_order_relaxed);

        // Notify the owner thread before start() so a synchronous spawn error
        // still arrives after requestStarted() in the owner's event queue.
        const QString tag = m_current.tag;
        QMetaObject::invokeMethod(
            m_facade,
            [f = m_facade, tag] {
                if (f) emit f->requestStarted(tag);
            },
            Qt::QueuedConnection);

        m_process->setProgram(m_current.program);
        m_process->setArguments(m_current.arguments);
        m_timer->start(qMax(1, m_current.timeoutMs));
        m_process->start(); // asynchronous; errors arrive via errorOccurred
    }

    // Teardown, invoked blocking-queued from the facade destructor. Kills the
    // in-flight child and reaps it ON THE WORKER THREAD so the QProcess can be
    // destroyed in NotRunning state. SIGKILL cannot be ignored, so the bounded
    // wait returns promptly; it never runs on a caller thread.
    void shutdown() {
        m_shutDown = true;
        m_running = false; // late finished() signals become no-ops
        m_timer->stop();
        if (m_process->state() != QProcess::NotRunning) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
        if (m_facade) m_facade->m_busy.store(false, std::memory_order_relaxed);
    }

private:
    void onFinished(int exitCode, QProcess::ExitStatus exitStatus) {
        if (!m_running) return;
        m_timer->stop();
        MixerProcessResult result;
        result.tag = m_current.tag;
        result.arguments = m_current.arguments;
        result.exitCode = exitCode;
        result.stdOut = QString::fromUtf8(m_process->readAllStandardOutput());
        result.stdErr = QString::fromUtf8(m_process->readAllStandardError());
        if (m_timedOut) {
            result.status = MixerProcessResult::Status::TimedOut;
            result.diagnostic =
                QStringLiteral("timed out after %1 ms; child killed").arg(m_current.timeoutMs);
        } else if (exitStatus == QProcess::CrashExit) {
            result.status = MixerProcessResult::Status::NonZeroExit;
            result.diagnostic = QStringLiteral("child crashed (signal %1)").arg(exitCode);
        } else if (exitCode != 0) {
            result.status = MixerProcessResult::Status::NonZeroExit;
            result.diagnostic = QStringLiteral("exit code %1").arg(exitCode);
        } else {
            result.status = MixerProcessResult::Status::Success;
        }
        completeCurrent(result);
    }

    void onError(QProcess::ProcessError error) {
        if (!m_running) return;
        if (error == QProcess::FailedToStart) {
            m_timer->stop();
            MixerProcessResult result;
            result.tag = m_current.tag;
            result.arguments = m_current.arguments;
            result.status = MixerProcessResult::Status::SpawnFailed;
            result.diagnostic = m_process->errorString();
            completeCurrent(result);
        }
        // Crashed: finished(CrashExit) follows and reports it timeout-aware.
        // Timedout/Read/Write errors are not terminal for our usage.
    }

    void onTimeout() {
        if (!m_running || m_timedOut) return;
        m_timedOut = true;
        m_process->kill(); // SIGKILL; finished(CrashExit) delivers TimedOut
    }

    void completeCurrent(const MixerProcessResult &result) {
        m_running = false;
        if (m_facade) m_facade->m_busy.store(false, std::memory_order_relaxed);
        // Marshal the raw outcome back to the owner thread; parsing and cache
        // updates happen there so backend lambdas never run on this thread.
        auto parser = m_current.parser;
        QMetaObject::invokeMethod(
            m_facade,
            [f = m_facade, result, parser] {
                if (f) f->handleResult(result, parser);
            },
            Qt::QueuedConnection);
        maybeStart(); // trailing coalesced request, if one is queued
    }

    QPointer<MixerProcessRunner> m_facade;
    QProcess *m_process = nullptr; // child of this worker
    QTimer *m_timer = nullptr;     // child of this worker
    MixerProcessRequest m_current;
    bool m_running = false;
    bool m_timedOut = false;
    bool m_shutDown = false;
};

// ---------------------------------------------------------------------------
// Facade (owner thread)
// ---------------------------------------------------------------------------
MixerProcessRunner::MixerProcessRunner(QObject *parent)
    : QObject(parent) {
    qRegisterMetaType<MixerProcessResult>();
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("MixerProcessRunner"));
    // No QObject parent: the worker lives on m_thread and is deleted by the
    // destructor after the thread is joined (its QProcess/QTimer children go
    // with it).
    m_worker = new MixerProcessWorker(this);
    m_worker->moveToThread(m_thread);
    m_thread->start();
}

MixerProcessRunner::~MixerProcessRunner() {
    {
        QMutexLocker lock(&m_mutex);
        m_shuttingDown = true;
        m_pending.reset();
    }
    if (m_thread->isRunning()) {
        // Synchronous stop: after this returns the worker touches nothing.
        QMetaObject::invokeMethod(
            m_worker, [w = m_worker] { w->shutdown(); }, Qt::BlockingQueuedConnection);
        m_thread->quit();
        if (!m_thread->wait(2000)) {
            // Last resort only: the worker loop is bounded, so this should be
            // unreachable; better than ~QThread aborting on a running thread.
            m_thread->terminate();
            m_thread->wait();
        }
    }
    delete m_worker; // thread is stopped; children (QProcess/QTimer) die here
    // m_thread is parented to this and is deleted by ~QObject.
}

void MixerProcessRunner::submit(const MixerProcessRequest &request) {
    {
        QMutexLocker lock(&m_mutex);
        if (m_shuttingDown) return;
        if (m_pending.has_value())
            m_coalescedCount.fetch_add(1, std::memory_order_relaxed);
        m_pending = request; // latest-wins: the trailing slot holds one request
    }
    QMetaObject::invokeMethod(
        m_worker, [w = m_worker] { w->maybeStart(); }, Qt::QueuedConnection);
}

void MixerProcessRunner::setCachedState(int level, bool muted) {
    QMutexLocker lock(&m_mutex);
    m_level = qBound(0, level, 100);
    m_muted = muted;
}

int MixerProcessRunner::cachedLevel() const {
    QMutexLocker lock(&m_mutex);
    return m_level;
}

bool MixerProcessRunner::cachedMuted() const {
    QMutexLocker lock(&m_mutex);
    return m_muted;
}

bool MixerProcessRunner::hasHardwareState() const {
    QMutexLocker lock(&m_mutex);
    return m_hasHardwareState;
}

quint64 MixerProcessRunner::startedCount() const {
    return m_startedCount.load(std::memory_order_relaxed);
}

quint64 MixerProcessRunner::coalescedCount() const {
    return m_coalescedCount.load(std::memory_order_relaxed);
}

bool MixerProcessRunner::isBusy() const {
    return m_busy.load(std::memory_order_relaxed);
}

std::optional<MixerProcessRequest> MixerProcessRunner::takePending() {
    QMutexLocker lock(&m_mutex);
    std::optional<MixerProcessRequest> req = m_pending;
    m_pending.reset();
    return req;
}

void MixerProcessRunner::handleResult(
    const MixerProcessResult &raw,
    const std::function<std::optional<MixerState>(const QString &)> &parser) {
    // Runs on the owner thread (queued from the worker).
    MixerProcessResult result = raw;
    if (result.status == MixerProcessResult::Status::Success && parser) {
        const std::optional<MixerState> parsed = parser(result.stdOut);
        if (parsed.has_value()) {
            int level = 0;
            bool muted = false;
            {
                QMutexLocker lock(&m_mutex);
                m_level = parsed->level;
                m_muted = parsed->muted;
                m_hasHardwareState = true;
                level = m_level;
                muted = m_muted;
            }
            emit stateChanged(level, muted);
        } else {
            // Exit 0 but the output is garbage: last known good cache stays.
            result.status = MixerProcessResult::Status::UnparseableOutput;
            result.diagnostic = QStringLiteral("successful run produced unparseable output");
        }
    }
    if (result.status != MixerProcessResult::Status::Success) {
        qCWarning(lcMixerRunner) << "mixer request failed:" << result.tag << result.arguments
                                 << statusName(result.status) << result.diagnostic;
    }
    emit finished(result);
}

} // namespace audio