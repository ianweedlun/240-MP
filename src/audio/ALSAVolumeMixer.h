#ifndef ALSAVOLUMAMIXER_H
#define ALSAVOLUMAMIXER_H

#include <QObject>
#include <QStringList>

#include <optional>

#include "audio/MixerProcessRunner.h"
#include "audio/VolumeMixer.h"

namespace audio {

// Controls system volume on an older Pi (or one without PipeWire) via ALSA's
// `amixer`. Falls back to the no-op mixer if amixer is missing.
//
// The control name is not portable: a desktop exposes 'Master', a Pi's bcm2835
// card exposes 'PCM'/'Headphone'. An empty `control` means auto-detect from
// `amixer scontrols`; pass a name to pin it.
//
// Mute lives on a *different* element than volume on most cards: `PCM Playback
// Volume` pairs with `PCM Playback Switch`. The two are resolved together from
// the discovery scan, and mute state is reconciled from hardware echoes —
// never assumed from what we last asked for.
//
// ASYNC CONTRACT (T1 P-5A, built on MixerProcessRunner):
//  * No public call — including construction — ever waits on a child process.
//    Construction submits the `scontrols` discovery scan and returns with sane
//    cached defaults (level 100, unmuted); stateReady() fires when discovery
//    completes and the initial hardware reads are in flight, and stateChanged()
//    follows whenever a successful read/mutation echo updates the cache.
//  * level()/isMuted() return cached values only; they never spawn.
//  * setLevel()/setMuted() seed the cache optimistically (so VolumeManager's
//    immediate persist/signal reads see the requested value), submit ONE amixer
//    child whose echoed output carries the resulting state, and return true
//    when the request was accepted. The echo parser reconciles the cache with
//    hardware; a failed call leaves the optimistic value in place (the historical
//    m_level = v behaviour) and the next successful observation corrects it.
//  * Mutations requested before discovery completes are held and flushed in
//    order (level, then mute — the restore() order) once the control is known,
//    so a persisted level/mute is never applied to a guessed control name.
//  * Rapid repeats coalesce latest-wins in the runner: a burst of presses costs
//    at most two children, and the last requested value always reaches hardware.
//  * Reads carry a generation stamp: a read that was in flight when a newer
//    mutation was requested is discarded, so a late hardware answer can never
//    resurrect a level the user already moved past.
class ALSAVolumeMixer : public QObject, public VolumeMixer {
    Q_OBJECT
public:
    explicit ALSAVolumeMixer(const QString &control = QString());
    // Test seam: a non-null `runner` replaces the owned process runner
    // (borrowed — it must outlive the mixer and live on the same thread);
    // nullptr means the mixer creates and owns its runner. `program` replaces
    // the `amixer` executable, so tests drive the whole mixer with a stub
    // child instead of a sound card.
    ALSAVolumeMixer(const QString &control, MixerProcessRunner *runner,
                    const QString &program = QStringLiteral("amixer"));
    ~ALSAVolumeMixer() override;

    bool setLevel(int percent) override;
    int level() const override;
    bool isMuted() const override;
    bool setMuted(bool muted) override;
    QString backendName() const override;
    // False until a successful parsed child output proves the cache reflects
    // hardware; true immediately for synchronous backends (base default).
    bool hasHardwareState() const override;
    // True when no submitted request is in flight and no held mutation is
    // pending — everything accepted so far has drained through the runner.
    // Diagnostic for tests and startup sequencing; never spawns.
    bool isIdle() const;

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

signals:
    // Control discovery finished (success or failure) and the initial state
    // requests are in flight. Emitted exactly once, on the owner thread.
    void stateReady();
    // A successful parsed child output updated the cache (owner thread).
    // Optimistic seeds do NOT emit this — it only carries hardware observations.
    void stateChanged(int level, bool muted);

private:
    void startDiscovery();
    void onRunnerFinished(const audio::MixerProcessResult &result);
    // Single submit point: one outstanding request at a time, chained from
    // finished(). Mutations outrank reads; latest pending value per dimension.
    void pump();
    void submitLevelRead();
    void submitMuteRead();
    // The element `mute`/`unmute`/`sget` must address: the paired switch when
    // the card exposes one, else the volume control itself.
    QString muteTarget() const;
    std::optional<MixerState> parseVolumeEcho(const QString &stdOut);
    std::optional<MixerState> parseSwitchEcho(const QString &stdOut);
    std::optional<MixerState> parseRead(const QString &stdOut, quint64 generation,
                                        bool isMuteElement);

    MixerProcessRunner *m_runner = nullptr; // owned (QObject child) or borrowed
    QString m_program;
    QString m_pinnedControl;
    QString m_control;
    QString m_switchControl;
    bool m_discovered = false;
    bool m_readyEmitted = false;
    bool m_outstanding = false; // a submitted request has not reported yet
    bool m_levelObserved = false;
    bool m_muteObserved = false;
    bool m_levelReadTried = false; // once per trigger; no retry spin on failure
    bool m_muteReadTried = false;
    // Bumped on every accepted mutation; reads stamp it at submit time so a
    // late read answer cannot overwrite a newer user request.
    quint64 m_generation = 0;
    // Mutations requested but not yet confirmed (latest wins per dimension).
    // Held until discovery completes; consumed only when the matching echo
    // reports, so a newer request arriving mid-flight is never dropped.
    std::optional<int> m_pendingLevel;
    std::optional<bool> m_pendingMute;
    std::optional<int> m_submittedLevel;
    std::optional<bool> m_submittedMute;
};

} // namespace audio

#endif // ALSAVOLUMAMIXER_H