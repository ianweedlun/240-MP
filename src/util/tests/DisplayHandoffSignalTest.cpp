#include <QtTest/QtTest>
#include <QStringList>
#include "../DisplayHandoff.h"
#include "../HandoffWindowPolicy.h"

// Contract tests for the #75 render-suspend mitigation (PR #77):
//
//   displayTaken() is emitted exactly once per completed hand-off, and only
//   AFTER the CRTC state has been captured. main.cpp hides the root window on
//   that signal, and hiding it is what makes Qt rebuild EGLFS on the way back —
//   which in turn makes the legacy CRTC restore redundant. So the ordering is the
//   fix, not an implementation detail: emit first, and the post-exit modeset
//   silently regresses to the double-blink / dead-CRTC behaviour #75 is about.
//
// Nothing here touches hardware. On a machine with no compositor
// (DisplayHandoff::isHeadless()) acquire() takes the hand-off path but every
// privileged step fails cleanly: /dev/tty0 and the VT fds are not openable by an
// unprivileged process, and there is no /dev/dri to find. The CRTC capture step
// itself cannot run without a DRM device, so it is injected through
// setCrtcCaptureHookForTest() — the hook is what makes "saved, then announced"
// assertable in the lab and on CI. Cases that need a real hand-off QSKIP when the
// platform never hands off (macOS, or Linux with a compositor).
namespace {

// True when acquire() can actually take the display on this machine. On a
// compositor (or macOS) hand-off is a no-op by design and every signal case below
// is vacuous.
bool canTakeDisplay()
{
    return DisplayHandoff::isHeadless();
}

} // namespace

class DisplayHandoffSignalTest : public QObject {
    Q_OBJECT

private slots:
    void takeEmitsOnceAfterCrtcStateIsCaptured();
    void returnEmitsOnceAndASecondCycleBehavesTheSame();
    void repeatedCompleteRestoreDoesNotReemitReturn();
    void refusedHandoffEmitsNothingAndLeavesNoHalfState();
    void compositorHandoffEmitsNothing();
    void keepWindowRequestedOnlyForNonEmptyValue();
    void keepWindowSkipsTheWindowHideAndTheEglfsReinit();
};

// The whole fix, in one assertion: capture happens, then the signal.
void DisplayHandoffSignalTest::takeEmitsOnceAfterCrtcStateIsCaptured()
{
    if (!canTakeDisplay())
        QSKIP("no hand-off path on this platform/compositor");

    DisplayHandoff handoff;
    QStringList order;
    handoff.setCrtcCaptureHookForTest([&order] { order << QStringLiteral("capture"); });

    int taken = 0;
    bool capturedWhenSlotRan = false;
    QString ownerInSlot;
    QObject::connect(&handoff, &DisplayHandoff::displayTaken, &handoff, [&] {
        ++taken;
        capturedWhenSlotRan = order.contains(QStringLiteral("capture"));
        ownerInSlot = handoff.currentOwner();
        order << QStringLiteral("taken");
    }, Qt::DirectConnection);

    const int previousVt = handoff.acquire(QStringLiteral("test"));

    // A completed hand-off hands the child a VT to run on.
    QVERIFY(previousVt > 0);
    QCOMPARE(taken, 1);
    // Capture first, announce second. Swap the two statements in acquire() and
    // this is ["taken", "capture"].
    QCOMPARE(order, QStringList({"capture", "taken"}));
    QVERIFY(capturedWhenSlotRan);
    // The hand-off is fully owned before anybody is told to hide anything.
    QCOMPARE(ownerInSlot, QStringLiteral("test"));
    QCOMPARE(handoff.currentOwner(), QStringLiteral("test"));
}

// The restore path is stateful (owner cleared, VT handed back, timer/callback
// reset), so one good cycle proves nothing about the second.
void DisplayHandoffSignalTest::returnEmitsOnceAndASecondCycleBehavesTheSame()
{
    if (!canTakeDisplay())
        QSKIP("no hand-off path on this platform/compositor");

    DisplayHandoff handoff;
    handoff.setCrtcCaptureHookForTest([] {});

    int taken = 0;
    int returned = 0;
    QObject::connect(&handoff, &DisplayHandoff::displayTaken, &handoff,
                     [&taken] { ++taken; }, Qt::DirectConnection);
    QObject::connect(&handoff, &DisplayHandoff::displayReturned, &handoff,
                     [&returned] { ++returned; }, Qt::DirectConnection);

    for (int cycle = 1; cycle <= 2; ++cycle) {
        const int returnedBefore = returned;
        QVERIFY(handoff.acquire(QStringLiteral("cycle")) > 0);
        QCOMPARE(taken, cycle);            // exactly one take per cycle, none carried over
        QCOMPARE(returned, returnedBefore); // nothing returned before it was taken

        bool restoredCallbackRan = false;
        handoff.releaseDeferred(QStringLiteral("cycle"), [&] { restoredCallbackRan = true; }, 0);

        QTRY_COMPARE(returned, returnedBefore + 1);
        QTRY_VERIFY(restoredCallbackRan);
        QCOMPARE(taken, cycle);   // the return did not re-announce a take
        // Fully released: the next acquire() is a fresh hand-off, not a refusal.
        QVERIFY(handoff.currentOwner().isEmpty());
    }
    QCOMPARE(taken, 2);
    QCOMPARE(returned, 2);
}

// main.cpp waits for frameSwapped AND has a timer inside DisplayHandoff, so
// completeRestore() really does arrive twice on hardware. The second one must not
// announce another return: the window is already up.
void DisplayHandoffSignalTest::repeatedCompleteRestoreDoesNotReemitReturn()
{
    if (!canTakeDisplay())
        QSKIP("no hand-off path on this platform/compositor");

    DisplayHandoff handoff;
    handoff.setCrtcCaptureHookForTest([] {});
    int returned = 0;
    QObject::connect(&handoff, &DisplayHandoff::displayReturned, &handoff,
                     [&returned] { ++returned; }, Qt::DirectConnection);

    QVERIFY(handoff.acquire(QStringLiteral("frame")) > 0);
    handoff.releaseDeferred(QStringLiteral("frame"), [] {}, 0);
    QTRY_COMPARE(returned, 1);

    handoff.completeRestore();
    handoff.completeRestore();
    QCOMPARE(returned, 1);
}

// A hand-off that never completes must stay silent. If displayTaken() fired on a
// refused acquire, main.cpp would hide the window and nothing would ever show it
// again — a black console with a live app behind it.
void DisplayHandoffSignalTest::refusedHandoffEmitsNothingAndLeavesNoHalfState()
{
    if (!canTakeDisplay())
        QSKIP("no hand-off path on this platform/compositor");

    DisplayHandoff handoff;
    handoff.setCrtcCaptureHookForTest([] {});
    int taken = 0;
    int returned = 0;
    QObject::connect(&handoff, &DisplayHandoff::displayTaken, &handoff,
                     [&taken] { ++taken; }, Qt::DirectConnection);
    QObject::connect(&handoff, &DisplayHandoff::displayReturned, &handoff,
                     [&returned] { ++returned; }, Qt::DirectConnection);

    QVERIFY(handoff.acquire(QStringLiteral("owner-a")) > 0);
    QCOMPARE(taken, 1);

    // Refused: somebody else owns the display. No signal, owner unchanged.
    QCOMPARE(handoff.acquire(QStringLiteral("owner-b")), -1);
    QCOMPARE(taken, 1);
    QCOMPARE(handoff.currentOwner(), QStringLiteral("owner-a"));

    // Refused: the same owner asking again while it already holds the display.
    // (isHeldBy() needs a previous VT to know it is held, so this half only
    // applies where the kernel reports one — it always does on the Pi.)
    if (handoff.isHeldBy(QStringLiteral("owner-a"))) {
        QCOMPARE(handoff.acquire(QStringLiteral("owner-a")), -1);
        QCOMPARE(taken, 1);
    }

    // The refused calls left the live hand-off intact, so it still returns once.
    handoff.releaseDeferred(QStringLiteral("owner-a"), [] {}, 0);
    QTRY_COMPARE(returned, 1);
    QCOMPARE(taken, 1);
}

// With a compositor there is nothing to hand off: acquire() declines (0) before
// it can announce anything, so the window is never hidden. Only displayTaken() is
// gated this way — a restore attempt still emits displayReturned(), which is
// harmless (the window is already visible) and is what callers' callbacks ride.
void DisplayHandoffSignalTest::compositorHandoffEmitsNothing()
{
    const QByteArray savedDisplay = qgetenv("DISPLAY");
    qputenv("DISPLAY", QByteArrayLiteral(":0"));   // forces the non-headless path
    QVERIFY(!DisplayHandoff::isHeadless());

    DisplayHandoff handoff;
    int taken = 0;
    QObject::connect(&handoff, &DisplayHandoff::displayTaken, &handoff,
                     [&taken] { ++taken; }, Qt::DirectConnection);
    QCOMPARE(handoff.acquire(QStringLiteral("compositor")), 0);
    QCOMPARE(taken, 0);

    if (savedDisplay.isEmpty())
        qunsetenv("DISPLAY");
    else
        qputenv("DISPLAY", savedDisplay);
}

// MP240_HANDOFF_KEEP_WINDOW is the A/B escape hatch on the CRT: set it and the
// window stays up for the whole hand-off, i.e. the pre-#75 behaviour.
void DisplayHandoffSignalTest::keepWindowRequestedOnlyForNonEmptyValue()
{
    const QByteArray saved = qgetenv("MP240_HANDOFF_KEEP_WINDOW");

    qunsetenv("MP240_HANDOFF_KEEP_WINDOW");
    QVERIFY(!handoffwindow::keepWindowRequested());

    qputenv("MP240_HANDOFF_KEEP_WINDOW", QByteArrayLiteral("1"));
    QVERIFY(handoffwindow::keepWindowRequested());

    qputenv("MP240_HANDOFF_KEEP_WINDOW", QByteArrayLiteral(""));
    QVERIFY(!handoffwindow::keepWindowRequested());

    // Any non-empty value keeps the window — "0" and "false" included. The hatch
    // is set by hand on a console; it must never be off by a typo.
    qputenv("MP240_HANDOFF_KEEP_WINDOW", QByteArrayLiteral("0"));
    QVERIFY(handoffwindow::keepWindowRequested());
    qputenv("MP240_HANDOFF_KEEP_WINDOW", QByteArrayLiteral("false"));
    QVERIFY(handoffwindow::keepWindowRequested());

    if (saved.isEmpty())
        qunsetenv("MP240_HANDOFF_KEEP_WINDOW");
    else
        qputenv("MP240_HANDOFF_KEEP_WINDOW", saved);
}

// The predicate main.cpp actually branches on: default hides the window (and so
// tells DisplayHandoff that EGLFS is re-initialised on return); the escape hatch
// skips the hide AND leaves the legacy CRTC restore in play. Both decisions come
// from this one value, which is what keeps them from disagreeing.
void DisplayHandoffSignalTest::keepWindowSkipsTheWindowHideAndTheEglfsReinit()
{
    const QByteArray saved = qgetenv("MP240_HANDOFF_KEEP_WINDOW");

    qunsetenv("MP240_HANDOFF_KEEP_WINDOW");
    QVERIFY(handoffwindow::hideWindowOnTake());        // default: hide the window

    qputenv("MP240_HANDOFF_KEEP_WINDOW", QByteArrayLiteral("1"));
    QVERIFY(!handoffwindow::hideWindowOnTake());       // old behaviour: keep it up

    if (saved.isEmpty())
        qunsetenv("MP240_HANDOFF_KEEP_WINDOW");
    else
        qputenv("MP240_HANDOFF_KEEP_WINDOW", saved);
}

QTEST_GUILESS_MAIN(DisplayHandoffSignalTest)
#include "DisplayHandoffSignalTest.moc"
