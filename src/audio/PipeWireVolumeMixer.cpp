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
    // own clamp guard and today's clamp-at-max behavior. The cache is
    // optimistic, so an equal value is a no-op even while an older child runs.
    if (v == m_runner.cachedLevel())
        return true;
    ++m_seq;
    // Optimistic update: VolumeManager emits its OSD from its own cache right
    // after this returns — the same value the synchronous path surfaced once
    // the child finished, just without the stall. Failure rolls back below.
    m_runner.setCachedState(v, m_runner.cachedMuted());
    if (m_pendingLevel.has_value())
        ++m_pendingDrops;   // superseded before it ever reached the runner
    m_pendingLevel = v;
    m_refreshNeeded = true;
    pump();
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
    ++m_seq;
    m_runner.setCachedState(m_runner.cachedLevel(), muted);
    if (m_pendingMute.has_value())
        ++m_pendingDrops;
    m_pendingMute = muted;
    // No refresh owed: mute is managed exclusively by set-mute (pre-fix
    // semantics), so an idle mute toggle stays a single invocation.
    pump();
    return true;
}

QString PipeWireVolumeMixer::backendName() const
{
    return QStringLiteral("pipewire");
}

bool PipeWireVolumeMixer::hasHardwareState() const
{
    return m_hardwareObserved;
}

quint64 PipeWireVolumeMixer::startedCount() const
{
    return m_runner.startedCount();
}

quint64 PipeWireVolumeMixer::coalescedCount() const
{
    return m_runner.coalescedCount() + m_pendingDrops;
}

bool PipeWireVolumeMixer::isBusy() const
{
    return m_runner.isBusy();
}

void PipeWireVolumeMixer::pump()
{
    // One outstanding request at a time: every submit is guaranteed a
    // finished() event, so the chain can neither wedge nor lose a mutation to
    // the runner's latest-wins trailing slot (the cross-dimension drop the
    // amplifier burst used to hit). Rapid repeats accumulate in the
    // per-dimension pending slots (latest wins) instead of piling up children.
    if (m_outstanding)
        return;
    if (m_pendingLevel.has_value()) {
        const int v = *m_pendingLevel;
        m_pendingLevel.reset();
        m_submittedLevel = v;
        MixerProcessRequest req;
        req.program = QStringLiteral("wpctl");
        // wpctl wants a float fraction: 50% -> 0.50.
        req.arguments = {QStringLiteral("set-volume"), m_sink,
                         QString::number(double(v) / 100.0, 'f', 2)};
        req.tag = QStringLiteral("set-volume#%1").arg(m_seq);
        m_outstanding = true;
        m_runner.submit(req);
        return;
    }
    if (m_pendingMute.has_value()) {
        const bool muted = *m_pendingMute;
        m_pendingMute.reset();
        m_submittedMute = muted;
        MixerProcessRequest req;
        req.program = QStringLiteral("wpctl");
        req.arguments = {QStringLiteral("set-mute"), m_sink,
                         muted ? QStringLiteral("true") : QStringLiteral("false")};
        req.tag = QStringLiteral("set-mute#%1").arg(m_seq);
        m_outstanding = true;
        m_runner.submit(req);
        return;
    }
    if (m_refreshNeeded)
        submitRefresh(m_seq);
}

void PipeWireVolumeMixer::submitRefresh(quint64 seq)
{
    MixerProcessRequest req;
    req.program = QStringLiteral("wpctl");
    req.arguments = {QStringLiteral("get-volume"), m_sink};
    req.tag = QStringLiteral("get-volume#%1").arg(seq);
    m_outstanding = true;
    m_runner.submit(req);
}

void PipeWireVolumeMixer::onFinished(const audio::MixerProcessResult &result)
{
    m_outstanding = false;
    const quint64 seq = seqFromTag(result.tag);
    const bool isRefresh = result.tag.startsWith(QLatin1String("get-volume"));

    if (isRefresh) {
        // A refresh only speaks for the generation that armed it. If a newer
        // mutation was accepted since, this read is stale: discard it. The
        // owed confirmation stays armed (m_refreshNeeded) and re-submits once
        // the newer mutations drain — a late read can never resurrect an old
        // value over a newer press.
        if (seq != m_seq) {
            pump();
            return;
        }
        m_refreshNeeded = false;
        const int parsed = result.status == MixerProcessResult::Status::Success
            ? parseVolumePercent(result.stdOut) : -1;
        if (parsed < 0) {
            // The mutation succeeded but the confirmation read failed (nonzero
            // exit / timeout / garbage). Keep the optimistic value — it is the
            // best knowledge available — and let the next press re-confirm.
            pump();
            return;
        }
        // Confirm the level from hardware; mute is managed solely by set-mute
        // (pre-fix never read mute back), so leave the cached mute untouched.
        m_confirmedLevel = parsed;
        m_hardwareObserved = true;
        m_runner.setCachedState(parsed, m_runner.cachedMuted());
        emit stateChanged(parsed, m_runner.cachedMuted());
        pump();
        return;
    }

    // Mutation (set-volume / set-mute). A newer same-dimension request owns
    // the state: this echo neither confirms nor rolls back anything — the
    // newer request is pending and pump() runs it next. (With the single
    // outstanding submit point nothing is ever coalesced away at the runner;
    // every submitted request reports here exactly once.)
    const bool isVolume = result.tag.startsWith(QLatin1String("set-volume"));
    const bool superseded = isVolume
        ? (m_pendingLevel.has_value() && m_submittedLevel.has_value()
           && *m_pendingLevel != *m_submittedLevel)
        : (m_pendingMute.has_value() && m_submittedMute.has_value()
           && *m_pendingMute != *m_submittedMute);
    if (isVolume) {
        if (m_pendingLevel.has_value() && m_submittedLevel.has_value()
            && *m_pendingLevel == *m_submittedLevel)
            m_pendingLevel.reset();
        m_submittedLevel.reset();
    } else {
        if (m_pendingMute.has_value() && m_submittedMute.has_value()
            && *m_pendingMute == *m_submittedMute)
            m_pendingMute.reset();
        m_submittedMute.reset();
    }
    if (superseded) {
        pump();
        return;
    }

    const bool ok = result.status == MixerProcessResult::Status::Success
        && mutationEchoOk(result.tag, result.stdOut);
    if (!ok) {
        // Failed mutation, nothing newer in this dimension: roll THAT
        // dimension back to its last confirmed value. Dimension-scoped — a
        // newer optimistic value in the other dimension (pending or cached)
        // is left untouched. No refresh retry against a failing mixer.
        if (isVolume) {
            m_refreshNeeded = false;
            m_runner.setCachedState(m_confirmedLevel, m_runner.cachedMuted());
            emit stateChanged(m_confirmedLevel, m_runner.cachedMuted());
        } else {
            m_runner.setCachedState(m_runner.cachedLevel(), m_confirmedMuted);
            emit stateChanged(m_runner.cachedLevel(), m_confirmedMuted);
        }
        pump();
        return;
    }

    if (isVolume) {
        // Last level mutation standing: the hardware confirmation is owed.
        // pump() drains any pending mute first, then arms the refresh.
        m_refreshNeeded = true;
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
    pump();
}

} // namespace audio