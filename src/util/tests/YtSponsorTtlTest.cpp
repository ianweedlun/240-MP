#include <QtTest/QtTest>

#include "../YtSponsorTtl.h"

// Cache/TTL gate for the SponsorBlock segment fetch (issue #28 plan E, latency
// follow-up): a confirmed-empty window is a permanent hit, a transient failure
// suppresses retries only inside its window, and the suppression map is pruned
// so it cannot grow without bound. Pure clock math — no network, no backend.

class YtSponsorTtlTest : public QObject {
    Q_OBJECT

private slots:
    void failureBlocksInsideWindowAndExpiresAtEdge();
    void zeroStampNeverBlocks();
    void pruneDropsOnlyExpiredEntries();
    void decisionPrefersCacheThenInFlightThenSuppressionThenFetch();
    void cacheHitOutranksAnActiveFailure();
};

void YtSponsorTtlTest::failureBlocksInsideWindowAndExpiresAtEdge()
{
    const qint64 failedAt = 5000;
    QVERIFY(ytsb::failureActive(failedAt, failedAt, ytsb::kFailureTtlMs));       // just failed
    QVERIFY(ytsb::failureActive(failedAt, failedAt + ytsb::kFailureTtlMs - 1));  // last ms inside
    QVERIFY(!ytsb::failureActive(failedAt, failedAt + ytsb::kFailureTtlMs));     // edge = expired
    QVERIFY(!ytsb::failureActive(failedAt, failedAt + ytsb::kFailureTtlMs + 1));
    // A different window length is honored rather than hard-coded.
    QVERIFY(ytsb::failureActive(failedAt, failedAt + 999, 1000));
    QVERIFY(!ytsb::failureActive(failedAt, failedAt + 1000, 1000));
}

void YtSponsorTtlTest::zeroStampNeverBlocks()
{
    // QHash::value() on an absent key yields 0; that must read as "no failure".
    QVERIFY(!ytsb::failureActive(0, 123456));
    QVERIFY(!ytsb::failureActive(-5, 123456));
}

void YtSponsorTtlTest::pruneDropsOnlyExpiredEntries()
{
    QHash<QString, qint64> failed;
    failed.insert(QStringLiteral("old"), 1000);
    failed.insert(QStringLiteral("fresh"), 9000);

    const int pruned = ytsb::pruneFailures(failed, 1000 + ytsb::kFailureTtlMs);
    QCOMPARE(pruned, 1);
    QCOMPARE(failed.size(), 1);
    QVERIFY(!failed.contains(QStringLiteral("old")));
    QVERIFY(failed.contains(QStringLiteral("fresh")));

    // Pruning an empty map is a no-op, and a second pass at the same instant
    // changes nothing.
    QHash<QString, qint64> empty;
    QCOMPARE(ytsb::pruneFailures(empty, 42), 0);
    QCOMPARE(ytsb::pruneFailures(failed, 1000 + ytsb::kFailureTtlMs), 0);
}

void YtSponsorTtlTest::decisionPrefersCacheThenInFlightThenSuppressionThenFetch()
{
    QCOMPARE(ytsb::decideFetch(true,  true,  true),  ytsb::FetchDecision::ServeCache);
    QCOMPARE(ytsb::decideFetch(false, true,  true),  ytsb::FetchDecision::AlreadyFetching);
    QCOMPARE(ytsb::decideFetch(false, false, true),  ytsb::FetchDecision::Suppressed);
    QCOMPARE(ytsb::decideFetch(false, false, false), ytsb::FetchDecision::Fetch);
}

void YtSponsorTtlTest::cacheHitOutranksAnActiveFailure()
{
    // A 4xx-confirmed empty window is permanent truth; a stale failure stamp
    // for the same video must not send us back to the network.
    QCOMPARE(ytsb::decideFetch(true, false, true), ytsb::FetchDecision::ServeCache);
}

QTEST_APPLESS_MAIN(YtSponsorTtlTest)
#include "YtSponsorTtlTest.moc"
