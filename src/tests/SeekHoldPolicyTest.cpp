// Unit tests for the held-key seek shaping policy (YouTube slow-stream fix).
// Pure logic — no mpv, no IPC, no timers; the owner drives flush() externally.
#include <QtTest>
#include "player/SeekHoldPolicy.h"

class SeekHoldPolicyTest : public QObject {
    Q_OBJECT
private slots:
    void rightTapSeeksOneStep();
    void rightHoldRampsSpeedAndNeverSeeks();
    void rightReleaseRestoresNormalSpeed();
    void rightTapAfterRampSeeksAgain();
    void leftTapSeeksOneStep();
    void leftHoldCoalescesIntoFlushes();
    void leftReleaseFlushesRemainder();
    void verticalTapsOpenMenuHoldsDrop();
    void otherKeysPassThroughOnTapOnly();
    void resetClearsAllState();
    void heldBurstStaysWithinSeekBudget();
};

using Cmd = SeekHoldPolicy::Command;

void SeekHoldPolicyTest::rightTapSeeksOneStep()
{
    SeekHoldPolicy p;
    const Cmd c = p.press("RIGHT", false);
    QCOMPARE(c.kind, Cmd::Seek);
    QCOMPARE(c.value, 10.0);
    QVERIFY(!p.flushTimerWanted());
}

void SeekHoldPolicyTest::rightHoldRampsSpeedAndNeverSeeks()
{
    SeekHoldPolicy p;
    p.press("RIGHT", false);                    // the initial tap
    const Cmd first = p.press("RIGHT", true);   // first synthesized repeat
    QCOMPARE(first.kind, Cmd::Speed);
    QCOMPARE(first.value, 2.0);
    for (int i = 1; i < SeekHoldPolicy::kRampEscalateTicks; ++i) {
        const Cmd c = p.press("RIGHT", true);
        QCOMPARE(c.kind, Cmd::None);            // no seek, no speed churn
    }
    const Cmd esc = p.press("RIGHT", true);     // tick == kRampEscalateTicks
    QCOMPARE(esc.kind, Cmd::Speed);
    QCOMPARE(esc.value, 4.0);
    for (int i = 0; i < 20; ++i)
        QCOMPARE(p.press("RIGHT", true).kind, Cmd::None);
    QVERIFY(!p.flushTimerWanted());
}

void SeekHoldPolicyTest::rightReleaseRestoresNormalSpeed()
{
    SeekHoldPolicy p;
    p.press("RIGHT", false);
    p.press("RIGHT", true);
    const Cmd up = p.release("RIGHT");
    QCOMPARE(up.kind, Cmd::Speed);
    QCOMPARE(up.value, 1.0);
    QCOMPARE(p.release("RIGHT").kind, Cmd::None);   // idempotent
}

void SeekHoldPolicyTest::rightTapAfterRampSeeksAgain()
{
    SeekHoldPolicy p;
    p.press("RIGHT", false);
    p.press("RIGHT", true);
    p.release("RIGHT");
    const Cmd c = p.press("RIGHT", false);
    QCOMPARE(c.kind, Cmd::Seek);
    QCOMPARE(c.value, 10.0);
}

void SeekHoldPolicyTest::leftTapSeeksOneStep()
{
    SeekHoldPolicy p;
    const Cmd c = p.press("LEFT", false);
    QCOMPARE(c.kind, Cmd::Seek);
    QCOMPARE(c.value, -10.0);
}

void SeekHoldPolicyTest::leftHoldCoalescesIntoFlushes()
{
    SeekHoldPolicy p;
    p.press("LEFT", false);
    for (int i = 0; i < 4; ++i) {
        QCOMPARE(p.press("LEFT", true).kind, Cmd::None);
        QVERIFY(p.flushTimerWanted());
    }
    const Cmd f = p.flush();
    QCOMPARE(f.kind, Cmd::Seek);
    QCOMPARE(f.value, -40.0);
    QVERIFY(!p.flushTimerWanted());
    QCOMPARE(p.flush().kind, Cmd::None);
}

void SeekHoldPolicyTest::leftReleaseFlushesRemainder()
{
    SeekHoldPolicy p;
    p.press("LEFT", false);
    p.press("LEFT", true);
    p.press("LEFT", true);
    QVERIFY(p.flushTimerWanted());
    const Cmd up = p.release("LEFT");
    QCOMPARE(up.kind, Cmd::Seek);
    QCOMPARE(up.value, -20.0);
    QVERIFY(!p.flushTimerWanted());
}

void SeekHoldPolicyTest::verticalTapsOpenMenuHoldsDrop()
{
    // #128: UP/DOWN are the owner's OSC-menu keys. A tap must pass through as
    // a byte-identical keypress (the lua forced bindings open the menu); it
    // must NOT seek. Held repeats drop so the menu cannot machine-gun.
    SeekHoldPolicy p;
    const Cmd up = p.press("UP", false);
    QCOMPARE(up.kind, Cmd::Key);
    QCOMPARE(up.key, QString("UP"));
    const Cmd down = p.press("DOWN", false);
    QCOMPARE(down.kind, Cmd::Key);
    QCOMPARE(down.key, QString("DOWN"));
    for (int i = 0; i < 10; ++i) {
        QCOMPARE(p.press("UP", true).kind, Cmd::None);
        QCOMPARE(p.press("DOWN", true).kind, Cmd::None);
    }
    QVERIFY(!p.flushTimerWanted());
    QCOMPARE(p.release("UP").kind, Cmd::None);
    QCOMPARE(p.release("DOWN").kind, Cmd::None);
}

void SeekHoldPolicyTest::otherKeysPassThroughOnTapOnly()
{
    SeekHoldPolicy p;
    const Cmd enter = p.press("ENTER", false);
    QCOMPARE(enter.kind, Cmd::Key);
    QCOMPARE(enter.key, QString("ENTER"));
    QCOMPARE(p.press("ENTER", true).kind, Cmd::None);   // no pause machine-gun
    QCOMPARE(p.press("SPACE", true).kind, Cmd::None);
    QCOMPARE(p.press("ESC", false).kind, Cmd::Key);
    QCOMPARE(p.release("ENTER").kind, Cmd::None);
}

void SeekHoldPolicyTest::resetClearsAllState()
{
    SeekHoldPolicy p;
    p.press("RIGHT", false);
    p.press("RIGHT", true);          // ramping
    p.press("LEFT", true);           // pending accumulator
    p.reset();
    QCOMPARE(p.release("RIGHT").kind, Cmd::None);
    QCOMPARE(p.flush().kind, Cmd::None);
    QVERIFY(!p.flushTimerWanted());
    QCOMPARE(p.press("RIGHT", false).kind, Cmd::Seek);
}

void SeekHoldPolicyTest::heldBurstStaysWithinSeekBudget()
{
    // The measured failure: 10 presses/s while held, one seek each. A 3-second
    // RIGHT hold must produce ZERO seeks; a 3-second LEFT hold (30 synthesized
    // repeats) at one flush per 4 ticks must produce at most 8 seeks (vs 30
    // unshaped), totaling -300 s.
    SeekHoldPolicy p;
    int rightSeeks = 0;
    p.press("RIGHT", false);
    for (int i = 0; i < 30; ++i)
        if (p.press("RIGHT", true).kind == Cmd::Seek) ++rightSeeks;
    p.release("RIGHT");
    QCOMPARE(rightSeeks, 0);

    SeekHoldPolicy q;
    int leftSeeks = 0;
    double total = 0.0;
    auto count = [&](const Cmd &c) {
        if (c.kind == Cmd::Seek) { ++leftSeeks; total += c.value; }
    };
    for (int i = 1; i <= 30; ++i) {
        count(q.press("LEFT", true));
        if (i % 4 == 0) count(q.flush());
    }
    count(q.release("LEFT"));
    QVERIFY(leftSeeks <= 8);
    QCOMPARE(total, -300.0);
}

QTEST_MAIN(SeekHoldPolicyTest)
#include "SeekHoldPolicyTest.moc"
