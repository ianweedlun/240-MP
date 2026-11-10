#include "audio/NoopVolumeMixer.h"

namespace audio {

bool NoopVolumeMixer::setLevel(int percent) {
    m_level = qBound(0, percent, 100);
    return true; // Succeeds, but nothing on the hardware changes.
}

int NoopVolumeMixer::level() const { return m_level; }

bool NoopVolumeMixer::isMuted() const { return m_muted; }

bool NoopVolumeMixer::setMuted(bool muted) {
    m_muted = muted;
    return true;
}

QString NoopVolumeMixer::backendName() const { return QStringLiteral("none"); }

} // namespace audio
