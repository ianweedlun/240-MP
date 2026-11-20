#include "player/SeekHoldPolicy.h"

SeekHoldPolicy::Command SeekHoldPolicy::press(const QString &key, bool autoRepeat)
{
    Command cmd;
    if (key == QLatin1String("RIGHT")) {
        if (!autoRepeat) {
            // A fresh tap ends any previous ramp and seeks one step.
            m_ramping = false;
            m_rampTicks = 0;
            cmd.kind = Command::Seek;
            cmd.value = kTapStepSec;
        } else if (!m_ramping) {
            m_ramping = true;
            m_rampTicks = 0;
            cmd.kind = Command::Speed;
            cmd.value = kRampSpeedNear;
        } else {
            ++m_rampTicks;
            if (m_rampTicks == kRampEscalateTicks) {
                cmd.kind = Command::Speed;
                cmd.value = kRampSpeedFar;
            }
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
    Command cmd;
    if (key == QLatin1String("RIGHT")) {
        if (m_ramping) {
            m_ramping = false;
            m_rampTicks = 0;
            cmd.kind = Command::Speed;
            cmd.value = 1.0;
        }
    } else if (key == QLatin1String("LEFT")) {
        return flush();
    }
    return cmd;
}

SeekHoldPolicy::Command SeekHoldPolicy::flush()
{
    Command cmd;
    if (m_leftAccum != 0.0) {
        cmd.kind = Command::Seek;
        cmd.value = m_leftAccum;
        m_leftAccum = 0.0;
    }
    return cmd;
}

void SeekHoldPolicy::reset()
{
    m_ramping = false;
    m_rampTicks = 0;
    m_leftAccum = 0.0;
}
