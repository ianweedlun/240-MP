#ifndef ALSAVOLUMAMIXER_H
#define ALSAVOLUMAMIXER_H

#include "audio/VolumeMixer.h"

namespace audio {

// Controls system volume on an older Pi (or one without PipeWire) via ALSA's
// `amixer`. Falls back to the no-op mixer if amixer is missing.
//
// The control name is not portable: a desktop exposes 'Master', a Pi's bcm2835
// card exposes 'PCM'/'Headphone'. An empty `control` means auto-detect from
// `amixer scontrols` at construction; pass a name to pin it.
class ALSAVolumeMixer : public VolumeMixer {
public:
    explicit ALSAVolumeMixer(const QString &control = QString());

    bool setLevel(int percent) override;
    int level() const override;
    bool isMuted() const override;
    bool setMuted(bool muted) override;
    QString backendName() const override;

    // Pure helpers, unit-tested without audio hardware.
    // First control from `amixer scontrols` we know how to drive, else
    // "Master" (amixer then fails cleanly rather than guessing elsewhere).
    static QString pickControl(const QString &scontrolsOutput);
    // "Front Left: Playback 50 [71%] [-10.00dB] [on]" -> 71; -1 when absent.
    static int parseLevelPercent(const QString &amixerGetOutput);

private:
    QString run(const QStringList &args) const;
    QString m_control;
    int m_level = 100;
    bool m_muted = false;
};

} // namespace audio

#endif // ALSAVOLUMAMIXER_H
