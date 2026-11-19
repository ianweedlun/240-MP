#include "audio/PipeWireVolumeMixer.h"

#include <QRegularExpression>

namespace audio {

namespace {

// Parse the level (percent) out of a wpctl "Volume: 0.85" reading. Returns -1
// when the output carries no usable volume number. wpctl may append "[MUTED]";
// that token does not affect the numeric parse.
int parseVolumePercent(const QString &out)
{
    static const QRegularExpression re(
        QStringLiteral("Volume:\\s*([0-9]+(?:\\.[0-9]+)?)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(out);
    if (!m.hasMatch())
        return -1;
    bool ok = false;
    const double frac = m.captured(1).toDouble(&ok);
    if (!ok)
        return -1;
    return qBound(0, qRound(frac * 100.0), 100);
}

// Historical success rules, preserved verbatim from the synchronous backend:
// set-volume succeeded iff the echo contains "Volume"; set-mute succeeded iff
// the echo contains "Mute" or is empty (real wpctl is silent on set-mute).
bool mutationEchoOk(const QString &tag, const QString &stdOut)
{
    if (tag.startsWith(QLatin1String("set-volume")))
        return stdOut.contains(QStringLiteral("Volume"), Qt::CaseInsensitive);
    return stdOut.contains(QStringLiteral("Mute"), Qt::CaseInsensitive)
        || stdOut.trimmed().isEmpty();
}

// Requests carry their mutation generation in the tag ("set-volume#7").
quint64 seqFromTag(const QString &tag)
{
    const qsizetype hash = tag.lastIndexOf(QLatin1Char('#'));
    if (hash < 0)
        return 0;
    bool ok = false;
    const quint64 seq = tag.mid(hash + 1).toULongLong(&ok);
    return ok ? seq : 0;
}

} // namespace

PipeWireVolumeMixer::PipeWireVolumeMixer(const QString &sink)
    : m_sink(sink)
{
    // Seed the runner cache with the historical initial state (100 / unmuted).
    // Construction spawns nothing.
    m_runner.setCachedState(m_confirmedLevel, m_confirmedMuted);
    connect(&m_runner, &MixerProcessRunner::finished,
            this, &PipeWireVolumeMixer::onFinished);
}

// m_runner is a member: it is destroyed here on the owner thread, which kills
// and reaps any in-flight child bounded (the seam's shutdown contract).
PipeWireVolumeMixer::~PipeWireVolumeMixer() = default;

bool PipeWireVolumeMixer::setLevel(int percent)
{
    const int v = qBound(0, percent, 100);
    // Genuine no-op at the clamp: no child, no signal — matches VolumeManager's
    // own clamp guard and today's clamp-at-max behavior.
    if (v == m_runner.cachedLevel())
        return true;
    const quint64 seq = ++m_seq;
    // Optimistic update: VolumeManager emits its OSD from level() right after
    // this returns — the same value the synchronous path surfaced once the
    // child finished, just without the stall. Failure rolls back in onFinished.
    m_runner.setCachedState(v, m_runner.cachedMuted());
    MixerProcessRequest req;
    req.program = QStringLiteral("wpctl");
    // wpctl wants a float fraction: 50% -> 0.50.
    req.arguments = {QStringLiteral("set-volume"), m_sink,
                     QString::number(double(v) / 100.0, 'f', 2)};
    req.tag = QStringLiteral("set-volume#%1").arg(seq);
    m_runner.submit(req);
    return true;
}

int PipeWireVolumeMixer::level() const
{
    return m_runner.cachedLevel();
}

bool PipeWireVolumeMixer::isMuted() const
{
    return m_runner.cachedMuted();
}

bool PipeWireVolumeMixer::setMuted(bool muted)
{
    if (muted == m_runner.cachedMuted())
        return true; // genuine no-op: no child, no signal
    const quint64 seq = ++m_seq;
    m_runner.setCachedState(m_runner.cachedLevel(), muted);
    MixerProcessRequest req;
    req.program = QStringLiteral("wpctl");
    req.arguments = {QStringLiteral("set-mute"), m_sink,
                     muted ? QStringLiteral("true") : QStringLiteral("false")};
    req.tag = QStringLiteral("set-mute#%1").arg(seq);
    m_runner.submit(req);
    return true;
}

QString PipeWireVolumeMixer::backendName() const
{
    return QStringLiteral("pipewire");
}

quint64 PipeWireVolumeMixer::startedCount() const
{
    return m_runner.startedCount();
}

quint64 PipeWireVolumeMixer::coalescedCount() const
{
    return m_runner.coalescedCount();
}

bool PipeWireVolumeMixer::isBusy() const
{
    return m_runner.isBusy();
}

void PipeWireVolumeMixer::submitRefresh(quint64 seq)
{
    MixerProcessRequest req;
    req.program = QStringLiteral("wpctl");
    req.arguments = {QStringLiteral("get-volume"), m_sink};
    req.tag = QStringLiteral("get-volume#%1").arg(seq);
    m_runner.submit(req);
}

void PipeWireVolumeMixer::rollbackToConfirmed()
{
    m_runner.setCachedState(m_confirmedLevel, m_confirmedMuted);
    emit stateChanged(m_confirmedLevel, m_confirmedMuted);
}

void PipeWireVolumeMixer::onFinished(const MixerProcessResult &result)
{
    const quint64 seq = seqFromTag(result.tag);
    const bool isRefresh = result.tag.startsWith(QLatin1String("get-volume"));

    if (isRefresh) {
        // A refresh only speaks for the generation that armed it. If a newer
        // mutation was submitted since, this read is stale: discard it — the
        // newer mutation's completion arms a fresh read. This is what keeps a
        // late refresh from resurrecting an old value over a newer press.
        if (seq != m_seq)
            return;
        const int parsed = result.status == MixerProcessResult::Status::Success
            ? parseVolumePercent(result.stdOut) : -1;
        if (parsed < 0) {
            // The mutation succeeded but the confirmation read failed (nonzero
            // exit / timeout / garbage). Keep the optimistic value — it is the
            // best knowledge available — and let the next press re-confirm.
            return;
        }
        // Confirm the level from hardware; mute is managed solely by set-mute
        // (pre-fix never read mute back), so leave the cached mute untouched.
        m_confirmedLevel = parsed;
        m_runner.setCachedState(parsed, m_runner.cachedMuted());
        emit stateChanged(parsed, m_runner.cachedMuted());
        return;
    }

    // Mutation (set-volume / set-mute). A result older than the latest
    // submitted mutation is stale: a newer request owns the state, so this one
    // must neither roll back nor arm a read over it. (If it was coalesced away
    // it never ran; the trailing request that replaced it carries the latest
    // value and will complete in turn.)
    if (seq != m_seq)
        return;

    const bool ok = result.status == MixerProcessResult::Status::Success
        && mutationEchoOk(result.tag, result.stdOut);
    if (!ok) {
        rollbackToConfirmed();
        return;
    }

    if (result.tag.startsWith(QLatin1String("set-volume"))) {
        // Last mutation standing: arm the confirmation read. Because seq ==
        // m_seq here, no newer mutation is pending, so this read cannot clobber
        // one — and if a press arrives later it coalesces the read away and
        // arms its own. At most two invocations per settled press.
        submitRefresh(seq);
    } else {
        // set-mute: the echo confirmed it. Record the requested mute as
        // confirmed (the optimistic cache already holds it); no read-back, so
        // a mute toggle is a single invocation, matching pre-fix semantics.
        const bool requested = result.arguments.size() >= 3
            && result.arguments.at(2) == QLatin1String("true");
        m_confirmedMuted = requested;
        m_runner.setCachedState(m_runner.cachedLevel(), requested);
        emit stateChanged(m_runner.cachedLevel(), requested);
    }
}

} // namespace audio
