#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QSet>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QJsonObject>
#include <QQueue>
#include <QNetworkAccessManager>
#include "../../util/YtSponsorEdl.h"

// Backend for the YouTube module (V1 "feed" approach — no auth).
//
// The user lists channel IDs (one per line) in <dataRoot>/youtube_subscriptions.txt.
// Video lists come from each channel's official RSS feed (titles, exact publish
// dates, channel name — ~15 newest videos), fetched unauthenticated. Results are
// cached in memory per channel for the session (kCacheTtlMs TTL), so the first
// entry into Subscriptions or Channels fills the cache for every other view.
//
// Playlists come from <dataRoot>/youtube_playlists.txt (one playlist URL or ID
// per line, optional "My Name | <url>" display-name prefix). RSS feeds for
// playlists stop at 15 entries, so playlist contents are fetched by spawning
// yt-dlp --flat-playlist instead (async QProcess, same session cache TTL).
//
// Two user files besides subscriptions/playlists:
//   youtube_history.json     — watch history + resume positions, keyed by videoId
//   youtube_watch_later.json — ordered saved-video list (newest first)
class YouTubeBackend : public QObject {
    Q_OBJECT
public:
    explicit YouTubeBackend(const QString &appRoot, const QString &dataRoot,
                            QObject *parent = nullptr);

    // Synchronous subscriptions-file check for the menu view:
    // { ok: bool, error: QString, fileExists: bool, channelCount: int }
    Q_INVOKABLE QVariantMap check_subscriptions();

    Q_INVOKABLE void load_subscriptions_feed(bool forceRefresh = false);
    Q_INVOKABLE void load_channels(bool forceRefresh = false);
    Q_INVOKABLE void load_channel_videos(const QString &channelId, bool forceRefresh = false);

    // Synchronous playlists-file check, mirroring check_subscriptions():
    // { ok: bool, error: QString, fileExists: bool, playlistCount: int }
    Q_INVOKABLE QVariantMap check_playlists();

    Q_INVOKABLE void load_playlists(bool forceRefresh = false);
    Q_INVOKABLE void load_playlist_videos(const QString &playlistId, bool forceRefresh = false);

    // Maps the playback_resolution setting ("480p"/"720p"/"1080p", unknown → 480p)
    // to a yt-dlp format string. H.264 is preferred first for RPi hardware decode.
    Q_INVOKABLE QString ytdlFormatForResolution(const QString &resolution) const;

    // Direct-playback path (issue #28 plan A): ytdl_hook re-resolves every stream
    // at each mpv launch, which is seconds on the Pi. resolvePlayback asks yt-dlp
    // for the signed URL in advance (background, fire-and-forget); cachedPlaybackUrl
    // hands it to the Player while its signed window is still fresh, and the Player
    // launches mpv with --ytdl=no around it. Freshness is the URL's own expire=
    // epoch (parse + margin math live in util/YtUrlFresh.h, unit-tested).
    //
    // The format string is usually bestvideo+bestaudio, so yt-dlp -g prints TWO
    // URLs (video line 1, audio line 2). Playing only line 1 hands mpv a silent
    // video-only stream; the pair must be cached and passed together
    // (url + --audio-file=<audio>). cachedPlaybackAudioUrl mirrors the video lookup.
    // A ONE-line output (live broadcasts, combined best[] formats) is a fully
    // muxed single stream: cached with a single=1 marker and played without
    // --audio-file (cachedPlaybackSingle exposes the marker). Entries missing
    // BOTH the audio half and the marker predate the pairing and stay misses.
    Q_INVOKABLE void resolvePlayback(const QString &videoId, const QString &resolution);
    Q_INVOKABLE QString cachedPlaybackUrl(const QString &videoId, const QString &resolution);
    Q_INVOKABLE QString cachedPlaybackAudioUrl(const QString &videoId, const QString &resolution);

    Q_INVOKABLE bool cachedPlaybackSingle(const QString &videoId,
                                          const QString &resolution);

    // Plan A warm-up: one pass over watch history finds the newest video with
    // an unfinished resume position and fires its background resolve, so the
    // resume-into-a-warm-stream case costs zero at play time.
    Q_INVOKABLE void warmResumePlayback(const QString &resolution);

    // Plan A round 4: speculative warm-up of the CURRENT list. Whenever a view
    // presents a video list (feed, channel, playlist, watch-later, history),
    // the front entries are resolved in the background — one chained yt-dlp
    // process at a time — so picking any of them plays with the direct-URL
    // path at the mpv-init floor instead of paying a cold resolve. Entries
    // already fresh or in flight are skipped; queue caps bound the backlog.
    Q_INVOKABLE void warmListPlayback(const QStringList &videoIds,
                                      const QString &resolution);

    // SponsorBlock: ask for the k-anonymous segment window for a video (cached
    // hits re-emit sponsorReady synchronously). Also fired for every warmed row
    // so list pages arrive with segments pre-cached before the first pick.
    Q_INVOKABLE void requestSponsorSegments(const QString &videoId);
    // Wrap the fresh cached URL pair in an edl:// virtual timeline honoring the
    // per-category actions ({category: "off"|"mark"|"skip"}). Returns {videoEdl,
    // audioEdl} (empty string = use plain URL); empty map = no fresh pair yet.
    Q_INVOKABLE QVariantMap buildPlaybackEdl(const QString &videoId,
                                             const QString &resolution,
                                             const QVariantMap &actions);

    // Plan D: device-flow sign-in (TV pairing) + account lists via InnerTube.
    // The public youtube-tv client pair (id/secret + InnerTube key) is taken
    // verbatim from the Kodi "youtube-tv" client of the plugin.video.youtube
    // add-on (github.com/koying/plugin.video.youtube, resources/lib/youtube/
    // client/login_client.py CONFIGS['youtube-tv']) — the same public pair
    // every limited-input YouTube client family uses; credited in THANKS.md.
    // Endpoints (verified live 2026-11-03): start POST www.youtube.com/o/oauth2/
    // device/code → {device_code,user_code,interval,expires_in}; poll/swap POST
    // www.youtube.com/o/oauth2/token (grant_type device/1.0) while consent is
    // pending ("authorization_pending"); refresh with grant_type=refresh_token.
    // Tokens persist in youtube_auth.json under the app data root (0600-style
    // private file, values never logged). When signed out / auth dead, the
    // module falls back to the upstream files (subscriptions.txt / history /
    // watch-later) — behavior-preserving for users who never sign in.
    Q_INVOKABLE bool signedIn();                // usable access token present
    // Pair-state accessors so a freshly-opened view can re-read a pending
    // device flow without restarting it.
    Q_INVOKABLE bool pairActive();
    Q_INVOKABLE QString pairUserCode();
    Q_INVOKABLE QString pairVerificationUrl();
    Q_INVOKABLE QVariantMap check_signin();     // { signedIn, hasRefresh } for views
    Q_INVOKABLE void startDeviceSignIn();       // fires devicePairRequested(userCode,url)
    Q_INVOKABLE void cancelDeviceSignIn();      // stop a pending poll loop
    Q_INVOKABLE void deviceSignOut();           // drop auth; file fallbacks resume
    // kind: "feed" (FEsubscriptions) | "history" (FEhistory) | "watchlater"
    // (discovered VL cursor) | "playlists" (discovered tab cursor: raw
    // continuation or UC/PU browseId on the account home)
    Q_INVOKABLE void loadAccountList(const QString &kind);
    // Videos of a signed-in account playlist via the account browse — the
    // signed twin of load_playlist_videos (yt-dlp). Emits the same
    // playlistVideosLoaded(playlistId, videos) contract with the SAME id the
    // caller passed, so views reuse one handler for both paths.
    Q_INVOKABLE void loadAccountPlaylistVideos(const QString &playlistId);

    // Watch history (youtube_history.json). A finished video stays in history
    // with pos 0 (so it lists under History but never prompts to resume);
    // entries are pruned to the kMaxHistoryItems most recently played.
    Q_INVOKABLE QVariantMap  getSavedPosition(const QString &videoId);
    Q_INVOKABLE void         savePosition(const QString &videoId, int positionMs,
                                          const QString &title, const QString &channelName);
    Q_INVOKABLE QVariantList getHistory() const;   // displayable entries, newest first
    Q_INVOKABLE void         delete_history();     // settings action slot

    // Watch later (youtube_watch_later.json), newest-saved first, manual removal only
    Q_INVOKABLE QVariantList getWatchLater() const;
    Q_INVOKABLE bool         isInWatchLater(const QString &videoId) const;
    Q_INVOKABLE void         addToWatchLater(const QString &videoId, const QString &title,
                                             const QString &channelName);
    Q_INVOKABLE void         removeFromWatchLater(const QString &videoId);
    Q_INVOKABLE void         delete_watch_later(); // settings action slot

signals:
    void subscriptionsFeedLoaded(const QVariant &videos);
    void channelsLoaded(const QVariant &channels);
    void channelVideosLoaded(const QString &channelId, const QVariant &videos);
    void playlistsLoaded(const QVariant &playlists);
    void playlistVideosLoaded(const QString &playlistId, const QVariant &videos);
    void errorOccurred(const QString &message);
    // A background resolvePlayback() for this video just stored a fresh
    // video+audio URL pair in the cache. The Player listens for this when it
    // launched on a cold cache: instead of paying ytdl_hook's re-resolve, it
    // waits (bounded) for this signal and then launches mpv on the direct pair.
    void playbackUrlReady(const QString &videoId);
    // Plan D: pairing card surfaced to the view (user code + where to type it),
    // the flow's outcome, and signed-in account lists (row model = the module's).
    void devicePairRequested(const QString &userCode, const QString &verificationUrl);
    void deviceSignInFinished(bool success, const QString &reason);
    void accountFeedLoaded(const QVariant &videos);
    void accountHistoryLoaded(const QVariant &videos);
    void accountWatchLaterLoaded(const QVariant &videos);
    // Signed-in account playlists tab: rows = {playlistId, title, videoCountText}
    // in YouTube tab order (recent activity first, never re-sorted).
    void accountPlaylistsLoaded(const QVariant &playlists);
    // SponsorBlock segment window finished for this video (fetch or cache hit);
    // the Player gates its EDL-bearing launch on this + playbackUrlReady.
    void sponsorReady(const QString &videoId);

private:
    // SponsorBlock state: normalized segments per videoId (an empty entry is a
    // confirmed "no segments" and suppresses re-fetch; transient failures stay
    // out of the map so the next play retries), in-flight set, lazy file cache.
    QHash<QString, QVector<yt::Segment>> m_sponsorCache;
    QSet<QString> m_sponsorFetching;
    QHash<QString, qint64> m_sponsorStartMs;
    bool m_sponsorCacheLoaded = false;
    QString sponsorCacheFilePath() const;
    void ensureSponsorCacheLoaded();
    void saveSponsorCache();

    struct ChannelEntry {
        QString      channelId;
        QString      channelName;      // from the RSS feed <title>
        QVariantList videos;           // newest first
        qint64       fetchedMs = 0;    // 0 = never fetched successfully
        bool         feedOk    = false;
    };

    struct PlaylistEntry {
        QString      playlistId;
        QString      fileName;         // optional "Name |" override from the file
        QString      fetchedTitle;     // playlist_title reported by yt-dlp
        QVariantList videos;           // playlist order
        qint64       fetchedMs = 0;
        bool         fetchOk   = false;
    };

    struct PlaylistFileRef {
        QString id;
        QString name;                  // empty when the line had no "Name |" prefix
    };

    QString      historyFilePath() const;
    QVariantMap  loadHistory() const;
    void         saveHistory(const QVariantMap &history);
    QString      watchLaterFilePath() const;
    QVariantList loadWatchLater() const;
    void         saveWatchLater(const QVariantList &list);

    // Direct-playback URL cache (issue #28 plan A). Persisted as
    // youtube_url_cache.json: { "videoId|formatSpec": { url, expire } }.
    QString      urlCacheFilePath() const;
    void         ensureUrlCacheLoaded();   // lazy one-time load + expiry prune
    // Shared lookup for the video/audio pair a resolve stored under one key.
    QString      cachedUrlField(const QString &videoId, const QString &resolution,
                                const QString &field);
    void         storeUrlCacheEntry(const QString &key, const QString &url,
                                    const QString &audioUrl, bool single,
                                    qint64 expireEpoch); // prune + persist

    QStringList  readSubscriptionIds(QString *error = nullptr) const;
    void         ensureFresh(bool forceRefresh);
    void         refreshChannel(const QString &channelId);
    void         finishAggregate();
    QVariantList buildFeed() const;
    QVariantList buildChannelList() const;
    QNetworkRequest makeRequest(const QUrl &url) const;

    QList<PlaylistFileRef> readPlaylistEntries(QString *error = nullptr) const;
    void         ensurePlaylistsFresh(bool forceRefresh);
    void         spawnNextPlaylistFetch();
    void         finishPlaylistAggregate();
    QVariantList buildPlaylistList() const;

    QString m_appRoot;
    QString m_dataRoot;
    QNetworkAccessManager m_nam;

    QHash<QString, ChannelEntry> m_channels;  // in-memory session cache
    QStringList m_channelOrder;               // channel IDs in file order (deduped)
    int m_pendingChannels = 0;

    // Emit-when-done flags: while one refresh is in flight, additional load
    // calls just queue their result signal on it instead of re-requesting.
    bool    m_emitFeedWhenDone     = false;
    bool    m_emitChannelsWhenDone = false;
    QString m_emitChannelVideosWhenDone;      // channelId, or empty

    // Playlist mirror of the channel cache/refresh state, fed by yt-dlp
    // subprocesses instead of RSS requests.
    QHash<QString, PlaylistEntry> m_playlists;
    QStringList m_playlistOrder;              // playlist IDs in file order (deduped)
    QStringList m_playlistFetchQueue;         // stale IDs waiting for a process slot
    int m_pendingPlaylists       = 0;
    int m_activePlaylistFetches  = 0;

    bool    m_emitPlaylistsWhenDone = false;
    QString m_emitPlaylistVideosWhenDone;     // playlistId, or empty

    // Direct-playback URL cache state (issue #28 plan A)
    QJsonObject m_urlCache;
    bool        m_urlCacheLoaded = false;

    // Speculative warm-up state (issue #28 plan A round 4). m_resolveKeys
    // tracks every cache key with a live yt-dlp process (press-initiated or
    // warm), so warm passes and a Player never duplicate work on one video.
    // m_warmChainKey is the warm entry currently owning the single chained
    // process; its completion pumps the queue to the next entry.
    QStringList         m_warmQueue;    // "videoId<US>resolution" tokens
    QSet<QString>       m_resolveKeys;
    QString             m_warmChainKey;
    QHash<QString, qint64> m_resolveStartMs; // key -> launch ms, for the journal elapsed figure
    QString              m_pendingListKind; // account list re-entered after a refresh swap
    QString       m_pendingPlaylistVideos;  // playlist-videos id re-entered after refresh
    QString       m_accountVideosEmitId;    // id echoed back in playlistVideosLoaded
    bool                hasFreshUrlPair(const QString &key) const;
    QString             warmToken(const QString &videoId,
                                 const QString &resolution) const; // US-separator form
    void                launchUrlResolve(const QString &videoId, const QString &resolution,
                                         const QString &fmt, const QString &key);
    void                finishUrlResolve(const QString &key);
    void                pumpWarmQueue();

    // Plan D auth/device-flow state (tokens NEVER appear in telemetry/logs).
    QString m_accessToken, m_refreshToken;
    qint64 m_accessExpiresEpoch = 0;   // epoch seconds; 0 = unknown
    bool   m_refreshInFlight = false;  // one token swap at a time (replay flags ride it)
    bool   m_pendingFeChannels = false; // FEchannels deferred behind an in-flight refresh
    bool   mAuthLoaded = false;        // lazy one-time youtube_auth.json read
    bool   m_pairActive = false;       // a device-flow poll loop is running
    QString m_pairDeviceCode;
    QString m_pairUserCode;   // dashed code shown on the card for the user to type
    QString m_pairVerifyUrl;  // verification URL the user opens on his own device
    int    m_pollIntervalS = 5;
    int    m_pollsLeft = 0;
    void   ensureAuthLoaded();         // lazy load + expiry note (no request)
    QString authFilePath() const;
    QString cookieHeaderValue() const;   // Netscape cookies.txt -> browse Cookie line ("" = absent)
    void   saveAuthFile();             // write the auth doc (atomic swap)
    void   requestDeviceCode();
    void   schedulePairPoll();         // QTimer self-chain until consent/expiry
    void   finishPairResponse(const QJsonObject &json);
    void   requestRefresh();           // grant_type=refresh_token swap
    QString bearerQuery() const;       // "?key=" tv key (public, embedded)
    void   browsePage(const QString &browseKind, const QString &cursor,
                     int page, const QVariantList &acc);
    void   finishBrowsePage(const QString &kind, QNetworkReply *reply,
                           QVariantList acc, int page);

    static constexpr qint64 kCacheTtlMs      = 15 * 60 * 1000;
    static constexpr int    kMaxFeedItems    = 100;
    static constexpr int    kMaxHistoryItems = 100;
    static constexpr int    kMaxPlaylistItems = 500;             // caps infinite Mix/Radio lists
    static constexpr int    kMaxConcurrentPlaylistFetches = 2;   // yt-dlp is heavy on the Pi
    static constexpr int kPlaylistFetchTimeoutMs = 60000;
    static constexpr qint64 kUrlSafetyMarginS   = 120;   // buffer+launch slack below expiry
    static constexpr int    kUrlCacheMaxEntries = 80;
    static constexpr int    kYtdlResolveTimeoutMs = 30000;
    static constexpr int    kWarmListMax  = 12;  // front entries warmed per list view
    static constexpr int    kWarmQueueMax = 24;  // backlog cap across rapid browsing

    // Wave-capped RSS refresh queue: the signed-in feed union touches every
    // subscribed channel (~311 RSS GETs); issuing them all at once blows up
    // the Pi's sockets. queueRefresh records the outstanding total, pumpRefresh
    // keeps active fetches under kMaxConcurrentFeedFetches, and each
    // completion admits the next in line.
    QQueue<QString> m_refreshQueue;
    int             m_activeRefresh = 0;
    bool            m_feedWantsRoster = false; // FEchannels roster hop feeds the union
    void            queueRefresh(const QStringList &stale);
    void            pumpRefresh();
    void            stormFeedFromRoster(const QStringList &ids, bool forceRefresh);
    static constexpr int kMaxConcurrentFeedFetches = 32;
};
