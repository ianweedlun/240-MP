#include <QtTest/QtTest>

#include "../YtUrlFresh.h"

// Pure-logic tests for the reactive warm-403 cure (invalidateCachedEntry).
// Ground truth from the 2026-11-07 night: clock-fresh cached googlevideo URLs
// STILL 403 on replay — the signature binds to the minting session and IP, not
// just to expire= — so the ONLY safe response to a failed warm launch is to
// evict the entry and force a cold re-mint. These tests pin the eviction
// semantics the Player's retry leans on: exact-key removal, format isolation,
// harmless misses, and no resurrection through the prune path.

namespace {

QJsonObject makeEntry(qint64 expire, const QString &url)
{
    QJsonObject e;
    e.insert(QStringLiteral("url"), url);
    e.insert(QStringLiteral("expire"), expire);
    return e;
}

} // anonymous namespace

class YtUrlCacheInvalidateTest : public QObject {
    Q_OBJECT

private slots:
    void invalidateDropsFreshEntryForItsFormatOnly();
    void invalidateMissIsHarmless();
    void invalidateThenPruneLeavesNothingResurrected();
};

void YtUrlCacheInvalidateTest::invalidateDropsFreshEntryForItsFormatOnly()
{
    const QString vid = QStringLiteral("ckEUIfrg_tU");
    const QString fmt480 = QStringLiteral("bestvideo[height<=480]+bestaudio/best[height<=480]");
    const QString fmt1080 = QStringLiteral("bestvideo[height<=1080]+bestaudio/best[height<=1080]");

    QJsonObject cache;
    cache.insert(yturl::cacheKeyFor(vid, fmt480), makeEntry(1790000000, QStringLiteral("u480")));
    cache.insert(yturl::cacheKeyFor(vid, fmt1080), makeEntry(1790000000, QStringLiteral("u1080")));

    QVERIFY(yturl::invalidateCachedEntry(cache, vid, fmt480));
    // The served-URL lookup for the evicted pair now misses — a Player retry
    // after a warm 403 therefore re-enters the cold resolve, exactly the
    // behavior cachedPlaybackUrl's empty-return contract drives.
    QVERIFY(!cache.contains(yturl::cacheKeyFor(vid, fmt480)));
    // A different resolution's window is a different mint: never collateral.
    QCOMPARE(cache.value(yturl::cacheKeyFor(vid, fmt1080)).toObject().value(QStringLiteral("url")).toString(),
             QStringLiteral("u1080"));
}

void YtUrlCacheInvalidateTest::invalidateMissIsHarmless()
{
    QJsonObject cache;
    cache.insert(QStringLiteral("other|fmt"), makeEntry(1790000000, QStringLiteral("keep")));
    // Cold-open double-fail: nothing stored for this pair (already evicted or
    // never minted). Returns false, cache untouched — the cure is idempotent.
    QVERIFY(!yturl::invalidateCachedEntry(cache, QStringLiteral("missing"), QStringLiteral("fmt")));
    QCOMPARE(cache.size(), 1);
    QVERIFY(cache.contains(QStringLiteral("other|fmt")));
}

void YtUrlCacheInvalidateTest::invalidateThenPruneLeavesNothingResurrected()
{
    const qint64 now = 1790000000;
    const QString vid = QStringLiteral("RL_q3t58R94");
    QJsonObject cache;
    cache.insert(yturl::cacheKeyFor(vid, QStringLiteral("fmt")), makeEntry(now + 50000, QStringLiteral("warm403")));
    cache.insert(QStringLiteral("dead|fmt"), makeEntry(now - 10, QStringLiteral("expired")));

    QVERIFY(yturl::invalidateCachedEntry(cache, vid, QStringLiteral("fmt")));
    yturl::pruneUrlCache(cache, now, 10);
    // The evicted fresh entry must not survive via any prune re-insertion,
    // and prune's cap logic never counts it as one of the survivors.
    QCOMPARE(cache.size(), 0);
}

QTEST_MAIN(YtUrlCacheInvalidateTest)
#include "YtUrlCacheInvalidateTest.moc"
