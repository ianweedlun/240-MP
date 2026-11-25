#include "../ThumbnailManifestCache.h"

#include <QtTest/QtTest>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QFile>

#include <algorithm>
#include <vector>

// RETRO-THUMB-CACHE: cache semantics are exercised against the real helper,
// not a source-text snapshot. A deterministic key provider lets mtime-only
// and size-only invalidation be proven without sleeping on filesystem clocks.
class ThumbnailManifestCacheTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;

    QString makeDir(const QString &name) {
        const QString path = m_tmp.path() + QStringLiteral("/") + name;
        QDir().mkpath(path);
        return path;
    }

    void writePng(const QString &dir, const QString &base) {
        QFile file(dir + QStringLiteral("/") + base + QStringLiteral(".png"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
    }

    struct KeyState { qint64 mtime = 1000; qint64 size = 2000; };

    static double medianMs(std::vector<qint64> ns) {
        std::nth_element(ns.begin(), ns.begin() + ns.size() / 2, ns.end());
        return static_cast<double>(ns[ns.size() / 2]) / 1e6;
    }

private slots:
    void hitsDoNotRelist() {
        const QString dir = makeDir(QStringLiteral("hits"));
        writePng(dir, QStringLiteral("Game"));

        retro::ThumbnailManifestCache cache(8);
        KeyState key;
        cache.setKeyProvider([&key](const QString &) { return qMakePair(key.mtime, key.size); });

        QCOMPARE(cache.resolve(dir, QStringLiteral("Game"), QStringLiteral("Game")),
                 dir + QStringLiteral("/Game.png"));
        QCOMPARE(cache.scanCount(), 1);
        QCOMPARE(cache.missCount(), 1);

        for (int i = 0; i < 20; ++i)
            QCOMPARE(cache.resolve(dir, QStringLiteral("Game"), QStringLiteral("Game")),
                     dir + QStringLiteral("/Game.png"));
        QCOMPARE(cache.scanCount(), 1);              // no entryInfoList on a hit
        QCOMPARE(cache.hitCount(), 20);
    }

    void mtimeChangeInvalidates() {
        const QString dir = makeDir(QStringLiteral("mtime"));
        writePng(dir, QStringLiteral("Game"));

        retro::ThumbnailManifestCache cache(8);
        KeyState key;
        cache.setKeyProvider([&key](const QString &) { return qMakePair(key.mtime, key.size); });

        QCOMPARE(cache.resolve(dir, QStringLiteral("Game"), QStringLiteral("Game")),
                 dir + QStringLiteral("/Game.png"));
        QCOMPARE(cache.scanCount(), 1);

        key.mtime += 1;                              // size deliberately unchanged
        QCOMPARE(cache.resolve(dir, QStringLiteral("Game"), QStringLiteral("Game")),
                 dir + QStringLiteral("/Game.png"));
        QCOMPARE(cache.scanCount(), 2);
        QCOMPARE(cache.missCount(), 2);
    }

    void sizeChangeInvalidates() {
        const QString dir = makeDir(QStringLiteral("size"));
        writePng(dir, QStringLiteral("Game"));

        retro::ThumbnailManifestCache cache(8);
        KeyState key;
        cache.setKeyProvider([&key](const QString &) { return qMakePair(key.mtime, key.size); });

        QCOMPARE(cache.resolve(dir, QStringLiteral("Game"), QStringLiteral("Game")),
                 dir + QStringLiteral("/Game.png"));
        QCOMPARE(cache.scanCount(), 1);

        key.size += 4096;                            // mtime deliberately unchanged
        QCOMPARE(cache.resolve(dir, QStringLiteral("Game"), QStringLiteral("Game")),
                 dir + QStringLiteral("/Game.png"));
        QCOMPARE(cache.scanCount(), 2);
    }

    void missingDirIsNotCached() {
        retro::ThumbnailManifestCache cache(8);
        const QString missing = m_tmp.path() + QStringLiteral("/does-not-exist");
        QVERIFY(cache.resolve(missing, QStringLiteral("Game"), QStringLiteral("Game")).isEmpty());
        QCOMPARE(cache.scanCount(), 0);
        QCOMPARE(cache.size(), 0);

        // Created later: the first resolve after creation must see it.
        makeDir(QStringLiteral("does-not-exist"));
        writePng(missing, QStringLiteral("Game"));
        QCOMPARE(cache.resolve(missing, QStringLiteral("Game"), QStringLiteral("Game")),
                 missing + QStringLiteral("/Game.png"));
        QCOMPARE(cache.scanCount(), 1);
    }

    void boundsEvictLru() {
        retro::ThumbnailManifestCache cache(3);
        QStringList dirs;
        for (int i = 0; i < 5; ++i) {
            const QString dir = makeDir(QStringLiteral("bounded-%1").arg(i));
            writePng(dir, QStringLiteral("g%1").arg(i));
            dirs << dir;
            QCOMPARE(cache.resolve(dir, QStringLiteral("g%1").arg(i),
                                   QStringLiteral("g%1").arg(i)),
                     dir + QStringLiteral("/g%1.png").arg(i));
        }
        QCOMPARE(cache.scanCount(), 5);
        QCOMPARE(cache.size(), 3);                   // cap respected

        // Touch the two newest; the oldest two are the eviction candidates.
        QCOMPARE(cache.resolve(dirs.at(4), QStringLiteral("g4"), QStringLiteral("g4")),
                 dirs.at(4) + QStringLiteral("/g4.png"));
        QCOMPARE(cache.resolve(dirs.at(3), QStringLiteral("g3"), QStringLiteral("g3")),
                 dirs.at(3) + QStringLiteral("/g3.png"));
        QCOMPARE(cache.scanCount(), 5);              // still resident

        // LRU tail (dirs 0/1) was evicted: a fresh listing is required.
        QCOMPARE(cache.resolve(dirs.at(0), QStringLiteral("g0"), QStringLiteral("g0")),
                 dirs.at(0) + QStringLiteral("/g0.png"));
        QCOMPARE(cache.scanCount(), 6);
        QCOMPARE(cache.size(), 3);
    }

    // The prewarm path builds the index on a worker thread via buildManifest()
    // and installs on the GUI thread. Prove the pure builder keeps the same
    // .png-over-.jpg and first-match tiers, and that install rejects work
    // captured under a stale generation or an obsolete directory key.
    void asyncBuildKeepsTiersAndRejectsStaleWork() {
        const QString dir = makeDir(QStringLiteral("async-build"));
        writePng(dir, QStringLiteral("Exact"));
        {   QFile jpg(dir + QStringLiteral("/Exact.jpg"));
            QVERIFY(jpg.open(QIODevice::WriteOnly)); jpg.close(); }
        writePng(dir, QStringLiteral("lower"));

        const QString tie = makeDir(QStringLiteral("async-tie"));
        writePng(tie, QStringLiteral("Case"));
        writePng(tie, QStringLiteral("CASE"));

        retro::ThumbnailManifestCache cache(8);
        KeyState key;
        cache.setKeyProvider([&key](const QString &) { return qMakePair(key.mtime, key.size); });

        const auto k = cache.keyFor(dir);
        const auto built = retro::ThumbnailManifestCache::buildManifest(dir, k);
        QVERIFY(built.valid);
        QCOMPARE(built.exact.value(QStringLiteral("Exact")), dir + QStringLiteral("/Exact.png"));
        QCOMPARE(built.exact.value(QStringLiteral("lower")), dir + QStringLiteral("/lower.png"));

        const auto tieBuilt = retro::ThumbnailManifestCache::buildManifest(tie, cache.keyFor(tie));
        const QString expectedTie = QDir(tie).entryInfoList(
            { QStringLiteral("*.png") }, QDir::Files).first().absoluteFilePath();
        QCOMPARE(tieBuilt.folded.value(QStringLiteral("case")), expectedTie);

        // Stale generation from the future (a clear raced the build): drop.
        const quint64 capturedGen = cache.generation();
        const quint64 stale = capturedGen + 1;
        QVERIFY(!cache.installPrewarmed(dir, k, stale, built));
        QCOMPARE(cache.size(), 0);
        QCOMPARE(cache.scanCount(), 0);

        // Current generation installs once; a warm resolve adds no listing.
        QVERIFY(cache.installPrewarmed(dir, k, capturedGen, built));
        QCOMPARE(cache.size(), 1);
        QCOMPARE(cache.scanCount(), 1);
        QCOMPARE(cache.resolve(dir, QStringLiteral("Exact"), QStringLiteral("Exact")),
                 dir + QStringLiteral("/Exact.png"));
        QCOMPARE(cache.scanCount(), 1);

        // A clear bumps the generation; the job captured under the previous
        // generation is now stale and must not install into the fresh cache.
        cache.clear();
        QVERIFY(!cache.installPrewarmed(dir, k, capturedGen, built));
        QVERIFY(!cache.installPrewarmed(dir, k, stale - 2, built));
        QCOMPARE(cache.size(), 0);
    }

    void asyncInstallRejectsChangedDirectoryKey() {
        const QString dir = makeDir(QStringLiteral("async-key"));
        writePng(dir, QStringLiteral("Game"));

        retro::ThumbnailManifestCache cache(8);
        KeyState key;
        cache.setKeyProvider([&key](const QString &) { return qMakePair(key.mtime, key.size); });

        const auto oldKey = cache.keyFor(dir);
        const auto built = retro::ThumbnailManifestCache::buildManifest(dir, oldKey);
        key.size += 1;                                // dir changed during the build
        const auto newKey = cache.keyFor(dir);
        QVERIFY(newKey != oldKey);

        // Old-key work must not be installed even with a current generation.
        QVERIFY(!cache.installPrewarmed(dir, oldKey, cache.generation(), built));
        QCOMPARE(cache.size(), 0);
        QCOMPARE(cache.scanCount(), 0);

        // A fresh build with the current key installs and resolves correctly.
        const auto rebuilt = retro::ThumbnailManifestCache::buildManifest(dir, newKey);
        QVERIFY(cache.installPrewarmed(dir, newKey, cache.generation(), rebuilt));
        QCOMPARE(cache.resolve(dir, QStringLiteral("Game"), QStringLiteral("Game")),
                 dir + QStringLiteral("/Game.png"));
    }

    void matchTiersPreserveShippedBehavior() {
        const QString dir = makeDir(QStringLiteral("tiers"));
        writePng(dir, QStringLiteral("Exact"));
        {   QFile jpg(dir + QStringLiteral("/Exact.jpg"));
            QVERIFY(jpg.open(QIODevice::WriteOnly)); jpg.close(); }
        writePng(dir, QStringLiteral("lower"));
        writePng(dir, QStringLiteral("F_Z Quest [T_S]"));

        retro::ThumbnailManifestCache cache(8);

        // Exact tier prefers .png over .jpg, as the shipped two-extension
        // lookup did.
        QCOMPARE(cache.resolve(dir, QStringLiteral("Exact"), QStringLiteral("Exact")),
                 dir + QStringLiteral("/Exact.png"));

        // Case-insensitive fallback still finds a differently-cased file.
        QCOMPARE(cache.resolve(dir, QStringLiteral("LOWER"), QStringLiteral("LOWER")),
                 dir + QStringLiteral("/lower.png"));

        // Sanitized tier: the caller supplies the mangled label; a direct hit
        // on the DB name must resolve.
        QCOMPARE(cache.resolve(dir, QStringLiteral("F/Z Quest [T/S]"),
                               QStringLiteral("F_Z Quest [T_S]")),
                 dir + QStringLiteral("/F_Z Quest [T_S].png"));

        // First-match tie-break: two case-variant files must return the same
        // one QDir lists first, never the last one inserted into the manifest.
        const QString tie = makeDir(QStringLiteral("tie"));
        writePng(tie, QStringLiteral("Case"));
        writePng(tie, QStringLiteral("CASE"));
        const QString expected = QDir(tie).entryInfoList(
            { QStringLiteral("*.png") }, QDir::Files).first().absoluteFilePath();
        QCOMPARE(cache.resolve(tie, QStringLiteral("case"), QStringLiteral("case")),
                 expected);
    }

    void warmLookupOverheadUnderOneMs() {
        const QString big = makeDir(QStringLiteral("timing-big"));
        const QString empty = makeDir(QStringLiteral("timing-empty"));
        for (int i = 0; i < 200; ++i)
            writePng(big, QStringLiteral("filler-%1").arg(i));

        retro::ThumbnailManifestCache cache(8);
        cache.resolve(big, QStringLiteral("nope"), QStringLiteral("nope"));
        cache.resolve(empty, QStringLiteral("nope"), QStringLiteral("nope"));
        const int scansAfterWarm = cache.scanCount();

        std::vector<qint64> bigNs, emptyNs;
        QElapsedTimer timer;
        for (int i = 0; i < 200; ++i) {
            timer.start();
            cache.resolve(big, QStringLiteral("nope"), QStringLiteral("nope"));
            bigNs.push_back(timer.nsecsElapsed());
            timer.start();
            cache.resolve(empty, QStringLiteral("nope"), QStringLiteral("nope"));
            emptyNs.push_back(timer.nsecsElapsed());
        }

        QCOMPARE(cache.scanCount(), scansAfterWarm);   // warm cache never relists
        const double bigMs = medianMs(bigNs);
        const double emptyMs = medianMs(emptyNs);
        const double overheadMs = bigMs - emptyMs;
        QVERIFY2(qAbs(overheadMs) < 1.0,
                 qPrintable(QStringLiteral("warm cache overhead %1 ms (big %2 / empty %3)")
                            .arg(overheadMs, 0, 'f', 4).arg(bigMs, 0, 'f', 4)
                            .arg(emptyMs, 0, 'f', 4)));
    }
};

QTEST_MAIN(ThumbnailManifestCacheTest)
#include "ThumbnailManifestCacheTest.moc"
