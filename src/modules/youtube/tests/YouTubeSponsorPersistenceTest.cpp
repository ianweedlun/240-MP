// Persistence gate for the bounded YouTube ID-keyed state (MEM-01 follow-up,
// policy spec §6). Seeds oversized / legacy / corrupt on-disk files, constructs
// the REAL backend, and asserts the LOAD path itself enforces the contract:
//   - post-load counts <= cap with segment/timestamp key-set equality,
//   - deterministic survivors (sponsor: newest fetchedAt; known: first cap in
//     file order, then history merge, then trim),
//   - the next save writes a bounded MRU-first file (legacy self-heal),
//   - corrupt/truncated files fail safe to empty, as before,
//   - the debounced sponsor save coalesces a burst and flushes on destruction.
// No network: every answer is seeded on disk or injected through the insert
// protocol, so this runs in the lab and on CI alike.
#include <QtTest/QtTest>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>

#include "../../../util/YtSponsorLru.h"
#include "../../../util/YtSponsorTtl.h"

// Test-only access seam: the contract is about PRIVATE container state (counts
// and key-set equality) and the production API stays unchanged. Same pattern
// as the MEM-01 acceptance harness. All Qt headers are included above, so the
// define only relaxes access specifiers in the backend header — never layout.
#define private public
#include "../YouTubeBackend.h"
#undef private

class YouTubeSponsorPersistenceTest : public QObject {
    Q_OBJECT

private slots:
    void oversizedLegacySponsorFilePrunesToNewestAtLoad();
    void oversizedLegacyKnownFileKeepsFirstCapInFileOrder();
    void staleLegacyEntriesDropFirstOnlyWhenOversized();
    void stamplessLegacyEntriesCompeteAsOldestAndAgeAfterPrune();
    void invalidRowsDuplicatesAndOrphanStampsFailSafe();
    void corruptAndTruncatedFilesFailSafeToEmpty();
    void boundedNewFormatFileLoadsWithoutRewrite();
    void historyMergeThenTrimStaysBounded();
    void debouncedSaveCoalescesAndFlushesOnDestruction();

private:
    static QString id(int i)
    {
        return QStringLiteral("legacy-%1-abcdefghijk").arg(i, 5, 10, QLatin1Char('0'));
    }
    static QString sponsorPath(const QTemporaryDir &dir)
    {
        return dir.path() + QStringLiteral("/youtube_sponsor.json");
    }
    static QString knownPath(const QTemporaryDir &dir)
    {
        return dir.path() + QStringLiteral("/youtube_known_ids.json");
    }
    static QJsonObject entry(const QString &vid, qint64 stamp,
                             const QJsonArray &segments = QJsonArray())
    {
        QJsonObject o;
        o.insert(QStringLiteral("videoId"), vid);
        o.insert(QStringLiteral("segments"), segments);
        o.insert(QStringLiteral("fetchedAt"), double(stamp));
        return o;
    }
    static void writeJson(const QString &path, const QJsonArray &arr)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    }
    static QJsonArray readArray(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return QJsonArray();
        return QJsonDocument::fromJson(f.readAll()).array();
    }
    static QByteArray readBytes(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return QByteArray();
        return f.readAll();
    }
    // I3 at the backend level: segment keys == timestamp keys == index keys,
    // and the index holds no duplicates.
    static void assertCoherent(const YouTubeBackend &b)
    {
        QSet<QString> segKeys, stampKeys;
        for (auto it = b.m_sponsorCache.cbegin(); it != b.m_sponsorCache.cend(); ++it)
            segKeys.insert(it.key());
        for (auto it = b.m_sponsorFetchedAt.cbegin(); it != b.m_sponsorFetchedAt.cend(); ++it)
            stampKeys.insert(it.key());
        const QSet<QString> idxKeys(b.m_sponsorLru.cbegin(), b.m_sponsorLru.cend());
        QCOMPARE(segKeys, stampKeys);
        QCOMPARE(segKeys, idxKeys);
        QCOMPARE(b.m_sponsorLru.size(), b.m_sponsorCache.size());
        QCOMPARE(b.m_sponsorCache.size(), b.m_sponsorFetchedAt.size());
        const QSet<QString> knownIdx(b.m_knownLru.cbegin(), b.m_knownLru.cend());
        QCOMPARE(knownIdx, b.m_knownVideoIds);
        QCOMPARE(b.m_knownLru.size(), b.m_knownVideoIds.size());
    }
};

// I8: a pre-fix 2,000-entry SponsorBlock file (mem01-data-run1 shape: empty
// segment arrays, millisecond stamps, arbitrary QHash-dump file order) must
// load straight into the cap — not "above cap until the first mutation".
void YouTubeSponsorPersistenceTest::oversizedLegacySponsorFilePrunesToNewestAtLoad()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const int cap = YouTubeBackend::kSponsorCacheMaxEntries;
    const int n = 2000;
    const qint64 base = QDateTime::currentMSecsSinceEpoch() - 60000; // all fresh
    QStringList ids;
    QJsonArray arr;
    for (int i = 0; i < n; ++i) {
        ids << id(i);
        arr.append(entry(id(i), base + qint64(i) * 7));
    }
    // Legacy files were QHash dumps: rotate so file order != stamp order and
    // the prune must provably sort by fetchedAt, not by position.
    QJsonArray rotated;
    for (int i = 0; i < n; ++i)
        rotated.append(arr.at((i + 700) % n));
    writeJson(sponsorPath(dir), rotated);
    const qint64 legacySize = QFileInfo(sponsorPath(dir)).size();

    YouTubeBackend backend(QString(), dir.path());
    backend.ensureSponsorCacheLoaded();
    QCOMPARE(backend.m_sponsorCache.size(), cap);
    QCOMPARE(backend.m_sponsorFetchedAt.size(), cap);
    QCOMPARE(backend.m_sponsorLru.size(), cap);
    assertCoherent(backend);
    // Survivors are exactly the newest-cap stamps: ids[1488..1999].
    for (int i = 0; i < n - cap; ++i)
        QVERIFY(!backend.m_sponsorCache.contains(ids.at(i)));
    for (int i = n - cap; i < n; ++i)
        QVERIFY(backend.m_sponsorCache.contains(ids.at(i)));
    // MRU-first order = stamp descending; original stamps preserved exactly.
    QCOMPARE(backend.m_sponsorLru.first(), ids.at(n - 1));
    QCOMPARE(backend.m_sponsorLru.last(), ids.at(n - cap));
    QCOMPARE(backend.m_sponsorFetchedAt.value(ids.at(n - 1)), base + qint64(n - 1) * 7);
    QCOMPARE(backend.m_sponsorFetchedAt.value(ids.at(n - cap)), base + qint64(n - cap) * 7);

    // The legacy file self-healed into the bounded form on load.
    const QJsonArray disk = readArray(sponsorPath(dir));
    QCOMPARE(disk.size(), cap);
    QCOMPARE(disk.at(0).toObject().value(QStringLiteral("videoId")).toString(), ids.at(n - 1));
    for (int i = 1; i < disk.size(); ++i) {
        const qint64 prev = qint64(disk.at(i - 1).toObject().value(QStringLiteral("fetchedAt")).toDouble());
        const qint64 cur = qint64(disk.at(i).toObject().value(QStringLiteral("fetchedAt")).toDouble());
        QVERIFY(prev >= cur); // MRU-first on disk
    }
    QVERIFY(QFileInfo(sponsorPath(dir)).size() < legacySize / 2);

    // A reload of the bounded file is stable: same state, no further rewrite.
    const QByteArray boundedBytes = readBytes(sponsorPath(dir));
    YouTubeBackend reloaded(QString(), dir.path());
    reloaded.ensureSponsorCacheLoaded();
    QCOMPARE(reloaded.m_sponsorCache.size(), cap);
    assertCoherent(reloaded);
    QCOMPARE(reloaded.m_sponsorLru.first(), ids.at(n - 1));
    QCOMPARE(readBytes(sponsorPath(dir)), boundedBytes);
}

// I8 known side: the legacy string array keeps its FIRST cap ids in file
// order, and the next mutation-driven save stays bounded.
void YouTubeSponsorPersistenceTest::oversizedLegacyKnownFileKeepsFirstCapInFileOrder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const int cap = YouTubeBackend::kKnownVideoIdMaxEntries;
    const int n = 2000;
    QStringList ids;
    QJsonArray arr;
    for (int i = 0; i < n; ++i) {
        ids << id(i);
        arr.append(id(i));
    }
    writeJson(knownPath(dir), arr);

    YouTubeBackend backend(QString(), dir.path());
    backend.ensureKnownIdsLoaded();
    QCOMPARE(backend.m_knownVideoIds.size(), cap);
    QCOMPARE(backend.m_knownLru.size(), cap);
    assertCoherent(backend);
    for (int i = 0; i < cap; ++i) {
        QVERIFY(backend.m_knownVideoIds.contains(ids.at(i)));
        QCOMPARE(backend.m_knownLru.at(i), ids.at(i)); // file order preserved
    }
    for (int i = cap; i < n; ++i)
        QVERIFY(!backend.m_knownVideoIds.contains(ids.at(i)));
    // Bounded rewrite happened at load.
    const QJsonArray disk = readArray(knownPath(dir));
    QCOMPARE(disk.size(), cap);
    QCOMPARE(disk.at(0).toString(), ids.at(0));
    QCOMPARE(disk.at(cap - 1).toString(), ids.at(cap - 1));

    // The NEXT save (a real mutation) is bounded too: the new id lands at the
    // MRU front and the file-order tail is evicted.
    const QString fresh = QStringLiteral("brand-new-id");
    backend.noteKnownVideoIds({fresh});
    QCOMPARE(backend.m_knownVideoIds.size(), cap);
    QCOMPARE(backend.m_knownLru.first(), fresh);
    QVERIFY(!backend.m_knownVideoIds.contains(ids.at(cap - 1)));
    assertCoherent(backend);
    const QJsonArray disk2 = readArray(knownPath(dir));
    QCOMPARE(disk2.size(), cap);
    QCOMPARE(disk2.at(0).toString(), fresh);
}

// Spec §6: stale entries drop FIRST, and only when the file is oversized.
// Under cap, stale residents keep serving playback until their refetch lands.
void YouTubeSponsorPersistenceTest::staleLegacyEntriesDropFirstOnlyWhenOversized()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 staleStamp = now - ytsb::kSuccessTtlMs - 1000;
    const int cap = YouTubeBackend::kSponsorCacheMaxEntries;
    {
        // 600 entries, oldest 200 stale: the stale drop alone lands under cap,
        // so ALL 400 fresh entries survive (no newest-cut).
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QJsonArray arr;
        for (int i = 0; i < 600; ++i) {
            const qint64 stamp = i < 200 ? staleStamp + i : now - qint64(600 - i) * 1000;
            arr.append(entry(id(i), stamp));
        }
        writeJson(sponsorPath(dir), arr);
        YouTubeBackend backend(QString(), dir.path());
        backend.ensureSponsorCacheLoaded();
        QCOMPARE(backend.m_sponsorCache.size(), 400);
        assertCoherent(backend);
        for (int i = 0; i < 200; ++i)
            QVERIFY(!backend.m_sponsorCache.contains(id(i)));
        for (int i = 200; i < 600; ++i)
            QVERIFY(backend.m_sponsorCache.contains(id(i)));
        QCOMPARE(readArray(sponsorPath(dir)).size(), 400);
    }
    {
        // 100 entries (under cap), 50 stale: NOTHING is pruned and the file is
        // not rewritten — serve-until-refetch semantics are preserved.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QJsonArray arr;
        for (int i = 0; i < 100; ++i)
            arr.append(entry(id(i), i < 50 ? staleStamp + i : now - 1000));
        writeJson(sponsorPath(dir), arr);
        const QByteArray before = readBytes(sponsorPath(dir));
        YouTubeBackend backend(QString(), dir.path());
        backend.ensureSponsorCacheLoaded();
        QCOMPARE(backend.m_sponsorCache.size(), 100);
        QVERIFY(backend.m_sponsorCache.contains(id(0))); // stale but resident
        assertCoherent(backend);
        QCOMPARE(readBytes(sponsorPath(dir)), before);
    }
}

// Pre-stamp legacy entries (no fetchedAt field) must NOT be re-stamped to now
// before the ordering decision: they compete as oldest, and only survivors
// get the gradual-aging stamp afterwards.
void YouTubeSponsorPersistenceTest::stamplessLegacyEntriesCompeteAsOldestAndAgeAfterPrune()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int cap = YouTubeBackend::kSponsorCacheMaxEntries;
    {
        // Mixed oversized file: 200 stampless + 400 stamped fresh = 600.
        // Survivors: all 400 stamped + the FIRST 112 stampless in file order.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QJsonArray arr;
        QStringList stampless, stamped;
        for (int i = 0; i < 200; ++i) {
            const QString vid = QStringLiteral("nostamp-%1").arg(i, 3, 10, QLatin1Char('0'));
            stampless << vid;
            QJsonObject o;
            o.insert(QStringLiteral("videoId"), vid);
            o.insert(QStringLiteral("segments"), QJsonArray()); // pre-stamp format
            arr.append(o);
        }
        for (int i = 0; i < 400; ++i) {
            const QString vid = QStringLiteral("stamped-%1").arg(i, 3, 10, QLatin1Char('0'));
            stamped << vid;
            arr.append(entry(vid, now - 100000 + qint64(i) * 10));
        }
        writeJson(sponsorPath(dir), arr);
        YouTubeBackend backend(QString(), dir.path());
        backend.ensureSponsorCacheLoaded();
        QCOMPARE(backend.m_sponsorCache.size(), cap);
        assertCoherent(backend);
        for (int i = 0; i < 400; ++i) {
            QVERIFY(backend.m_sponsorCache.contains(stamped.at(i)));
            // Original stamps preserved EXACTLY — never re-stamped to now.
            QCOMPARE(backend.m_sponsorFetchedAt.value(stamped.at(i)), now - 100000 + qint64(i) * 10);
        }
        for (int i = 0; i < 112; ++i) {
            QVERIFY(backend.m_sponsorCache.contains(stampless.at(i)));
            QVERIFY(backend.m_sponsorFetchedAt.value(stampless.at(i)) > 0); // aged after prune
        }
        for (int i = 112; i < 200; ++i)
            QVERIFY(!backend.m_sponsorCache.contains(stampless.at(i)));
        // MRU front = newest stamped; stampless survivors hold the tail.
        QCOMPARE(backend.m_sponsorLru.first(), stamped.at(399));
        QCOMPARE(backend.m_sponsorLru.at(400), stampless.at(0));
        QCOMPARE(backend.m_sponsorLru.last(), stampless.at(111));
        const QJsonArray disk = readArray(sponsorPath(dir));
        QCOMPARE(disk.size(), cap);
        for (const QJsonValue &v : disk)
            QVERIFY(v.toObject().value(QStringLiteral("fetchedAt")).toDouble() > 0);
    }
    {
        // Fully pre-stamp oversized file (the real pre-682ca03 format): every
        // stamp ties at 0, so the FIRST cap in file order survive and age.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QJsonArray arr;
        for (int i = 0; i < 600; ++i) {
            QJsonObject o;
            o.insert(QStringLiteral("videoId"), id(i));
            o.insert(QStringLiteral("segments"), QJsonArray());
            arr.append(o);
        }
        writeJson(sponsorPath(dir), arr);
        YouTubeBackend backend(QString(), dir.path());
        backend.ensureSponsorCacheLoaded();
        QCOMPARE(backend.m_sponsorCache.size(), cap);
        assertCoherent(backend);
        for (int i = 0; i < cap; ++i) {
            QVERIFY(backend.m_sponsorCache.contains(id(i)));
            QCOMPARE(backend.m_sponsorLru.at(i), id(i));
            QVERIFY(backend.m_sponsorFetchedAt.value(id(i)) > 0);
        }
        QVERIFY(!backend.m_sponsorCache.contains(id(599)));
        QCOMPARE(readArray(sponsorPath(dir)).size(), cap);
    }
}

// Atomic entries: a stamp-only row (fetchedAt, no segments) drops WHOLE — no
// orphan timestamp. Duplicates keep the first occurrence's payload and stamp.
void YouTubeSponsorPersistenceTest::invalidRowsDuplicatesAndOrphanStampsFailSafe()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const qint64 stamp = QDateTime::currentMSecsSinceEpoch() - 5000;
    QJsonArray segs;
    QJsonObject row;
    row.insert(QStringLiteral("category"), QStringLiteral("sponsor"));
    row.insert(QStringLiteral("segmentType"), QStringLiteral("skip"));
    row.insert(QStringLiteral("segment"), QJsonArray{1.0, 2.0});
    segs.append(row);

    QJsonArray arr;
    arr.append(entry(QStringLiteral("good-1"), stamp));            // valid, empty segments
    QJsonObject orphan;                                            // stamp-only row
    orphan.insert(QStringLiteral("videoId"), QStringLiteral("orphan-1"));
    orphan.insert(QStringLiteral("fetchedAt"), double(stamp));
    arr.append(orphan);
    arr.append(entry(QStringLiteral("nostamp-1"), 0, segs));       // segments, no stamp
    arr.append(entry(QString(), stamp));                           // empty videoId
    arr.append(entry(QStringLiteral("good-1"), stamp + 999, segs)); // duplicate: first wins
    arr.append(QJsonValue(QStringLiteral("not-an-object")));       // garbage row
    writeJson(sponsorPath(dir), arr);

    YouTubeBackend backend(QString(), dir.path());
    backend.ensureSponsorCacheLoaded();
    QCOMPARE(backend.m_sponsorCache.size(), 2);
    assertCoherent(backend);
    QVERIFY(backend.m_sponsorCache.contains(QStringLiteral("good-1")));
    QVERIFY(backend.m_sponsorCache.contains(QStringLiteral("nostamp-1")));
    QVERIFY(!backend.m_sponsorFetchedAt.contains(QStringLiteral("orphan-1"))); // no orphan
    QVERIFY(!backend.m_sponsorCache.contains(QStringLiteral("orphan-1")));
    // First occurrence wins whole: original stamp AND empty payload.
    QCOMPARE(backend.m_sponsorFetchedAt.value(QStringLiteral("good-1")), stamp);
    QCOMPARE(backend.m_sponsorCache.value(QStringLiteral("good-1")).size(), 0);
    // The segments-only entry survives and ages gradually.
    QCOMPARE(backend.m_sponsorCache.value(QStringLiteral("nostamp-1")).size(), 1);
    QVERIFY(backend.m_sponsorFetchedAt.value(QStringLiteral("nostamp-1")) > 0);
    // Bounded self-heal rewrite dropped the invalid rows.
    const QJsonArray disk = readArray(sponsorPath(dir));
    QCOMPARE(disk.size(), 2);
}

// Corrupt / truncated / empty files fail safe to empty state (as today), and
// the backend stays fully usable afterwards.
void YouTubeSponsorPersistenceTest::corruptAndTruncatedFilesFailSafeToEmpty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QJsonArray arr;
    for (int i = 0; i < 3; ++i)
        arr.append(entry(id(i), QDateTime::currentMSecsSinceEpoch()));
    const QByteArray full = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    {
        QFile f(sponsorPath(dir));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(full.left(full.size() / 2)); // truncated mid-array
    }
    {
        QFile f(knownPath(dir));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not json at all {{{");
    }
    YouTubeBackend backend(QString(), dir.path());
    backend.ensureSponsorCacheLoaded();
    backend.ensureKnownIdsLoaded();
    QVERIFY(backend.m_sponsorCache.isEmpty());
    QVERIFY(backend.m_sponsorFetchedAt.isEmpty());
    QVERIFY(backend.m_knownVideoIds.isEmpty());
    assertCoherent(backend);
    // Still usable: a normal mutation writes a valid bounded file.
    backend.noteKnownVideoIds({QStringLiteral("a"), QStringLiteral("b")});
    QCOMPARE(backend.m_knownVideoIds.size(), 2);
    const QJsonArray disk = readArray(knownPath(dir));
    QCOMPARE(disk.size(), 2);
    QCOMPARE(disk.at(0).toString(), QStringLiteral("b")); // MRU-first
    // Zero-byte files are equally safe.
    QTemporaryDir dir2;
    QVERIFY(dir2.isValid());
    QVERIFY(QFile(sponsorPath(dir2)).open(QIODevice::WriteOnly));
    QVERIFY(QFile(knownPath(dir2)).open(QIODevice::WriteOnly));
    YouTubeBackend backend2(QString(), dir2.path());
    backend2.ensureSponsorCacheLoaded();
    backend2.ensureKnownIdsLoaded();
    QVERIFY(backend2.m_sponsorCache.isEmpty());
    QVERIFY(backend2.m_knownVideoIds.isEmpty());
    assertCoherent(backend2);
}

// A new-format bounded file (<= cap, MRU-first) loads in file order with no
// prune and NO rewrite — the reload path is read-only for healthy files.
void YouTubeSponsorPersistenceTest::boundedNewFormatFileLoadsWithoutRewrite()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const int cap = YouTubeBackend::kSponsorCacheMaxEntries;
    const qint64 base = QDateTime::currentMSecsSinceEpoch() - 10000;
    QJsonArray sarr;
    QJsonArray karr;
    for (int i = 0; i < cap; ++i) {
        sarr.append(entry(id(i), base + qint64(cap - i) * 10)); // MRU-first
        karr.append(id(i));
    }
    writeJson(sponsorPath(dir), sarr);
    writeJson(knownPath(dir), karr);
    const QByteArray sBefore = readBytes(sponsorPath(dir));
    const QByteArray kBefore = readBytes(knownPath(dir));

    YouTubeBackend backend(QString(), dir.path());
    backend.ensureSponsorCacheLoaded();
    backend.ensureKnownIdsLoaded();
    QCOMPARE(backend.m_sponsorCache.size(), cap);
    QCOMPARE(backend.m_knownVideoIds.size(), cap);
    assertCoherent(backend);
    QCOMPARE(backend.m_sponsorLru.first(), id(0));
    QCOMPARE(backend.m_sponsorLru.last(), id(cap - 1));
    QCOMPARE(backend.m_sponsorFetchedAt.value(id(0)), base + qint64(cap) * 10);
    QCOMPARE(backend.m_knownLru.first(), id(0));
    QCOMPARE(readBytes(sponsorPath(dir)), sBefore);
    QCOMPARE(readBytes(knownPath(dir)), kBefore);
}

// Known load = file (first cap) -> history merge -> final trim. The merge can
// never push the pool above cap, and file ids keep their front positions.
void YouTubeSponsorPersistenceTest::historyMergeThenTrimStaysBounded()
{
    const int cap = YouTubeBackend::kKnownVideoIdMaxEntries;
    const auto writeHistory = [](const QTemporaryDir &dir, const QStringList &keys) {
        QJsonObject o;
        for (const QString &k : keys)
            o.insert(k, QJsonObject());
        QFile f(dir.path() + QStringLiteral("/youtube_history.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    };
    {
        // File already at cap + 20 history keys (10 overlapping): the pool
        // stays at cap, every file id survives, the file is not rewritten.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QJsonArray karr;
        for (int i = 0; i < cap; ++i)
            karr.append(id(i));
        writeJson(knownPath(dir), karr);
        QStringList hist;
        for (int i = 0; i < 10; ++i)
            hist << id(i); // overlap
        for (int i = 0; i < 10; ++i)
            hist << QStringLiteral("hist-%1").arg(i);
        writeHistory(dir, hist);
        const QByteArray before = readBytes(knownPath(dir));
        YouTubeBackend backend(QString(), dir.path());
        backend.ensureKnownIdsLoaded();
        QCOMPARE(backend.m_knownVideoIds.size(), cap);
        assertCoherent(backend);
        for (int i = 0; i < cap; ++i)
            QCOMPARE(backend.m_knownLru.at(i), id(i));
        QVERIFY(!backend.m_knownVideoIds.contains(QStringLiteral("hist-0")));
        QCOMPARE(readBytes(knownPath(dir)), before);
    }
    {
        // File at cap-10 + 20 NEW history keys: merge appends, final trim keeps
        // the first 10 history keys (QJsonObject iterates in key order).
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QJsonArray karr;
        for (int i = 0; i < cap - 10; ++i)
            karr.append(id(i));
        writeJson(knownPath(dir), karr);
        QStringList hist;
        for (int i = 0; i < 20; ++i)
            hist << QStringLiteral("h-%1").arg(i, 2, 10, QLatin1Char('0'));
        writeHistory(dir, hist);
        YouTubeBackend backend(QString(), dir.path());
        backend.ensureKnownIdsLoaded();
        QCOMPARE(backend.m_knownVideoIds.size(), cap);
        assertCoherent(backend);
        for (int i = 0; i < cap - 10; ++i)
            QCOMPARE(backend.m_knownLru.at(i), id(i));
        QCOMPARE(backend.m_knownLru.at(cap - 10), QStringLiteral("h-00"));
        QCOMPARE(backend.m_knownLru.last(), QStringLiteral("h-09"));
        QVERIFY(!backend.m_knownVideoIds.contains(QStringLiteral("h-10")));
    }
}

// The optional spec §6 debounce: a burst of dirty marks coalesces into ONE
// bounded write, and destruction inside the interval flushes so no confirmed
// window is lost. Driven through the insert protocol — no network needed.
void YouTubeSponsorPersistenceTest::debouncedSaveCoalescesAndFlushesOnDestruction()
{
    const auto seedWindow = [](YouTubeBackend &b, const QString &vid) {
        // Mirrors the success chokepoint: both maps + index touch.
        b.m_sponsorCache.insert(vid, QVector<yt::Segment>{});
        b.m_sponsorFetchedAt.insert(vid, QDateTime::currentMSecsSinceEpoch());
        ytlru::touch(b.m_sponsorLru, vid);
    };
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        YouTubeBackend backend(QString(), dir.path());
        seedWindow(backend, QStringLiteral("burst-1"));
        backend.markSponsorCacheDirty();
        QTest::qWait(50);
        QVERIFY(!QFile::exists(sponsorPath(dir))); // inside the interval: no write
        for (int i = 0; i < 4; ++i) {
            backend.markSponsorCacheDirty(); // restart slides the write along
            QTest::qWait(40);
        }
        QVERIFY(!QFile::exists(sponsorPath(dir))); // burst coalesced, still pending
        QTest::qWait(700); // past the interval since the last mark
        QVERIFY(QFile::exists(sponsorPath(dir)));
        QCOMPARE(readArray(sponsorPath(dir)).size(), 1);
        const QDateTime mtime = QFileInfo(sponsorPath(dir)).lastModified();
        QTest::qWait(700);
        QCOMPARE(QFileInfo(sponsorPath(dir)).lastModified(), mtime); // ONE write, no repeats
    }
    {
        // Destruction inside the debounce interval flushes the pending save.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            YouTubeBackend backend(QString(), dir.path());
            seedWindow(backend, QStringLiteral("exit-1"));
            backend.markSponsorCacheDirty();
            QVERIFY(!QFile::exists(sponsorPath(dir)));
        } // ~YouTubeBackend flushes here
        QVERIFY(QFile::exists(sponsorPath(dir)));
        const QJsonArray disk = readArray(sponsorPath(dir));
        QCOMPARE(disk.size(), 1);
        QCOMPARE(disk.at(0).toObject().value(QStringLiteral("videoId")).toString(),
                 QStringLiteral("exit-1"));
    }
    {
        // A clean backend writes nothing on destruction.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            YouTubeBackend backend(QString(), dir.path());
            backend.ensureSponsorCacheLoaded();
        }
        QVERIFY(!QFile::exists(sponsorPath(dir)));
    }
}

QTEST_GUILESS_MAIN(YouTubeSponsorPersistenceTest)
#include "YouTubeSponsorPersistenceTest.moc"