#include "audio/PipeWireVolumeMixer.h"

#include <QProcess>

namespace audio {

PipeWireVolumeMixer::PipeWireVolumeMixer(const QString &sink)
    : m_sink(sink) {}

QString PipeWireVolumeMixer::run(const QStringList &args) const {
    QProcess p;
    p.start("wpctl", args);
    p.waitForFinished(1000);
    return QString(p.readAllStandardOutput()).trimmed();
}

bool PipeWireVolumeMixer::setLevel(int percent) {
    const int v = qBound(0, percent, 100);
    // wpctl wants a float fraction: 50% -> 0.50.
    const QString spec = QString::number(double(v) / 100.0, 'f', 2);
    const QString out = run({"set-volume", m_sink, spec});
    m_level = v;
    // wpctl prints "Volume 0.50" on success; anything else means it failed.
    return out.contains(QStringLiteral("Volume"), Qt::CaseInsensitive);
}

int PipeWireVolumeMixer::level() const { return m_level; }

bool PipeWireVolumeMixer::isMuted() const { return m_muted; }

bool PipeWireVolumeMixer::setMuted(bool muted) {
    const QString out = run({"set-mute", m_sink, muted ? "true" : "false"});
    m_muted = muted;
    return out.contains(QStringLiteral("Mute"), Qt::CaseInsensitive)
        || out.isEmpty(); // wpctl is silent on success.
}

QString PipeWireVolumeMixer::backendName() const { return QStringLiteral("pipewire"); }

} // namespace audio
