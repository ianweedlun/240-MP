#ifndef ALSAVOLUMAMIXER_H
#define ALSAVOLUMAMIXER_H

#include "audio/VolumeMixer.h"

namespace audio {

// Controls system volume on an older Pi (or one without PipeWire) via ALSA's
// `amixer`. Falls back to the no-op mixer if amixer is missing.
class ALSAVolumeMixer : public VolumeMixer {
public:
    explicit ALSAVolumeMixer(const QString &control = QStringLiteral("Master"));

    bool setLevel(int percent) override;
    int level() const override;
    bool isMuted() const override;
    bool setMuted(bool muted) override;
    QString backendName() const override;

private:
    QString run(const QStringList &args) const;
    QString m_control;
    int m_level = 100;
    bool m_muted = false;
};

} // namespace audio

#endif // ALSAVOLUMAMIXER_H
