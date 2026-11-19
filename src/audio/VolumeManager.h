#ifndef VOLUME_MANAGER_H
#define VOLUME_MANAGER_H

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <memory>
#include "audio/VolumeMixer.h"
// App-wide system-audio owner — the single source of truth #34 asks for.
//
// Volume keys land here (not on a per-player mixer), the app-wide OSD reads it,
// and the level/mute persist across launches. When a child owns the display
// (mpv playing, RetroArch running) the mixer still holds the number, so a game
// gets the same volume keys as the shell — that is the point of "app-wide".
//
// Mute is tracked SEPARATELY from level: a mute leaves the level where it was
// (physical-button behaviour), and raising volume while muted unmutes at the
// new level (what an amplifier dial does).
//
// NON-BLOCKING CONTRACT (T1 P-5A decoupling, t_82bed393):
//  * level()/muted() read this object's own cache — they never call into the
//    mixer, so no key press, OSD repaint or QML binding can spawn a process.
//  * A mutation computes the new state from the cache (optimistic update),
//    submits the async mixer request, and emits volumeChanged IMMEDIATELY
//    from the computed values — exactly one coherent OSD update per effective
//    press (a clamp no-op emits nothing). changeVolume applies the amplifier
//    unmute+raise atomically, so one press is still one update.
//  * The backend reconciles later: async mixers emit stateChanged(level,muted)
//    with hardware-confirmed observations that are generation-current by
//    backend contract (a late read/echo can never resurrect a value over a
//    newer user request). Adoption updates cache + signals + persistence only
//    when a value actually differs, so a confirming observation adds no OSD
//    flicker, while hardware rounding, external changes and failure rollbacks
//    converge the UI and the persisted state instead of stranding them.
//  * Persistence is debounced (a held key's auto-repeat burst coalesces to one
//    write) and runs on a dedicated single-thread pool OFF the UI thread. The
//    destructor drains in-flight writes and flushes synchronously, so a
//    restart always restores the last state. Keys/format unchanged.
class VolumeManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int level READ level NOTIFY levelChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    Q_PROPERTY(QString backend READ backendName NOTIFY backendChanged)

public:
    // Builds one system mixer via the factory (PipeWire > ALSA > no-op) and
    // restores the persisted level/mute. dataRoot = the app's data directory.
    // The two-arg form injects a mixer, which is how the unit tests drive it
    // without touching real hardware. Construction never waits on mixer I/O:
    // async backends probe in the background (mixerReady() is the gate).
    explicit VolumeManager(const QString &dataRoot,
                           std::unique_ptr<audio::VolumeMixer> mixer = nullptr);
    ~VolumeManager() override;

    // Percentage points per Volume +/- press. The lua OSD bar and the app bar
    // both quantise to this, so the tick count matches across renderers.
    static constexpr int kStep = 5;
    // Persistence debounce window: long enough to coalesce an auto-repeat
    // burst into one write, short enough that a crash loses at most the last
    // gesture. The destructor flushes whatever is still pending.
    static constexpr int kPersistDebounceMs = 250;

    int level() const;
    bool muted() const;
    QString backendName() const;   // for the OSD/status line, e.g. "pipewire"
    QString dataRoot() const { return m_dataRoot; }
    // True once the backend's cache reflects hardware (async mixers probe in
    // the background after construction; synchronous ones are ready at once).
    // Startup sequencing waits on this instead of blocking on mixer I/O.
    bool mixerReady() const { return m_mixer ? m_mixer->hasHardwareState() : true; }

    Q_INVOKABLE void changeVolume(int delta);
    Q_INVOKABLE void setLevel(int percent);
    Q_INVOKABLE bool toggleMute();
    Q_INVOKABLE bool setMuted(bool isMuted);

    // Write the current state NOW (synchronous; drains any in-flight async
    // write first so the newest snapshot always lands last). The key path
    // uses the debounced async path instead; this is the shutdown/explicit
    // flush entry point. Returns false when there is no data root.
    bool persist();

signals:
    // Semantic change signal: level and/or mute moved. Consumers that mirror
    // the app level into a child renderer (the mpv bar) listen here. Emitted
    // once per effective key press (optimistic values) and once more only if
    // a hardware-confirmed observation differs (reconciliation).
    void volumeChanged(int level, bool muted);
    // Property notify signals, kept single-purpose so QML bindings fire on the
    // exact property that changed.
    void levelChanged();
    void mutedChanged();
    void backendChanged();

private slots:
    // Hardware-confirmed observation from an async backend (see contract
    // above). Connected via the backend's stateChanged(int,bool) signal.
    void onMixerStateChanged(int level, bool muted);

private:
    void restore();
    // Mutation helpers: submit to the mixer, update the cache on acceptance,
    // emit the property notify. Return true when the dimension moved. They
    // never persist and never emit volumeChanged — the caller decides when
    // one coherent OSD update goes out.
    bool submitLevel(int clamped);
    bool submitMuted(bool isMuted);
    void notifyAndPersist();
    void schedulePersist();

    QString m_dataRoot;
    std::unique_ptr<audio::VolumeMixer> m_mixer;
    // UI truth: optimistic on mutation, reconciled by onMixerStateChanged.
    // Seeded from the mixer's cached accessors at construction (never spawn).
    int m_level = 100;
    bool m_muted = false;
    // True while a debounced write is pending; the destructor flushes it.
    bool m_dirty = false;
    QTimer m_persistTimer;   // member, NOT parented (QObject would double-delete)
    // One writer thread: tasks are value-captured snapshots queued FIFO, so
    // the last requested state always lands last. QtCore only — no extra Qt
    // module dependency. Declared after the mixer so it drains first on exit.
    QThreadPool m_writePool;
};

#endif // VOLUME_MANAGER_H