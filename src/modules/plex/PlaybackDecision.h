#pragma once
// Resolution-aware playback decision for the Plex module's video_quality
// setting. A ladder selection now means "direct play at or below this
// resolution class; transcode above it to this target" instead of the old
// "always transcode at this target". Pure logic (no network, no I/O) so it
// is unit-testable in isolation.

#include <QHash>
#include <QtGlobal>
#include <QLocale>

namespace PlexQuality {

// Transcode ladder id -> highest source resolution class (shorter display
// edge, in px) that still direct-plays. Anything above is transcoded to
// that ladder target.
inline const QHash<QString, int> &ladderThresholds() {
    static const QHash<QString, int> t{{{"8000", 1080},
                                        {"4000", 720},
                                        {"2000", 480}}};
    return t;
}

// width/height are the source video stream's metadata dimensions.
// Convention: resolution class is the SHORTER edge (standard "1080p"
// semantics; correct for rotated metadata where width > height).
// Unknown dimensions (0) with a transcode ladder selected -> transcode:
// direct-playing an unmeasurable source on a CRT is the risky branch.
inline bool shouldTranscode(const QString &quality, int width, int height) {
    if (quality.isEmpty() || quality == QLatin1String("auto"))
        return false; // "Direct Play (always)" — unchanged default
    const int threshold = ladderThresholds().value(quality, -1);
    if (threshold < 0)
        return true; // id not on our ladder (custom bitrate, legacy config) —
                     // keep pre-reorg semantics: always transcode at it
    if (width <= 0 || height <= 0)
        return true;
    return qMin(width, height) > threshold;
}

} // namespace PlexQuality
