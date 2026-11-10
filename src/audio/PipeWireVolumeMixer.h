#ifndef PIPEWIREVOLUMAMIXER_H
#define PIPEWIREVOLUMAMIXER_H

#include "audio/VolumeMixer.h"

namespace audio {

// Controls system volume on a PipeWire/PulseAudio Pi via `wpctl`.
class PipeWireVolumeMixer : public VolumeMixer {
public:
    explicit PipeWireVolumeMixer(const QString &sink = QStringLiteral("@DEFAULT_AUDIO_SINK"));

    bool setLevel(int percent) override;
    int level() const override;
    bool isMuted() const override;
    bool setMuted(bool muted) override;
    QString backendName() const override;

private:
    QString run(const QStringList &args) const;
    QString m_sink;
    int m_level = 100;
    bool m_muted = false;
};

} // namespace audio

#endif // PIPEWIREVOLUMAMIXER_H
