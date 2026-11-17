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

} // namespace

ALSAVolumeMixer::ALSAVolumeMixer(const QString &control)
    : m_control(control) {
    if (m_control.isEmpty()) {
        QProcess p;
        p.start("amixer", {"scontrols"});
        p.waitForFinished(1000);
        m_control = pickControl(QString(p.readAllStandardOutput()));
    }
}

QString ALSAVolumeMixer::pickControl(const QString &scontrolsOutput) {
    // Lines look like: Simple mixer control 'PCM',0
    static const QRegularExpression re(
        QStringLiteral("Simple mixer control '([^']+)'"));
    QStringList present;
    auto it = re.globalMatch(scontrolsOutput);
    while (it.hasNext()) present << it.next().captured(1);

    for (const QString &candidate : controlCandidates()) {
        if (present.contains(candidate, Qt::CaseInsensitive))
            return candidate;
    }
    // Nothing recognised (or amixer absent): fall back to the historical
    // default so setLevel fails cleanly instead of driving an arbitrary control.
    return QStringLiteral("Master");
}

int ALSAVolumeMixer::parseLevelPercent(const QString &amixerGetOutput) {
    // "Front Left: Playback 50 [71%] [-10.00dB] [on]" -> 71
    static const QRegularExpression re(QStringLiteral("\\[?(\\d+)%\\]?"));
    auto m = re.match(amixerGetOutput);
    if (!m.hasMatch()) return -1;
    return m.captured(1).toInt();
}

QString ALSAVolumeMixer::run(const QStringList &args) const {
    QProcess p;
    p.start("amixer", args);
    p.waitForFinished(1000);
    return QString(p.readAllStandardOutput());
}

bool ALSAVolumeMixer::setLevel(int percent) {
    const int v = qBound(0, percent, 100);
    // `amixer sset Master 50%` — the % is literal in the argument.
    const QString out = run({"sset", m_control, QString::number(v) + "%"});
    m_level = v;
    // amixer echoes the new state on success; empty means the call failed.
    return !out.trimmed().isEmpty() && out.contains(m_control, Qt::CaseInsensitive);
}

int ALSAVolumeMixer::level() const {
    const int parsed = parseLevelPercent(run({"get", m_control}));
    return parsed >= 0 ? parsed : m_level;
}

bool ALSAVolumeMixer::isMuted() const { return m_muted; }

bool ALSAVolumeMixer::setMuted(bool muted) {
    const QString out = run({"set", m_control, muted ? "mute" : "unmute"});
    m_muted = muted;
    return !out.trimmed().isEmpty();
}

QString ALSAVolumeMixer::backendName() const { return QStringLiteral("alsa"); }

} // namespace audio
