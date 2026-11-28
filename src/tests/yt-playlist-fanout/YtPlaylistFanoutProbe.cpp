// TWO-SIDED PROBE: YT-PLAYLIST-FETCH-FANOUT-UNBOUNDED-01 CTest gate.
//
// RED arm  = verbatim pre-fix product text (sha256-pinned .inc files from the
//            detached 1045ee5 torture snapshot). O(N^2) QStringList dedup,
//            unbounded id fanout, no global row budget.
// GREEN arm = verbatim product text extracted from the tree at build time by
//            extract_yt_playlist_entries.py (QSet dedup, id cap, row budget).
//
// No product file is edited by this probe. Every arm is compiled into the same
// binary in its own namespace so the two shapes can be compared in one run.
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QHash>
#include <QVariant>
#include <QVariantMap>
#include <QVariantList>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QDateTime>
#include <chrono>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

// Same file-scope constant as product YouTubeBackend.cpp:33.
static const char *kPlaylistsFileName = "youtube_playlists.txt";

namespace yt_red {
class YouTubeBackend {
public:
    struct PlaylistFileRef { QString id; QString name; };
    QString m_dataRoot;
    QList<PlaylistFileRef> readPlaylistEntries(QString *error = nullptr) const;
};
#include "playlistIdFromToken.pre_fix.inc"
#include "readPlaylistEntries.pre_fix.inc"
}  // namespace yt_red

namespace yt_green {
class YouTubeBackend {
public:
    struct PlaylistFileRef { QString id; QString name; };
    QString m_dataRoot;
    QList<PlaylistFileRef> readPlaylistEntries(QString *error = nullptr) const;
};
#include "yt_playlist_entries_green.inc"
}  // namespace yt_green

static QString jsonEscape(const QString &s)
{
    QString out;
    for (const QChar &c : s) {
        if (c == QLatin1Char('\\')) out += QStringLiteral("\\\\");
        else if (c == QLatin1Char('"')) out += QStringLiteral("\\\"");
        else if (c == QLatin1Char('\n')) out += QStringLiteral("\\n");
        else if (c == QLatin1Char('\r')) out += QStringLiteral("\\r");
        else if (c == QLatin1Char('\t')) out += QStringLiteral("\\t");
        else out += c;
    }
    return out;
}

static long rssKb()
{
#ifdef __linux__
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long size = 0, resident = 0;
    if (fscanf(f, "%ld %ld", &size, &resident) != 2) { fclose(f); return -1; }
    fclose(f);
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
#else
    return -1;  // /proc is Linux-only; orchestrator skips RSS gates when < 0.
#endif
}

static double median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

static QString arg(const QStringList &a, const QString &key, const QString &def = QString())
{
    const int i = a.indexOf(key);
    return (i >= 0 && i + 1 < a.size()) ? a[i + 1] : def;
}
static bool hasFlag(const QStringList &a, const QString &flag) { return a.contains(flag); }

// Faithful model of spawnNextPlaylistFetch() retention (product :613-615):
// each fetch yields up to kMaxPlaylistItems parsed rows into m_playlists with
// no global cap (RED) or the extracted global budget (GREEN).
static QVariantList makeRows(int plNum, int perFetch)
{
    QVariantList videos;
    videos.reserve(perFetch);
    for (int i = 0; i < perFetch; ++i) {
        QVariantMap v;
        v[QStringLiteral("videoId")]     = QStringLiteral("vid_%1_%2").arg(plNum).arg(i);
        v[QStringLiteral("title")]       = QStringLiteral("Video %1 %2").arg(plNum).arg(i);
        v[QStringLiteral("channelId")]   = QString();
        v[QStringLiteral("channelName")] = QStringLiteral("Chan%1").arg(plNum);
        v[QStringLiteral("publishedAt")] = QString();
        v[QStringLiteral("publishedMs")] = qint64(0);
        v[QStringLiteral("url")]         = QStringLiteral("https://youtube.com/watch?v=vid_%1_%2").arg(plNum).arg(i);
        v[QStringLiteral("isShort")]     = false;
        videos.append(v);
    }
    return videos;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStringList a;
    for (int i = 1; i < argc; ++i) a << QString::fromLocal8Bit(argv[i]);

    const QString mode  = arg(a, QStringLiteral("--mode"), QStringLiteral("red"));
    const int n         = arg(a, QStringLiteral("--n"), QStringLiteral("400")).toInt();
    const QString data  = arg(a, QStringLiteral("--data"), QStringLiteral("/tmp/ytfp-data"));
    // Defaults come from the extracted product constants; CLI overrides are for
    // manual probing only. The orchestrator never overrides them.
    const int idCap     = arg(a, QStringLiteral("--id-cap"),
                              QString::number(yt_green::kMaxPlaylistFileIds)).toInt();
    const int rowBudget = arg(a, QStringLiteral("--row-budget"),
                              QString::number(yt_green::kMaxRetainedPlaylistRows)).toInt();
    const int perFetch  = arg(a, QStringLiteral("--rows-per-fetch"),
                              QString::number(yt_green::kMaxPlaylistItems)).toInt();
    const int concCap   = arg(a, QStringLiteral("--concurrency"),
                              QString::number(yt_green::kMaxConcurrentPlaylistFetches)).toInt();
    const int dups      = arg(a, QStringLiteral("--dups"), QStringLiteral("0")).toInt();
    const bool comments = hasFlag(a, QStringLiteral("--comments"));
    const bool malformed= hasFlag(a, QStringLiteral("--malformed"));
    const bool zero     = hasFlag(a, QStringLiteral("--zero"));
    const bool allSame  = hasFlag(a, QStringLiteral("--all-same"));
    const bool noRetain = hasFlag(a, QStringLiteral("--no-retain"));
    const int repeats   = arg(a, QStringLiteral("--repeats"), QStringLiteral("7")).toInt();
    const bool isRed    = (mode == QLatin1String("red"));

    QDir().mkpath(data);
    const QString path = data + QLatin1Char('/') + QString::fromLatin1(kPlaylistsFileName);
    QStringList lines;
    if (comments) lines << QStringLiteral("# generated fanout fixture")
                        << QStringLiteral("")
                        << QStringLiteral("   ");
    if (!zero) {
        for (int i = 0; i < n; ++i)
            lines << QStringLiteral("PL%1").arg(allSame ? 0 : i, 6, 10, QChar('0'));
        for (int i = 0; i < dups; ++i)
            lines << QStringLiteral("PL%1").arg(i % qMax(1, n), 6, 10, QChar('0'));
        if (malformed) {
            lines << QStringLiteral("https://example.com/no/list/here");
            lines << QStringLiteral("https://www.youtube.com/watch?v=abc");
        }
    }
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            fprintf(stderr, "cannot write fixture\n");
            return 2;
        }
        for (const QString &l : lines) { f.write(l.toUtf8()); f.write("\n"); }
    }

    // Dedup timing: verbatim RED text vs verbatim GREEN product text.
    std::vector<double> samples;
    QList<QString> ids;
    QString err;
    for (int r = 0; r < repeats; ++r) {
        auto t0 = std::chrono::steady_clock::now();
        if (isRed) {
            yt_red::YouTubeBackend b; b.m_dataRoot = data;
            const auto refs = b.readPlaylistEntries(&err);
            if (r == 0) for (const auto &ref : refs) ids << ref.id;
        } else {
            yt_green::YouTubeBackend b; b.m_dataRoot = data;
            const auto refs = b.readPlaylistEntries(&err);
            if (r == 0) for (const auto &ref : refs) ids << ref.id;
        }
        auto t1 = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    }
    const double dedupUs = median(samples);
    const int accepted = ids.size();

    // Fetch queue: RED = stale.size() with no cap; GREEN = capped ids.
    int fetches = isRed ? accepted : qMin(accepted, idCap);

    // Concurrency scheduler model (both arms, faithful to product :557).
    int active = 0, maxActive = 0, remaining = fetches;
    QStringList queue;
    for (int i = 0; i < fetches; ++i) queue << QString::number(i);
    while (!queue.isEmpty()) {
        while (active < concCap && !queue.isEmpty()) {
            queue.takeFirst(); ++active;
            if (active > maxActive) maxActive = active;
        }
        if (active > 0) { --active; --remaining; }
    }

    // Retention model. RED has no global cap; GREEN stops when the next
    // kMaxPlaylistItems-sized fetch would exceed the global budget.
    int retained = 0;
    const long rssBefore = rssKb();
    if (!noRetain) {
        QHash<QString, QVariantList> playlists;
        for (int i = 0; i < fetches; ++i) {
            if (!isRed && retained + perFetch > rowBudget) break;
            playlists[QStringLiteral("PL%1").arg(i, 6, 10, QChar('0'))] = makeRows(i, perFetch);
            retained += perFetch;
        }
        volatile size_t sink = playlists.size();  // keep the hash alive for RSS
        (void)sink;
    }
    const long rssAfter = rssKb();

    printf("{\"mode\":\"%s\",\"n\":%d,\"accepted\":%d,\"fetches\":%d,"
           "\"retained\":%d,\"dedup_us\":%.1f,\"rss_before_kb\":%ld,"
           "\"rss_after_kb\":%ld,\"max_active\":%d,\"remaining\":%d,"
           "\"id_cap\":%d,\"row_budget\":%d,\"rows_per_fetch\":%d,"
           "\"concurrency_cap\":%d,\"err\":\"%s\"}\n",
           isRed ? "red" : "green", n, accepted, fetches, retained,
           dedupUs, rssBefore, rssAfter, maxActive, remaining,
           idCap, rowBudget, perFetch, concCap,
           jsonEscape(err).toLocal8Bit().constData());
    fflush(stdout);
    return 0;
}
