#include "audio/ALSAVolumeMixer.h"

#include <QProcess>
#include <QRegularExpression>

namespace audio {

ALSAVolumeMixer::ALSAVolumeMixer(const QString &control)
    : m_control(control) {}

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
    // Parse "  [0000]  Front Left: Capture 50 %" -> 50.
    const QString out = run({"get", m_control});
    QRegularExpression re(QStringLiteral("%(\\d+)%"));
    QRegularExpressionMatch m = re.match(out);
    if (m.hasMatch()) return m.captured(1).toInt();
    return m_level;
}

bool ALSAVolumeMixer::isMuted() const { return m_muted; }

bool ALSAVolumeMixer::setMuted(bool muted) {
    const QString out = run({"set", m_control, muted ? "mute" : "unmute"});
    m_muted = muted;
    return !out.trimmed().isEmpty();
}

QString ALSAVolumeMixer::backendName() const { return QStringLiteral("alsa"); }

} // namespace audio
