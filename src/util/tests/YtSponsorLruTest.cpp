#include <QtTest/QtTest>

#include "../YtSponsorLru.h"

// Bounded eviction: the ID-keyed SponsorBlock/known-id state must
// plateau at a hard cap, evict the least-recently-used tail, keep hot entries
// alive under pressure, and co-evict segments+timestamps so their key sets
// never diverge. Pure container math — no network, no backend instance.

class YtSponsorLruTest : public QObject {
    Q_OBJECT

private slots:
    void touchMovesHitToFrontAndDedupes();
    void appendIfAbsentKeepsOrderAndFirstOccurrence();
    void sponsorPlateauAtCapWithKeySetEquality();
    void sponsorEvictsLruTailInOrder();
    void hotSponsorEntrySurvivesRepeatedPressure();
    void knownPlateauAtCapAndHotIdSurvives();
    void evictedSponsorIdIsCleanlyRefetchable();
    void capFailuresDropsOldestDeterministically();
    void mixedOpsNeverOrphanATimestamp();
    void pruneSponsorOnLoadKeepsNewestStampsDeterministically();
    void pruneSponsorOnLoadDropsStaleFirstAndNeverRestamps();
    void dropOrphanStampsRemovesOnlyOrphans();
};

// The backend's insert protocol, mirrored exactly: insert into both maps,
// touch the index, then co-evict to the cap.
static void sponsorInsert(QStringList &lru, QHash<QString, QVector<int>> &segs,
                          QHash<QString, qint64> &stamps, const QString &id,
                          qint64 stamp, int cap)
{
    segs.insert(id, {1});
    stamps.insert(id, stamp);
    ytlru::touch(lru, id);
    ytlru::evictSponsorToCap(lru, segs, stamps, cap);
}

template <typename V>
static QSet<QString> keySetOf(const QHash<QString, V> &map)
{
    QSet<QString> keys;
    for (auto it = map.cbegin(); it != map.cend(); ++it)
        keys.insert(it.key());
    return keys;
}

// Index set == segment keys == timestamp keys, index size == map size (which
// together also prove the index holds no duplicates).
static void assertCoherent(const QStringList &lru,
                           const QHash<QString, QVector<int>> &segs,
                           const QHash<QString, qint64> &stamps)
{
    const QSet<QString> idx(lru.cbegin(), lru.cend());
    QCOMPARE(idx, keySetOf(segs));
    QCOMPARE(idx, keySetOf(stamps));
    QCOMPARE(lru.size(), segs.size());
    QCOMPARE(segs.size(), stamps.size());
}

void YtSponsorLruTest::touchMovesHitToFrontAndDedupes()
{
    QStringList lru{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
    ytlru::touch(lru, QStringLiteral("b")); // a hit, not an insert
    QCOMPARE(lru, QStringList({QStringLiteral("b"), QStringLiteral("a"), QStringLiteral("c")}));
    ytlru::touch(lru, QStringLiteral("b")); // repeat hit: no duplicate
    QCOMPARE(lru.size(), 3);
    ytlru::touch(lru, QStringLiteral("new")); // absent key lands at the front
    QCOMPARE(lru, QStringList({QStringLiteral("new"), QStringLiteral("b"),
                               QStringLiteral("a"), QStringLiteral("c")}));
}

void YtSponsorLruTest::appendIfAbsentKeepsOrderAndFirstOccurrence()
{
    QStringList lru;
    ytlru::appendIfAbsent(lru, QStringLiteral("a"));
    ytlru::appendIfAbsent(lru, QStringLiteral("b"));
    ytlru::appendIfAbsent(lru, QStringLiteral("a")); // legacy duplicate: ignored
    ytlru::appendIfAbsent(lru, QStringLiteral("c"));
    ytlru::appendIfAbsent(lru, QString());           // empty never enters
    QCOMPARE(lru, QStringList({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}));
}

void YtSponsorLruTest::sponsorPlateauAtCapWithKeySetEquality()
{
    // I1 + I3 at the production cap: 2,500 distinct inserts must plateau at
    // 512 and never exceed it, with segments/timestamps/index always coherent.
    const int cap = 512;
    QStringList lru;
    QHash<QString, QVector<int>> segs;
    QHash<QString, qint64> stamps;
    for (int i = 0; i < 2500; ++i) {
        const QString id = QStringLiteral("vid-%1").arg(i, 5, 10, QLatin1Char('0'));
        sponsorInsert(lru, segs, stamps, id, 1000 + i, cap);
        QVERIFY(segs.size() <= cap);
        QVERIFY(stamps.size() <= cap);
        QVERIFY(lru.size() <= cap);
        if (i >= cap - 1) {
            QCOMPARE(segs.size(), cap); // plateau, not a dip
            QCOMPARE(stamps.size(), cap);
        }
        if (i % 100 == 0)
            assertCoherent(lru, segs, stamps);
    }
    assertCoherent(lru, segs, stamps);
    // The survivors are exactly the most recent `cap` ids.
    QVERIFY(segs.contains(QStringLiteral("vid-02499")));
    QVERIFY(!segs.contains(QStringLiteral("vid-01987"))); // 2500-512 = first evicted boundary
    QVERIFY(segs.contains(QStringLiteral("vid-01988")));
}

void YtSponsorLruTest::sponsorEvictsLruTailInOrder()
{
    const int cap = 3;
    QStringList lru;
    QHash<QString, QVector<int>> segs;
    QHash<QString, qint64> stamps;
    sponsorInsert(lru, segs, stamps, QStringLiteral("a"), 1, cap);
    sponsorInsert(lru, segs, stamps, QStringLiteral("b"), 2, cap);
    sponsorInsert(lru, segs, stamps, QStringLiteral("c"), 3, cap);
    const QStringList evicted = [&] {
        segs.insert(QStringLiteral("d"), {1});
        stamps.insert(QStringLiteral("d"), 4);
        ytlru::touch(lru, QStringLiteral("d"));
        return ytlru::evictSponsorToCap(lru, segs, stamps, cap);
    }();
    QCOMPARE(evicted, QStringList({QStringLiteral("a")})); // oldest goes first
    // A hit on "b" moves it off the tail; the next insert evicts "c" instead.
    ytlru::touch(lru, QStringLiteral("b"));
    sponsorInsert(lru, segs, stamps, QStringLiteral("e"), 5, cap);
    QVERIFY(segs.contains(QStringLiteral("b")));
    QVERIFY(!segs.contains(QStringLiteral("c")));
    QVERIFY(!stamps.contains(QStringLiteral("c"))); // co-evicted, no orphan
    assertCoherent(lru, segs, stamps);
}

void YtSponsorLruTest::hotSponsorEntrySurvivesRepeatedPressure()
{
    // I5: an entry touched on every round survives indefinitely while cold
    // entries churn around it.
    const int cap = 4;
    QStringList lru;
    QHash<QString, QVector<int>> segs;
    QHash<QString, qint64> stamps;
    const QString hot = QStringLiteral("hot");
    sponsorInsert(lru, segs, stamps, hot, 0, cap);
    sponsorInsert(lru, segs, stamps, QStringLiteral("c1"), 1, cap);
    sponsorInsert(lru, segs, stamps, QStringLiteral("c2"), 2, cap);
    sponsorInsert(lru, segs, stamps, QStringLiteral("c3"), 3, cap);
    for (int i = 0; i < 50; ++i) {
        ytlru::touch(lru, hot); // the warm-repeat hit
        sponsorInsert(lru, segs, stamps, QStringLiteral("new-%1").arg(i), 10 + i, cap);
        QVERIFY(segs.contains(hot));
        QVERIFY(stamps.contains(hot));
        QCOMPARE(segs.size(), cap);
    }
    QVERIFY(!segs.contains(QStringLiteral("c1")));
    QVERIFY(!segs.contains(QStringLiteral("c2")));
    QVERIFY(!segs.contains(QStringLiteral("c3")));
    assertCoherent(lru, segs, stamps);
}

void YtSponsorLruTest::knownPlateauAtCapAndHotIdSurvives()
{
    // I2 at the production cap.
    const int cap = 512;
    QStringList lru;
    QSet<QString> ids;
    for (int i = 0; i < 2500; ++i) {
        const QString id = QStringLiteral("known-%1").arg(i, 5, 10, QLatin1Char('0'));
        ids.insert(id);
        ytlru::touch(lru, id);
        ytlru::evictKnownToCap(lru, ids, cap);
        QVERIFY(ids.size() <= cap);
        if (i >= cap - 1)
            QCOMPARE(ids.size(), cap);
    }
    QCOMPARE(QSet<QString>(lru.cbegin(), lru.cend()), ids);
    QCOMPARE(lru.size(), ids.size());
    QVERIFY(ids.contains(QStringLiteral("known-02499")));
    QVERIFY(!ids.contains(QStringLiteral("known-00000")));

    // Hot known id survives pressure when re-surfaced (small cap, exact tail).
    QStringList klru;
    QSet<QString> kids;
    const auto surface = [&](const QString &id) {
        kids.insert(id);
        ytlru::touch(klru, id);
        ytlru::evictKnownToCap(klru, kids, 4);
    };
    surface(QStringLiteral("hot"));
    surface(QStringLiteral("a"));
    surface(QStringLiteral("b"));
    surface(QStringLiteral("c"));
    for (int i = 0; i < 20; ++i) {
        surface(QStringLiteral("hot")); // re-surfaced = hit
        surface(QStringLiteral("page-%1").arg(i));
        QVERIFY(kids.contains(QStringLiteral("hot")));
        QCOMPARE(kids.size(), 4);
    }
    QVERIFY(!kids.contains(QStringLiteral("a")));
}

void YtSponsorLruTest::evictedSponsorIdIsCleanlyRefetchable()
{
    // I6 (container side): an evicted id leaves no residue in any structure,
    // and a later answer re-inserts it exactly like a cold id.
    const int cap = 2;
    QStringList lru;
    QHash<QString, QVector<int>> segs;
    QHash<QString, qint64> stamps;
    sponsorInsert(lru, segs, stamps, QStringLiteral("a"), 1, cap);
    sponsorInsert(lru, segs, stamps, QStringLiteral("b"), 2, cap);
    sponsorInsert(lru, segs, stamps, QStringLiteral("c"), 3, cap); // evicts "a"
    QVERIFY(!segs.contains(QStringLiteral("a")));
    QVERIFY(!stamps.contains(QStringLiteral("a")));
    QVERIFY(!lru.contains(QStringLiteral("a")));
    // The refetch lands: fresh insert, fresh stamp, back at the cap.
    sponsorInsert(lru, segs, stamps, QStringLiteral("a"), 99, cap); // evicts "b"
    QVERIFY(segs.contains(QStringLiteral("a")));
    QCOMPARE(stamps.value(QStringLiteral("a")), 99);
    QVERIFY(!segs.contains(QStringLiteral("b")));
    QCOMPARE(segs.size(), cap);
    assertCoherent(lru, segs, stamps);
}

void YtSponsorLruTest::capFailuresDropsOldestDeterministically()
{
    // I10: a burst of distinct failures inside one TTL window stays bounded;
    // the oldest stamps (closest to expiry) go first.
    QHash<QString, qint64> failed;
    for (int i = 0; i < 600; ++i)
        failed.insert(QStringLiteral("f-%1").arg(i, 4, 10, QLatin1Char('0')), qint64(i));
    QCOMPARE(ytlru::capFailures(failed, 512), 88);
    QCOMPARE(failed.size(), 512);
    QVERIFY(!failed.contains(QStringLiteral("f-0000")));
    QVERIFY(!failed.contains(QStringLiteral("f-0087")));
    QVERIFY(failed.contains(QStringLiteral("f-0088")));
    QVERIFY(failed.contains(QStringLiteral("f-0599")));
    // Under the cap: no-op.
    QCOMPARE(ytlru::capFailures(failed, 512), 0);

    // Equal stamps break ties by key, so the prune is deterministic.
    QHash<QString, qint64> tied;
    tied.insert(QStringLiteral("b"), 7);
    tied.insert(QStringLiteral("a"), 7);
    tied.insert(QStringLiteral("c"), 7);
    QCOMPARE(ytlru::capFailures(tied, 1), 2);
    QCOMPARE(tied.size(), 1);
    QVERIFY(tied.contains(QStringLiteral("c")));
}

void YtSponsorLruTest::mixedOpsNeverOrphanATimestamp()
{
    // I3 stress: a deterministic mix of fresh inserts and hits under a tight
    // cap must keep the three structures coherent after EVERY operation.
    const int cap = 16;
    QStringList lru;
    QHash<QString, QVector<int>> segs;
    QHash<QString, qint64> stamps;
    quint32 rng = 42;
    int nextId = 0;
    for (int op = 0; op < 5000; ++op) {
        rng = rng * 1664525u + 1013904223u;
        if (!lru.isEmpty() && (rng % 3) == 0) {
            const QString hit = lru.at(int(rng % quint32(lru.size())));
            ytlru::touch(lru, hit); // ServeCache-style hit
        } else {
            sponsorInsert(lru, segs, stamps,
                          QStringLiteral("m-%1").arg(nextId++), op, cap);
        }
        ytlru::evictSponsorToCap(lru, segs, stamps, cap);
        QVERIFY(segs.size() <= cap);
        assertCoherent(lru, segs, stamps);
    }
}

// --- Load-time legacy prune (persistence contract) -------------------------

// Build a loaded state in file order without any pruning, mirroring the
// backend parse: one index entry, one segment vector, one ORIGINAL stamp.
static void loadEntry(QStringList &lru, QHash<QString, QVector<int>> &segs,
                      QHash<QString, qint64> &stamps, const QString &id, qint64 stamp)
{
    segs.insert(id, {1});
    stamps.insert(id, stamp);
    ytlru::appendIfAbsent(lru, id);
}

void YtSponsorLruTest::pruneSponsorOnLoadKeepsNewestStampsDeterministically()
{
    // Over cap: newest stamps win, equal stamps keep the earlier file
    // position, and the index is rebuilt MRU-first in surviving order.
    const int cap = 3;
    QStringList lru;
    QHash<QString, QVector<int>> segs;
    QHash<QString, qint64> stamps;
    loadEntry(lru, segs, stamps, QStringLiteral("a"), 10);
    loadEntry(lru, segs, stamps, QStringLiteral("b"), 30);
    loadEntry(lru, segs, stamps, QStringLiteral("c"), 20);
    loadEntry(lru, segs, stamps, QStringLiteral("d"), 30); // tie with b, later in file
    loadEntry(lru, segs, stamps, QStringLiteral("e"), 5);
    loadEntry(lru, segs, stamps, QStringLiteral("f"), 25);
    loadEntry(lru, segs, stamps, QStringLiteral("g"), 1);
    QCOMPARE(ytlru::pruneSponsorOnLoad(lru, segs, stamps, 1000, cap), 4);
    QCOMPARE(lru, QStringList({QStringLiteral("b"), QStringLiteral("d"), QStringLiteral("f")}));
    QCOMPARE(segs.size(), cap);
    QCOMPARE(stamps.value(QStringLiteral("b")), 30); // original stamps untouched
    QCOMPARE(stamps.value(QStringLiteral("d")), 30);
    QCOMPARE(stamps.value(QStringLiteral("f")), 25);
    QVERIFY(!segs.contains(QStringLiteral("a")));
    QVERIFY(!stamps.contains(QStringLiteral("g"))); // co-evicted, no orphan
    assertCoherent(lru, segs, stamps);

    // Bounded file: absolute no-op, even with stale residents (they keep
    // serving playback until their refetch lands — prune is capacity-only).
    QStringList blru;
    QHash<QString, QVector<int>> bsegs;
    QHash<QString, qint64> bstamps;
    loadEntry(blru, bsegs, bstamps, QStringLiteral("old"), 1); // ancient
    loadEntry(blru, bsegs, bstamps, QStringLiteral("new"), 999);
    QCOMPARE(ytlru::pruneSponsorOnLoad(blru, bsegs, bstamps, 1000, cap), 0);
    QCOMPARE(blru, QStringList({QStringLiteral("old"), QStringLiteral("new")}));
    QCOMPARE(bsegs.size(), 2);
}

void YtSponsorLruTest::pruneSponsorOnLoadDropsStaleFirstAndNeverRestamps()
{
    const qint64 now = 2000000000LL;
    const qint64 staleCutoff = now - ytsb::kSuccessTtlMs;
    const int cap = 3;
    QStringList lru;
    QHash<QString, QVector<int>> segs;
    QHash<QString, qint64> stamps;
    loadEntry(lru, segs, stamps, QStringLiteral("s1"), 100);            // stale
    loadEntry(lru, segs, stamps, QStringLiteral("s2"), 200);            // stale
    loadEntry(lru, segs, stamps, QStringLiteral("n1"), now - 100000);   // fresh
    loadEntry(lru, segs, stamps, QStringLiteral("u"), 0);               // pre-stamp
    loadEntry(lru, segs, stamps, QStringLiteral("n2"), now - 50000);
    loadEntry(lru, segs, stamps, QStringLiteral("n3"), now - 10000);
    loadEntry(lru, segs, stamps, QStringLiteral("n4"), now - 1000);
    QVERIFY(staleCutoff > 200); // s1/s2 really are stale, n* really are fresh
    // Stale drop leaves 5 > cap 3 -> newest-wins keeps n4,n3,n2. The stampless
    // entry sorts LAST (known-fresh beats unknown-age) and is dropped; the
    // helper never re-stamps anything, so ordering used ORIGINAL stamps.
    // 7 loaded - 3 kept = 4 dropped (2 stale + n1 + u).
    QCOMPARE(ytlru::pruneSponsorOnLoad(lru, segs, stamps, now, cap), 4);
    QCOMPARE(lru, QStringList({QStringLiteral("n4"), QStringLiteral("n3"), QStringLiteral("n2")}));
    QCOMPARE(stamps.value(QStringLiteral("n4")), now - 1000);
    QVERIFY(!segs.contains(QStringLiteral("u")));
    QVERIFY(!segs.contains(QStringLiteral("s1")));
    assertCoherent(lru, segs, stamps);

    // If the stale drop alone brings the file under cap, NO newest-cut happens
    // and a stampless survivor keeps its 0 for the caller to stamp after.
    QStringList lru2;
    QHash<QString, QVector<int>> segs2;
    QHash<QString, qint64> stamps2;
    loadEntry(lru2, segs2, stamps2, QStringLiteral("s1"), 100);
    loadEntry(lru2, segs2, stamps2, QStringLiteral("s2"), 200);
    loadEntry(lru2, segs2, stamps2, QStringLiteral("n1"), now - 100000);
    loadEntry(lru2, segs2, stamps2, QStringLiteral("u"), 0);
    loadEntry(lru2, segs2, stamps2, QStringLiteral("n2"), now - 50000);
    loadEntry(lru2, segs2, stamps2, QStringLiteral("n3"), now - 10000);
    loadEntry(lru2, segs2, stamps2, QStringLiteral("n4"), now - 1000);
    QCOMPARE(ytlru::pruneSponsorOnLoad(lru2, segs2, stamps2, now, 5), 2);
    QCOMPARE(lru2, QStringList({QStringLiteral("n1"), QStringLiteral("u"),
                                QStringLiteral("n2"), QStringLiteral("n3"),
                                QStringLiteral("n4")})); // file order preserved
    QCOMPARE(stamps2.value(QStringLiteral("u")), 0); // untouched by the helper
    assertCoherent(lru2, segs2, stamps2);
}

void YtSponsorLruTest::dropOrphanStampsRemovesOnlyOrphans()
{
    QHash<QString, QVector<int>> segs;
    QHash<QString, qint64> stamps;
    segs.insert(QStringLiteral("a"), {1});
    segs.insert(QStringLiteral("b"), {1});
    stamps.insert(QStringLiteral("a"), 10);
    stamps.insert(QStringLiteral("b"), 20);
    stamps.insert(QStringLiteral("orphan"), 30); // stamp without segments
    QCOMPARE(ytlru::dropOrphanStamps(segs, stamps), 1);
    QCOMPARE(stamps.size(), 2);
    QVERIFY(!stamps.contains(QStringLiteral("orphan")));
    QCOMPARE(stamps.value(QStringLiteral("a")), 10);
    QCOMPARE(ytlru::dropOrphanStamps(segs, stamps), 0); // clean pair: no-op
}

QTEST_APPLESS_MAIN(YtSponsorLruTest)
#include "YtSponsorLruTest.moc"