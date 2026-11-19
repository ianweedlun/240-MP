#ifndef PIPEWIREVOLUMAMIXER_H
#define PIPEWIREVOLUMAMIXER_H

#include "audio/VolumeMixer.h"

#include <QObject>

#include <optional>

#include "audio/MixerProcessRunner.h"

namespace audio {

// Controls system volume on a PipeWire/PulseAudio Pi via `wpctl`.
//
// ASYNC CONTRACT (T1 P-5A, built on MixerProcessRunner; ordering hardened in
// t_82bed393 so the VolumeManager key path can be fully decoupled):
//  * No public call spawns or waits on a subprocess. setLevel()/setMuted()
//    clamp, update the cache optimistically, record the request in a
//    per-dimension pending slot (latest-wins) and return true ("accepted")
//    immediately; level()/isMuted() return cached values only. The historical
//    synchronous wpctl round trip blocked the calling thread for the whole
//    child runtime (~211 ms mean with a 200 ms mixer).
//  * A genuine no-op at the clamp (requested value == cached value) spawns
//    nothing and returns true, matching VolumeManager's own no-op guard:
//    no child, no signal, no OSD — exactly today's clamp behavior.
//  * pump() submits AT MOST ONE request to the runner at a time and chains
//    the next on completion (level before mute, mutations before refreshes).
//    The runner's latest-wins trailing slot therefore only ever holds this
//    backend's single outstanding request, and a queued set-mute can NEVER be
//    coalesced away by a following set-level — the amplifier raise-while-muted
//    burst used to lose the mute exactly that way. Coalescing happens in the
//    pending slots instead: intermediate steps are dropped, the last requested
//    value per dimension always runs, and a held key can never pile up wpctl
//    processes. coalescedCount() counts pending-slot drops plus runner drops,
//    so the conservation law started + coalesced == submitted stays exact.
//    Per settled press: at most two invocations (mutation + refresh); an
//    amplifier press costs one child per dimension plus the refresh — the
//    pre-fix call count.
//  * After the LAST pending mutation completes successfully, one
//    `wpctl get-volume` refresh is re-armed; its parsed output becomes the
//    hardware-confirmed state. A refresh discarded as stale leaves the
//    confirmation owed, so it re-arms once the newer mutations drain.
//  * Every emission is generation-current: a mutation echo superseded by a
//    newer same-dimension request neither confirms nor rolls back anything,
//    and a refresh tagged before the newest mutation is discarded. A late
//    echo/read can never resurrect an old value over a newer press.
//  * Failure (nonzero exit, spawn failure, timeout, unparseable echo) rolls
//    the FAILED DIMENSION back to its last confirmed value when no newer
//    same-dimension request is pending — dimension-scoped, so a failed level
//    write never clobbers a newer optimistic mute. A superseded failure keeps
//    the newer optimistic value (its request still runs). A failed refresh
//    keeps the optimistic level (no rollback, no retry spin). The pre-fix code
//    assigned the cache BEFORE the success check and poisoned it on failure —
//    that defect is gone.
//  * Return values mean "request accepted", not "hardware confirmed".
//    VolumeManager emits its OSD from its own optimistic cache immediately
//    and reconciles via stateChanged() below when hardware reports differently.
//  * Construct and destroy on the owner (UI) thread. Construction spawns
//    nothing and keeps the historical cached initial state (level 100,
//    unmuted); hasHardwareState() stays false until the first confirmed
//    refresh (the VolumeManager::mixerReady() gate) — cached defaults are
//    not a hardware observation. Mute is managed exclusively by set-mute
//    (pre-fix never read it back), so an idle mute toggle stays a single
//    invocation. Destruction kills any in-flight child bounded via the
//    runner's shutdown contract. Do not recreate per key press.
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
    // Requests dropped by a newer submit: pending-slot drops (this backend)
    // plus runner coalescing. Conservation: started + coalesced == submitted.
    quint64 coalescedCount() const;
    bool isBusy() const;

signals:
    // Cached state changed: a hardware-confirmed refresh was adopted, a
    // set-mute echo confirmed, or a failed mutation rolled back to the last
    // confirmed state. Owner thread. VolumeManager reconciles UI + persistence
    // from this; every emission is generation-current (see contract).
    void stateChanged(int level, bool muted);

private:
    void onFinished(const audio::MixerProcessResult &result);
    // Submit the next pending request (level before mute, mutations before
    // refreshes) — at most one outstanding at the runner at any time.
    void pump();
    void submitRefresh(quint64 seq);

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
    // Generation counter: every accepted mutation increments m_seq; refresh
    // requests carry it in their tag ("get-volume#7") and results tagged with
    // an older generation are stale and never applied.
    quint64 m_seq = 0;
    // Per-dimension latest-wins pending slots + what is actually in flight.
    // A pending value equal to the submitted one is consumed on completion;
    // a newer one is kept and chained next. m_outstanding guards the single
    // submit point so the runner's trailing slot can never eat a mutation.
    bool m_outstanding = false;
    std::optional<int> m_pendingLevel;
    std::optional<bool> m_pendingMute;
    std::optional<int> m_submittedLevel;
    std::optional<bool> m_submittedMute;
    quint64 m_pendingDrops = 0;
    // A level mutation owes one hardware confirmation. Set on accept, cleared
    // when a refresh settles (success OR failure — never retried in a spin).
    // A refresh discarded as stale leaves it set, so the next pump re-arms.
    bool m_refreshNeeded = false;
};

} // namespace audio

#endif // PIPEWIREVOLUMAMIXER_H