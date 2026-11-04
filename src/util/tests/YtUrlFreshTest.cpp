#include <QtTest/QtTest>

#include "../YtUrlFresh.h"

// Pure-logic tests for the YouTube direct-playback URL cache helpers (issue
// #28 plan A): expire-parameter extraction from signed googlevideo URLs,
// freshness against a safety margin, and cache pruning (expired removal +
// newest-window cap). No processes, no network.

namespace {

QJsonObject makeEntry(qint64 expire, const QString &url)
{
    QJsonObject e;
    e.insert(QStringLiteral("url"), url);
    e.insert(QStringLiteral("expire"), expire);
    return e;
}

} // anonymous namespace

class YtUrlFreshTest : public QObject {
    Q_OBJECT

private slots:
    void parseExpireParamBoundaries();
    void parseMissingAndMalformed();
    void parseManifestSlashForm();
    void freshVsSafetyMargin();
    void pruneDropsExpiredThenCapsByNewestWindow();
};

void YtUrlFreshTest::parseExpireParamBoundaries()
{
    // ? marks the first parameter, & separates later ones — both must match.
    const qint64 e1 = yturl::expireEpochFromUrl(
        QStringLiteral("https://gitvd.com/videoplayback?expire=1790000000&ip=1.2.3.4"));
    const qint64 e2 = yturl::expireEpochFromUrl(
        QStringLiteral("https://gitvd.com/videoplayback?ip=1.2.3.4&signature=ABC&expire=1790000123&sig2=x"));
    QVERIFY(e1 == 1790000000LL);
    QVERIFY(e2 == 1790000123LL);

    // Decoy: a parameter whose NAME ends in "expire" must not match — the
    // match point must sit right after ? or &.
    QVERIFY(yturl::expireEpochFromUrl(
               QStringLiteral("https://x/y?eexpire=999&other=1")) == 0);
    // Value must be all digits up to the next separator.
    QVERIFY(yturl::expireEpochFromUrl(
               QStringLiteral("https://x/y?expire=1790abc")) == 0);
    // No query at all, and a query without the parameter.
    QVERIFY(yturl::expireEpochFromUrl(QStringLiteral("https://x/y")) == 0);
    QVERIFY(yturl::expireEpochFromUrl(QStringLiteral("https://x/y?a=1&b=2")) == 0);
}

void YtUrlFreshTest::parseMissingAndMalformed()
{
    // Empty value / bare separator.
    QVERIFY(yturl::expireEpochFromUrl(QStringLiteral("https://x/y?expire=")) == 0);
    QVERIFY(yturl::expireEpochFromUrl(QStringLiteral("https://x/y?expire&&a=1")) == 0);
}

void YtUrlFreshTest::parseManifestSlashForm()
{
    // The yt-dlp HLS-manifest fallback (live rebroadcasts) keeps the expiry in
    // slash-delimited path segments — same epoch, different dialect.
    QVERIFY(yturl::expireEpochFromUrl(
        QStringLiteral("https://manifest.googlevideo.com/api/manifest/hls_playlist/id/abc.1/expire/1793832910/ei/x&sig=ABC")) == 1793832910LL);
    // Same strictness as the query dialect: mixed value rejected, not truncated.
    QVERIFY(yturl::expireEpochFromUrl(QStringLiteral("https://m/x/expire/1790a/y")) == 0);
    // Name must sit right after a delimiter: "xexpire/" must not match.
    QVERIFY(yturl::expireEpochFromUrl(QStringLiteral("https://m/xexpire/1790/y")) == 0);
}

void YtUrlFreshTest::freshVsSafetyMargin()
{
    const qint64 expire = 1000000;
    // Inside the window by exactly the margin: NOT fresh (needs margin of slack).
    QVERIFY(!yturl::urlFresh(expire, expire - 90, 90));
    // One second inside the margin: fresh.
    QVERIFY(yturl::urlFresh(expire, expire - 91, 90));
    // Far past expiry: not fresh.
    QVERIFY(!yturl::urlFresh(expire, expire + 1, 0));
    // Zero (absent) expiry is never fresh, whatever now is.
    QVERIFY(!yturl::urlFresh(0, 0, 0));
    // Zero margin at the exact expiry second: not fresh (window closed).
    QVERIFY(!yturl::urlFresh(expire, expire, 0));
}

void YtUrlFreshTest::pruneDropsExpiredThenCapsByNewestWindow()
{
    const qint64 now = 1000000;
    QJsonObject cache;
    cache.insert(QStringLiteral("a|fmt"), makeEntry(now - 10, QStringLiteral("ga")));   // expired
    cache.insert(QStringLiteral("b|fmt"), makeEntry(now + 500, QStringLiteral("gb")));  // near expiry
    cache.insert(QStringLiteral("c|fmt"), makeEntry(0, QStringLiteral("gc")));          // no expiry -> dead
    cache.insert(QStringLiteral("d|fmt"), QStringLiteral("not an object"));             // corrupt value
    cache.insert(QStringLiteral("e|fmt"), makeEntry(now + 90000, QStringLiteral("ge")));
    cache.insert(QStringLiteral("f|fmt"), makeEntry(now + 80000, QStringLiteral("gf")));

    yturl::pruneUrlCache(cache, now, /*maxEntries=*/ 2);

    // Expired / zero-expiry / corrupt gone; two survivors, the FARTHEST windows
    // (e: +90000, f: +80000) — not the insertion order (b first would lose).
    QCOMPARE(cache.size(), 2);
    QVERIFY(cache.contains(QStringLiteral("e|fmt")));
    QVERIFY(cache.contains(QStringLiteral("f|fmt")));
    QVERIFY(!cache.contains(QStringLiteral("b|fmt")));

    // Cap above survivor count: only the new expired entry drops.
    QJsonObject cache2 = cache; // already the 2 survivors
    cache2.insert(QStringLiteral("g|fmt"), makeEntry(now - 1, QStringLiteral("gg")));
    yturl::pruneUrlCache(cache2, now, 10);
    QCOMPARE(cache2.size(), 2);
}

QTEST_MAIN(YtUrlFreshTest)
#include "YtUrlFreshTest.moc"
