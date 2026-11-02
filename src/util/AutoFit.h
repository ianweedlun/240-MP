#pragma once

#include <algorithm>

// Pure geometry behind the app-level "auto_fit" setting. No mpv or Qt state —
// testable in isolation (see src/util/tests/).
//
// The rule (#30, refined by the maintainer): with AUTO FIT ON, a video whose
// geometry is known ALWAYS FILLS the display (panscan = 1) — letterbox and
// pillarbox never appear.
//   * content WIDER than the display fits the display's aspect ratio with
//     pan & scan: the excess width is cropped and video-pan-x sweeps so the
//     hidden sides get shown too.
//   * content NARROWER than the display is scaled up to fit the display; the
//     vertical overflow is center-cropped.
// Unknown aspects (0) keep the original aspect — never guess.
//
// Fill box at panscan=1 (mirrors video/out/aspect.c aspect_calc_panscan;
// `aspect` is video-params/aspect, i.e. already PAR-corrected):
//     outW = max(windowW, windowH * aspect)
//     outH = max(windowH, windowW / aspect)
// mpv's video-pan-x is a fraction of the SCALED video size (DOCS: --video-pan-x=-0.1
// moves a 1920-wide scaled video 192px), so showing every column of the content
// takes panning half the fill box's horizontal overflow on each side.
namespace mpvfit {

inline int fitPanscanTarget(double contentAspect, double windowAspect)
{
    if (contentAspect <= 0.0 || windowAspect <= 0.0)
        return 0; // unknown geometry: original aspect, never a guess
    return 1;     // known geometry: always fill — bars are never produced
}

inline double fillBoxWidth(double windowW, double windowH, double aspect)
{
    return std::max(windowW, windowH * aspect);
}

// Pan-x amplitude (fraction of the scaled width) that reveals the full width of
// the content when the window is centered; 0 when there is no horizontal
// overflow (square/taller content in this window).
inline double panSweepAmplitude(double windowW, double windowH, double aspect)
{
    if (windowW <= 0.0 || windowH <= 0.0 || aspect <= 0.0)
        return 0.0;
    const double outW = fillBoxWidth(windowW, windowH, aspect);
    return (outW - windowW) / (2.0 * outW);
}

} // namespace mpvfit
