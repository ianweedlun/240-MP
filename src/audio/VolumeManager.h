#ifndef VOLUME_MANAGER_H
#define VOLUME_MANAGER_H

#include <QObject>
#include <QString>
#include <memory>

#include "audio/VolumeMixer.h"

// App-wide system-audio owner — the single source of truth #34 asks for.
// Volume keys land here (not on a per-player mixer), the OSD, the player and
// the weather mpv all read the same level, and the level/mute persist across
// launches (a living room does not forget its volume when the app restarts).
class VolumeManager : public QObject
{
    Q_OBJECT
public:
    // Builds one system mixer via the factory (PipeWire > ALSA > no-op) and
    // restores the persisted level/mute. dataRoot = the app's data directory.
    explicit VolumeManager(const QString &dataRoot);

    // Clamp to 0..100. Mute is SEPARATE from level: a mute leaves the level
    // where it was (physical-button behaviour), and raising volume while
    // muted unmutes at the new level (what an amplifier dial does).
    void changeVolume(int delta);
    void setLevel(int percent);
    bool toggleMute();
    bool setMuted(bool isMuted);

    int level() const;
    bool muted() const;
    QString backendName() const;   // for the OSD/status line, e.g. "pipewire"

    // Persist current state (called on every change; cheap JSON).
    bool persist();

private:
    void restore();
    QString m_dataRoot;
    std::unique_ptr<audio::VolumeMixer> m_mixer;

signals:
    // The OSD and any consumer that mirrors the system level listens here.
    void volumeChanged(int level, bool muted);
};

#endif // VOLUME_MANAGER_H
