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
    void successStaleOnlyAfterTtlAndNeverForZeroStamp();
    void idleSelectSkipsFreshKeepsStaleAndUnknown();
    void idleSelectDedupesPreservesOrderAndHonoursCap();
    void idleSelectSkipsInFlightAndRecentlyFailed();
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

void YtSponsorTtlTest::successStaleOnlyAfterTtlAndNeverForZeroStamp()
{
    // now must exceed the TTL, otherwise the computed stamp goes negative and
    // the zero/negative guard (correctly) refuses to call it stale.
    const qint64 now = 4 * ytsb::kSuccessTtlMs;
    // Just inside the window: still fresh.
    QVERIFY(!ytsb::successStale(now - ytsb::kSuccessTtlMs + 1, now));
    // Exactly at the edge: stale (>=, so a durable cache always self-heals).
    QVERIFY(ytsb::successStale(now - ytsb::kSuccessTtlMs, now));
    QVERIFY(ytsb::successStale(now - ytsb::kSuccessTtlMs - 1, now));
    // A zero stamp means "predates stamping" — never stale, so an upgraded
    // cache does not refetch every video on first boot.
    QVERIFY(!ytsb::successStale(0, now));
}

void YtSponsorTtlTest::idleSelectSkipsFreshKeepsStaleAndUnknown()
{
    const qint64 now = 5'000'000'000;
    QHash<QString, qint64> fetchedAt;
    QSet<QString> cached;
    QSet<QString> fetching;
    QHash<QString, qint64> failedAt;

    cached.insert(QStringLiteral("fresh"));
    fetchedAt.insert(QStringLiteral("fresh"), now - 1000);
    cached.insert(QStringLiteral("stale"));
    fetchedAt.insert(QStringLiteral("stale"), now - ytsb::kSuccessTtlMs - 1);
    // "unknown" is in neither map.

    const QStringList got = ytsb::idleSelect(
        {QStringLiteral("fresh"), QStringLiteral("stale"), QStringLiteral("unknown")},
        fetchedAt, cached, fetching, failedAt, now, 100);

    QCOMPARE(got, QStringList({QStringLiteral("stale"), QStringLiteral("unknown")}));
}

void YtSponsorTtlTest::idleSelectDedupesPreservesOrderAndHonoursCap()
{
    const qint64 now = 9'000;
    QHash<QString, qint64> fetchedAt, failedAt;
    QSet<QString> cached, fetching;

    const QStringList got = ytsb::idleSelect(
        {QStringLiteral("a"), QStringLiteral(""), QStringLiteral("b"),
         QStringLiteral("a"), QStringLiteral("c")},
        fetchedAt, cached, fetching, failedAt, now, 2);

    // Empty dropped, duplicate collapsed, input order kept, cap honoured.
    QCOMPARE(got, QStringList({QStringLiteral("a"), QStringLiteral("b")}));
}

void YtSponsorTtlTest::idleSelectSkipsInFlightAndRecentlyFailed()
{
    const qint64 now = 20'000;
    QHash<QString, qint64> fetchedAt, failedAt;
    QSet<QString> cached, fetching;

    fetching.insert(QStringLiteral("busy"));
    failedAt.insert(QStringLiteral("failed"), now - 1000);          // inside TTL
    failedAt.insert(QStringLiteral("recovered"),
                    now - ytsb::kFailureTtlMs - 1);                 // expired

    const QStringList got = ytsb::idleSelect(
        {QStringLiteral("busy"), QStringLiteral("failed"),
         QStringLiteral("recovered"), QStringLiteral("ok")},
        fetchedAt, cached, fetching, failedAt, now, 100);

    QCOMPARE(got, QStringList({QStringLiteral("recovered"), QStringLiteral("ok")}));
}

QTEST_APPLESS_MAIN(YtSponsorTtlTest)
#include "YtSponsorTtlTest.moc"
