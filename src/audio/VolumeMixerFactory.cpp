#include "audio/VolumeMixerFactory.h"

#include <QFileInfo>
#include <QStandardPaths>
#include <memory>

#include "audio/ALSAVolumeMixer.h"
#include "audio/NoopVolumeMixer.h"
#include "audio/PipeWireVolumeMixer.h"

namespace audio {

static bool hasExecutable(const QString &name) {
    // findExecutable returns an empty string if not found.
    return !QStandardPaths::findExecutable(name).isEmpty();
}

std::unique_ptr<VolumeMixer> makeSystemVolumeMixer() {
    if (hasExecutable("wpctl")) return std::make_unique<PipeWireVolumeMixer>();
    if (hasExecutable("amixer")) return std::make_unique<ALSAVolumeMixer>();
    return std::make_unique<NoopVolumeMixer>();
}

} // namespace audio
