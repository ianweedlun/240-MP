#include "../RecentGamesStore.h"

#include <QtTest/QtTest>
#include <QFile>
#include <QTemporaryDir>

// The bookkeeping rules behind the recently-played row and playlist position
// memory. Pure file I/O against a temp dir: no QObject, no RetroArch, no display.
class RecentGamesStoreTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tmp;

    static QVariantMap game(const QString &name, const QString &romPath,
                            const QString &playlist = QStringLiteral("/p/snes.lpl"))
    {
        return QVariantMap{
            { "name", name },
            { "romPath", romPath },
            { "corePath", "/cores/snes_libretro.so" },
            { "coreName", "snes_libretro" },
            { "playlistPath", playlist },
            { "playlistBasename", QFileInfo(playlist).baseName() },
            { "systemId", QStringLiteral("nintendo - super nintendo entertainment system") },
            { "shortName", QStringLiteral("SNES") },
        };
    }

    QString path(const QString &leaf = QStringLiteral("recent.json")) const
    {
        return m_tmp.filePath(leaf);
    }

private slots:
    void emptyStoreHasNoLastPlayed();
    void recordLaunchPrependsNewestFirst();
    void relaunchMovesToFrontWithoutDuplicating();
    void capDropsTheOldestEntry();
    void entriesCarryWhatALaunchNeedsAndDropCachedThumbnails();
    void positionRoundTripsAndTopOfListClearsIt();
    void saveLoadRoundTrip();
    void corruptFileReadsAsEmpty();
    void missingFileReadsAsEmpty();
    void handEditedDuplicateEntriesAreCollapsed();
};

void RecentGamesStoreTest::emptyStoreHasNoLastPlayed()
{
    RecentGamesStore store(path());
    QVERIFY(store.entries().isEmpty());
    QVERIFY(store.lastPlayed().isEmpty());
    QCOMPARE(store.position(QStringLiteral("/p/snes.lpl")), -1);
    // load() on a file that was never written is not an error state.
    QVERIFY(!store.load());
    QVERIFY(store.entries().isEmpty());
}

void RecentGamesStoreTest::recordLaunchPrependsNewestFirst()
{
    RecentGamesStore store(path());
    store.recordLaunch(game("Super Metroid", "/r/sm.sfc"), 100);
    store.recordLaunch(game("Chrono Trigger", "/r/ct.sfc"), 200);

    QCOMPARE(store.entries().size(), 2);
    QCOMPARE(store.entries().first().toMap().value("name").toString(),
             QStringLiteral("Chrono Trigger"));
    QCOMPARE(store.lastPlayed().value("name").toString(), QStringLiteral("Chrono Trigger"));
    QCOMPARE(store.entries().at(1).toMap().value("lastPlayed").toLongLong(), qint64(100));
}

void RecentGamesStoreTest::relaunchMovesToFrontWithoutDuplicating()
{
    RecentGamesStore store(path());
    store.recordLaunch(game("Super Metroid", "/r/sm.sfc"), 100);
    store.recordLaunch(game("Chrono Trigger", "/r/ct.sfc"), 200);
    store.recordLaunch(game("Super Metroid", "/r/sm.sfc"), 300);

    QCOMPARE(store.entries().size(), 2);
    QCOMPARE(store.entries().at(0).toMap().value("name").toString(),
             QStringLiteral("Super Metroid"));
    QCOMPARE(store.entries().at(0).toMap().value("lastPlayed").toLongLong(), qint64(300));
    QCOMPARE(store.entries().at(1).toMap().value("name").toString(),
             QStringLiteral("Chrono Trigger"));
}

void RecentGamesStoreTest::capDropsTheOldestEntry()
{
    RecentGamesStore store(path());
    for (int i = 0; i < RecentGamesStore::kMaxEntries + 3; ++i) {
        store.recordLaunch(game(QStringLiteral("Game %1").arg(i),
                                QStringLiteral("/r/g%1.rom").arg(i)), i);
    }
    QCOMPARE(store.entries().size(), RecentGamesStore::kMaxEntries);
    // Newest kept, oldest three gone.
    QCOMPARE(store.entries().first().toMap().value("name").toString(),
             QStringLiteral("Game 14"));
    QCOMPARE(store.entries().last().toMap().value("name").toString(),
             QStringLiteral("Game 3"));
}

void RecentGamesStoreTest::entriesCarryWhatALaunchNeedsAndDropCachedThumbnails()
{
    RecentGamesStore store(path());
    QVariantMap g = game("Super Metroid", "/r/sm.sfc");
    g.insert("thumbnail", "/thumbs/snes/Named_Boxarts/Super Metroid.png");
    store.recordLaunch(g, 42);

    const QVariantMap e = store.lastPlayed();
    for (const char *key : { "name", "romPath", "corePath", "coreName",
                             "playlistPath", "playlistBasename", "systemId", "shortName" }) {
        QVERIFY2(!e.value(QString::fromLatin1(key)).toString().isEmpty(), key);
    }
    // A cached absolute thumbnail path would survive a folder move and then lie.
    QVERIFY(!e.contains(QStringLiteral("thumbnail")));
}

void RecentGamesStoreTest::positionRoundTripsAndTopOfListClearsIt()
{
    RecentGamesStore store(path());
    QCOMPARE(store.position("/p/snes.lpl"), -1);
    store.recordPosition("/p/snes.lpl", 812);
    QCOMPARE(store.position("/p/snes.lpl"), 812);
    QCOMPARE(store.position("/p/other.lpl"), -1);

    // Back at the top: no position, so the file stays clean for the common case.
    store.recordPosition("/p/snes.lpl", 0);
    QCOMPARE(store.position("/p/snes.lpl"), -1);
    store.recordPosition("/p/snes.lpl", -5);
    QCOMPARE(store.position("/p/snes.lpl"), -1);
}

void RecentGamesStoreTest::saveLoadRoundTrip()
{
    const QString file = path();
    {
        RecentGamesStore store(file);
        store.recordLaunch(game("Super Metroid", "/r/sm.sfc"), 100);
        store.recordLaunch(game("Chrono Trigger", "/r/ct.sfc"), 200);
        store.recordPosition("/p/snes.lpl", 640);
        QVERIFY(store.save());
    }
    RecentGamesStore reloaded(file);
    QVERIFY(reloaded.load());
    QCOMPARE(reloaded.entries().size(), 2);
    QCOMPARE(reloaded.lastPlayed().value("name").toString(), QStringLiteral("Chrono Trigger"));
    QCOMPARE(reloaded.position("/p/snes.lpl"), 640);
}

void RecentGamesStoreTest::corruptFileReadsAsEmpty()
{
    const QString file = path("corrupt.json");
    QFile f(file);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{ this is not json ");
    f.close();

    RecentGamesStore store(file);
    QVERIFY(!store.load());
    QVERIFY(store.entries().isEmpty());
    QCOMPARE(store.position("/p/snes.lpl"), -1);
    // ...and a later save repairs it rather than propagating the damage.
    store.recordLaunch(game("Super Metroid", "/r/sm.sfc"), 1);
    QVERIFY(store.save());
    RecentGamesStore reloaded(file);
    QVERIFY(reloaded.load());
    QCOMPARE(reloaded.entries().size(), 1);
}

void RecentGamesStoreTest::missingFileReadsAsEmpty()
{
    RecentGamesStore store(m_tmp.filePath("nope.json"));
    QVERIFY(!store.load());
    QVERIFY(store.lastPlayed().isEmpty());
}

void RecentGamesStoreTest::handEditedDuplicateEntriesAreCollapsed()
{
    const QString file = path("dupes.json");
    QFile f(file);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(R"({"version":1,"entries":[)"
            R"({"name":"A","romPath":"/r/a.rom"},)"
            R"({"name":"A","romPath":"/r/a.rom"},)"
            R"({"name":"B","romPath":"/r/b.rom"},)"
            R"({"name":""})"
            R"(],"positions":{}})");
    f.close();

    RecentGamesStore store(file);
    QVERIFY(store.load());
    QCOMPARE(store.entries().size(), 2);
    QCOMPARE(store.entries().at(0).toMap().value("name").toString(), QStringLiteral("A"));
    QCOMPARE(store.entries().at(1).toMap().value("name").toString(), QStringLiteral("B"));
}

QTEST_GUILESS_MAIN(RecentGamesStoreTest)
#include "RecentGamesStoreTest.moc"
