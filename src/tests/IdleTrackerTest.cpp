#include "../input/IdleTracker.h"

#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QElapsedTimer>

// Unit tests for the idle-blocking flags, focused on gameActive (IDLE-02):
// RetroArch gameplay must freeze the idle count exactly like mpv/script
// takeovers, and the Main.qml writer's paired resetActivity() on game end
// must give the keyless return a full fresh threshold.
//
// Accelerated threshold (2 s) with REAL 1000 ms ticks — same mechanism as the
// preserved IDLE-02 component harness, no timer scaling. The reset-pairing
// assertions are landing-order independent: they hold whether the count reset
// comes from the paired resetActivity() (this base) or from the IDLE-01 class
// edge reset once that lands (the extra resetActivity() is then a no-op).
class IdleTrackerTest : public QObject {
    Q_OBJECT

private slots:
    void gameActiveBlocksAccrualAndFire();
    void gameEndPairedResetGivesFullThreshold();
    void gameActiveNoOpWriteDoesNotEmit();
    void otherBlockerStillHoldsWhenGameEnds();
    void unblockClearsActiveLikeSiblingSetters();
    void gameActiveBlocksCountEvenWithSaverDisabled();
};

// Pre-idle to threshold-1, then block: the count must not accrue and the
// saver must not fire even after more than a full threshold of gameplay.
void IdleTrackerTest::gameActiveBlocksAccrualAndFire()
{
    IdleTracker tracker(2);
    tracker.setEnabled(true);
    QSignalSpy activeSpy(&tracker, &IdleTracker::activeChanged);
    QSignalSpy countSpy(&tracker, &IdleTracker::idleSecondsChanged);

    QTRY_VERIFY_WITH_TIMEOUT(tracker.idleSeconds() >= 1, 3000);
    tracker.setGameActive(true);
    const int countAtBlock = tracker.idleSeconds();
    const int countEmissionsAtBlock = countSpy.count();

    QTest::qWait(2500); // > threshold: the unfixed tracker fires in here

    QCOMPARE(tracker.idleSeconds(), countAtBlock);
    QCOMPARE(countSpy.count(), countEmissionsAtBlock);
    QVERIFY(!tracker.isActive());
    QCOMPARE(activeSpy.count(), 0);
}

// The defect's second symptom: a short game must not leak its accrued idle
// into the return. Game end pairs setGameActive(false) + resetActivity()
// exactly like the Main.qml writer; the saver then needs a FULL threshold
// of post-return idleness, not threshold-minus-accrued.
void IdleTrackerTest::gameEndPairedResetGivesFullThreshold()
{
    IdleTracker tracker(2);
    tracker.setEnabled(true);
    QTRY_VERIFY_WITH_TIMEOUT(tracker.idleSeconds() >= 1, 3000);
    tracker.setGameActive(true);
    QTest::qWait(1500); // count frozen at 1; unfixed it would be >= 2
    QVERIFY(!tracker.isActive());

    QElapsedTimer sinceReset;
    sinceReset.start();
    tracker.setGameActive(false);
    tracker.resetActivity(); // the QML writer's pair
    QCOMPARE(tracker.idleSeconds(), 0);
    QVERIFY(!tracker.isActive());

    // No early fire: one tick short of the threshold must still be quiet.
    QTest::qWait(900);
    QVERIFY(!tracker.isActive());
    QTRY_VERIFY_WITH_TIMEOUT(tracker.isActive(), 1600);
    const qint64 fireMs = sinceReset.elapsed();
    QVERIFY2(fireMs >= 950,
             qPrintable(QStringLiteral("saver fired too early after game end: %1 ms").arg(fireMs)));
    QVERIFY2(fireMs <= 2600,
             qPrintable(QStringLiteral("saver fired too late after game end: %1 ms").arg(fireMs)));
}

// Edge semantics parity with setMpvActive/setScriptActive: a no-op write
// emits nothing, a real edge emits exactly once.
void IdleTrackerTest::gameActiveNoOpWriteDoesNotEmit()
{
    IdleTracker tracker(2);
    QSignalSpy spy(&tracker, &IdleTracker::gameActiveChanged);

    tracker.setGameActive(false); // already false
    QCOMPARE(spy.count(), 0);
    tracker.setGameActive(true);
    QCOMPARE(spy.count(), 1);
    QVERIFY(tracker.isGameActive());
    tracker.setGameActive(true); // no-op
    QCOMPARE(spy.count(), 1);
    tracker.setGameActive(false);
    QCOMPARE(spy.count(), 2);
    QVERIFY(!tracker.isGameActive());
}

// Independent producers: ending the game while mpv still holds the screen
// must NOT unblock the saver (the same rule the script flag documents).
void IdleTrackerTest::otherBlockerStillHoldsWhenGameEnds()
{
    IdleTracker tracker(2);
    tracker.setEnabled(true);
    QTRY_VERIFY_WITH_TIMEOUT(tracker.idleSeconds() >= 1, 3000);
    tracker.setMpvActive(true);
    tracker.setGameActive(true);
    const int countAtBlock = tracker.idleSeconds();

    tracker.setGameActive(false); // mpv still owns the screen
    QTest::qWait(2300);

    QCOMPARE(tracker.idleSeconds(), countAtBlock);
    QVERIFY(!tracker.isActive());
}

// Setter parity: unblocking while nothing else holds the screen clears an
// active saver immediately, so it can't flash on the menu after the game.
void IdleTrackerTest::unblockClearsActiveLikeSiblingSetters()
{
    IdleTracker tracker(2);
    tracker.setEnabled(true);
    QTRY_VERIFY_WITH_TIMEOUT(tracker.isActive(), 4000);

    QSignalSpy activeSpy(&tracker, &IdleTracker::activeChanged);
    tracker.setGameActive(true);
    tracker.setGameActive(false);
    QVERIFY(!tracker.isActive());
    QCOMPARE(activeSpy.count(), 1);
    tracker.resetActivity(); // keep the frozen count from re-firing (IDLE-01)
}

// At the current dev tip the tick counts even with the screensaver OFF (the
// YouTube idle SponsorBlock fill reads idleSeconds). gameActive must freeze
// the COUNT, not just the fire, so a long game can't start idle-fill fetches.
void IdleTrackerTest::gameActiveBlocksCountEvenWithSaverDisabled()
{
    IdleTracker tracker(60); // enabled stays false — production default
    QTRY_VERIFY_WITH_TIMEOUT(tracker.idleSeconds() >= 1, 3000);
    tracker.setGameActive(true);
    const int countAtBlock = tracker.idleSeconds();

    QTest::qWait(2300);

    QCOMPARE(tracker.idleSeconds(), countAtBlock);
    QVERIFY(!tracker.isActive());
}

QTEST_GUILESS_MAIN(IdleTrackerTest)
#include "IdleTrackerTest.moc"