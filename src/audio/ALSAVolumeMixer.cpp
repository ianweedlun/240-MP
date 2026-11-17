#include "audio/ALSAVolumeMixer.h"

#include <QProcess>
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

// The real backend: one amixer process per call, 1 s budget.
QString runAmixer(const QStringList &args) {
    QProcess p;
    p.start(QStringLiteral("amixer"), args);
    p.waitForFinished(1000);
    return QString(p.readAllStandardOutput());
}

} // namespace

ALSAVolumeMixer::ALSAVolumeMixer(const QString &control)
    : ALSAVolumeMixer(control, &runAmixer) {}

ALSAVolumeMixer::ALSAVolumeMixer(const QString &control, AmixerRunner runner)
    : m_runner(std::move(runner)) {
    // One `scontrols` scan resolves both elements: the volume control we drive
    // and the paired playback switch that actually carries mute.
    const QString scontrols = run({QStringLiteral("scontrols")});
    m_control = control.isEmpty() ? pickControl(scontrols) : control;
    m_switchControl = pickSwitchControl(scontrols, m_control);
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

QString ALSAVolumeMixer::run(const QStringList &args) const {
    return m_runner ? m_runner(args) : QString();
}

QString ALSAVolumeMixer::muteTarget() const {
    return m_switchControl.isEmpty() ? m_control : m_switchControl;
}

bool ALSAVolumeMixer::setLevel(int percent) {
    const int v = qBound(0, percent, 100);
    // `amixer sset Master 50%` — the % is literal in the argument.
    const QString out = run({QStringLiteral("sset"), m_control, QString::number(v) + QStringLiteral("%")});
    m_level = v;
    // amixer echoes the new state on success; empty means the call failed.
    return !out.trimmed().isEmpty() && out.contains(m_control, Qt::CaseInsensitive);
}

int ALSAVolumeMixer::level() const {
    const int parsed = parseLevelPercent(run({QStringLiteral("get"), m_control}));
    return parsed >= 0 ? parsed : m_level;
}

int ALSAVolumeMixer::parseLevelPercent(const QString &amixerGetOutput) {
    // "Front Left: Playback 50 [71%] [-10.00dB] [on]" -> 71
    static const QRegularExpression re(QStringLiteral("\\[?(\\d+)%\\]?"));
    auto m = re.match(amixerGetOutput);
    if (!m.hasMatch()) return -1;
    return m.captured(1).toInt();
}

bool ALSAVolumeMixer::isMuted() const {
    // Read the hardware every time: a mute applied by amixer, another process
    // or a boot-time mixer restore is otherwise invisible to the app.
    bool ok = false;
    const bool muted = parseSwitchMuted(run({QStringLiteral("sget"), muteTarget()}), &ok);
    if (!ok) return m_muted; // unreadable card — best knowledge we have
    m_muted = muted;
    return muted;
}

bool ALSAVolumeMixer::setMuted(bool muted) {
    const QString target = muteTarget();
    run({QStringLiteral("set"), target, muted ? QStringLiteral("mute") : QStringLiteral("unmute")});

    // Confirm against hardware before touching the cache: a failed or
    // mis-targeted call must not leave the app believing the new state.
    bool ok = false;
    const bool now = parseSwitchMuted(run({QStringLiteral("sget"), target}), &ok);
    if (!ok) return false;
    m_muted = now;
    return now == muted;
}

QString ALSAVolumeMixer::backendName() const { return QStringLiteral("alsa"); }

} // namespace audio
