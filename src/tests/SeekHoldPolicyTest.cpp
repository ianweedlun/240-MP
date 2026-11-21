// Unit tests for the held-key seek shaping policy (YouTube slow-stream fix).
// Pure logic — no mpv, no IPC, no timers; the owner drives flush() externally.
#include <QtTest>
#include "player/SeekHoldPolicy.h"

class SeekHoldPolicyTest : public QObject {
    Q_OBJECT
private slots:
    void rightTapSeeksOneStep();
    void rightHoldCoalescesIntoFlushes();
    void rightReleaseFlushesRemainder();
    void rightTapAfterHoldSeeksAgain();
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

void SeekHoldPolicyTest::rightHoldCoalescesIntoFlushes()
{
    SeekHoldPolicy p;
    p.press("RIGHT", false);
    for (int i = 0; i < 4; ++i) {
        QCOMPARE(p.press("RIGHT", true).kind, Cmd::None);
        QVERIFY(p.flushTimerWanted());
    }
    const Cmd f = p.flush();
    QCOMPARE(f.kind, Cmd::Seek);
    QCOMPARE(f.value, 40.0);
    QVERIFY(!p.flushTimerWanted());
    QCOMPARE(p.flush().kind, Cmd::None);
}

void SeekHoldPolicyTest::rightReleaseFlushesRemainder()
{
    SeekHoldPolicy p;
    p.press("RIGHT", false);
    p.press("RIGHT", true);
    p.press("RIGHT", true);
    QVERIFY(p.flushTimerWanted());
    const Cmd up = p.release("RIGHT");
    QCOMPARE(up.kind, Cmd::Seek);
    QCOMPARE(up.value, 20.0);
    QVERIFY(!p.flushTimerWanted());
}

void SeekHoldPolicyTest::rightTapAfterHoldSeeksAgain()
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
    p.press("RIGHT", true);          // pending accumulator
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
    // hold (30 synthesized repeats) at one flush per 400 ms must produce at
    // most 8 seeks (vs 30 unshaped), totaling 300 s in the held direction.
    // Both directions are symmetric — the owner removed the speed ramp, so
    // RIGHT coalesces exactly like LEFT.
    auto run = [](const QString &key, double sign) {
        SeekHoldPolicy p;
        int seeks = 0;
        double total = 0.0;
        auto count = [&](const Cmd &c) {
            if (c.kind == Cmd::Seek) { ++seeks; total += c.value; }
        };
        for (int i = 1; i <= 30; ++i) {
            count(p.press(key, true));
            if (i % 4 == 0) count(p.flush());
        }
        count(p.release(key));
        QVERIFY2(seeks <= 8,
                 qPrintable(QStringLiteral("%1 seeks=%2").arg(key).arg(seeks)));
        QVERIFY2(qFuzzyCompare(total, 300.0 * sign),
                 qPrintable(QStringLiteral("%1 total=%2").arg(key).arg(total)));
    };
    run("RIGHT", +1.0);
    run("LEFT", -1.0);
}

QTEST_MAIN(SeekHoldPolicyTest)
#include "SeekHoldPolicyTest.moc"
