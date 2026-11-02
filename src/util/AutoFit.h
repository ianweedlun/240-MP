#pragma once

// Pure geometry behind the app-level "auto_fit" setting. No mpv or Qt state —
// testable in isolation (see src/util/tests/).
//
// The rule (#30, refined by the maintainer): with AUTO FIT ON, a video whose
// geometry is known ALWAYS FILLS the display (panscan = 1) — letterbox and
// pillarbox never appear, and the fit is applied ONCE at playback start
// (static centered fill — no continuous re-framing):
//   * content WIDER than the display is scaled to display height and centered;
//     its side columns are permanently cropped.
//   * content NARROWER than the display is scaled up to display width; the
//     vertical overflow is center-cropped.
// Unknown aspects (0) keep the original aspect — never guess.
//
// Fill box at panscan=1 (mirrors video/out/aspect.c aspect_calc_panscan;
// `aspect` is video-params/aspect, i.e. already PAR-corrected):
//     outW = max(windowW, windowH * aspect)
//     outH = max(windowH, windowW / aspect)
namespace mpvfit {

inline int fitPanscanTarget(double contentAspect, double windowAspect)
{
    if (contentAspect <= 0.0 || windowAspect <= 0.0)
        return 0; // unknown geometry: original aspect, never a guess
    return 1;     // known geometry: always fill — bars are never produced
}

} // namespace mpvfit
