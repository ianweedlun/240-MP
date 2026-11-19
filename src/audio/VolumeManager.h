#ifndef VOLUME_MANAGER_H
#define VOLUME_MANAGER_H

#include <QObject>
#include <QString>
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
    // without touching real hardware.
    explicit VolumeManager(const QString &dataRoot,
                           std::unique_ptr<audio::VolumeMixer> mixer = nullptr);
    ~VolumeManager() override;

    // Percentage points per Volume +/- press. The lua OSD bar and the app bar
    // both quantise to this, so the tick count matches across renderers.
    static constexpr int kStep = 5;

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

    // Persist current state (called on every change; cheap JSON).
    bool persist();

signals:
    // Semantic change signal: level and/or mute moved. Consumers that mirror
    // the app level into a child renderer (the mpv bar) listen here.
    void volumeChanged(int level, bool muted);
    // Property notify signals, kept single-purpose so QML bindings fire on the
    // exact property that changed.
    void levelChanged();
    void mutedChanged();
    void backendChanged();

private:
    void restore();

    QString m_dataRoot;
    std::unique_ptr<audio::VolumeMixer> m_mixer;
};

#endif // VOLUME_MANAGER_H
