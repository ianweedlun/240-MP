#pragma once
#include <QtGlobal>

// The one environment switch around the #75 render-suspend mitigation.
//
// While a child process owns the screen, main.cpp hides the root window on
// DisplayHandoff::displayTaken() and shows it again on displayReturned(), which
// is the only lever that actually stops this build's EGLFS render loop (the VT
// switch does NOT suspend it — see the displayTaken comment in DisplayHandoff.h).
//
// MP240_HANDOFF_KEEP_WINDOW restores the pre-#75 behaviour — window left up for
// the whole hand-off — so the two can be A/B'd on hardware without a rebuild.
//
// This lives in a header of its own, not inline in main.cpp, so the predicate is
// unit-testable: the decision it encodes changes what the user sees on the CRT,
// and a silent flip would look like "the #75 fix stopped working".
namespace handoffwindow {

// Any non-empty value counts, so `=1`, `=true` and `=0` all mean "keep the
// window". Deliberately not an integer parse: the escape hatch has to be
// impossible to set by accident and impossible to mis-type into "off".
inline bool keepWindowRequested()
{
    return !qgetenv("MP240_HANDOFF_KEEP_WINDOW").isEmpty();
}

// True when displayTaken() should hide the root window — the default. False only
// under MP240_HANDOFF_KEEP_WINDOW, which is also what tells DisplayHandoff that
// EGLFS is NOT being re-initialised on the way back (setRendererSuspendsWindow),
// so the legacy CRTC restore stays in play. main.cpp feeds this ONE value to both
// decisions: hiding the window without saying so double-modesets on return, and
// saying so without hiding leaves EGLFS drawing with DRM master dropped.
inline bool hideWindowOnTake()
{
    return !keepWindowRequested();
}

} // namespace handoffwindow
