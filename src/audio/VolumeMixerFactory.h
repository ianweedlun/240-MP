#ifndef VOLUMAMIXERFACTORY_H
#define VOLUMAMIXERFACTORY_H

#include "audio/VolumeMixer.h"

namespace audio {

// Picks a system mixer at runtime: PipeWire (wpctl) if present, else ALSA
// (amixer), else a no-op so the app never crashes on a box with no audio
// control. Returns nullptr only if Qt's QProcess can't even run — callers
// should fall back to a NoopVolumeMixer in that case.
std::unique_ptr<VolumeMixer> makeSystemVolumeMixer();

} // namespace audio

#endif // VOLUMAMIXERFACTORY_H
