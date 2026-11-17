#ifndef ALSAVOLUMAMIXER_H
#define ALSAVOLUMAMIXER_H

#include <QStringList>

#include <functional>

#include "audio/VolumeMixer.h"

namespace audio {

// Controls system volume on an older Pi (or one without PipeWire) via ALSA's
// `amixer`. Falls back to the no-op mixer if amixer is missing.
//
// The control name is not portable: a desktop exposes 'Master', a Pi's bcm2835
// card exposes 'PCM'/'Headphone'. An empty `control` means auto-detect from
// `amixer scontrols` at construction; pass a name to pin it.
//
// Mute lives on a *different* element than volume on most cards: `PCM Playback
// Volume` pairs with `PCM Playback Switch`. The two are resolved together at
// construction, and mute state is always read back from hardware — never
// assumed from what we last asked for.
class ALSAVolumeMixer : public VolumeMixer {
public:
    // Every amixer invocation goes through this, so tests can drive the whole
    // mixer (control detection, set, read-back) with a stub instead of a card.
    using AmixerRunner = std::function<QString(const QStringList &args)>;

    explicit ALSAVolumeMixer(const QString &control = QString());
    // Test seam: `runner` replaces the real `amixer` process for every call.
    ALSAVolumeMixer(const QString &control, AmixerRunner runner);

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
    // 'PCM Playback Volume' -> 'PCM Playback Switch' when the card exposes that
    // pair (a short 'PCM' control pairs the same way). Empty when there is no
    // separate playback switch — combined controls carry the switch themselves.
    static QString pickSwitchControl(const QString &scontrolsOutput,
                                     const QString &volumeControl);
    // "Mono: Playback [on]" -> false, "[off]" -> true. With per-channel rows,
    // any muted channel counts as muted. `ok` is false when the output carries
    // no playback switch value at all (caller then falls back to its cache).
    static bool parseSwitchMuted(const QString &amixerGetOutput, bool *ok = nullptr);

private:
    QString run(const QStringList &args) const;
    // The element `mute`/`unmute`/`sget` must address: the paired switch when
    // the card exposes one, else the volume control itself.
    QString muteTarget() const;

    AmixerRunner m_runner;
    QString m_control;
    QString m_switchControl;
    int m_level = 100;
    // Only ever holds a hardware-observed mute state, so it is a sane fallback
    // when amixer cannot be read; mutable because isMuted() refreshes it.
    mutable bool m_muted = false;
};

} // namespace audio

#endif // ALSAVOLUMAMIXER_H
