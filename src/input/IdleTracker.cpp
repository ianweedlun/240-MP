#include "IdleTracker.h"

#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>

IdleTracker::IdleTracker(int thresholdSec, QObject *parent)
    : QObject(parent)
    , m_threshold(thresholdSec)
{
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &IdleTracker::tick);

    // App-wide event filter catches all keyboard and gamepad-synthesised key
    // events before they reach QML, so we reset the idle counter on any input.
    QCoreApplication::instance()->installEventFilter(this);

    m_timer.start();
}

void IdleTracker::setThreshold(int seconds)
{
    if (m_threshold == seconds)
        return;
    m_threshold = seconds;
    emit thresholdChanged();
}

void IdleTracker::setEnabled(bool on)
{
    if (m_enabled == on)
        return;
    m_enabled = on;
    emit enabledChanged();

    if (!m_enabled && m_active) {
        m_active = false;
        emit activeChanged();
    }
}

void IdleTracker::setMpvActive(bool active)
{
    if (m_mpvActive == active)
        return;

    const bool wasBlocked = blocked();
    m_mpvActive = active;
    emit mpvActiveChanged();

    if (wasBlocked && !blocked()) {
        // Blocking -> unblocked edge: nothing owns the screen any more. Reset
        // the frozen pre-playback idle count (and any active saver) so a
        // keyless return — natural EOF, failed launch — starts a full idle
        // period instead of firing the saver on the first tick (IDLE-01).
        // Safe by construction: callers need no paired resetActivity().
        resetIdleState();
    }
    // Unblocked -> blocking: no reset — the count simply freezes while
    // blocked() (tick() early-returns). No-op writes returned above.
}

// Shared mechanical reset: zero the idle count, clear any active saver, and
// emit the count/activity signals. resetActivity() (explicit activity) and
// the blocking->unblocked edges of both setters funnel through here so the
// three paths cannot diverge.
void IdleTracker::resetIdleState()
{
    const bool wasActive = m_active;
    m_idleSeconds = 0;
    m_active = false;

    emit idleSecondsChanged();
    if (wasActive)
        emit activeChanged();
    emit activityDetected();
}

void IdleTracker::resetActivity()
{
    resetIdleState();
}

bool IdleTracker::eventFilter(QObject *obj, QEvent *event)
{
    Q_UNUSED(obj)
    if (event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        // Ignore auto-repeat events — holding a key shouldn't keep resetting
        // the idle timer forever (the first press resets, held repeats don't).
        if (!ke->isAutoRepeat())
            resetActivity();
    }
    // Never consume the event — let it reach QML normally.
    return false;
}

void IdleTracker::tick()
{
    if (m_active || blocked())
        return;

    // Count idle time even when the screensaver is switched OFF. `enabled`
    // gates only the screensaver's `active` flag; background work (the YouTube
    // idle SponsorBlock fill) needs to know the box is idle regardless of
    // whether the owner wants a screensaver. Nothing else reads idleSeconds.
    ++m_idleSeconds;
    emit idleSecondsChanged();

    if (m_enabled && m_idleSeconds >= m_threshold) {
        m_active = true;
        emit activeChanged();
    }
}

void IdleTracker::setScriptActive(bool active)
{
    if (m_scriptActive == active)
        return;

    const bool wasBlocked = blocked();
    m_scriptActive = active;
    emit scriptActiveChanged();

    if (wasBlocked && !blocked()) {
        // Same contract as setMpvActive: only the session that ends LAST
        // unblocks, and that blocking->unblocked edge resets the idle count
        // (IDLE-01). Ending one session while the other still owns the screen
        // changes nothing — no reset, no saver flash.
        resetIdleState();
    }
}
