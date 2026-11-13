#ifndef NOOPVOLUMAMIXER_H
#define NOOPVOLUMAMIXER_H

#include "audio/VolumeMixer.h"

namespace audio {

// Fallback when no system mixer is available (e.g. WSL CI, or a Pi with no
// audio control wired up). All calls succeed but change nothing, so the app
// never crashes on a box that can't control its own volume.
class NoopVolumeMixer : public VolumeMixer {
public:
    bool setLevel(int percent) override;
    int level() const override;
    bool isMuted() const override;
    bool setMuted(bool muted) override;
    QString backendName() const override;

private:
    int m_level = 100;
    bool m_muted = false;
};

} // namespace audio

#endif // NOOPVOLUMAMIXER_H
