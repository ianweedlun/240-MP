#include "TestHarness.h"
#include "../LibraryDb.h"

#include <QDir>
#include <QFile>
#include <QVector>
#include <QVariantList>

using namespace vchan;

// A throwaway DB path per run; the file must not survive the test.
static QString dbPath() {
    return QDir::tempPath() + QStringLiteral("/240mp-librarydb-test.sqlite");
}

static void dropDb(const QString &path) { QFile::remove(path); }

int runLibraryDbTests() {
    vtest::section("LibraryDb");

    const QString path = dbPath();

    // --- a build without Qt's Sql module: the layer refuses and answers unknown
#ifdef APP_HAVE_SQLITE
    vtest::check(true, "Sql module present in this build");

    LibraryDb db;
    vtest::check(db.open(path), "db opens at the temp path");
    vtest::check(db.isEmpty(), "a fresh index is empty");
    vtest::checkEq(db.lastRebuildMs(), 0, "never rebuilt before the first walk");

    // --- one walk, two servers, one show on both: presence + counts ---
    {
        QVector<LibraryRecord> walk;
        {
            LibraryRecord r;
            r.src = QStringLiteral("plex");
            r.channel = QStringLiteral("tv");
            r.title = QStringLiteral("Scrubs");
            r.genres = { QStringLiteral("Comedy"), QStringLiteral("Drama") };
            r.updatedMs = 1000;
            walk.append(r);
            LibraryRecord r2;
            r2.src = QStringLiteral("jellyfin");
            r2.channel = QStringLiteral("tv");
            r2.title = QStringLiteral("scrubs");     // other server, other case
            r2.genres = { QStringLiteral("comedy"), QStringLiteral("Horror") };
            r2.updatedMs = 2000;
            walk.append(r2);
            LibraryRecord m;
            m.src = QStringLiteral("plex");
            m.channel = QStringLiteral("movies");
            m.title = QStringLiteral("Private Snafu");
            m.genres = { QStringLiteral("Comedy") };
            m.updatedMs = 1500;
            walk.append(m);
        }
        vtest::check(db.replaceAll(walk), "walk wrote the index");
        vtest::check(!db.isEmpty(), "index no longer empty after the walk");
        vtest::check(db.lastRebuildMs() > 0, "rebuild stamp advanced");

        // Deduped by (side, lower(title)): one Scrubs row, so Comedy counts ONCE
        // even though two servers carry it.
        const QVariantList tv = db.presence(QStringLiteral("genre"), QStringLiteral("tv"), 10);
        vtest::checkEq(tv.size(), 3, "tv genres are Comedy, Drama, Horror");
        {
            int comedyCount = -1;
            for (const QVariant &row : tv) {
                const QVariantMap m = row.toMap();
                if (m.value(QStringLiteral("label")).toString() == QStringLiteral("Comedy"))
                    comedyCount = m.value(QStringLiteral("count")).toInt();
            }
            vtest::checkEq(comedyCount, 1, "a show on two servers tallies once");
        }
        vtest::checkEq(db.countFor(QStringLiteral("genre"), QStringLiteral("horror"), QStringLiteral("tv")), 1,
                       "count lookup is case-insensitive");
        vtest::checkEq(db.countFor(QStringLiteral("genre"), QStringLiteral("Comedy"), QStringLiteral("movies")), 1,
                       "movies side counts its own Comedy");
        vtest::checkEq(db.countFor(QStringLiteral("genre"), QStringLiteral("Comedy"), QString()), 2,
                       "empty channel pools both sides of the dial");
        vtest::checkEq(db.countFor(QStringLiteral("genre"), QStringLiteral("Noir"), QStringLiteral("tv")), 0,
                       "an absent genre reads zero");

        // The movie channel sees ONLY the movie side: Private Snafu's Comedy is
        // not a tv row and Scrubs' Drama/Horror are not a movies row.
        const QVariantList movies = db.presence(QStringLiteral("genre"), QStringLiteral("movies"), 10);
        vtest::checkEq(movies.size(), 1, "movie genres are only the movie side");

        // Walks that never mention a field leave it unknown, not wrong.
        vtest::check(db.metadataFor(QStringLiteral("Scrubs"), QStringLiteral("tv"),
                                    QStringLiteral("networks")).isEmpty(),
                     "unresolved network stays unknown");

        // A resolve fills the gap, and the gap survives the next walk.
        {
            QVector<LibraryRecord> second;
            LibraryRecord again;
            again.src = QStringLiteral("plex");
            again.channel = QStringLiteral("tv");
            again.title = QStringLiteral("Scrubs");
            again.updatedMs = 3000;          // genres empty: a partial walk
            second.append(again);
            vtest::check(db.mergeAndRebuild(second), "partial walk merged");
            const QVariantList still = db.presence(QStringLiteral("genre"), QStringLiteral("tv"), 10);
            vtest::checkEq(still.size(), 3, "a walk with empty genres does not darken the channel");
        }
        {
            vtest::check(db.noteMetadata(QStringLiteral("scrubs"), QStringLiteral("tv"),
                                         QStringLiteral("networks"), {QStringLiteral("NBC")}),
                         "resolve stored the network under the deduped row");
            vtest::checkEq(db.metadataFor(QStringLiteral("Scrubs"), QStringLiteral("tv"),
                                          QStringLiteral("networks")).size(), 1,
                           "network read survives, case-insensitive on the title");
            QVector<LibraryRecord> third;
            LibraryRecord noNetwork;
            noNetwork.src = QStringLiteral("local");
            noNetwork.channel = QStringLiteral("tv");
            noNetwork.title = QStringLiteral("Scrubs");
            noNetwork.updatedMs = 4000;
            third.append(noNetwork);
            vtest::check(db.mergeAndRebuild(third), "third walk merged");
            vtest::checkEq(db.metadataFor(QStringLiteral("Scrubs"), QStringLiteral("tv"),
                                          QStringLiteral("networks")).size(), 1,
                           "the cached network survived a walk that did not re-resolve");
        }

        // Re-opening the same file keeps the index (the app does this every
        // launch; the walk then refreshes).
        {
            db.close();
            LibraryDb again;
            vtest::check(again.open(path), "re-open of the same file works");
            vtest::check(!again.isEmpty(), "index survived close + re-open");
            vtest::check(again.lastRebuildMs() > 0, "rebuild stamp survived re-open");
            const QVariantList tv2 = again.presence(QStringLiteral("genre"), QStringLiteral("tv"), 10);
            vtest::checkEq(tv2.size(), 3, "presence answers after a cold re-open");
            dropDb(path);
        }
    }
#else
    vtest::check(true, "Sql module absent: stub answers unknown");
    LibraryDb db;
    QString why;
    vtest::check(!db.open(dbPath()), "open refuses without Sql");
    QVector<LibraryRecord> empty;
    vtest::check(!db.mergeAndRebuild(empty, &why), "merge refuses without Sql");
    vtest::check(why.contains(QStringLiteral("without Sql")), "refusal says why");
    dropDb(dbPath());
#endif

    return vtest::g_failed;
}
