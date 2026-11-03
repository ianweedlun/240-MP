#include "YouTubeBackend.h"
#include "../../util/YtDlpLocator.h"
#include "../../util/YtUrlFresh.h"
#include "../../util/YtBrowseRows.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QProcess>
#include <QSaveFile>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>
#include <functional>

static const char *kSubscriptionsFileName = "youtube_subscriptions.txt";
static const char *kPlaylistsFileName     = "youtube_playlists.txt";

static QString watchUrlFor(const QString &videoId) {
    return QStringLiteral("https://www.youtube.com/watch?v=") + videoId;
}

YouTubeBackend::YouTubeBackend(const QString &appRoot, const QString &dataRoot, QObject *parent)
    : QObject(parent), m_appRoot(appRoot), m_dataRoot(dataRoot)
{
}

// ---------------------------------------------------------------------------
// Subscriptions file
// ---------------------------------------------------------------------------

QStringList YouTubeBackend::readSubscriptionIds(QString *error) const {
    const QString path = m_dataRoot + "/" + kSubscriptionsFileName;
    if (!QFile::exists(path)) {
        if (error)
            *error = QStringLiteral("NO SUBSCRIPTIONS FILE FOUND\n"
                                    "CREATE YOUTUBE_SUBSCRIPTIONS.TXT IN THE DATA DIRECTORY\n"
                                    "WITH ONE CHANNEL ID PER LINE");
        return {};
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("COULD NOT READ YOUTUBE_SUBSCRIPTIONS.TXT");
        return {};
    }
    QStringList ids;
    while (!f.atEnd()) {
        QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        // Be lenient with pasted channel URLs: take the segment after "channel/".
        const int slash = line.indexOf(QLatin1String("channel/"));
        if (slash >= 0) {
            line = line.mid(slash + 8);
            const int end = line.indexOf(QRegularExpression(QStringLiteral("[/?#]")));
            if (end >= 0)
                line = line.left(end);
        }
        if (!line.isEmpty() && !ids.contains(line))
            ids << line;
    }
    if (ids.isEmpty() && error)
        *error = QStringLiteral("NO CHANNELS FOUND IN YOUTUBE_SUBSCRIPTIONS.TXT");
    return ids;
}

QVariantMap YouTubeBackend::check_subscriptions() {
    QString error;
    const QStringList ids = readSubscriptionIds(&error);
    QVariantMap result;
    result["ok"]           = error.isEmpty();
    result["error"]        = error;
    result["fileExists"]   = QFile::exists(m_dataRoot + "/" + kSubscriptionsFileName);
    result["channelCount"] = ids.size();
    return result;
}

// ---------------------------------------------------------------------------
// Loaders — all route through one cache-fill path so a single in-flight
// refresh can serve every waiting view.
// ---------------------------------------------------------------------------

void YouTubeBackend::load_subscriptions_feed(bool forceRefresh) {
    m_emitFeedWhenDone = true;
    ensureFresh(forceRefresh);
}

void YouTubeBackend::load_channels(bool forceRefresh) {
    m_emitChannelsWhenDone = true;
    ensureFresh(forceRefresh);
}

void YouTubeBackend::load_channel_videos(const QString &channelId, bool forceRefresh) {
    m_emitChannelVideosWhenDone = channelId;
    ensureFresh(forceRefresh);
}

void YouTubeBackend::ensureFresh(bool forceRefresh) {
    if (m_pendingChannels > 0)
        return; // refresh already in flight — the emit flags queue on it

    QString error;
    const QStringList ids = readSubscriptionIds(&error);
    if (ids.isEmpty()) {
        m_emitFeedWhenDone     = false;
        m_emitChannelsWhenDone = false;
        m_emitChannelVideosWhenDone.clear();
        emit errorOccurred(error);
        return;
    }
    m_channelOrder = ids;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList stale;
    for (const QString &id : ids) {
        ChannelEntry &entry = m_channels[id];
        entry.channelId = id;
        if (forceRefresh || !entry.feedOk || now - entry.fetchedMs > kCacheTtlMs)
            stale << id;
    }

    if (stale.isEmpty()) {
        finishAggregate(); // everything fresh — serve from cache
        return;
    }
    m_pendingChannels = stale.size();
    for (const QString &id : stale)
        refreshChannel(id);
}

// ---------------------------------------------------------------------------
// Per-channel fetch: the official RSS feed
// ---------------------------------------------------------------------------

QNetworkRequest YouTubeBackend::makeRequest(const QUrl &url) const {
    QNetworkRequest req(url);
    req.setTransferTimeout(10000);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                                 "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"));
    return req;
}

// Atom feed → channel name + video maps (newest first, as served).
// Partial parses are kept: only a parse error with zero entries counts as failure.
static bool parseRssFeed(const QByteArray &data, const QString &channelId,
                         QString *channelName, QVariantList *videos) {
    static const QLatin1String kAtomNs("http://www.w3.org/2005/Atom");
    QXmlStreamReader xml(data);
    bool inEntry = false;
    QString videoId, title, altLink;
    QDateTime published;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = xml.name();
            if (name == QLatin1String("entry")) {
                inEntry = true;
                videoId.clear();
                title.clear();
                altLink.clear();
                published = QDateTime();
            } else if (!inEntry && name == QLatin1String("title") && channelName->isEmpty()) {
                *channelName = xml.readElementText();
            } else if (inEntry && name == QLatin1String("videoId")) {
                videoId = xml.readElementText();
            } else if (inEntry && title.isEmpty() && name == QLatin1String("title")
                       && xml.namespaceUri() == kAtomNs) {
                // namespace check keeps <media:title> (inside media:group) out
                title = xml.readElementText();
            } else if (inEntry && name == QLatin1String("published")) {
                published = QDateTime::fromString(xml.readElementText(), Qt::ISODate);
            } else if (inEntry && name == QLatin1String("link")
                       && xml.attributes().value(QLatin1String("rel")) == QLatin1String("alternate")) {
                // Shorts expose a /shorts/<id> alternate href; normal uploads use /watch?v=<id>
                altLink = xml.attributes().value(QLatin1String("href")).toString();
            }
        } else if (xml.isEndElement() && xml.name() == QLatin1String("entry")) {
            inEntry = false;
            if (videoId.isEmpty())
                continue;
            QVariantMap v;
            v["videoId"]     = videoId;
            v["title"]       = title;
            v["channelId"]   = channelId;
            v["channelName"] = QString(); // filled in once the feed title is known
            v["publishedAt"] = published.isValid() ? published.toUTC().toString(Qt::ISODate)
                                                   : QString();
            v["publishedMs"] = published.isValid() ? published.toMSecsSinceEpoch() : qint64(0);
            v["url"]         = watchUrlFor(videoId);
            v["isShort"]     = altLink.contains(QLatin1String("/shorts/"));
            videos->append(v);
        }
    }
    return !(xml.hasError() && videos->isEmpty());
}

void YouTubeBackend::refreshChannel(const QString &channelId) {
    QUrl rssUrl(QStringLiteral("https://www.youtube.com/feeds/videos.xml"));
    rssUrl.setQuery(QStringLiteral("channel_id=") + channelId);
    QNetworkReply *reply = m_nam.get(makeRequest(rssUrl));
    connect(reply, &QNetworkReply::finished, this, [this, reply, channelId]() {
        reply->deleteLater();
        ChannelEntry &e = m_channels[channelId];
        if (reply->error() == QNetworkReply::NoError) {
            QString name;
            QVariantList videos;
            if (parseRssFeed(reply->readAll(), channelId, &name, &videos)) {
                for (QVariant &v : videos) {
                    QVariantMap m = v.toMap();
                    m["channelName"] = name;
                    v = m;
                }
                e.channelName = name;
                e.videos      = videos;
                e.feedOk      = true;
                e.fetchedMs   = QDateTime::currentMSecsSinceEpoch();
            }
        }
        // On failure: keep any previously cached videos (stale beats empty);
        // fetchedMs stays old so the next load retries this channel.
        if (--m_pendingChannels <= 0) {
            m_pendingChannels = 0;
            finishAggregate();
        }
    });
}

void YouTubeBackend::finishAggregate() {
    const bool    feedWanted     = m_emitFeedWhenDone;
    const bool    channelsWanted = m_emitChannelsWhenDone;
    const QString videosWanted   = m_emitChannelVideosWhenDone;
    m_emitFeedWhenDone     = false;
    m_emitChannelsWhenDone = false;
    m_emitChannelVideosWhenDone.clear();

    bool anyOk = false;
    for (const QString &id : m_channelOrder)
        anyOk = anyOk || m_channels.value(id).feedOk;
    if (!anyOk) {
        emit errorOccurred(QStringLiteral("COULD NOT LOAD SUBSCRIPTIONS\n"
                                          "CHECK YOUR NETWORK CONNECTION"));
        return;
    }

    if (feedWanted)
        emit subscriptionsFeedLoaded(buildFeed());
    if (channelsWanted)
        emit channelsLoaded(buildChannelList());
    if (!videosWanted.isEmpty()) {
        const ChannelEntry entry = m_channels.value(videosWanted);
        if (entry.feedOk)
            emit channelVideosLoaded(videosWanted, entry.videos);
        else
            emit errorOccurred(QStringLiteral("COULD NOT LOAD CHANNEL FEED"));
    }
}

QVariantList YouTubeBackend::buildFeed() const {
    QVariantList all;
    for (const QString &id : m_channelOrder)
        all += m_channels.value(id).videos;
    std::sort(all.begin(), all.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("publishedMs").toLongLong()
             > b.toMap().value("publishedMs").toLongLong();
    });
    return all.mid(0, kMaxFeedItems);
}

QVariantList YouTubeBackend::buildChannelList() const {
    QVariantList channels;
    for (const QString &id : m_channelOrder) {
        const ChannelEntry entry = m_channels.value(id);
        QVariantMap c;
        // Fall back to the raw ID so a channel whose feed failed is still visible
        c["channelId"]  = id;
        c["title"]      = entry.channelName.isEmpty() ? id : entry.channelName;
        c["videoCount"] = entry.videos.size();
        channels << c;
    }
    std::sort(channels.begin(), channels.end(), [](const QVariant &a, const QVariant &b) {
        return QString::compare(a.toMap().value("title").toString(),
                                b.toMap().value("title").toString(),
                                Qt::CaseInsensitive) < 0;
    });
    return channels;
}

// ---------------------------------------------------------------------------
// Playlists file (youtube_playlists.txt)
// Line format: [My Display Name | ] <playlist URL or bare playlist ID>
// ---------------------------------------------------------------------------

// "list=" query param when present, bare token otherwise. A URL without a
// list= param isn't a playlist link — rejected so it can't be fed to yt-dlp
// as something else entirely.
static QString playlistIdFromToken(QString token) {
    const int listPos = token.indexOf(QLatin1String("list="));
    if (listPos >= 0) {
        token = token.mid(listPos + 5);
        const int end = token.indexOf(QRegularExpression(QStringLiteral("[&#?/]")));
        if (end >= 0)
            token = token.left(end);
        return token;
    }
    if (token.contains(QLatin1String("://")))
        return {};
    return token;
}

QList<YouTubeBackend::PlaylistFileRef> YouTubeBackend::readPlaylistEntries(QString *error) const {
    const QString path = m_dataRoot + "/" + kPlaylistsFileName;
    if (!QFile::exists(path)) {
        if (error)
            *error = QStringLiteral("NO PLAYLISTS FILE FOUND\n"
                                    "CREATE YOUTUBE_PLAYLISTS.TXT IN THE DATA DIRECTORY\n"
                                    "WITH ONE PLAYLIST URL PER LINE");
        return {};
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("COULD NOT READ YOUTUBE_PLAYLISTS.TXT");
        return {};
    }
    QList<PlaylistFileRef> refs;
    QStringList seen;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        // Split the optional display-name prefix at the last '|' (URLs never
        // contain one, display names conceivably could).
        QString name, token = line;
        const int bar = line.lastIndexOf('|');
        if (bar >= 0) {
            name  = line.left(bar).trimmed();
            token = line.mid(bar + 1).trimmed();
        }
        const QString id = playlistIdFromToken(token);
        if (id.isEmpty() || seen.contains(id))
            continue;
        seen << id;
        refs.append({id, name});
    }
    if (refs.isEmpty() && error)
        *error = QStringLiteral("NO PLAYLISTS FOUND IN YOUTUBE_PLAYLISTS.TXT");
    return refs;
}

QVariantMap YouTubeBackend::check_playlists() {
    QString error;
    const QList<PlaylistFileRef> refs = readPlaylistEntries(&error);
    QVariantMap result;
    result["ok"]            = error.isEmpty();
    result["error"]         = error;
    result["fileExists"]    = QFile::exists(m_dataRoot + "/" + kPlaylistsFileName);
    result["playlistCount"] = refs.size();
    return result;
}

// ---------------------------------------------------------------------------
// Playlist loaders — yt-dlp --flat-playlist subprocesses feeding the same
// cache/queue shape as the RSS channel path. yt-dlp is used (rather than the
// playlist RSS feed) because the feed stops at 15 entries.
// ---------------------------------------------------------------------------

void YouTubeBackend::load_playlists(bool forceRefresh) {
    m_emitPlaylistsWhenDone = true;
    ensurePlaylistsFresh(forceRefresh);
}

void YouTubeBackend::load_playlist_videos(const QString &playlistId, bool forceRefresh) {
    m_emitPlaylistVideosWhenDone = playlistId;
    ensurePlaylistsFresh(forceRefresh);
}

void YouTubeBackend::ensurePlaylistsFresh(bool forceRefresh) {
    if (m_pendingPlaylists > 0)
        return; // refresh already in flight — the emit flags queue on it

    QString error;
    const QList<PlaylistFileRef> refs = readPlaylistEntries(&error);
    if (refs.isEmpty()) {
        m_emitPlaylistsWhenDone = false;
        m_emitPlaylistVideosWhenDone.clear();
        emit errorOccurred(error);
        return;
    }
    m_playlistOrder.clear();
    for (const PlaylistFileRef &ref : refs) {
        m_playlistOrder << ref.id;
        PlaylistEntry &entry = m_playlists[ref.id];
        entry.playlistId = ref.id;
        entry.fileName   = ref.name; // re-read every refresh so file edits apply
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList stale;
    for (const QString &id : m_playlistOrder) {
        const PlaylistEntry &entry = m_playlists.value(id);
        if (forceRefresh || !entry.fetchOk || now - entry.fetchedMs > kCacheTtlMs)
            stale << id;
    }

    if (stale.isEmpty()) {
        finishPlaylistAggregate(); // everything fresh — serve from cache
        return;
    }
    // Resolve the same user-updatable yt-dlp mpv's ytdl_hook will use at
    // playback time (data-dir drop-in → sibling → PATH), so app and mpv agree.
    if (ytdlp::locate(m_dataRoot).isEmpty()) {
        // Nothing can be fetched; report against whatever the cache holds.
        finishPlaylistAggregate();
        return;
    }
    m_pendingPlaylists   = stale.size();
    m_playlistFetchQueue = stale;
    spawnNextPlaylistFetch();
}

void YouTubeBackend::spawnNextPlaylistFetch() {
    const QString bin = ytdlp::locate(m_dataRoot);
    while (m_activePlaylistFetches < kMaxConcurrentPlaylistFetches
           && !m_playlistFetchQueue.isEmpty()) {
        const QString playlistId = m_playlistFetchQueue.takeFirst();
        ++m_activePlaylistFetches;

        auto *proc = new QProcess(this);
        const QStringList args{
            QStringLiteral("--flat-playlist"),
            QStringLiteral("-I"), QStringLiteral("1:%1").arg(kMaxPlaylistItems),
            QStringLiteral("--no-warnings"),
            // One JSON object per entry — robust against '|' etc. in titles
            QStringLiteral("--print"),
            QStringLiteral("%(.{id,title,channel,uploader,playlist_title})j"),
            QStringLiteral("--"),
            QStringLiteral("https://www.youtube.com/playlist?list=") + playlistId,
        };

        auto finish = [this, proc, playlistId]() {
            proc->deleteLater();
            QString      title;
            QVariantList videos;
            const QList<QByteArray> lines = proc->readAllStandardOutput().split('\n');
            for (const QByteArray &line : lines) {
                const QJsonObject obj = QJsonDocument::fromJson(line.trimmed()).object();
                if (obj.isEmpty())
                    continue;
                if (title.isEmpty())
                    title = obj.value(QLatin1String("playlist_title")).toString();
                const QString videoId    = obj.value(QLatin1String("id")).toString();
                const QString videoTitle = obj.value(QLatin1String("title")).toString();
                if (videoId.isEmpty())
                    continue;
                // Tombstones YouTube leaves in place of removed entries
                if (videoTitle == QLatin1String("[Private video]")
                    || videoTitle == QLatin1String("[Deleted video]"))
                    continue;
                QString channel = obj.value(QLatin1String("channel")).toString();
                if (channel.isEmpty())
                    channel = obj.value(QLatin1String("uploader")).toString();
                QVariantMap v;
                v["videoId"]     = videoId;
                v["title"]       = videoTitle;
                v["channelId"]   = QString();
                v["channelName"] = channel;
                // Flat entries carry no publish date; playlist order stands in
                v["publishedAt"] = QString();
                v["publishedMs"] = qint64(0);
                v["url"]         = watchUrlFor(videoId);
                v["isShort"]     = false; // not detectable from flat entries
                videos.append(v);
            }
            // Non-zero exit with parsed entries still counts (partial page
            // failures on huge lists) — same "partial parses kept" stance as RSS.
            const bool ok = proc->exitStatus() == QProcess::NormalExit
                            && (proc->exitCode() == 0 || !videos.isEmpty());
            if (ok) {
                PlaylistEntry &entry = m_playlists[playlistId];
                entry.fetchedTitle = title;
                entry.videos       = videos;
                entry.fetchOk      = true;
                entry.fetchedMs    = QDateTime::currentMSecsSinceEpoch();
            }
            // On failure: keep any previously cached videos (stale beats empty)

            --m_activePlaylistFetches;
            if (--m_pendingPlaylists <= 0) {
                m_pendingPlaylists      = 0;
                m_activePlaylistFetches = 0;
                m_playlistFetchQueue.clear();
                finishPlaylistAggregate();
            } else {
                spawnNextPlaylistFetch();
            }
        };
        connect(proc, &QProcess::finished, this, finish);
        // finished() is never emitted when the binary fails to launch
        connect(proc, &QProcess::errorOccurred, this,
                [finish](QProcess::ProcessError processError) {
                    if (processError == QProcess::FailedToStart)
                        finish();
                });
        QTimer::singleShot(kPlaylistFetchTimeoutMs, proc, [proc]() { proc->kill(); });
        proc->start(bin, args);
    }
}

void YouTubeBackend::finishPlaylistAggregate() {
    const bool    listWanted   = m_emitPlaylistsWhenDone;
    const QString videosWanted = m_emitPlaylistVideosWhenDone;
    m_emitPlaylistsWhenDone = false;
    m_emitPlaylistVideosWhenDone.clear();

    bool anyOk = false;
    for (const QString &id : m_playlistOrder)
        anyOk = anyOk || m_playlists.value(id).fetchOk;
    if (!anyOk) {
        emit errorOccurred(QStringLiteral("COULD NOT LOAD PLAYLISTS\n"
                                          "CHECK YOUR NETWORK CONNECTION AND THAT\n"
                                          "YT-DLP IS INSTALLED AND UP TO DATE"));
        return;
    }

    if (listWanted)
        emit playlistsLoaded(buildPlaylistList());
    if (!videosWanted.isEmpty()) {
        const PlaylistEntry entry = m_playlists.value(videosWanted);
        if (entry.fetchOk)
            emit playlistVideosLoaded(videosWanted, entry.videos);
        else
            emit errorOccurred(QStringLiteral("COULD NOT LOAD PLAYLIST"));
    }
}

QVariantList YouTubeBackend::buildPlaylistList() const {
    QVariantList playlists;
    for (const QString &id : m_playlistOrder) {
        const PlaylistEntry entry = m_playlists.value(id);
        QVariantMap p;
        p["playlistId"] = id;
        // File-name override wins; fall back to the raw ID so a playlist whose
        // fetch failed is still visible (same choice as buildChannelList)
        p["title"]      = !entry.fileName.isEmpty()     ? entry.fileName
                        : !entry.fetchedTitle.isEmpty() ? entry.fetchedTitle
                                                        : id;
        p["videoCount"] = entry.videos.size();
        playlists << p;
    }
    return playlists; // file order — the user's own curation is the sort
}

// ---------------------------------------------------------------------------
// Playback resolution → yt-dlp format
// ---------------------------------------------------------------------------

QString YouTubeBackend::ytdlFormatForResolution(const QString &resolution) const {
    int height = 480;
    if (resolution == QLatin1String("720p"))
        height = 720;
    else if (resolution == QLatin1String("1080p"))
        height = 1080;
    // H.264 first (RPi hardware decode), then any codec at the cap, then best
    return QStringLiteral("bestvideo[height<=?%1][vcodec^=avc1]+bestaudio/"
                          "bestvideo[height<=?%1]+bestaudio/"
                          "best[height<=?%1]/best")
        .arg(height);
}

// ---------------------------------------------------------------------------
// Watch history (youtube_history.json, keyed by videoId)
// Entry: { pos: <ms>, title, channelName, lastPlayed: <epoch ms> }
// Legacy pos-only entries are tolerated: they resume fine but are skipped by
// the History list (nothing to display) and pruned first (lastPlayed 0).
// ---------------------------------------------------------------------------

QString YouTubeBackend::historyFilePath() const {
    return m_dataRoot + "/youtube_history.json";
}

QVariantMap YouTubeBackend::loadHistory() const {
    QFile file(historyFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object().toVariantMap();
}

void YouTubeBackend::saveHistory(const QVariantMap &history) {
    QFile file(historyFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(QJsonDocument(QJsonObject::fromVariantMap(history)).toJson(QJsonDocument::Compact));
}

QVariantMap YouTubeBackend::getSavedPosition(const QString &videoId) {
    const QVariant val = loadHistory().value(videoId);
    if (!val.isValid())
        return {};
    return val.toMap();
}

void YouTubeBackend::savePosition(const QString &videoId, int positionMs,
                                  const QString &title, const QString &channelName) {
    QVariantMap history = loadHistory();
    QVariantMap entry;
    entry["pos"]         = positionMs;
    entry["title"]       = title;
    entry["channelName"] = channelName;
    entry["lastPlayed"]  = QDateTime::currentMSecsSinceEpoch();
    history[videoId] = entry;

    if (history.size() > kMaxHistoryItems) {
        QStringList keys = history.keys();
        std::sort(keys.begin(), keys.end(), [&history](const QString &a, const QString &b) {
            return history.value(a).toMap().value("lastPlayed").toLongLong()
                 > history.value(b).toMap().value("lastPlayed").toLongLong();
        });
        for (int i = kMaxHistoryItems; i < keys.size(); ++i)
            history.remove(keys[i]);
    }
    saveHistory(history);
}

QVariantList YouTubeBackend::getHistory() const {
    const QVariantMap history = loadHistory();
    QVariantList items;
    for (auto it = history.begin(); it != history.end(); ++it) {
        const QVariantMap entry = it.value().toMap();
        const QString title = entry.value("title").toString();
        if (title.isEmpty())
            continue; // legacy resume-only entry — nothing to display
        QVariantMap v;
        v["videoId"]     = it.key();
        v["title"]       = title;
        v["channelName"] = entry.value("channelName").toString();
        v["lastPlayed"]  = entry.value("lastPlayed").toLongLong();
        v["url"]         = watchUrlFor(it.key());
        items << v;
    }
    std::sort(items.begin(), items.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("lastPlayed").toLongLong()
             > b.toMap().value("lastPlayed").toLongLong();
    });
    return items;
}

void YouTubeBackend::delete_history() {
    QFile::remove(historyFilePath());
}

// ---------------------------------------------------------------------------
// Watch later (youtube_watch_later.json — JSON array, newest-saved first)
// Entry: { videoId, title, channelName, addedMs }
// ---------------------------------------------------------------------------

QString YouTubeBackend::watchLaterFilePath() const {
    return m_dataRoot + "/youtube_watch_later.json";
}

QVariantList YouTubeBackend::loadWatchLater() const {
    QFile file(watchLaterFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).array().toVariantList();
}

void YouTubeBackend::saveWatchLater(const QVariantList &list) {
    QFile file(watchLaterFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(QJsonDocument(QJsonArray::fromVariantList(list)).toJson(QJsonDocument::Compact));
}

QVariantList YouTubeBackend::getWatchLater() const {
    QVariantList items = loadWatchLater();
    for (QVariant &v : items) {
        QVariantMap m = v.toMap();
        m["url"] = watchUrlFor(m.value("videoId").toString());
        v = m;
    }
    return items;
}

bool YouTubeBackend::isInWatchLater(const QString &videoId) const {
    const QVariantList list = loadWatchLater();
    for (const QVariant &v : list) {
        if (v.toMap().value("videoId").toString() == videoId)
            return true;
    }
    return false;
}

void YouTubeBackend::addToWatchLater(const QString &videoId, const QString &title,
                                     const QString &channelName) {
    if (videoId.isEmpty() || isInWatchLater(videoId))
        return;
    QVariantList list = loadWatchLater();
    QVariantMap entry;
    entry["videoId"]     = videoId;
    entry["title"]       = title;
    entry["channelName"] = channelName;
    entry["addedMs"]     = QDateTime::currentMSecsSinceEpoch();
    list.prepend(entry);
    saveWatchLater(list);
}

void YouTubeBackend::removeFromWatchLater(const QString &videoId) {
    QVariantList list = loadWatchLater();
    for (int i = list.size() - 1; i >= 0; --i) {
        if (list[i].toMap().value("videoId").toString() == videoId)
            list.removeAt(i);
    }
    if (list.isEmpty())
        QFile::remove(watchLaterFilePath());
    else
        saveWatchLater(list);
}

void YouTubeBackend::delete_watch_later() {
    QFile::remove(watchLaterFilePath());
}

// ---------------------------------------------------------------------------
// Direct-playback path (issue #28 plan A). mpv's ytdl_hook re-resolves the
// signed stream URL on every launch; the app pays that at each play. This
// resolves with yt-dlp ahead of time (fire-and-forget) and keeps the URL
// under its own expire= epoch; the Player takes the cached URL plus
// --ytdl=no while it is fresh, else falls back to the ytdl path unchanged.

QString YouTubeBackend::urlCacheFilePath() const
{
    return m_dataRoot + QStringLiteral("/youtube_url_cache.json");
}

void YouTubeBackend::ensureUrlCacheLoaded()
{
    if (m_urlCacheLoaded)
        return;
    m_urlCacheLoaded = true;
    QFile f(urlCacheFilePath());
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
        if (doc.isObject())
            m_urlCache = doc.object();
    }
    // Dead windows never come back; prune on load so the file stays small.
    yturl::pruneUrlCache(m_urlCache, QDateTime::currentMSecsSinceEpoch() / 1000,
                        kUrlCacheMaxEntries);
}

QString YouTubeBackend::cachedUrlField(const QString &videoId,
                                       const QString &resolution,
                                       const QString &field)
{
    if (videoId.isEmpty())
        return QString();
    ensureUrlCacheLoaded();
    const QString key = yturl::cacheKeyFor(videoId, ytdlFormatForResolution(resolution));
    const QJsonValue entry = m_urlCache.value(key);
    if (!entry.isObject())
        return QString();
    const QJsonObject obj = entry.toObject();
    const QString url = obj.value(field).toString();
    if (url.isEmpty()
        || !yturl::urlFresh(static_cast<qint64>(obj.value(QStringLiteral("expire")).toDouble()),
                            QDateTime::currentMSecsSinceEpoch() / 1000,
                            kUrlSafetyMarginS))
        return QString();
    return url;
}

QString YouTubeBackend::cachedPlaybackUrl(const QString &videoId,
                                         const QString &resolution)
{
    return cachedUrlField(videoId, resolution, QStringLiteral("url"));
}

QString YouTubeBackend::cachedPlaybackAudioUrl(const QString &videoId,
                                              const QString &resolution)
{
    return cachedUrlField(videoId, resolution, QStringLiteral("audioUrl"));
}

bool YouTubeBackend::cachedPlaybackSingle(const QString &videoId,
                                         const QString &resolution)
{
    if (videoId.isEmpty())
        return false;
    ensureUrlCacheLoaded();
    const QString key = yturl::cacheKeyFor(videoId, ytdlFormatForResolution(resolution));
    const QJsonValue entry = m_urlCache.value(key);
    if (!entry.isObject())
        return false;
    const QJsonObject obj = entry.toObject();
    return obj.value(QStringLiteral("single")).toInt() == 1
        && yturl::urlFresh(static_cast<qint64>(obj.value(QStringLiteral("expire")).toDouble()),
                           QDateTime::currentMSecsSinceEpoch() / 1000, kUrlSafetyMarginS);
}

void YouTubeBackend::storeUrlCacheEntry(const QString &key, const QString &url,
                                       const QString &audioUrl, bool single, qint64 expireEpoch)
{
    QJsonObject entry;
    entry.insert(QStringLiteral("url"), url);
    if (!audioUrl.isEmpty())
        entry.insert(QStringLiteral("audioUrl"), audioUrl);
    if (single)
        entry.insert(QStringLiteral("single"), 1); // one muxed stream: no audio half needed
    entry.insert(QStringLiteral("expire"), static_cast<qint64>(expireEpoch));
    m_urlCache.insert(key, entry);
    yturl::pruneUrlCache(m_urlCache, QDateTime::currentMSecsSinceEpoch() / 1000,
                        kUrlCacheMaxEntries);
    QFile f(urlCacheFilePath());
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(QJsonDocument(m_urlCache).toJson(QJsonDocument::Compact));
}

bool YouTubeBackend::hasFreshUrlPair(const QString &key) const
{
    const QJsonValue entry = m_urlCache.value(key);
    if (!entry.isObject())
        return false;
    // Entries written before the audio-URL pairing (issue #28 round 2) carry
    // no audioUrl — treat them as misses so the re-resolve below stores the
    // pair instead of letting mpv play video-only silence.
    const QJsonObject obj = entry.toObject();
    // Entries without the paired audio half OR the single-stream marker predate
    // the pairing (issue #28 round 2): treat them as misses so the re-resolve
    // below stores a complete playable entry instead of a video-only silence.
    const bool playable = obj.contains(QStringLiteral("audioUrl"))
                          || obj.value(QStringLiteral("single")).toInt() == 1;
    return playable
        && yturl::urlFresh(static_cast<qint64>(obj.value(QStringLiteral("expire")).toDouble()),
                           QDateTime::currentMSecsSinceEpoch() / 1000, kUrlSafetyMarginS);
}

QString YouTubeBackend::warmToken(const QString &videoId,
                                 const QString &resolution) const
{
    return videoId + QChar(0x1F) + resolution; // US separator: absent from ids and format names
}

void YouTubeBackend::launchUrlResolve(const QString &videoId, const QString &resolution,
                                     const QString &fmt, const QString &key)
{
    const QString bin = ytdlp::locate(m_dataRoot);
    if (bin.isEmpty()) {
        m_resolveKeys.remove(key);
        if (m_warmChainKey == key) {
            m_warmChainKey.clear();
            pumpWarmQueue();
        }
        return;
    }

    auto *proc = new QProcess(this);
    const QStringList args{
        QStringLiteral("-g"),
        QStringLiteral("--no-playlist"),
        QStringLiteral("--no-warnings"),
        QStringLiteral("-f"), fmt,
        QStringLiteral("--"),
        watchUrlFor(videoId),
    };
    connect(proc, &QProcess::finished, this, [this, proc, key, videoId]() {
        // yt-dlp -g prints one URL per selected format: the usual
        // bestvideo+bestaudio split prints video on line 1 and audio on line 2;
        // a combined best[...] fallback prints just one. Both lines carry their
        // own expire= epoch; the entry lives under the EARLIER one, since a
        // dead audio URL kills playback exactly as dead video does.
        const QList<QByteArray> lines = proc->readAllStandardOutput().split('\n');
        QStringList urls;
        for (const QByteArray &line : lines) {
            const QString u = QString::fromUtf8(line).trimmed();
            if (!u.isEmpty())
                urls << u;
        }
        if (!urls.isEmpty()) {
            const QString videoUrl = urls.value(0);
            const QString audioUrl = urls.value(1); // empty for combined formats
            qint64 expire = yturl::expireEpochFromUrl(videoUrl);
            if (!audioUrl.isEmpty()) {
                const qint64 audioExpire = yturl::expireEpochFromUrl(audioUrl);
                if (audioExpire > 0 && (expire <= 0 || audioExpire < expire))
                    expire = audioExpire;
            }
            if (expire > QDateTime::currentMSecsSinceEpoch() / 1000) {
                storeUrlCacheEntry(key, videoUrl, audioUrl, urls.size() == 1, expire);
                emit playbackUrlReady(videoId);
            }
        }
        finishUrlResolve(key);
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, key](QProcess::ProcessError processError) {
                if (processError == QProcess::FailedToStart) {
                    // launch failure: no finished signal, so unwind bookkeeping here
                    finishUrlResolve(key);
                    proc->deleteLater();
                }
            });
    QTimer::singleShot(kYtdlResolveTimeoutMs, proc, [proc]() { proc->kill(); });
    proc->start(bin, args);
}

void YouTubeBackend::finishUrlResolve(const QString &key)
{
    m_resolveKeys.remove(key);
    if (m_warmChainKey == key) {
        m_warmChainKey.clear();
        pumpWarmQueue(); // this warm entry just owned the chain — continue it
    }
}

void YouTubeBackend::pumpWarmQueue()
{
    while (!m_warmQueue.isEmpty()) {
        const QString token = m_warmQueue.takeFirst();
        const int sep = token.indexOf(QChar(0x1F));
        const QString videoId    = token.left(sep);
        const QString resolution = token.mid(sep + 1);
        const QString fmt = ytdlFormatForResolution(resolution);
        const QString key = yturl::cacheKeyFor(videoId, fmt);
        if (hasFreshUrlPair(key) || m_resolveKeys.contains(key))
            continue; // someone else warmed it meanwhile — next entry
        m_warmChainKey = key;
        m_resolveKeys.insert(key);
        launchUrlResolve(videoId, resolution, fmt, key);
        return;
    }
}

void YouTubeBackend::warmListPlayback(const QStringList &videoIds, const QString &resolution)
{
    // One speculative resolve pass over the front of a freshly presented list.
    // A single chained yt-dlp process at a time (kWarmQueueMax bounds the
    // backlog; kWarmListMax the front per call): browsing a list is slower
    // than a resolve, so by the time a row is chosen its URL pair is very
    // likely cached and Player takes its direct path.
    ensureUrlCacheLoaded();
    const QString fmt = ytdlFormatForResolution(resolution);
    int queued = 0;
    for (const QString &videoId : videoIds) {
        if (videoId.isEmpty() || queued >= kWarmListMax || m_warmQueue.size() >= kWarmQueueMax)
            break;
        const QString key = yturl::cacheKeyFor(videoId, fmt);
        if (hasFreshUrlPair(key) || m_resolveKeys.contains(key))
            continue;
        const QString token = warmToken(videoId, resolution);
        if (m_warmQueue.contains(token))
            continue;
        m_warmQueue << token;
        ++queued;
    }
    if (m_warmChainKey.isEmpty())
        pumpWarmQueue();
}

void YouTubeBackend::resolvePlayback(const QString &videoId, const QString &resolution)
{
    if (videoId.isEmpty())
        return;
    ensureUrlCacheLoaded();
    const QString fmt = ytdlFormatForResolution(resolution);
    const QString key = yturl::cacheKeyFor(videoId, fmt);
    if (hasFreshUrlPair(key) || m_resolveKeys.contains(key))
        return; // fresh window already on file, or a resolve is already in flight
    m_resolveKeys.insert(key);
    launchUrlResolve(videoId, resolution, fmt, key);
}

void YouTubeBackend::warmResumePlayback(const QString &resolution)
{
    const QVariantMap history = loadHistory();
    QString bestVideoId;
    qint64 bestLastPlayed = 0;
    for (auto it = history.begin(); it != history.end(); ++it) {
        const QVariantMap entry = it.value().toMap();
        if (entry.value(QStringLiteral("pos")).toInt() <= 0)
            continue; // finished videos never resume
        const qint64 lastPlayed = entry.value(QStringLiteral("lastPlayed")).toLongLong();
        if (lastPlayed > bestLastPlayed) {
            bestLastPlayed = lastPlayed;
            bestVideoId    = it.key();
        }
    }
    if (!bestVideoId.isEmpty())
        resolvePlayback(bestVideoId, resolution);
}

// ---------- Plan D: device-flow sign-in + account lists (issue #28) ----------
// Client pair verbatim from the Kodi plugin.video.youtube add-on's
// CONFIGS['youtube-tv'] (public, same family every limited-input client uses);
// credited in THANKS.md. Endpoints verified live 2026-11-03. Token values are
// never logged; only state words cross signal boundaries.

static const QString kTvClientId =
    QStringLiteral("861556708454-d6dlm3lh05idd8npek18k6be8ba3oc68.apps.googleusercontent.com");
static const QString kTvClientSecret =
    QStringLiteral("SboVhoG9s0rNafixCSGGKXAT");
static const QString kInnerTubeTvKey =
    QStringLiteral("AIzaSyAd-YEOqZz9nXVzGtn3KWzYLbLaajhqIDA");
static const QString kDeviceCodeEndpoint =
    QStringLiteral("https://www.youtube.com/o/oauth2/device/code");
static const QString kTokenEndpoint =
    QStringLiteral("https://www.youtube.com/o/oauth2/token");
static constexpr int kPairPollMax = 120;          // 10 min at 5 s polls
static constexpr int kAuthSlackS = 120;           // refresh before expiry
static constexpr int kBrowseMaxPages = 5;         // continuation cap
static constexpr int kBrowseMaxRows = 200;        // hard display cap

// The watch-later cursor hunt lives in YtBrowseRows.h (ytui::findWatchLaterCursor)
// next to the browse-row parser: pure JSON logic, unit-tested, and covering both
// the desktop playlistId/VL shapes AND the TVHTML5 tab chip whose "cursor" is a
// raw reloadContinuationData continuation.

QString YouTubeBackend::authFilePath() const
{
    return m_dataRoot + QStringLiteral("/youtube_auth.json");
}

void YouTubeBackend::ensureAuthLoaded()
{
    if (mAuthLoaded)
        return;
    mAuthLoaded = true;
    QFile f(authFilePath());
    if (!f.open(QIODevice::ReadOnly))
        return; // no auth yet — signed-out is the default state
    const QJsonObject doc = QJsonDocument::fromJson(f.readAll()).object();
    m_accessToken         = doc.value(QStringLiteral("access")).toString();
    m_refreshToken        = doc.value(QStringLiteral("refresh")).toString();
    m_accessExpiresEpoch  = static_cast<qint64>(doc.value(QStringLiteral("expires")).toDouble());
}

void YouTubeBackend::saveAuthFile()
{
    const QString path = authFilePath();
    QJsonObject doc;
    if (!m_accessToken.isEmpty())
        doc.insert(QStringLiteral("access"), m_accessToken);
    if (!m_refreshToken.isEmpty())
        doc.insert(QStringLiteral("refresh"), m_refreshToken);
    if (m_accessExpiresEpoch > 0)
        doc.insert(QStringLiteral("expires"), static_cast<double>(m_accessExpiresEpoch));
    // QSaveFile = write to a private temp in the same directory, then commit()
    // renames it over the target ATOMICALLY. A hand-rolled QFile tmp + QFile::
    // rename silently failed on the Pi when the target already existed (left a
    // .tmp next to a stale `{}` doc — the 2026-11-03 signed-but-unpersisted
    // auth incident; the token was rescued by a manual rename on the device).
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning("[Youtube] auth file open failed: %s", qPrintable(path));
        return;
    }
    // QSaveFile has no text-mode setter in Qt 6; the doc is single-line
    // compact JSON so line endings never come into play.
    f.write(QJsonDocument(doc).toJson(QJsonDocument::Compact));
    if (!f.commit())
        qWarning("[Youtube] auth file commit failed: %s", qPrintable(path));
}

// Pair-state accessors so a freshly-opened view can re-read a pending
// device flow without restarting it (the card must show the code that's already
// polling, not ask the user to redo a flow that is running).
bool YouTubeBackend::pairActive()
{
    return m_pairActive;
}

QString YouTubeBackend::pairUserCode()
{
    return m_pairUserCode;
}

QString YouTubeBackend::pairVerificationUrl()
{
    return m_pairVerifyUrl;
}

bool YouTubeBackend::signedIn()
{
    ensureAuthLoaded();
    if (m_accessToken.isEmpty())
        return false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch() / 1000;
    return m_accessExpiresEpoch <= 0 || now + kAuthSlackS < m_accessExpiresEpoch;
}

QVariantMap YouTubeBackend::check_signin()
{
    ensureAuthLoaded();
    QVariantMap m;
    m[QStringLiteral("signedIn")]      = !m_accessToken.isEmpty()
            && (m_accessExpiresEpoch <= 0
                || QDateTime::currentMSecsSinceEpoch() / 1000 + kAuthSlackS < m_accessExpiresEpoch);
    m[QStringLiteral("hasRefresh")]    = !m_refreshToken.isEmpty();
    m[QStringLiteral("pairingActive")] = m_pairActive;
    return m;
}

void YouTubeBackend::deviceSignOut()
{
    if (!m_refreshToken.isEmpty()) {
        // Best-effort revoke; the local clear is what the app honors.
        QNetworkRequest req{QUrl(QStringLiteral("https://www.youtube.com/o/oauth2/revoke"))};
        req.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
        QUrlQuery revokeQuery{
            {QStringLiteral("token"), m_refreshToken},
            {QStringLiteral("key"), kInnerTubeTvKey}};
        QNetworkReply *reply = m_nam.post(req, revokeQuery.toString().toUtf8());
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
        });
    }
    m_accessToken.clear();
    m_refreshToken.clear();
    m_accessExpiresEpoch = 0;
    saveAuthFile();
    emit deviceSignInFinished(false, QStringLiteral("signed-out"));
}

void YouTubeBackend::startDeviceSignIn()
{
    if (m_pairActive || signedIn()) {
        qWarning("[Youtube] start ignored — pairing already active or signed in");
        return; // one flow at a time; already signed in does nothing
    }
    m_pairActive = true;
    qWarning("[Youtube] device pair requested");
    requestDeviceCode();
}

void YouTubeBackend::requestDeviceCode()
{
    QNetworkRequest req{QUrl(kDeviceCodeEndpoint)};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));
    QUrlQuery deviceQuery{
        {QStringLiteral("client_id"), kTvClientId},
        {QStringLiteral("scope"), QStringLiteral("https://www.googleapis.com/auth/youtube")}};
    QNetworkReply *reply = m_nam.post(req, deviceQuery.toString().toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() != QNetworkReply::NoError)
            qWarning("[Youtube] device-code request failed: %s", qPrintable(reply->errorString()));
        const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        const QString userCode     = doc.value(QStringLiteral("user_code")).toString();
        m_pairDeviceCode           = doc.value(QStringLiteral("device_code")).toString();
        const QString url          = doc.value(QStringLiteral("verification_url")).toString();
        const int interval         = doc.value(QStringLiteral("interval")).toInt();
        const int expiresIn        = doc.value(QStringLiteral("expires_in")).toInt();
        if (userCode.isEmpty() || m_pairDeviceCode.isEmpty()) {
            m_pairActive = false;
            qWarning("[Youtube] device-code response had no codes — endpoint rejected request");
            emit deviceSignInFinished(false, QStringLiteral("PAIRING-REQUEST-FAILED"));
            return;
        }
        m_pairUserCode = userCode;
        m_pairVerifyUrl = url.isEmpty() ? QStringLiteral("https://www.google.com/device") : url;
        qWarning("[Youtube] pair card issued: open %s and type the code",
                 qPrintable(m_pairVerifyUrl));
        m_pollIntervalS = interval > 0 ? interval : 5;
        m_pollsLeft     = qMin(kPairPollMax, expiresIn > 0 ? expiresIn / m_pollIntervalS : kPairPollMax);
        emit devicePairRequested(userCode, url.isEmpty()
                                 ? QStringLiteral("https://www.google.com/device") : url);
        schedulePairPoll();
    });
}

void YouTubeBackend::schedulePairPoll()
{
    if (!m_pairActive)
        return;
    QTimer::singleShot(qint64(m_pollIntervalS) * 1000, this, [this]() {
        if (!m_pairActive)
            return;
        QNetworkRequest req{QUrl(kTokenEndpoint)};
        req.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
        QUrlQuery pairQuery{
            {QStringLiteral("client_id"), kTvClientId},
            {QStringLiteral("client_secret"), kTvClientSecret},
            {QStringLiteral("code"), m_pairDeviceCode},
            {QStringLiteral("grant_type"), QStringLiteral("http://oauth.net/grant_type/device/1.0")}};
        QNetworkReply *reply = m_nam.post(req, pairQuery.toString().toUtf8());
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
            reply->deleteLater();
            finishPairResponse(doc);
        });
    });
}

void YouTubeBackend::finishPairResponse(const QJsonObject &doc)
{
    const QString token = doc.value(QStringLiteral("access_token")).toString();
    if (!token.isEmpty()) {
        m_accessToken         = token;
        m_refreshToken        = doc.value(QStringLiteral("refresh_token")).toString();
        const int expiresIn   = doc.value(QStringLiteral("expires_in")).toInt();
        m_accessExpiresEpoch  = expiresIn > 0
                ? QDateTime::currentMSecsSinceEpoch() / 1000 + expiresIn : 0;
        m_pairActive = false;
        saveAuthFile();
        qWarning("[Youtube] device flow paired — account lists now available");
        emit deviceSignInFinished(true, QString());
        return;
    }
    const QString err = doc.value(QStringLiteral("error")).toString();
    if (err == QStringLiteral("authorization_pending")
        || err == QStringLiteral("access_denied_temporary")) {
        if (m_pairActive && --m_pollsLeft > 0)
            schedulePairPoll();
        else
            m_pairActive = false; // code expired client-side; user_code dies anyway
        return;
    }
    m_pairActive = false;
    qWarning("[Youtube] device flow declined: %s",
             qPrintable(err.isEmpty() ? QStringLiteral("PAIRING-FAILED") : err));
    emit deviceSignInFinished(false, err.isEmpty() ? QStringLiteral("PAIRING-FAILED")
                                                  : err.toUpper().replace(QLatin1Char(' '), QLatin1Char('-')));
}

void YouTubeBackend::cancelDeviceSignIn()
{
    m_pairActive = false; // an in-flight poll checks the flag on completion
}

void YouTubeBackend::requestRefresh()
{
    if (m_refreshToken.isEmpty())
        return;
    QNetworkRequest req{QUrl(kTokenEndpoint)};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));
    QUrlQuery refreshQuery{
        {QStringLiteral("client_id"), kTvClientId},
        {QStringLiteral("client_secret"), kTvClientSecret},
        {QStringLiteral("refresh_token"), m_refreshToken},
        {QStringLiteral("grant_type"), QStringLiteral("refresh_token")}};
    QNetworkReply *reply = m_nam.post(req, refreshQuery.toString().toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QJsonObject doc = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        const QString token = doc.value(QStringLiteral("access_token")).toString();
        if (!token.isEmpty()) {
            m_accessToken         = token;
            const int expiresIn   = doc.value(QStringLiteral("expires_in")).toInt();
            m_accessExpiresEpoch  = expiresIn > 0
                    ? QDateTime::currentMSecsSinceEpoch() / 1000 + expiresIn : 0;
            // a rotated refresh_token replaces the old one when returned
            const QString rotated = doc.value(QStringLiteral("refresh_token")).toString();
            if (!rotated.isEmpty())
                m_refreshToken = rotated;
            saveAuthFile();
            if (!m_pendingListKind.isEmpty()) {
                const QString kind = m_pendingListKind;
                m_pendingListKind.clear();
                loadAccountList(kind); // retry now that the token is live
            } else if (!m_pendingPlaylistVideos.isEmpty()) {
                const QString id = m_pendingPlaylistVideos;
                m_pendingPlaylistVideos.clear();
                loadAccountPlaylistVideos(id); // same re-entry, videos path
            }
        } else {
            m_accessToken = QString(); // dead auth — views fall back to files
            saveAuthFile();
        }
    });
}

QString YouTubeBackend::bearerQuery() const
{
    return QStringLiteral("?key=") + kInnerTubeTvKey;
}

void YouTubeBackend::loadAccountList(const QString &kind)
{
    ensureAuthLoaded();
    if (m_accessToken.isEmpty()) {
        emit errorOccurred(QStringLiteral("NOT SIGNED IN"));
        return;
    }
    m_pendingListKind = kind;
    if (m_accessExpiresEpoch > 0
        && QDateTime::currentMSecsSinceEpoch() / 1000 + kAuthSlackS >= m_accessExpiresEpoch) {
        requestRefresh(); // the list re-enters from requestRefresh's success leg
        return;
    }
    // feed/history/watch-later have stable entry points; the playlists
    // account surface is the LIBRARY (FElibrary) — the account home's Playlists
    // tab chip is unselectable and cursor-less on the TV client (verified
    // against the live session on the Pi; a chip hunt there is a dead end).
    const QString startId = kind == QStringLiteral("history") ? QStringLiteral("FEhistory")
                            : kind == QStringLiteral("feed") ? QStringLiteral("FEsubscriptions")
                                                             : kind == QStringLiteral("playlists") ? QStringLiteral("FElibrary")
                                                              : QStringLiteral("FEmy_youtube");
    browsePage(kind, startId, 1, QVariantList());
}

void YouTubeBackend::browsePage(const QString &browseKind, const QString &cursor,
                                int page, const QVariantList &acc)
{
    if (m_accessToken.isEmpty())
        return;
    QJsonObject body;
    body.insert(QStringLiteral("context"), QJsonObject{{QStringLiteral("client"), QJsonObject{
        {QStringLiteral("clientName"), QStringLiteral("TVHTML5")},
        {QStringLiteral("clientVersion"), QStringLiteral("7.20210818.00.00-ks")},
        {QStringLiteral("hl"), QStringLiteral("en")},
        {QStringLiteral("gl"), QStringLiteral("US")}}}});
    if (cursor.startsWith(QStringLiteral("VL")) || cursor.startsWith(QStringLiteral("FE"))
        || cursor.startsWith(QStringLiteral("UC")) || cursor.startsWith(QStringLiteral("PU"))
        || cursor.startsWith(QStringLiteral("RD")))
        body.insert(QStringLiteral("browseId"), cursor);
    else
        body.insert(QStringLiteral("continuation"), cursor);
    QNetworkRequest req{QUrl(QStringLiteral("https://www.youtube.com/youtubei/v1/browse")
                             + bearerQuery())};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // The session token is what makes FEsubscriptions/FEhistory account-truth;
    // without it those browseIds 400 server-side (observed live). setRawHeader
    // is the repo's cross-Qt-version header pattern (Emby uses it too).
    req.setRawHeader("Authorization",
                     QStringLiteral("Bearer %1").arg(m_accessToken).toUtf8());
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, browseKind, page, acc]() {
        finishBrowsePage(browseKind, reply, acc, page);
    });
}

void YouTubeBackend::finishBrowsePage(const QString &kind, QNetworkReply *reply,
                                     QVariantList acc, int page)
{
    const bool netError = (reply->error() != QNetworkReply::NoError);
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    reply->deleteLater();
    const QJsonObject root = doc.object();
    if (kind == QStringLiteral("watchlater") && acc.isEmpty() && page == 1) {
        // Discovery hop: the account's watch-later playlist id lives on the
        // signed-in my-youtube page next to its title. No stable browseId
        // exists — this is what makes the account list truly account-truth.
        const QJsonValue wlId = ytui::findWatchLaterCursor(root);
        if (wlId.isNull()) {
            qWarning("[Youtube] watch-later discovery miss on the account home (http %d) — falling back",
                     netError ? int(reply->error()) : 0);
            emit errorOccurred(QStringLiteral("WATCH-LATER-NOT-FOUND"));
            return; // the QML view keeps the upstream file display
        }
        browsePage(kind, wlId.toString(), 2, acc);
        return;
    }
    if (kind == QStringLiteral("playlists")) {
        // The tab lists PLAYLISTS, not videos — different extractor, and the
        // video-row walker must not run here (its tiles are playlist tiles).
        // FElibrary mixes in auto-mixes and chips; only PL…/LL are the user's.
        acc += ytui::keepAccountPlaylists(
                   ytui::extractPlaylistRows(root, kBrowseMaxRows - acc.size()));
    } else {
        acc += ytui::extractBrowseRows(root, kBrowseMaxRows - acc.size());
    }
    const QString next = ytui::continuationToken(root);
    // One line per page, always: the account lists were observed empty on the
    // Pi after pairing, and the ONLY unproven link was the response shape
    // (parse failures are silent by design — a genuinely empty account looks
    // identical). This log makes the shape visible without instrumenting the
    // parser: rows count, http status, and any server error object.
    qWarning("[Youtube] browse kind=%s page=%d rows=%lld http_ok=%d continued=%d",
             qPrintable(kind), page, static_cast<long long>(acc.size()), netError ? 0 : 1,
             next.isEmpty() ? 0 : 1);
    if (acc.size() < kBrowseMaxRows && !next.isEmpty() && page < kBrowseMaxPages) {
        browsePage(kind, next, page + 1, acc);
        return;
    }
    // An empty signed-in list is legal; only the network/HTTP surface is an
    // error — decided by the reply itself, never a heuristic. Empty valid pages
    // MUST still emit (the view renders its "no videos" state); a page that
    // parses nothing used to leave the view stuck LOADING forever — one of the
    // observed "doesn't load" symptoms.
    if (netError || root.value(QStringLiteral("error")).isObject()) {
        emit errorOccurred(QStringLiteral("AUTH-LIST-FAILED"));
    } else if (kind == QStringLiteral("feed")) {
        emit accountFeedLoaded(acc);
    } else if (kind == QStringLiteral("history")) {
        emit accountHistoryLoaded(acc);
    } else if (kind == QStringLiteral("playlists")) {
        emit accountPlaylistsLoaded(acc);
    } else if (kind == QStringLiteral("playlistvideos")) {
        // Account playlist contents reuse the file path's signal so the
        // module views keep ONE handler for both sources; the id echoed
        // back is exactly what loadAccountPlaylistVideos() was handed.
        emit playlistVideosLoaded(m_accountVideosEmitId, acc);
    } else {
        emit accountWatchLaterLoaded(acc);
    }
}

void YouTubeBackend::loadAccountPlaylistVideos(const QString &playlistId)
{
    ensureAuthLoaded();
    if (m_accessToken.isEmpty()) {
        emit errorOccurred(QStringLiteral("NOT SIGNED IN"));
        return;
    }
    if (playlistId.isEmpty())
        return;
    if (m_accessExpiresEpoch > 0
        && QDateTime::currentMSecsSinceEpoch() / 1000 + kAuthSlackS >= m_accessExpiresEpoch) {
        m_pendingPlaylistVideos = playlistId; // re-enters from requestRefresh's success leg
        requestRefresh();
        return;
    }
    // Account playlist ids come as PL…/UU…/LL/RD…; contents are browsed with
    // the VL-prefixed cursor for PL/UU/LL (LL -> VLLL, the liked-videos
    // pseudo-playlist; RD mixes browse directly). The emit echoes
    // the CALLER's id so view-side id matching works either way.
    m_accountVideosEmitId = playlistId;
    QString cursor = playlistId;
    if (cursor.startsWith(QStringLiteral("PL")) || cursor.startsWith(QStringLiteral("UU"))
        || cursor == QStringLiteral("LL"))
        cursor = QStringLiteral("VL") + cursor;
    browsePage(QStringLiteral("playlistvideos"), cursor, 1, QVariantList());
}

