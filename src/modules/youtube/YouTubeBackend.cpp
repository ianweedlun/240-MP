#include "YouTubeBackend.h"
#include "YtInnertubeResolver.h"
#include "../../util/YtStreamSelect.h"
#include "../../util/YtUrlFresh.h"
#include "../../util/YtNegativeCache.h"
#include "../../util/YtSponsorLru.h"
#include "../../util/YtAuthGate.h"
#include "../../util/YtBrowseRows.h"
#include "../../util/YtHlsSelect.h"
#include "../../util/YtFeedDedup.h"

// Backend for the YouTube module

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QCryptographicHash>
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
// Device-flow tokens carry ~10 min expiry claims; slack guards clock skew
// and lets callers refresh instead of using a dead bearer.
constexpr qint64 kAuthSlackS = ytauth::kAuthSlackS; // seconds (single source: util/YtAuthGate.h)
// Guard rail beside YtSponsorEdl's segment cap: never parse a pathological
// skipSegments body on the GUI thread. 1 MiB is far above a real 4-hex bucket.
constexpr qsizetype kSponsorMaxResponseBytes = 1 << 20;

static QString watchUrlFor(const QString &videoId) {
    return QStringLiteral("https://www.youtube.com/watch?v=") + videoId;
}

YouTubeBackend::YouTubeBackend(const QString &appRoot, const QString &dataRoot, QObject *parent)
    : QObject(parent), m_appRoot(appRoot), m_dataRoot(dataRoot)
{
    m_itResolver = new YtInnertubeResolver(this);
    // One in-process attempt per key; on failure the SAME key surfaces to the
    // caller (transient) or lands in the negative cache (permanent) — the
    // yt-dlp subprocess ladder is gone (NEG-01 convergence).
    connect(m_itResolver, &YtInnertubeResolver::finished, this,
            [this](const YtResolveResult &r) {
        if (r.ok) {
            clearNegativeCacheEntry(r.videoId); // a post-TTL success retires the old failure
            if (!r.hlsManifestUrl.isEmpty()) {
                // #128 live route: fetch + shape the HLS master; never cache a
                // progressive pair for live content. Telemetry fires at the
                // playlist hand-off so `ms` covers the whole live ladder.
                m_resolveStartMs.insert(r.key,
                                        QDateTime::currentMSecsSinceEpoch() - r.elapsedMs);
                launchHlsResolve(r.videoId, r.resolution, r.key,
                                 r.hlsManifestUrl, r.postLive);
                return;
            }
            emit resolveTelemetry(r.videoId, QStringLiteral("innertube"), r.elapsedMs);
            if (r.expireEpoch > QDateTime::currentSecsSinceEpoch()) {
                storeUrlCacheEntry(r.key, r.videoUrl, r.audioUrl, r.single,
                                   r.expireEpoch);
                emit playbackUrlReady(r.videoId);
            }
            finishUrlResolve(r.key);
            return;
        }
        if (r.reason == QLatin1String("live-waiting")) {
            // #128: the event has not started. Not a permanent failure — never
            // negative-cache it; tell the viewer.
            emit resolveTelemetry(r.videoId, QStringLiteral("live-waiting"), r.elapsedMs);
            emit playbackResolveFailed(r.videoId, QStringLiteral("LIVE_WAITING"));
            finishUrlResolve(r.key);
            return;
        }
        qWarning("[Youtube] innertube-fail vid=%s reason=%s ms=%lld",
                 qPrintable(r.videoId), qPrintable(r.reason),
                 static_cast<long long>(r.elapsedMs));
        emit resolveTelemetry(r.videoId,
                              QStringLiteral("innertube-fail:") + r.reason, r.elapsedMs);
        const QString failureClass = storeNegativeCacheEntry(r.videoId, r.reason);
        if (!failureClass.isEmpty()) {
            // The response itself proved this video cannot resolve. Do not
            // retry it on any warm/focus/press path for 24 h.
            emit playbackResolveFailed(r.videoId, failureClass);
            finishUrlResolve(r.key);
            return;
        }
        // No yt-dlp ladder (removed, NEG-01 convergence): a transient innertube
        // failure surfaces to the caller and the next warm/focus/press triggers
        // a fresh resolve. The resolver's bounded visitorData refetch budget
        // heals mid-session network recoveries without a subprocess fallback.
        emit playbackResolveFailed(r.videoId, r.reason);
        finishUrlResolve(r.key);
    });
}

YouTubeBackend::~YouTubeBackend()
{
    // Flush a debounced save before the members die: app exit must not lose a
    // confirmed window that landed inside the debounce interval. The timer is
    // a child QObject and cannot fire during destruction (no event loop here).
    if (m_sponsorSaveDirty)
        saveSponsorCache();
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
    ensureAuthLoaded();
    const bool tokenLive = !m_accessToken.isEmpty()
        && !ytauth::accessExpired(m_accessExpiresEpoch, QDateTime::currentMSecsSinceEpoch() / 1000);
    if (tokenLive) {
        // The signed-in feed does NOT come from FEsubscriptions: the TV
        // client's subscription feed is a server-precomputed cache — the
        // 2026-11-04 live probe showed 28 of 30 sampled channels' latest
        // uploads entirely absent from its 29-43 unordered tiles while the
        // official mobile app showed them. Account truth = live union:
        // one FEchannels roster hop (fresh + real names, probe: 313 tiles)
        // then wave-capped per-channel RSS (probe: ~110 ms/request from the
        // Pi; TTL cache below makes repeat opens free).
        m_feedWantsRoster = true;
        m_emitFeedWhenDone = true;
        browsePage(QStringLiteral("subschannels"), QStringLiteral("FEchannels"), 1, QVariantList());
        return;
    }
    m_emitFeedWhenDone = true;
    ensureFresh(forceRefresh);
}

void YouTubeBackend::load_channels(bool forceRefresh) {
    m_emitChannelsWhenDone = true;
    if (!m_accessToken.isEmpty()) {
        if (ytauth::accessExpired(m_accessExpiresEpoch, QDateTime::currentMSecsSinceEpoch() / 1000)) {
            m_pendingReplay.deferChannels();
            requestRefresh(); // success leg replays load_channels with a fresh bearer
            return;
        }
        m_pendingReplay.clearChannels(); // going out live: no deferred work left
        // Signed-in truth (user rule: InnerTube primary; text files are
        // legacy fallback): FEchannels returns the whole subscribed-channel
        // list in ONE page — 313 channelRenderer-free but TV-tiled tiles
        // (probe-verified live: TILE_CONTENT_TYPE_CHANNEL + contentId +
        // tileMetadataRenderer.title). No RSS storm, no continuation.
        browsePage(QStringLiteral("subschannels"), QStringLiteral("FEchannels"), 1, QVariantList());
        return;
    }
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
    queueRefresh(stale);
}

void YouTubeBackend::stormFeedFromRoster(const QStringList &ids, bool forceRefresh)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList stale;
    for (const QString &id : ids) {
        ChannelEntry &entry = m_channels[id];
        entry.channelId = id;
        if (forceRefresh || !entry.feedOk || now - entry.fetchedMs > kCacheTtlMs)
            stale << id;
    }
    if (stale.isEmpty()) {
        finishAggregate(); // roster already fresh within TTL — serve from cache
        return;
    }
    queueRefresh(stale);
}

// Wave-capped refresh plumbing (see header comment on kMaxConcurrentFeedFetches).
void YouTubeBackend::queueRefresh(const QStringList &stale)
{
    for (const QString &id : stale)
        m_refreshQueue.enqueue(id);
    m_pendingChannels = m_refreshQueue.size();
    pumpRefresh();
}

void YouTubeBackend::pumpRefresh()
{
    while (m_activeRefresh < kMaxConcurrentFeedFetches && !m_refreshQueue.isEmpty()) {
        ++m_activeRefresh;
        refreshChannel(m_refreshQueue.dequeue());
    }
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
        if (m_activeRefresh > 0)
            --m_activeRefresh;
        if (m_pendingChannels > 0)
            --m_pendingChannels; // one completion per queued id (queueRefresh sized it)
        pumpRefresh(); // release the slot and admit the next wave member
        if (m_pendingChannels <= 0 && m_refreshQueue.isEmpty()) {
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
    // #128: collapse live double-posts and cross-channel videoId repeats
    // before truncation so duplicates cannot eat the 100-row budget.
    return ytfeed::dedupFeed(all).mid(0, kMaxFeedItems);
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
// Playlist loaders — native Innertube browse walks feeding the same
// cache/queue shape as the RSS channel path. Browse (rather than the playlist
// RSS feed) because the feed stops at 15 entries.
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
    m_pendingPlaylists   = stale.size();
    m_playlistFetchQueue = stale;
    spawnNextPlaylistFetch();
}

void YouTubeBackend::spawnNextPlaylistFetch() {
    while (m_activePlaylistFetches < kMaxConcurrentPlaylistFetches
           && !m_playlistFetchQueue.isEmpty()) {
        const QString playlistId = m_playlistFetchQueue.takeFirst();
        ++m_activePlaylistFetches;
        browsePlaylist(playlistId, QStringLiteral("VL") + playlistId, 1, QVariantList());
    }
}

// Walk one public playlist via Innertube browse, accumulating video tiles and
// following playlistContinuationToken cursors until kMaxPlaylistItems or the
// cursor runs out. Public playlists need no sign-in (the TV key suffices);
// entries render as video tiles that extractBrowseRows already parses (the
// W3 team's own tests pin the TILE_STYLE_YTLR_VERTICAL_LIST shape). Stops early
// when a page fetched no rows AND carries no continuation token — a genuine
// single-page list, not a stall.
void YouTubeBackend::browsePlaylist(const QString &playlistId, const QString &cursor,
                                    int page, const QVariantList &acc) {
    QJsonObject body;
    body.insert(QStringLiteral("context"), QJsonObject{{QStringLiteral("client"), QJsonObject{
        {QStringLiteral("clientName"), QStringLiteral("TVHTML5")},
        {QStringLiteral("clientVersion"), QStringLiteral("7.20210818.00.00-ks")},
        {QStringLiteral("hl"), QStringLiteral("en")},
        {QStringLiteral("gl"), QStringLiteral("US")}}}});
    if (cursor.startsWith(QStringLiteral("VL")))
        body.insert(QStringLiteral("browseId"), cursor);
    else
        body.insert(QStringLiteral("continuation"), cursor);
    QNetworkRequest req{QUrl(QStringLiteral("https://www.youtube.com/youtubei/v1/browse")
                             + bearerQuery())};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QString cookieHeader = cookieHeaderValue();
    if (!cookieHeader.isEmpty())
        req.setRawHeader("Cookie", cookieHeader.toUtf8());
    else if (!m_accessToken.isEmpty())
        req.setRawHeader("Authorization",
                         QStringLiteral("Bearer %1").arg(m_accessToken).toUtf8());
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QTimer::singleShot(kPlaylistFetchTimeoutMs, reply, [reply]() { reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, playlistId, page, acc]() {
        reply->deleteLater();
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject root = doc.object();
        const QVariantList pageRows = ytui::extractBrowseRows(
            root, qMax(0, kMaxPlaylistItems - acc.size()));
        QVariantList rows = acc;
        rows.reserve(acc.size() + pageRows.size());
        rows += pageRows;
        const QString next = ytui::playlistContinuationToken(root);
        qWarning("[Youtube] playlist browse id=%s page=%d rows=%lld continued=%d",
                 qPrintable(playlistId), page, static_cast<long long>(rows.size()),
                 next.isEmpty() ? 0 : 1);
        if (!next.isEmpty() && rows.size() < kMaxPlaylistItems) {
            browsePlaylist(playlistId, next, page + 1, rows);
            return;
        }
        assemblePlaylistFromBrowse(playlistId, rows);
    });
}

// Keep the total cached rows across m_playlists under the global budget before
// a fresh fetch replaces one playlist's rows. Eviction is deterministic:
// oldest fetchedMs first, playlistId as the tiebreak. Entries are cleared (not
// erased) so they stay listed and are refetched next load (fetchOk=false makes
// them stale). The playlist being stored and the one whose videos were asked
// for are protected, so an in-flight emit cannot lose its target. Nothing is
// mutated while iterating: ids are gathered first, then re-found with an end()
// check before touching the hash.
bool YouTubeBackend::evictPlaylistRowsToFit(const QString &keepId, int &incomingRows)
{
    incomingRows = qBound(0, incomingRows, qMin(kMaxPlaylistItems, kMaxRetainedPlaylistRows));

    // keepId's old rows get replaced, not added, so they must not count here.
    int projected = 0;
    for (auto it = m_playlists.constBegin(); it != m_playlists.constEnd(); ++it) {
        if (it.key() != keepId)
            projected += it->videos.size();
    }
    if (projected + incomingRows <= kMaxRetainedPlaylistRows)
        return incomingRows > 0;

    QSet<QString> protectedIds{keepId};
    if (!m_emitPlaylistVideosWhenDone.isEmpty())
        protectedIds.insert(m_emitPlaylistVideosWhenDone);

    QStringList evictable;
    for (auto it = m_playlists.constBegin(); it != m_playlists.constEnd(); ++it) {
        if (it->videos.isEmpty() || protectedIds.contains(it.key()))
            continue;
        evictable << it.key();
    }
    std::sort(evictable.begin(), evictable.end(),
              [this](const QString &a, const QString &b) {
                  const auto ia = m_playlists.constFind(a);
                  const auto ib = m_playlists.constFind(b);
                  const qint64 ta = ia != m_playlists.constEnd() ? ia->fetchedMs : 0;
                  const qint64 tb = ib != m_playlists.constEnd() ? ib->fetchedMs : 0;
                  return ta != tb ? ta < tb : a < b;
              });

    for (const QString &id : evictable) {
        if (projected + incomingRows <= kMaxRetainedPlaylistRows)
            break;
        auto it = m_playlists.find(id);
        if (it == m_playlists.end())   // defensive: entry gone since collection
            continue;
        projected -= it->videos.size();
        it->videos.clear();
        it->fetchOk   = false;         // stale -> refetch; never serve empty rows
        it->fetchedMs = 0;
    }

    // Protected entries alone can still fill the budget. Shrink this fetch
    // rather than break the cap. Zero means "keep the previous cached rows".
    if (projected + incomingRows > kMaxRetainedPlaylistRows)
        incomingRows = qMax(0, kMaxRetainedPlaylistRows - projected);
    return incomingRows > 0;
}

// Rebuild the exact yt-dlp --flat-playlist output shape the playlist model
// consumed: id/title/channel with tombstones dropped and flat-entry defaults
// (no publish date, not a short). Playlist order is browse order, exactly
// matching -I 1:N on the source playlist. Keep any previously cached videos on
// failure (stale beats empty).
void YouTubeBackend::assemblePlaylistFromBrowse(const QString &playlistId, const QVariantList &rows)
{
    QString     title;
    QVariantList videos;
    for (const QVariant &rowV : rows) {
        const QVariantMap row = rowV.toMap();
        const QString videoId    = row.value(QStringLiteral("videoId")).toString();
        const QString videoTitle = row.value(QStringLiteral("title")).toString();
        const QString channel    = row.value(QStringLiteral("channelName")).toString();
        if (videoId.isEmpty() || videoTitle.isEmpty())
            continue;
        if (title.isEmpty())
            title = videoTitle;
        // Tombstones YouTube leaves in place of removed entries
        if (videoTitle == QLatin1String("[Private video]")
            || videoTitle == QLatin1String("[Deleted video]"))
            continue;
        QVariantMap v;
        v["videoId"]     = videoId;
        v["title"]       = videoTitle;
        v["channelId"]   = QString();
        v["channelName"] = channel;
        // Flat entries carry no publish date; playlist order stands in
        v["publishedAt"] = QString();
        v["publishedMs"] = qint64(0);
        v["url"]         = watchUrlFor(videoId);
        v["isShort"]     = false;
        videos.append(v);
    }
    if (!videos.isEmpty()) {
        int rowsToStore = videos.size();
        if (evictPlaylistRowsToFit(playlistId, rowsToStore) && rowsToStore > 0) {
            if (rowsToStore < videos.size())
                videos = videos.mid(0, rowsToStore);
            PlaylistEntry &entry = m_playlists[playlistId];
            entry.fetchedTitle = title;
            entry.videos       = videos;
            entry.fetchOk      = true;
            entry.fetchedMs    = QDateTime::currentMSecsSinceEpoch();
        }
        // No room under the global budget: keep any previously cached videos.
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
}

// ---------------------------------------------------------------------------
// Playback resolution -> yt-dlp format selector (cache-key helper only; the
// yt-dlp ladder itself was removed in NEG-01, this survives purely to name
// HLS format variants for the URL cache).
// ---------------------------------------------------------------------------

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
                                          "CHECK YOUR NETWORK CONNECTION"));
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
// resolves with the Innertube resolver ahead of time (fire-and-forget) and
// keeps the URL under its own expire= epoch; the Player takes the cached URL
// plus --ytdl=no while it is fresh, else falls back to the ytdl path
// unchanged.

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

QString YouTubeBackend::negativeCacheFilePath() const
{
    return m_dataRoot + QStringLiteral("/youtube_resolve_failures.json");
}

void YouTubeBackend::ensureNegativeCacheLoaded()
{
    if (m_negativeCacheLoaded)
        return;
    m_negativeCacheLoaded = true;
    m_negativeCache = ytneg::readNegativeCache(
        negativeCacheFilePath(), QDateTime::currentSecsSinceEpoch());
}

void YouTubeBackend::saveNegativeCache()
{
    ytneg::pruneNegativeCache(m_negativeCache, QDateTime::currentSecsSinceEpoch());
    if (!ytneg::writeNegativeCache(negativeCacheFilePath(), m_negativeCache))
        qWarning("[Youtube] could not persist negative resolve cache");
}

QString YouTubeBackend::storeNegativeCacheEntry(const QString &videoId, const QString &reason)
{
    ensureNegativeCacheLoaded();
    const QString failureClass = ytneg::recordFailure(
        m_negativeCache, videoId, reason, QDateTime::currentSecsSinceEpoch());
    if (!failureClass.isEmpty())
        saveNegativeCache();
    return failureClass;
}

void YouTubeBackend::clearNegativeCacheEntry(const QString &videoId)
{
    ensureNegativeCacheLoaded();
    if (!m_negativeCache.contains(videoId))
        return;
    m_negativeCache.remove(videoId);
    saveNegativeCache();
}

bool YouTubeBackend::negativeCacheBlocks(const QString &videoId)
{
    ensureNegativeCacheLoaded();
    const QString failureClass = ytneg::freshFailureClass(
        m_negativeCache, videoId, QDateTime::currentSecsSinceEpoch());
    if (failureClass.isEmpty())
        return false;

    emit resolveTelemetry(videoId, QStringLiteral("negative-cache"), 0);
    // resolvePlayback() runs synchronously inside Player.preparePlayback(),
    // before playAt() sets waitingForResolve. Deliver cached failures on the
    // next event-loop turn so the Player cannot miss the signal and fall into
    // its timed mpv/ytdl_hook fallback.
    QTimer::singleShot(0, this, [this, videoId, failureClass]() {
        emit playbackResolveFailed(videoId, failureClass);
    });
    return true;
}

QString YouTubeBackend::sponsorCacheFilePath() const
{
    return m_dataRoot + QStringLiteral("/youtube_sponsor.json");
}

void YouTubeBackend::ensureSponsorCacheLoaded()
{
    if (m_sponsorCacheLoaded)
        return;
    m_sponsorCacheLoaded = true;
    QFile f(sponsorCacheFilePath());
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
        if (doc.isArray()) {
            // Anything dropped here (invalid row, duplicate, orphan, prune)
            // means the legacy file no longer matches memory -> self-heal it
            // into the bounded MRU-first form with one synchronous rewrite.
            bool legacyDirty = false;
            for (const QJsonValue &e : doc.array()) {
                const QJsonObject o = e.toObject();
                const QString vid = o.value(QStringLiteral("videoId")).toString();
                if (vid.isEmpty()) {
                    legacyDirty = true;
                    continue;
                }
                // Atomic entry: a row without a segments array gets NO cache
                // entry and NO stamp, so a stamp-only legacy row can never
                // orphan a timestamp. An EMPTY array is valid (confirmed "no
                // segments"); every real legacy format wrote the array.
                const QJsonValue rows = o.value(QStringLiteral("segments"));
                if (!rows.isArray()) {
                    legacyDirty = true;
                    continue;
                }
                // Oversized legacy entry: drop it whole and self-heal the file
                // instead of loading an unbounded cut list at startup.
                const QJsonArray rowArray = rows.toArray();
                if (rowArray.size() > yt::kMaxRawRowsPerGroup) {
                    legacyDirty = true;
                    continue;
                }
                // Duplicates keep the FIRST occurrence whole (payload, stamp
                // and position), so the legacy prune stays deterministic.
                if (m_sponsorCache.contains(vid)) {
                    legacyDirty = true;
                    continue;
                }
                m_sponsorCache.insert(vid, yt::segmentsFromRows(rowArray));
                // Keep the ORIGINAL stamp (0 = pre-stamp legacy entry) through
                // the prune; re-stamping happens only AFTER ordering, below.
                m_sponsorFetchedAt.insert(vid, static_cast<qint64>(
                    o.value(QStringLiteral("fetchedAt")).toDouble(0)));
                // Rebuild the recency index in file order (writes are
                // MRU-first); legacy files have no hit-recency signal.
                ytlru::appendIfAbsent(m_sponsorLru, vid);
            }
            if (ytlru::dropOrphanStamps(m_sponsorCache, m_sponsorFetchedAt) > 0)
                legacyDirty = true;
            // Oversized legacy file -> deterministic prune to the cap (newest
            // stamps win). A bounded file is untouched, stale residents keep
            // serving playback until their refetch lands.
            if (ytlru::pruneSponsorOnLoad(m_sponsorLru, m_sponsorCache,
                                          m_sponsorFetchedAt,
                                          QDateTime::currentMSecsSinceEpoch(),
                                          kSponsorCacheMaxEntries) > 0)
                legacyDirty = true;
            // Entries written before the stamp existed are stamped NOW, so
            // an upgraded cache ages out gradually rather than refetching
            // every video on first boot. AFTER the prune, never before: a
            // re-stamp to now must not bias which legacy entries survive.
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            for (auto it = m_sponsorFetchedAt.begin(); it != m_sponsorFetchedAt.end(); ++it) {
                if (it.value() <= 0)
                    it.value() = now;
            }
            if (legacyDirty)
                saveSponsorCache();
        }
    }
}

void YouTubeBackend::saveSponsorCache()
{
    QJsonArray arr;
    // Recency order (MRU first): the file order is the reload recency signal,
    // and the index is capped, so the rewrite stays bounded.
    for (const QString &vid : m_sponsorLru) {
        QJsonArray rows;
        for (const yt::Segment &s : m_sponsorCache.value(vid)) {
            QJsonObject o;
            QJsonArray range;
            range.append(s.start);
            range.append(s.end);
            o.insert(QStringLiteral("category"), s.category);
            o.insert(QStringLiteral("segmentType"), s.action);
            o.insert(QStringLiteral("segment"), range);
            o.insert(QStringLiteral("votes"), s.votes);
            o.insert(QStringLiteral("locked"), s.locked ? 1 : 0);
            o.insert(QStringLiteral("videoDuration"), s.videoDuration);
            rows.append(o);
        }
        QJsonObject e;
        e.insert(QStringLiteral("videoId"), vid);
        e.insert(QStringLiteral("segments"), rows);
        e.insert(QStringLiteral("fetchedAt"), double(m_sponsorFetchedAt.value(vid)));
        arr.append(e);
    }
    QFile f(sponsorCacheFilePath());
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void YouTubeBackend::markSponsorCacheDirty()
{
    m_sponsorSaveDirty = true;
    if (!m_sponsorSaveTimer) {
        m_sponsorSaveTimer = new QTimer(this);
        m_sponsorSaveTimer->setSingleShot(true);
        m_sponsorSaveTimer->setInterval(kSponsorSaveDebounceMs);
        connect(m_sponsorSaveTimer, &QTimer::timeout, this, [this]() {
            if (m_sponsorSaveDirty) {
                m_sponsorSaveDirty = false;
                saveSponsorCache();
            }
        });
    }
    // Restart on every answer: while answers keep landing inside the interval
    // (idle-fill burst) the write keeps sliding to the end of the burst, so
    // 400 paced confirmations cost ONE bounded rewrite, not 400.
    m_sponsorSaveTimer->start();
}

// ---------------------------------------------------------------------------
// Idle SponsorBlock fill
// ---------------------------------------------------------------------------

QString YouTubeBackend::knownIdsFilePath() const
{
    return m_dataRoot + QStringLiteral("/youtube_known_ids.json");
}

void YouTubeBackend::ensureKnownIdsLoaded()
{
    if (m_knownIdsLoaded)
        return;
    m_knownIdsLoaded = true;
    bool legacyDirty = false;
    QFile f(knownIdsFilePath());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        for (const QJsonValue &v : doc.array()) {
            const QString id = v.toString();
            if (id.isEmpty()) {
                legacyDirty = true;
                continue;
            }
            // Duplicates keep the first occurrence (file order is the only
            // recency signal a legacy string array has).
            if (m_knownVideoIds.contains(id)) {
                legacyDirty = true;
                continue;
            }
            m_knownVideoIds.insert(id);
            ytlru::appendIfAbsent(m_knownLru, id);
        }
    }
    // Oversized legacy file: keep the FIRST cap ids in file order BEFORE the
    // history merge. Dropped ids are re-surfaced on the next browse — a clean
    // miss, never a corruption.
    if (!ytlru::evictKnownToCap(m_knownLru, m_knownVideoIds,
                                kKnownVideoIdMaxEntries).isEmpty())
        legacyDirty = true;
    // Watch history is a second source: rewatches are the most likely plays.
    QFile h(m_dataRoot + QStringLiteral("/youtube_history.json"));
    if (h.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(h.readAll());
        const QJsonObject o = doc.object();
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
            if (!it.key().isEmpty()) {
                m_knownVideoIds.insert(it.key());
                ytlru::appendIfAbsent(m_knownLru, it.key());
            }
        }
    }
    // Final trim: the merge can push even a bounded file over cap. The tail
    // goes first — those are the just-appended history keys, so every id the
    // file itself carried keeps its front position. No load path may leave
    // the pool above cap, even before the first mutation.
    ytlru::evictKnownToCap(m_knownLru, m_knownVideoIds, kKnownVideoIdMaxEntries);
    // Self-heal an oversized/dirty legacy file once. A clean bounded file is
    // not rewritten: its ids all survive the merge trim, so memory == file.
    if (legacyDirty)
        saveKnownIds();
}

void YouTubeBackend::saveKnownIds()
{
    QJsonArray arr;
    // Recency order (MRU first), bounded by the index cap.
    for (const QString &id : m_knownLru)
        arr.append(id);
    QFile f(knownIdsFilePath());
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void YouTubeBackend::noteKnownVideoIds(const QStringList &videoIds)
{
    ensureKnownIdsLoaded();
    bool changed = false;
    for (const QString &id : videoIds) {
        if (id.isEmpty())
            continue;
        if (m_knownVideoIds.contains(id)) {
            // Re-surfacing is a hit: refresh recency so hot ids survive
            // eviction pressure, and persist the new order.
            ytlru::touch(m_knownLru, id);
            changed = true;
            continue;
        }
        m_knownVideoIds.insert(id);
        ytlru::touch(m_knownLru, id);
        changed = true;
    }
    ytlru::evictKnownToCap(m_knownLru, m_knownVideoIds, kKnownVideoIdMaxEntries);
    if (changed)
        saveKnownIds();
}

QStringList YouTubeBackend::idleCandidates(const QStringList &candidates,
                                           qint64 nowMs) const
{
    QSet<QString> cached;
    for (auto it = m_sponsorCache.cbegin(); it != m_sponsorCache.cend(); ++it)
        cached.insert(it.key());
    return ytsb::idleSelect(candidates, m_sponsorFetchedAt, cached,
                            m_sponsorFetching, m_sponsorFailedAt,
                            nowMs, kIdleFillMax);
}

void YouTubeBackend::setIdleFillActive(bool idle)
{
    if (!idle) {
        m_idleFilling = false;
        m_idleQueue.clear();
        if (m_idleTimer)
            m_idleTimer->stop();
        return;
    }
    if (m_idleFilling)
        return;
    ensureSponsorCacheLoaded();
    ensureKnownIdsLoaded();
    // MRU-first known pool: idle fill confirms the hottest ids first.
    m_idleQueue = idleCandidates(m_knownLru,
                                 QDateTime::currentMSecsSinceEpoch());
    if (m_idleQueue.isEmpty())
        return;
    m_idleFilling = true;
    if (!m_idleTimer) {
        m_idleTimer = new QTimer(this);
        m_idleTimer->setInterval(kIdleFillPaceMs);
        connect(m_idleTimer, &QTimer::timeout, this, &YouTubeBackend::pumpIdleFill);
    }
    m_idleTimer->start();
    qWarning("[Youtube] idle-fill start queued=%d", int(m_idleQueue.size()));
}

void YouTubeBackend::pumpIdleFill()
{
    if (!m_idleFilling || m_idleQueue.isEmpty()) {
        setIdleFillActive(false);
        return;
    }
    // Yield to the foreground. A live warm queue or an in-flight resolve owns
    // the connection pool; competing with it is what made foreground sponsor
    // fetches ~2x slower than a bare request. Re-check on the next tick.
    if (!m_warmQueue.isEmpty() || (m_itResolver && m_itResolver->inFlight()))
        return;
    const QString id = m_idleQueue.takeFirst();
    requestSponsorSegments(id);
}

// k-anonymous window fetch (Red doctrine): only the first 4 hex chars of the
// SHA-256 of the videoId leave the device; the response is filtered client-side
// to the exact videoId. 4xx (prefix unknown = no segments) caches an empty
// entry permanently; a transient failure is negative-cached for a short TTL so
// a dead sponsor.ajay.app costs one request per video per window, not one per
// play. Playback never waits on any of this — the Player freezes its EDL at the
// launch instant and late segments apply on the next play.
void YouTubeBackend::requestSponsorSegments(const QString &videoId)
{
    if (videoId.isEmpty())
        return;
    ensureSponsorCacheLoaded();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    ytsb::pruneFailures(m_sponsorFailedAt, now);
    const bool failureBlocks = ytsb::failureActive(m_sponsorFailedAt.value(videoId), now);
    // A stale window is refetched, but the old segments stay in the cache and
    // keep serving playback until the new answer lands — a refresh never
    // removes skipping from a video that already had it.
    const bool fresh = m_sponsorCache.contains(videoId)
                       && !ytsb::successStale(m_sponsorFetchedAt.value(videoId), now);
    switch (ytsb::decideFetch(fresh,
                              m_sponsorFetching.contains(videoId),
                              failureBlocks)) {
    case ytsb::FetchDecision::ServeCache:
        // A hit refreshes recency: the video is in active use, so it must
        // outlive cold entries under eviction pressure.
        ytlru::touch(m_sponsorLru, videoId);
        emit sponsorReady(videoId);
        return;
    case ytsb::FetchDecision::AlreadyFetching:
    case ytsb::FetchDecision::Suppressed:
        return;
    case ytsb::FetchDecision::Fetch:
        break;
    }
    m_sponsorFetching.insert(videoId);

    const QByteArray digest =
        QCryptographicHash::hash(videoId.toUtf8(), QCryptographicHash::Sha256);
    const QString prefix = QString::fromLatin1(digest.toHex()).left(4);
    const QString cats = QStringLiteral(
        "%5B%22sponsor%22%2C%22selfpromo%22%2C%22intro%22%2C%22outro%22%2C%22inter"
        "mission%22%2C%22preview%22%2C%22filler%22%2C%22interaction%22%5D");
    const QString types = QStringLiteral("%5B%22skip%22%2C%22mute%22%2C%22full%22%5D");
    QUrl url(QStringLiteral("https://sponsor.ajay.app/api/skipSegments/") + prefix
             + QStringLiteral("?categories=") + cats
             + QStringLiteral("&actionTypes=") + types);
    m_sponsorStartMs.insert(videoId, QDateTime::currentMSecsSinceEpoch());
    qWarning("[Youtube] sponsor start vid=%s", qPrintable(videoId));

    QNetworkReply *reply = m_nam.get(makeRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, videoId]() {
        const QNetworkReply::NetworkError err = reply->error();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool clientErr = (status >= 400 && status < 500); // 4xx = no segments for prefix
        if (err != QNetworkReply::NoError && !clientErr) {
            qWarning("[Youtube] sponsor transient err=%d vid=%s", int(err), qPrintable(videoId));
            m_sponsorFetching.remove(videoId);
            // Negative-cache the failure: retries inside the TTL are suppressed
            // so a dead domain cannot turn every cold play into a fetch.
            m_sponsorFailedAt.insert(videoId, QDateTime::currentMSecsSinceEpoch());
            // Defense-in-depth beside the TTL prune: a burst of distinct
            // failures inside one window cannot grow the map unbounded.
            ytlru::capFailures(m_sponsorFailedAt, kSponsorFailedAtMaxEntries);
            reply->deleteLater();
            return;
        }
        const QByteArray body = reply->readAll();
        if (body.size() > kSponsorMaxResponseBytes) {
            // Fail closed before JSON parsing: keep any cut list already cached
            // for this video, and negative-cache the oversize answer so a
            // pathological endpoint cannot be re-parsed on every play.
            qWarning("[Youtube] sponsor oversize vid=%s bytes=%lld",
                     qPrintable(videoId), static_cast<long long>(body.size()));
            m_sponsorStartMs.remove(videoId);
            m_sponsorFetching.remove(videoId);
            m_sponsorFailedAt.insert(videoId, QDateTime::currentMSecsSinceEpoch());
            ytlru::capFailures(m_sponsorFailedAt, kSponsorFailedAtMaxEntries);
            reply->deleteLater();
            return;
        }
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        const QVector<yt::Segment> segs =
            doc.isArray() ? yt::sponsorSegmentsFromJson(doc.array(), videoId)
                          : QVector<yt::Segment>{};
        m_sponsorCache.insert(videoId, segs);
        m_sponsorFetchedAt.insert(videoId, QDateTime::currentMSecsSinceEpoch());
        // Single insert chokepoint: refresh recency, then co-evict the LRU
        // tail from BOTH maps so no timestamp ever orphans.
        ytlru::touch(m_sponsorLru, videoId);
        ytlru::evictSponsorToCap(m_sponsorLru, m_sponsorCache, m_sponsorFetchedAt,
                                 kSponsorCacheMaxEntries);
        m_sponsorFailedAt.remove(videoId); // a real answer clears any stale stamp
        markSponsorCacheDirty();
        m_sponsorFetching.remove(videoId);
        const qint64 start = m_sponsorStartMs.take(videoId);
        qWarning("[Youtube] sponsor done vid=%s segs=%d ms=%lld",
               qPrintable(videoId), static_cast<int>(segs.size()),
               static_cast<long long>(QDateTime::currentMSecsSinceEpoch() - start));
        emit sponsorReady(videoId);
        reply->deleteLater();
    });
}

bool YouTubeBackend::hasSponsorSegments(const QString &videoId)
{
    if (videoId.isEmpty())
        return false;
    ensureSponsorCacheLoaded();
    return m_sponsorCache.contains(videoId);
}

bool YouTubeBackend::sponsorFetchInFlight(const QString &videoId) const
{
    return m_sponsorFetching.contains(videoId);
}

// Per-category Off/Mark/Skip: "skip" ranges are cut out of the virtual timeline
// (both halves identically, so audio stays in sync across every jump); "mark"
// ranges stay playable but surface their boundaries as OSC chapter titles.
QVariantMap YouTubeBackend::buildPlaybackEdl(const QString &videoId,
                                             const QString &resolution,
                                             const QVariantMap &actions)
{
    QVariantMap out;
    const QString url = cachedPlaybackUrl(videoId, resolution);
    if (url.isEmpty())
        return out;
    const QString audio = cachedPlaybackAudioUrl(videoId, resolution);
    ensureSponsorCacheLoaded();
    const QVector<yt::Segment> all = m_sponsorCache.value(videoId);
    QVector<yt::Segment> skipSegs;
    QVector<yt::Segment> markSegs;
    for (const yt::Segment &s : all) {
        const QString act = actions.value(s.category).toString().toLower();
        if (act == QStringLiteral("skip"))
            skipSegs.append(s);
        else if (act == QStringLiteral("mark"))
            markSegs.append(s);
    }
    static const QHash<QString, QString> labels{
        {QStringLiteral("sponsor"), QStringLiteral("Sponsor")},
        {QStringLiteral("selfpromo"), QStringLiteral("Self promo")},
        {QStringLiteral("intro"), QStringLiteral("Intro")},
        {QStringLiteral("outro"), QStringLiteral("Outro")},
        {QStringLiteral("intermission"), QStringLiteral("Intermission")},
        {QStringLiteral("preview"), QStringLiteral("Preview")},
        {QStringLiteral("filler"), QStringLiteral("Filler")},
        {QStringLiteral("interaction"), QStringLiteral("Interaction")}};

    // OSC paints category-colored segment marks from this side-channel
    // ("start-end:Label:action;" in RAW source seconds; the OSC converts to
    // virtual time using the skip cuts). EDL itself only carries the cuts.
    QStringList ranges;
    const QSet<QString> drop{QStringLiteral("skip"), QStringLiteral("mute")};
    QVector<QPair<double, double>> cuts;
    auto collect = [&](const QVector<yt::Segment> &segs, const QString &kind) {
        QStringList cats;
        for (const yt::Segment &s : segs)
            if (!cats.contains(s.category))
                cats.append(s.category);
        for (const QString &cat : cats) {
            QVector<yt::Segment> catSegs;
            for (const yt::Segment &s : segs)
                if (s.category == cat)
                    catSegs.append(s);
            const auto catMerged = yt::mergeRanges(catSegs, drop);
            for (const auto &pair : catMerged) {
                if (kind == QLatin1String("skip"))
                    cuts.append(pair);
                ranges << QStringLiteral("%1-%2:%3:%4")
                              .arg(pair.first, 0, 'f', 1)
                              .arg(pair.second, 0, 'f', 1)
                              .arg(labels.value(cat, cat))
                              .arg(kind);
            }
        }
    };
    collect(skipSegs, QStringLiteral("skip"));
    collect(markSegs, QStringLiteral("mark"));
    std::sort(cuts.begin(), cuts.end(),
              [](const QPair<double, double> &a, const QPair<double, double> &b) {
                  return a.first != b.first ? a.first < b.first : a.second < b.second;
              });
    QVector<QPair<double, double>> merged;
    for (const auto &r : cuts) {
        if (!merged.isEmpty() && r.first <= merged.last().second)
            merged.last().second = qMax(merged.last().second, r.second);
        else
            merged.append(r);
    }
    out.insert(QStringLiteral("videoEdl"), yt::buildEdl(url, merged, {}));
    out.insert(QStringLiteral("audioEdl"),
               audio.isEmpty() ? QString() : yt::buildEdl(audio, merged, {}));
    out.insert(QStringLiteral("ranges"), ranges.join(QLatin1Char(';')));
    return out;
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

void YouTubeBackend::invalidateCachedUrl(const QString &videoId,
                                         const QString &resolution,
                                         bool allowRetry)
{
    if (videoId.isEmpty())
        return;
    ensureUrlCacheLoaded();
    const QString fmt = ytdlFormatForResolution(resolution);
    if (!yturl::invalidateCachedEntry(m_urlCache, videoId, fmt))
        return; // nothing stored for this pair: the cold path is already in effect
    // A cached URL that failed to PLAY must not be re-minted by the same
    // resolver — unless the caller is the warm-403 cure, whose whole point
    // is a same-session re-resolve (see header). Spending the key on that
    // path made every cure an instant RESOLVE_BUSY.
    if (!allowRetry)
        m_itTried.insert(yturl::cacheKeyFor(videoId, fmt));
    QFile f(urlCacheFilePath());
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(QJsonDocument(m_urlCache).toJson(QJsonDocument::Compact));
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
                                         const QString &key)
{
    if (negativeCacheBlocks(videoId)) {
        // A blocked video is a failure terminal like any other. Emitting
        // nothing here left the player view waiting on LOADING until the
        // 4 s watchdog fell back (same family as the 2026-11-22 defect).
        emit playbackResolveFailed(videoId, QStringLiteral("NEGATIVE_CACHE"));
        finishUrlResolve(key);
        return;
    }
    // One Innertube attempt per key. A spent key or a busy resolver means this
    // key never resolves in-process — surface a transient failure (the
    // yt-dlp ladder is gone, NEG-01 convergence).
    if (m_itTried.contains(key)) {
        emit playbackResolveFailed(videoId, QStringLiteral("RESOLVE_BUSY"));
        finishUrlResolve(key);
        return;
    }
    m_itTried.insert(key);
    if (!m_itResolver->resolve(videoId, resolution, key)) {
        // Resolver busy with another key: this key never got a completion
        // signal. Surface a transient failure instead of stalling or spawning.
        m_itTried.remove(key);
        emit playbackResolveFailed(videoId, QStringLiteral("RESOLVE_BUSY"));
        finishUrlResolve(key);
    }
}

// #128 live route: one bounded GET of the HLS master, shaped to the resolution
// cap, written to a per-video playlist file mpv demuxes as a single stream
// (ffmpeg keeps A/V sync inside one HLS presentation — two independent live
// playlists would drift). The cache entry is the FILE PATH. Every read path
// checks freshness with the 120 s URL margin, so the old 60 s TTL made the
// entry born stale: the shared live TTL leaves a ~60 s usable warm window,
// after which the master is re-resolved.
static constexpr qint64 kMaxHlsMasterBytes = 1024 * 1024;

void YouTubeBackend::launchHlsResolve(const QString &videoId, const QString &resolution,
                                      const QString &key, const QString &hlsUrl,
                                      bool postLive)
{
    QNetworkRequest req{QUrl(hlsUrl)};
    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, key, videoId, resolution, hlsUrl, postLive]() {
        reply->deleteLater();
        const QByteArray body = reply->read(kMaxHlsMasterBytes);
        const qint64 startedMs = m_resolveStartMs.take(key);
        const qint64 ms = QDateTime::currentMSecsSinceEpoch() - startedMs;

        QString playlistPath;
        QString shaped;
        if (reply->error() == QNetworkReply::NoError && !body.isEmpty()) {
            shaped = ythls::filterMaster(
                QString::fromUtf8(body), QUrl(hlsUrl),
                ytstream::maxHeightForResolution(resolution));
            if (!shaped.isEmpty()) {
                const QString path = QDir::tempPath() + QStringLiteral("/240mp-yt-hls-")
                                     + videoId + QStringLiteral(".m3u8");
                QFile f(path);
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    f.write(shaped.toUtf8());
                    f.close();
                    playlistPath = path;
                }
            }
        }

        if (playlistPath.isEmpty()) {
            // One release of redundancy removed with the yt-dlp ladder: a live
            // HLS master that failed to shape surfaces a transient failure and
            // the next warm/focus/press re-resolves natively.
            qWarning("[Youtube] live-hls-fail vid=%s ms=%lld err=%d",
                     qPrintable(videoId), static_cast<long long>(ms),
                     static_cast<int>(reply->error()));
            emit resolveTelemetry(videoId, QStringLiteral("live-hls-fail"), ms);
            emit playbackResolveFailed(videoId, QStringLiteral("LIVE_WAITING"));
            finishUrlResolve(key);
            return;
        }

        const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
        qint64 cacheExpiry = nowSecs + yturl::kLivePlaylistTtlS;
        if (postLive) {
            // A finished broadcast is a fixed snapshot, not a rolling live
            // feed: keep the shaped file until its own manifest URLs expire.
            // The short live TTL minus the 120 s safety margin left only a
            // ~60 s warm window, so a slow resume-overlay choice went stale
            // and fell through to mpv's ytdl hook (PLAYBACK FAILED on the
            // owner's CRT). No new segments appear after the event ends.
            const qint64 manifestExpiry = yturl::minExpiryInPlaylist(shaped);
            if (manifestExpiry > nowSecs)
                cacheExpiry = manifestExpiry;
        }
        storeUrlCacheEntry(key, playlistPath, QString(), true, cacheExpiry);
        qWarning("[Youtube] resolve done (live-hls) vid=%s ms=%lld ttl=%lld",
                 qPrintable(videoId), static_cast<long long>(ms),
                 static_cast<long long>(cacheExpiry - nowSecs));
        emit resolveTelemetry(videoId, QStringLiteral("live-hls"), ms);
        emit playbackUrlReady(videoId);
        finishUrlResolve(key);
    });
}

void YouTubeBackend::finishUrlResolve(const QString &key)
{
    m_itTried.remove(key);
    m_resolveKeys.remove(key);
    if (m_warmChainKey == key) {
        m_warmChainKey.clear();
        pumpWarmQueue();
    }
}

void YouTubeBackend::pumpWarmQueue()
{
    while (!m_warmQueue.isEmpty()) {
        const QString token = m_warmQueue.takeFirst();
        const int sep = token.indexOf(QChar(0x1F));
        const QString videoId    = token.left(sep);
        const QString resolution = token.mid(sep + 1);
        if (negativeCacheBlocks(videoId))
            continue;
        const QString fmt = ytdlFormatForResolution(resolution);
        const QString key = yturl::cacheKeyFor(videoId, fmt);
        if (hasFreshUrlPair(key) || m_resolveKeys.contains(key))
            continue; // someone else warmed it meanwhile — next entry
        m_warmChainKey = key;
        m_resolveKeys.insert(key);
        qWarning("[Youtube] warm take vid=%s", qPrintable(videoId));
        launchUrlResolve(videoId, resolution, key);
        return;
    }
}

void YouTubeBackend::warmListPlayback(const QStringList &videoIds, const QString &resolution)
{
    // One speculative resolve pass over a freshly presented list. A single
    // chained resolve at a time (kWarmQueueMax bounds the backlog;
    // kWarmListMax the entries per call): browsing a list is slower than an
    // in-process resolve, so by the time a row is chosen its URL pair is very
    // likely cached and Player takes its direct path. The cap covers a whole
    // list page now, not just the front dozen rows.
    ensureUrlCacheLoaded();
    const QString fmt = ytdlFormatForResolution(resolution);
    int queued = 0;
    for (const QString &videoId : videoIds) {
        if (videoId.isEmpty() || queued >= kWarmListMax || m_warmQueue.size() >= kWarmQueueMax)
            break;
        if (negativeCacheBlocks(videoId))
            continue;
        const QString key = yturl::cacheKeyFor(videoId, fmt);
        if (hasFreshUrlPair(key) || m_resolveKeys.contains(key))
            continue;
        const QString token = warmToken(videoId, resolution);
        if (m_warmQueue.contains(token))
            continue;
        m_warmQueue << token;
        ++queued;
    }
    if (queued > 0)
        qWarning("[Youtube] warm queued=%d queue-size=%d", queued, m_warmQueue.size());
    // Pre-fetch segment windows for the WHOLE list page, not just the warmed
    // front: the first pick then usually finds its window already on file and
    // the launch carries the EDL with no added wait. Bounded so a pathological
    // list cannot fan out without limit — the view's focused-row prefetch
    // covers anything past the cap as the user scrolls.
    int sponsorQueued = 0;
    noteKnownVideoIds(videoIds);
    for (const QString &id : videoIds) {
        if (sponsorQueued >= kWarmSponsorPrefetchMax)
            break;
        if (id.isEmpty() || m_sponsorCache.contains(id) || m_sponsorFetching.contains(id))
            continue;
        ++sponsorQueued;
        requestSponsorSegments(id);
    }
    if (m_warmChainKey.isEmpty())
        pumpWarmQueue();
}

void YouTubeBackend::resolvePlayback(const QString &videoId, const QString &resolution)
{
    if (videoId.isEmpty())
        return;
    ensureUrlCacheLoaded();
    if (negativeCacheBlocks(videoId))
        return;
    const QString fmt = ytdlFormatForResolution(resolution);
    const QString key = yturl::cacheKeyFor(videoId, fmt);
    if (hasFreshUrlPair(key)) {
        emit resolveTelemetry(videoId, QStringLiteral("cache"), 0);
        return; // fresh window already on file
    }
    if (m_resolveKeys.contains(key)
        || m_warmQueue.contains(warmToken(videoId, resolution)))
        return; // a resolve is already in flight, or queued behind the warm chain
    m_resolveKeys.insert(key);
    launchUrlResolve(videoId, resolution, key);
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
// (kAuthSlackS hoisted to file scope above — refresh-before-expiry slack)
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

// Cookie fallback carrier (beta contract #44, user-locked ladder): Netscape
// cookies.txt dropped at <dataRoot>/cookies.txt via SSH from any signed-in
// browser. ~1-year life, zero refresh plumbing. SmartTube prior art carries
// NO cookie path (their SAPISID block is commented-out innertube.py dead code),
// so this is our own shape — and read-only browse GETs need no SAPISIDHASH
// signature header, just the Cookie line. Account-safety: presence only; the
// values never reach logs or QML.
QString YouTubeBackend::cookieHeaderValue() const
{
    QFile f(m_dataRoot + QStringLiteral("/cookies.txt"));
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray raw = f.readAll();
    f.close();
    QString header;
    for (const QByteArray &lineRaw : raw.split('\n')) {
        const QString line = QString::fromLatin1(lineRaw).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const QStringList fields = line.split(QLatin1Char('\t'));
        if (fields.size() < 7)
            continue;
        const QString domain = fields.at(0);
        if (!(domain.endsWith(QStringLiteral("youtube.com")) || domain.endsWith(QStringLiteral("google.com"))))
            continue;
        if (!header.isEmpty())
            header += QStringLiteral("; ");
        header += fields.at(5) + QLatin1Char('=') + fields.at(6);
    }
    return header;
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
    // Expired-but-refreshable is ordinary life (access tokens last ~1h): revive
    // at FIRST auth touch, not behind a signed-in UI gate — the gate is what
    // hid the very entry point whose only path back to refresh lived below it.
    if (!m_refreshToken.isEmpty()
        && ytauth::accessExpired(m_accessExpiresEpoch, QDateTime::currentMSecsSinceEpoch() / 1000))
        requestRefresh();
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
    // A credential with a refresh token is USABLE even past its access-token
    // expiry: loaders revive on demand. Gating this on liveness alone is the
    // deadman bug that hid the PLAYLISTS row one hour after pairing.
    return !ytauth::accessExpired(m_accessExpiresEpoch, now)
            || !m_refreshToken.isEmpty();
}

QVariantMap YouTubeBackend::check_signin()
{
    ensureAuthLoaded();
    QVariantMap m;
    const qint64 now = QDateTime::currentMSecsSinceEpoch() / 1000;
    // refreshable counts as signed-in (see signedIn() above)
    m[QStringLiteral("signedIn")]      = !m_accessToken.isEmpty()
            && (!ytauth::accessExpired(m_accessExpiresEpoch, now)
                || !m_refreshToken.isEmpty());
    m[QStringLiteral("hasRefresh")]    = !m_refreshToken.isEmpty();
    m[QStringLiteral("pairingActive")] = m_pairActive;
    // Ladder telemetry — presence word only, cookie bytes never leave the file.
    const bool cookiePresent = QFile::exists(m_dataRoot + QStringLiteral("/cookies.txt"));
    m[QStringLiteral("cookieFallback")] = cookiePresent;
    // Post-axe telemetry: the label IS the credentialMode() ladder (single
    // source). The old inline paraphrase claimed an "oauth" for dead tokens
    // and printed "file" — a mode that is deliberately no longer a credential.
    const auto credMode = ytauth::credentialMode(
        cookiePresent, m[QStringLiteral("signedIn")].toBool());
    qWarning("[Youtube] auth-mode=%s",
             credMode == ytauth::CredentialMode::Cookie ? "cookie"
             : credMode == ytauth::CredentialMode::OAuth ? "oauth"
                                                        : "signedOut");
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
    if (m_refreshInFlight)
        return; // one swap at a time; queued pending requests replay on its success leg
    m_refreshInFlight = true;
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
        m_refreshInFlight = false; // the swap completed (or errored)
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
            qWarning("[Youtube] access token refreshed — account paths live again");
            if (m_pendingReplay.takeChannels())
                load_channels(false); // FEchannels retry now that the bearer is fresh
            const ytui::PendingReplay::ListRequest replay = m_pendingReplay.takeListOrVideos();
            if (replay.kind == ytui::PendingReplay::ListRequest::Kind::AccountList)
                loadAccountList(replay.value); // retry now that the token is live
            else if (replay.kind == ytui::PendingReplay::ListRequest::Kind::PlaylistVideos)
                loadAccountPlaylistVideos(replay.value); // same re-entry, videos path
        } else {
            m_accessToken = QString(); // dead auth — views fall back to files
            saveAuthFile();
            qWarning("[Youtube] token refresh declined — signed out, re-pair on the CRT");
            m_pendingReplay.clearAll();
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
    if (ytauth::accessExpired(m_accessExpiresEpoch, QDateTime::currentMSecsSinceEpoch() / 1000)) {
        m_pendingReplay.deferList(kind); // re-enters from requestRefresh's success leg
        requestRefresh();
        return;
    }
    m_pendingReplay.clearList(); // live request: nothing to replay later
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
    // Credential ladder (ytauth::credentialMode in YtAuthGate.h, cookie-first
    // per locked decision): a present pasted cookie outranks the 1h bearer.
    const QString cookieHeader = cookieHeaderValue();
    if (!cookieHeader.isEmpty())
        req.setRawHeader("Cookie", cookieHeader.toUtf8());
    else if (!m_accessToken.isEmpty())
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
    if (kind == QStringLiteral("subschannels")) {
        // FEchannels: whole subscribed-channel list, one page (probe: 313
        // tiles, no continuation). Names land in m_channels so buildChannelList
        // shows real names; video lists stay lazy per-channel (RSS on open).
        acc += ytui::extractChannelSyncRows(root);
        const QString next = ytui::continuationToken(root);
        qWarning("[Youtube] channel-sync page=%d rows=%lld http_ok=%d continued=%d",
                 page, static_cast<long long>(acc.size()), netError ? 0 : 1,
                 next.isEmpty() ? 0 : 1);
        if (acc.isEmpty() && !next.isEmpty() && page < kBrowseMaxPages) {
            browsePage(kind, next, page + 1, acc);
            return;
        }
        QStringList ids;
        for (const QVariant &row : acc) {
            const QVariantMap r = row.toMap();
            const QString id = r.value(QStringLiteral("channelId")).toString();
            if (id.isEmpty() || ids.contains(id))
                continue;
            ids << id;
            ChannelEntry &e = m_channels[id];
            e.channelId = id;
            if (!r.value(QStringLiteral("title")).toString().isEmpty())
                e.channelName = r.value(QStringLiteral("title")).toString();
        }
        if (!ids.isEmpty())
            m_channelOrder = ids;
        else if (m_channelOrder.isEmpty())
            qWarning("[Youtube] channel-sync yielded no ids (parse shape drift?)");
        if (m_feedWantsRoster) {
            // Signed-in Subscriptions opened: the roster just landed fresh —
            // kick the RSS union off it (emit lands via finishAggregate).
            m_feedWantsRoster = false;
            const QStringList roster = ids.isEmpty() ? m_channelOrder : ids;
            if (!roster.isEmpty())
                stormFeedFromRoster(roster, /*forceRefresh=*/false);
            else
                ensureFresh(true); // roster dead-end — retry on the SEEDED roster
                // (subs-text seed is a pre-pairing bootstrap, not a credential mode:
                // the 2026-11-06 axe removed the file as the ladder's last stand)
            return;
        }
        if (m_emitChannelsWhenDone) {
            m_emitChannelsWhenDone = false;
            emit channelsLoaded(buildChannelList());
        }
        return;
    }
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
    if (ytauth::accessExpired(m_accessExpiresEpoch, QDateTime::currentMSecsSinceEpoch() / 1000)) {
        m_pendingReplay.deferPlaylistVideos(playlistId); // re-enters from requestRefresh's success leg
        requestRefresh();
        return;
    }
    m_pendingReplay.clearVideos(); // live request: nothing to replay later
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

