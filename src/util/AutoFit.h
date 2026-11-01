#pragma once

// Pure decision table behind the app-level "auto_fit" setting: which panscan
// target a just-loaded video gets, from the content's aspect and the window's.
// No mpv or Qt state — testable in isolation (see src/util/tests/).
//
// The rule the maintainer asked for (#30): content at or below the display's
// aspect ratio FILLS the display (panscan = 1; the overflow is cropped);
// content wider than the display LETTERBOXES (panscan = 0, original aspect).
// Unknown aspects (0) keep the original aspect rather than guessing.
namespace mpvfit {

inline int fitPanscanTarget(double contentAspect, double windowAspect)
{
    if (contentAspect <= 0.0 || windowAspect <= 0.0)
        return 0; // unknown geometry: original aspect, never a guess
    return contentAspect <= windowAspect ? 1 : 0;
}

} // namespace mpvfit
