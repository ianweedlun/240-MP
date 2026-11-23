#include "audio/ALSAVolumeMixer.h"

#include <QPointer>
#include <QRegularExpression>

namespace audio {

namespace {

// Preference order for auto-detection. Desktops expose 'Master'; a Pi's
// bcm2835 card exposes 'Headphone' plus 'PCM'. First one present wins, so a
// real Master is never shadowed by PCM on a machine that has both.
const QStringList &controlCandidates() {
    static const QStringList candidates{
        QStringLiteral("Master"),
        QStringLiteral("PCM"),
        QStringLiteral("Headphone"),
        QStringLiteral("Headphones"),
        QStringLiteral("Speaker"),
    };
    return candidates;
}

bool hasPlaybackVolumeSuffix(QString *name) {
    static const QString suffix = QStringLiteral(" Playback Volume");
    if (!name->endsWith(suffix, Qt::CaseInsensitive)) return false;
    name->chop(suffix.size());
    return true;
}

// Lines look like: Simple mixer control 'PCM Playback Switch',0
QStringList simpleControlNames(const QString &scontrolsOutput) {
    static const QRegularExpression re(
        QStringLiteral("Simple mixer control '([^']+)'"));
    QStringList names;
    auto it = re.globalMatch(scontrolsOutput);
    while (it.hasNext()) names << it.next().captured(1);
    return names;
}

} // namespace

ALSAVolumeMixer::ALSAVolumeMixer(const QString &control)
    : ALSAVolumeMixer(control, nullptr, QStringLiteral("amixer")) {}

ALSAVolumeMixer::ALSAVolumeMixer(const QString &control, MixerProcessRunner *runner,
                                 const QString &program)
    // nullptr = own the runner. It must be created HERE, not as a delegating
    // argument: `new MixerProcessRunner(this)` in the delegating ctor's arg
    // list runs before the QObject base exists, and parenting to a
    // not-yet-constructed QObject is UB (SEGV in QObject::thread()).
    // In this mem-init list the QObject base is already initialized.
    : m_runner(runner ? runner : new MixerProcessRunner(this)),
      m_program(program), m_pinnedControl(control) {
    // The runner never spawns on this thread: construction only queues the
    // discovery scan and returns with the historical defaults (100, unmuted).
    connect(m_runner, &MixerProcessRunner::finished,
            this, &ALSAVolumeMixer::onRunnerFinished);
    connect(m_runner, &MixerProcessRunner::stateChanged,
            this, &ALSAVolumeMixer::stateChanged);
    startDiscovery();
}

ALSAVolumeMixer::~ALSAVolumeMixer() = default;

void ALSAVolumeMixer::startDiscovery() {
    MixerProcessRequest req;
    req.program = m_program;
    req.arguments = {QStringLiteral("scontrols")};
    req.tag = QStringLiteral("scontrols");
    // No parser: the raw control list is handled in onRunnerFinished().
    m_outstanding = true;
    m_runner->submit(req);
}

QString ALSAVolumeMixer::pickControl(const QString &scontrolsOutput) {
    const QStringList present = simpleControlNames(scontrolsOutput);
    for (const QString &candidate : controlCandidates()) {
        if (present.contains(candidate, Qt::CaseInsensitive))
            return candidate;
    }
    // Cards that split the elements expose '<Ctl> Playback Volume' instead of a
    // combined '<Ctl>'; same preference order, and pickSwitchControl() can then
    // pair it with '<Ctl> Playback Switch'.
    for (const QString &candidate : controlCandidates()) {
        const QString split = candidate + QStringLiteral(" Playback Volume");
        for (const QString &name : present) {
            if (name.compare(split, Qt::CaseInsensitive) == 0)
                return name;
        }
    }
    // Nothing recognised (or amixer absent): fall back to the historical
    // default so setLevel fails cleanly instead of driving an arbitrary control.
    return QStringLiteral("Master");
}

QString ALSAVolumeMixer::pickSwitchControl(const QString &scontrolsOutput,
                                           const QString &volumeControl) {
    if (volumeControl.isEmpty()) return QString();

    // '<Ctl> Playback Volume' pairs with '<Ctl> Playback Switch'. A short
    // control name ('PCM') from `scontrols` pairs the same way, because that is
    // how the card names the switch element.
    QString base = volumeControl;
    hasPlaybackVolumeSuffix(&base);
    const QString wanted = base + QStringLiteral(" Playback Switch");

    const QStringList present = simpleControlNames(scontrolsOutput);
    for (const QString &name : present) {
        if (name.compare(wanted, Qt::CaseInsensitive) == 0)
            return name; // hardware spelling, so amixer resolves it
    }
    // No separate switch: the control is combined (volume + pswitch), which the
    // caller drives directly.
    return QString();
}

bool ALSAVolumeMixer::parseSwitchMuted(const QString &amixerGetOutput, bool *ok) {
    if (ok) *ok = false;
    static const QRegularExpression re(QStringLiteral("\\[(on|off)\\]"),
                                      QRegularExpression::CaseInsensitiveOption);

    bool sawValue = false;
    bool sawOff = false;
    const QStringList lines = amixerGetOutput.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        // Only playback rows carry the switch we care about; skipping other
        // rows keeps a capture switch from answering for playback.
        if (!line.contains(QLatin1String("Playback"), Qt::CaseInsensitive))
            continue;
        auto it = re.globalMatch(line);
        while (it.hasNext()) {
            const QString value = it.next().captured(1);
            sawValue = true;
            if (value.compare(QLatin1String("off"), Qt::CaseInsensitive) == 0)
                sawOff = true;
        }
    }
    if (!sawValue) return false; // unknown state — caller decides
    if (ok) *ok = true;
    return sawOff;
}

int ALSAVolumeMixer::parseLevelPercent(const QString &amixerGetOutput) {
    // "Front Left: Playback 50 [71%] [-10.00dB] [on]" -> 71; -1 when absent.
    // Must match Playback rows specifically: on cards where Capture rows
    // appear before Playback (BAD_ARM case), the first \d+% would be capture.
    static const QRegularExpression re(QStringLiteral("\\[?(\\d+)%\\]?"));
    const QStringList lines = amixerGetOutput.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        // Only playback rows carry the level we care about; skipping other
        // rows keeps a capture level from answering for playback.
        if (!line.contains(QLatin1String("Playback"), Qt::CaseInsensitive))
            continue;
        auto m = re.match(line);
        if (m.hasMatch()) return m.captured(1).toInt();
    }
    return -1;
}

QString ALSAVolumeMixer::muteTarget() const {
    return m_switchControl.isEmpty() ? m_control : m_switchControl;
}

bool ALSAVolumeMixer::setLevel(int percent) {
    const int v = qBound(0, percent, 100);
    // Optimistic seed: VolumeManager persists and signals from level() right
    // after this returns, so the cache must already carry the requested value.
    // The sset echo reconciles it with hardware; a failed call keeps the
    // request (the historical m_level = v behaviour) until the next observation.
    m_runner->setCachedState(v, m_runner->cachedMuted());
    ++m_generation;
    m_pendingLevel = v;
    m_levelReadTried = false;
    m_muteReadTried = false;
    pump();
    return true; // accepted — hardware confirmation arrives via stateChanged()
}

int ALSAVolumeMixer::level() const { return m_runner->cachedLevel(); }

bool ALSAVolumeMixer::isMuted() const { return m_runner->cachedMuted(); }

bool ALSAVolumeMixer::setMuted(bool muted) {
    m_runner->setCachedState(m_runner->cachedLevel(), muted);
    ++m_generation;
    m_pendingMute = muted;
    m_levelReadTried = false;
    m_muteReadTried = false;
    pump();
    return true; // accepted — the set echo reconciles the cache with hardware
}

bool ALSAVolumeMixer::hasHardwareState() const { return m_runner->hasHardwareState(); }

bool ALSAVolumeMixer::isIdle() const {
    return !m_outstanding && !m_pendingLevel && !m_pendingMute;
}

QString ALSAVolumeMixer::backendName() const { return QStringLiteral("alsa"); }

void ALSAVolumeMixer::pump() {
    // One outstanding request at a time: every submit is guaranteed a
    // finished() event, so the chain can neither wedge nor lose a mutation to
    // the runner's latest-wins trailing slot. Rapid repeats accumulate in the
    // per-dimension pending slots (latest wins) instead of piling up children.
    if (m_outstanding || !m_discovered) return;

    if (m_pendingLevel) {
        const int v = *m_pendingLevel;
        m_submittedLevel = v;
        MixerProcessRequest req;
        req.program = m_program;
        // `amixer sset Master 50%` — the % is literal in the argument. The
        // echo carries the resulting level (and mute on combined controls).
        req.arguments = {QStringLiteral("sset"), m_control,
                         QString::number(v) + QStringLiteral("%")};
        req.tag = QStringLiteral("sset");
        // Generation stamp at SUBMIT time (held mutations flush after later
        // accepts already bumped the counter). An echo superseded by a newer
        // request must not clobber the newer optimistic cache or emit a stale
        // value over it — the newer request's own echo reports the truth.
        const quint64 generation = m_generation;
        req.parser = [guard = QPointer<ALSAVolumeMixer>(this), generation](const QString &out)
            -> std::optional<MixerState> {
            if (!guard || generation != guard->m_generation)
                return std::nullopt;
            return guard->parseVolumeEcho(out);
        };
        m_outstanding = true;
        m_runner->submit(req);
        return;
    }
    if (m_pendingMute) {
        const bool muted = *m_pendingMute;
        m_submittedMute = muted;
        MixerProcessRequest req;
        req.program = m_program;
        req.arguments = {QStringLiteral("set"), muteTarget(),
                         muted ? QStringLiteral("mute") : QStringLiteral("unmute")};
        req.tag = QStringLiteral("set-mute");
        // Same submit-time generation guard as the level echo above.
        const quint64 generation = m_generation;
        req.parser = [guard = QPointer<ALSAVolumeMixer>(this), generation](const QString &out)
            -> std::optional<MixerState> {
            if (!guard || generation != guard->m_generation)
                return std::nullopt;
            return guard->parseSwitchEcho(out);
        };
        m_outstanding = true;
        m_runner->submit(req);
        return;
    }
    // No mutation pending: observe whatever hardware has not confirmed yet.
    // A read is tried once per trigger (discovery or accepted mutation) — a
    // failing card must not spin the queue with retries.
    if (!m_levelObserved && !m_levelReadTried) {
        submitLevelRead();
        return;
    }
    if (!m_switchControl.isEmpty() && !m_muteObserved && !m_muteReadTried) {
        submitMuteRead();
        return;
    }
}

void ALSAVolumeMixer::submitLevelRead() {
    m_levelReadTried = true;
    MixerProcessRequest req;
    req.program = m_program;
    req.arguments = {QStringLiteral("sget"), m_control};
    req.tag = QStringLiteral("sget-volume");
    const quint64 generation = m_generation;
    req.parser = [guard = QPointer<ALSAVolumeMixer>(this), generation](const QString &out)
        -> std::optional<MixerState> {
        return guard ? guard->parseRead(out, generation, /*isMuteElement=*/false)
                     : std::nullopt;
    };
    m_outstanding = true;
    m_runner->submit(req);
}

void ALSAVolumeMixer::submitMuteRead() {
    m_muteReadTried = true;
    MixerProcessRequest req;
    req.program = m_program;
    req.arguments = {QStringLiteral("sget"), muteTarget()};
    req.tag = QStringLiteral("sget-mute");
    const quint64 generation = m_generation;
    req.parser = [guard = QPointer<ALSAVolumeMixer>(this), generation](const QString &out)
        -> std::optional<MixerState> {
        return guard ? guard->parseRead(out, generation, /*isMuteElement=*/true)
                     : std::nullopt;
    };
    m_outstanding = true;
    m_runner->submit(req);
}

std::optional<MixerState> ALSAVolumeMixer::parseVolumeEcho(const QString &stdOut) {
    // Same success shape the synchronous backend checked: amixer echoes the
    // control it drove, with the resulting level. Empty/foreign output means
    // the call failed — leave the (optimistic) cache untouched.
    if (!stdOut.contains(m_control, Qt::CaseInsensitive)) return std::nullopt;
    const int level = parseLevelPercent(stdOut);
    if (level < 0) return std::nullopt;
    m_levelObserved = true;
    bool ok = false;
    const bool muted = parseSwitchMuted(stdOut, &ok);
    if (ok) m_muteObserved = true; // combined controls carry the switch too
    return MixerState{level, ok ? muted : m_runner->cachedMuted()};
}

std::optional<MixerState> ALSAVolumeMixer::parseSwitchEcho(const QString &stdOut) {
    if (!stdOut.contains(muteTarget(), Qt::CaseInsensitive)) return std::nullopt;
    bool ok = false;
    const bool muted = parseSwitchMuted(stdOut, &ok);
    if (!ok) return std::nullopt;
    m_muteObserved = true;
    // A mute write does not move the level; keep the cached one.
    return MixerState{m_runner->cachedLevel(), muted};
}

std::optional<MixerState> ALSAVolumeMixer::parseRead(const QString &stdOut,
                                                     quint64 generation,
                                                     bool isMuteElement) {
    // Staleness guard: a read that was in flight when a newer mutation was
    // accepted must not resurrect the old hardware value over the user's
    // request. The mutation's own echo will report the truth.
    if (generation != m_generation) return std::nullopt;
    if (isMuteElement) return parseSwitchEcho(stdOut);
    return parseVolumeEcho(stdOut);
}

void ALSAVolumeMixer::onRunnerFinished(const audio::MixerProcessResult &result) {
    m_outstanding = false;

    if (result.tag == QLatin1String("scontrols")) {
        // One scan resolves both elements. A failed scan (missing amixer,
        // nonzero exit, garbage) yields the historical 'Master' fallback, so
        // mutations fail cleanly instead of driving an arbitrary control.
        m_control = m_pinnedControl.isEmpty() ? pickControl(result.stdOut) : m_pinnedControl;
        m_switchControl = pickSwitchControl(result.stdOut, m_control);
        m_discovered = true;
        if (!m_readyEmitted) {
            m_readyEmitted = true;
            emit stateReady();
        }
        pump(); // held mutations first (restore order), then initial reads
        return;
    }

    if (result.tag == QLatin1String("sset")) {
        // Consume the pending request only if no newer value arrived while the
        // child ran; a newer request stays pending and pump() re-submits it.
        if (m_pendingLevel && m_submittedLevel && *m_pendingLevel == *m_submittedLevel)
            m_pendingLevel.reset();
        m_submittedLevel.reset();
    } else if (result.tag == QLatin1String("set-mute")) {
        if (m_pendingMute && m_submittedMute && *m_pendingMute == *m_submittedMute)
            m_pendingMute.reset();
        m_submittedMute.reset();
    }
    // Reads need no bookkeeping: the parser already applied (or discarded)
    // them, and a failed read is never retried on its own.
    pump();
}

} // namespace audio