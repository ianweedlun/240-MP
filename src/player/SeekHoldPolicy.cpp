#include "player/SeekHoldPolicy.h"

SeekHoldPolicy::Command SeekHoldPolicy::press(const QString &key, bool autoRepeat)
{
    Command cmd;
    if (key == QLatin1String("RIGHT")) {
        if (!autoRepeat) {
            m_rightAccum = 0.0;
            cmd.kind = Command::Seek;
            cmd.value = kTapStepSec;
        } else {
            m_rightAccum += kTapStepSec;   // flushed by the owner's timer
        }
    } else if (key == QLatin1String("LEFT")) {
        if (!autoRepeat) {
            m_leftAccum = 0.0;
            cmd.kind = Command::Seek;
            cmd.value = -kTapStepSec;
        } else {
            m_leftAccum -= kTapStepSec;   // flushed by the owner's timer
        }
    } else if (!autoRepeat) {
        cmd.kind = Command::Key;
        cmd.key = key;
    }
    // Repeats of non-directional keys (ENTER/SPACE/ESC/...) are dropped.
    return cmd;
}

SeekHoldPolicy::Command SeekHoldPolicy::release(const QString &key)
{
    if (key == QLatin1String("RIGHT") || key == QLatin1String("LEFT"))
        return flush();
    return Command();
}

SeekHoldPolicy::Command SeekHoldPolicy::flush()
{
    Command cmd;
    if (m_rightAccum != 0.0) {
        cmd.kind = Command::Seek;
        cmd.value = m_rightAccum;
        m_rightAccum = 0.0;
    } else if (m_leftAccum != 0.0) {
        cmd.kind = Command::Seek;
        cmd.value = m_leftAccum;
        m_leftAccum = 0.0;
    }
    return cmd;
}

void SeekHoldPolicy::reset()
{
    m_leftAccum  = 0.0;
    m_rightAccum = 0.0;
}
