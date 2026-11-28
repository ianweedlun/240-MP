// CTest gate for YT-PLAYLIST-FETCH-FANOUT-UNBOUNDED-01 (GREEN side).
// Drives the REAL YouTubeBackend::assemblePlaylistFromBrowse and
// readPlaylistEntries; private access is widened only in this translation unit
// so the retained rows can be measured directly. No product file is modified.
// Access-specifier changes do not alter layout, so linking the product object
// compiled with normal access is ABI-safe.
#include <QtCore>
#include <QtNetwork>
#include <QtTest>

#define private public
#define protected public
#include "modules/youtube/YouTubeBackend.h"
#undef protected
#undef private

class YtPlaylistFanoutRetentionTest : public QObject
{
    Q_OBJECT

    static QVariantList makeRows(int pl, int count = 500)
    {
        QVariantList rows;
        for (int i = 0; i < count; ++i) {
            QVariantMap v;
            v[QStringLiteral("videoId")] = QStringLiteral("vid_%1_%2").arg(pl).arg(i);
            v[QStringLiteral("title")]   = QStringLiteral("Video %1 %2").arg(pl).arg(i);
            rows << v;
        }
        return rows;
    }

    static int totalRows(const YouTubeBackend &b)
    {
        int n = 0;
        for (auto it = b.m_playlists.constBegin(); it != b.m_playlists.constEnd(); ++it)
            n += it->videos.size();
        return n;
    }

    static QString id(int i) { return QStringLiteral("PL%1").arg(i, 6, 10, QChar('0')); }

    // Feed one fetch without triggering aggregate emission or network spawn.
    static void feed(YouTubeBackend &b, int i, int rows = 500)
    {
        b.m_activePlaylistFetches = 1;   // assemble decrements this once
        b.assemblePlaylistFromBrowse(id(i), makeRows(i, rows));
    }

    static void writePlaylistFile(const QString &dir, const QStringList &lines)
    {
        QFile f(dir + QStringLiteral("/youtube_playlists.txt"));
        QVERIFY2(f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate),
                 "cannot write playlist fixture");
        for (const QString &line : lines) {
            f.write(line.toUtf8());
            f.write("\n");
        }
    }

private slots:
    void retainedRowsPlateauAtGlobalBudget()
    {
        QTemporaryDir dir;
        YouTubeBackend b(QDir::tempPath(), dir.path());
        b.m_pendingPlaylists = 10000;    // keep the aggregate/emission path inert
        const int checkpoints[] = {25, 100, 200, 400, 600};
        int fed = 0;
        for (int cp : checkpoints) {
            while (fed < cp) {
                feed(b, fed);
                ++fed;
            }
            QCOMPARE(totalRows(b), YouTubeBackend::kMaxRetainedPlaylistRows);
        }
    }

    void evictionIsDeterministicOldestFirst()
    {
        QTemporaryDir dir;
        YouTubeBackend b(QDir::tempPath(), dir.path());
        b.m_pendingPlaylists = 10000;
        for (int i = 0; i < 4; ++i) {
            feed(b, i);
            b.m_playlists[id(i)].fetchedMs = 1000 + i;   // distinct, ordered
        }
        QCOMPARE(totalRows(b), 2000);

        feed(b, 4);   // fifth full fetch must evict exactly PL000000
        QCOMPARE(totalRows(b), 2000);
        QCOMPARE(b.m_playlists[id(0)].videos.size(), 0);
        QVERIFY(!b.m_playlists[id(0)].fetchOk);          // stale -> refetch
        for (int i = 1; i <= 4; ++i)
            QCOMPARE(b.m_playlists[id(i)].videos.size(), 500);
    }

    void emitTargetIsProtectedFromEviction()
    {
        QTemporaryDir dir;
        YouTubeBackend b(QDir::tempPath(), dir.path());
        b.m_pendingPlaylists = 10000;
        for (int i = 0; i < 4; ++i) {
            feed(b, i);
            b.m_playlists[id(i)].fetchedMs = 1000 + i;
        }
        b.m_emitPlaylistVideosWhenDone = id(2);
        feed(b, 4);

        QCOMPARE(b.m_playlists[id(2)].videos.size(), 500);
        QVERIFY(b.m_playlists[id(2)].fetchOk);
        QVERIFY2(totalRows(b) <= 2000, qPrintable(QString("retained=%1").arg(totalRows(b))));
    }

    void replacementAndFailureKeepBudget()
    {
        QTemporaryDir dir;
        YouTubeBackend b(QDir::tempPath(), dir.path());
        b.m_pendingPlaylists = 10000;
        for (int i = 0; i < 8; ++i)
            feed(b, i);
        QVERIFY(totalRows(b) <= 2000);

        feed(b, 7);   // replacement must not double-count existing rows
        QVERIFY(totalRows(b) <= 2000);

        const int before = totalRows(b);
        b.m_activePlaylistFetches = 1;
        b.assemblePlaylistFromBrowse(id(99), QVariantList());
        QCOMPARE(totalRows(b), before);
    }

    void emptyAndDuplicateInputsAreSafe()
    {
        QTemporaryDir dir;
        YouTubeBackend b(QDir::tempPath(), dir.path());
        b.m_pendingPlaylists = 10000;
        for (int i = 0; i < 5; ++i)
            feed(b, 0);   // same id repeatedly: replacement, never growth
        QCOMPARE(totalRows(b), 500);

        b.m_activePlaylistFetches = 1;
        b.assemblePlaylistFromBrowse(id(0), QVariantList());
        QCOMPARE(totalRows(b), 500);
        for (int i = 1; i < 600; ++i)
            feed(b, i);
        QVERIFY2(totalRows(b) <= 2000, qPrintable(QString("retained=%1").arg(totalRows(b))));
    }

    void playlistFileIdsCappedAndDeduped()
    {
        QTemporaryDir dir;
        QStringList lines;
        lines << QStringLiteral("https://example.com/no/list/here")
              << QStringLiteral("https://www.youtube.com/watch?v=abc");
        for (int i = 0; i < 80; ++i) {
            lines << id(i);
            if (i % 10 == 0) lines << id(0);   // duplicates must not consume the cap
        }
        writePlaylistFile(dir.path(), lines);

        YouTubeBackend b(QDir::tempPath(), dir.path());
        QString err;
        const auto refs = b.readPlaylistEntries(&err);
        QCOMPARE(refs.size(), YouTubeBackend::kMaxPlaylistFileIds);
        QCOMPARE(refs.first().id, id(0));       // malformed lines were skipped
        QSet<QString> ids;
        for (const auto &ref : refs) ids.insert(ref.id);
        QCOMPARE(ids.size(), refs.size());      // duplicates deduplicated
    }

    void playlistFileZeroSingleAndComments()
    {
        QTemporaryDir dir;
        YouTubeBackend b(QDir::tempPath(), dir.path());
        QString err;

        writePlaylistFile(dir.path(), QStringList()
                          << QStringLiteral("# comment") << QStringLiteral("")
                          << QStringLiteral("   "));
        QVERIFY(b.readPlaylistEntries(&err).isEmpty());

        writePlaylistFile(dir.path(), QStringList()
                          << QStringLiteral("My Mix | ") + id(7)
                          << QStringLiteral("PL000008"));
        const auto refs = b.readPlaylistEntries(&err);
        QCOMPARE(refs.size(), 2);
        QCOMPARE(refs.at(0).id, id(7));
        QCOMPARE(refs.at(0).name, QStringLiteral("My Mix"));
        QCOMPARE(refs.at(1).id, id(8));
    }
};

QTEST_MAIN(YtPlaylistFanoutRetentionTest)
#include "YtPlaylistFanoutRetentionTest.moc"
