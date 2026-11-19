#pragma once
#include <QString>

// Held-key seek shaping for slow-stream playback (YouTube over throttled
// WiFi). InputManager synthesizes 10 presses/s while a direction is held;
// firing one mpv seek per press backlogs the demuxer — measured on the Pi:
// 1.3-9.5 s per video seek, 22 s from first press to resumed playback.
//
// Policy:
//   RIGHT hold  -> playback-speed ramp (2x, then 4x after ~1 s). Zero seeks
//                  while held: playback runs from cache, no demuxer thrash.
//   LEFT hold   -> coalesced backward seeks: accumulate -10 s per tick, one
//                  flush per ~400 ms. Backward usually lands in cache.
//   UP/DOWN hold-> ignored. Chapter-sized jumps stay per-tap; coalescing
//                  them would build enormous forward scans.
//   taps        -> single relative+exact seek (10 s horizontal, 60 s vertical).
//   other keys  -> pass through on tap; repeats dropped (pause/quit must not
//                  machine-gun either).
//
// Pure logic, no timers: the owner drives flush() from a QTimer and consults
// flushTimerWanted() after every event. Unit-tested in SeekHoldPolicyTest.
class SeekHoldPolicy {
public:
    struct Command {
        enum Kind { None, Seek, Speed, Key };
        Kind kind = None;
        double value = 0.0;   // Seek: seconds, relative+exact. Speed: factor.
        QString key;          // Key: mpv key name to pass through unchanged.
    };

    Command press(const QString &key, bool autoRepeat);
    Command release(const QString &key);
    Command flush();          // coalesce timer fired
    bool flushTimerWanted() const { return m_leftAccum != 0.0; }
    void reset();

    static constexpr double kTapStepSec       = 10.0;
    static constexpr double kVerticalStepSec  = 60.0;
    static constexpr double kRampSpeedNear    = 2.0;
    static constexpr double kRampSpeedFar     = 4.0;
    static constexpr int    kRampEscalateTicks = 10;  // ~1 s at 10 ticks/s
    static constexpr int    kCoalesceFlushMs  = 400;

private:
    bool   m_ramping   = false;
    int    m_rampTicks = 0;
    double m_leftAccum = 0.0;
};
