#ifndef PIPEWIREVOLUMAMIXER_H
#define PIPEWIREVOLUMAMIXER_H

#include "audio/VolumeMixer.h"

#include <QObject>

#include "audio/MixerProcessRunner.h"

namespace audio {

// Controls system volume on a PipeWire/PulseAudio Pi via `wpctl`.
//
// ASYNC CONTRACT (T1 P-5A, built on MixerProcessRunner):
//  * No public call spawns or waits on a subprocess. setLevel()/setMuted()
//    clamp, update the cache optimistically, submit the wpctl mutation to
//    the runner and return true ("accepted") immediately; level()/isMuted()
//    return cached values only. The historical synchronous wpctl round trip
//    blocked the calling thread for the whole child runtime (~211 ms mean
//    with a 200 ms mixer).
//  * A genuine no-op at the clamp (requested value == cached value) spawns
//    nothing and returns true, matching VolumeManager's own no-op guard:
//    no child, no signal, no OSD — exactly today's clamp behavior.
//  * After the LAST pending mutation completes successfully, one
//    `wpctl get-volume` refresh is re-armed; its parsed output becomes the
//    hardware-confirmed state. Rapid key repeats coalesce latest-wins in
//    the runner, so a held key can never pile up wpctl processes:
//    intermediate steps are dropped and the last requested value always
//    runs. Per settled press: at most two invocations (mutation + refresh).
//  * Failure (nonzero exit, spawn failure, timeout, unparseable echo) rolls
//    the cache back to the last confirmed state when no newer request is in
//    flight. A newer request always owns the state: a late failure or a
//    stale refresh can never resurrect an old value over a newer press.
//    The pre-fix code assigned the cache BEFORE the success check and
//    poisoned it on failure — that defect is gone.
//  * Return values mean "request accepted", not "hardware confirmed".
//    VolumeManager persists and emits its OSD from the optimistic cache
//    immediately — the same values the synchronous path surfaced after the
//    child finished, just without the stall. Reconciling UI/persistence
//    with late hardware results is the VolumeManager layer's job;
//    stateChanged() below is the seam for it.
//  * Construct and destroy on the owner (UI) thread. Construction spawns
//    nothing and keeps the historical cached initial state (level 100,
//    unmuted); hasHardwareState() stays false until the first confirmed
//    refresh (the VolumeManager::mixerReady() gate) — cached defaults are
//    not a hardware observation. Destruction kills any in-flight child
//    bounded via the runner's shutdown contract. Do not recreate per key
//    press.
class PipeWireVolumeMixer : public QObject, public VolumeMixer {
    Q_OBJECT
public:
    explicit PipeWireVolumeMixer(const QString &sink = QStringLiteral("@DEFAULT_AUDIO_SINK"));
    ~PipeWireVolumeMixer() override;

    bool setLevel(int percent) override;
    int level() const override;
    bool isMuted() const override;
    bool setMuted(bool muted) override;
    QString backendName() const override;

    // False until the first hardware-confirmed refresh: construction seeds
    // cached defaults, not an observation (VolumeManager::mixerReady gate).
    bool hasHardwareState() const override;

    // Lock-free diagnostics for tests and the regression harness.
    quint64 startedCount() const;
    quint64 coalescedCount() const;
    bool isBusy() const;

signals:
    // Cached state changed: a hardware-confirmed refresh was adopted, or a
    // failed mutation rolled back to the last confirmed state. Owner thread.
    void stateChanged(int level, bool muted);

private:
    void onFinished(const audio::MixerProcessResult &result);
    void submitRefresh(quint64 seq);
    void rollbackToConfirmed();

    QString m_sink;
    MixerProcessRunner m_runner; // owner-thread facade; worker thread inside
    // Last hardware-confirmed state (owner thread only). Starts at the
    // historical cached initial values; a failed mutation rolls back here.
    int m_confirmedLevel = 100;
    bool m_confirmedMuted = false;
    // True once a refresh has been parsed and adopted (a hardware
    // observation). The runner's built-in flag only serves requests parsed
    // by the runner itself; this backend parses in onFinished behind the
    // generation guard, so it tracks its own observation. Never revoked:
    // failures roll back to the last confirmed (hardware-derived) state.
    bool m_hardwareObserved = false;
    // Generation counter: every mutation submit increments m_seq and carries
    // it in the request tag ("set-volume#7"). Results whose generation is
    // older than the latest submitted mutation are stale and never applied.
    quint64 m_seq = 0;
};

} // namespace audio

#endif // PIPEWIREVOLUMAMIXER_H
