#pragma once
#include <QString>

// Held-key seek shaping for slow-stream playback (YouTube over throttled
// WiFi). InputManager synthesizes 10 presses/s while a direction is held;
// firing one mpv seek per press backlogs the demuxer — measured on the Pi:
// 1.3-9.5 s per video seek, 22 s from first press to resumed playback.
//
// Policy (owner decision 2026-11-20: the playback-speed ramp was an unrequested
// feature and is gone):
//   RIGHT hold   -> coalesced forward seeks: accumulate +10 s per tick, one
//                   flush per ~400 ms.
//   LEFT hold    -> coalesced backward seeks: accumulate -10 s per tick, one
//                   flush per ~400 ms.
//   UP/DOWN      -> pass through on tap (the lua forced bindings open the mpv
//                   OSC menu); repeats dropped so a held key cannot machine-gun
//                   the menu. #128: the ±60 s vertical seek repurposing was an
//                   unrequested behavior change — the menu is the owner's
//                   contract for these keys.
//   taps         -> single relative+exact seek (10 s horizontal).
//   other keys   -> pass through on tap; repeats dropped (pause/quit must not
//                   machine-gun either).
//
// Pure logic, no timers: the owner drives flush() from a QTimer and consults
// flushTimerWanted() after every event. Unit-tested in SeekHoldPolicyTest.
class SeekHoldPolicy {
public:
    struct Command {
        enum Kind { None, Seek, Key };
        Kind kind = None;
        double value = 0.0;   // Seek: seconds, relative+exact.
        QString key;          // Key: mpv key name to pass through unchanged.
    };

    Command press(const QString &key, bool autoRepeat);
    Command release(const QString &key);
    Command flush();          // coalesce timer fired
    bool flushTimerWanted() const { return m_leftAccum != 0.0 || m_rightAccum != 0.0; }
    void reset();

    static constexpr double kTapStepSec      = 10.0;
    static constexpr int    kCoalesceFlushMs = 400;

private:
    double m_leftAccum  = 0.0;
    double m_rightAccum = 0.0;
};
