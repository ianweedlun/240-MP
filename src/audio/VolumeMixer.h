#ifndef VOLUMAMIXER_H
#define VOLUMAMIXER_H

#include <QString>

namespace audio {

// App-wide system-audio control. All modules route volume through a single
// VolumeManager that owns one of these; it is the single source of truth so
// the OSD, the player, and the weather mpv all reflect the same level.
//
// Percentages are 0..100. Mute is tracked separately (a mute leaves the level
// where it was, matching how a physical volume button behaves).
class VolumeMixer {
public:
    virtual ~VolumeMixer() = default;

    virtual bool setLevel(int percent) = 0;
    virtual int level() const = 0;
    virtual bool isMuted() const = 0;
    virtual bool setMuted(bool muted) = 0;

    // Human-readable backend name, e.g. "pipewire", "alsa", "none".
    virtual QString backendName() const = 0;
};

} // namespace audio

#endif // VOLUMAMIXER_H
