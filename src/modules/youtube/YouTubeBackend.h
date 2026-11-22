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
#include <QTimer>
#include <QNetworkAccessManager>
#include "../../util/YtSponsorEdl.h"
#include "../../util/YtSponsorTtl.h"
#include "../../util/YtPendingReplay.h"
#include "../../util/YtUrlFresh.h"

class YtInnertubeResolver;

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
// playlists stop at 15 entries, so playlist contents are fetched natively via
// the Innertube browse walk (browsePlaylist, same session cache TTL).
//
// Two user files besides subscriptions/playlists:
//   youtube_history.json     — watch history + resume positions, keyed by videoId
//   youtube_watch_later.json — ordered saved-video list (newest first)
class YouTubeBackend : public QObject {
    Q_OBJECT
public:
    explicit YouTubeBackend(const QString &appRoot, const QString &dataRoot,
                            QObject *parent = nullptr);
    // Flushes a pending debounced sponsor save so no confirmed window is lost
    // on app exit (the save timer coalesces idle-fill bursts into one write).
    ~YouTubeBackend() override;

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
    // at each mpv launch, which is seconds on the Pi. resolvePlayback asks the
    // Innertube resolver for the signed URL in advance (background,
    // fire-and-forget); cachedPlaybackUrl hands it to the Player while its
    // signed window is still fresh, and the Player launches mpv with --ytdl=no
    // around it. Freshness is the URL's own expire= epoch (parse + margin math
    // live in util/YtUrlFresh.h, unit-tested).
    //
    // The format string is usually bestvideo+bestaudio, so the resolver prints
    // TWO URLs (video line 1, audio line 2). Playing only line 1 hands mpv a
    // silent video-only stream; the pair must be cached and passed together
    // (url + --audio-file=<audio>). cachedPlaybackAudioUrl mirrors the video
    // lookup. A ONE-line output (live broadcasts, combined best[] formats) is
    // a fully muxed single stream: cached with a single=1 marker and played
    // without --audio-file (cachedPlaybackSingle exposes the marker). Entries
    // missing BOTH the audio half and the marker predate the pairing and stay
    // misses.
    Q_INVOKABLE void resolvePlayback(const QString &videoId, const QString &resolution);
    Q_INVOKABLE QString cachedPlaybackUrl(const QString &videoId, const QString &resolution);
    Q_INVOKABLE QString cachedPlaybackAudioUrl(const QString &videoId, const QString &resolution);

    Q_INVOKABLE bool cachedPlaybackSingle(const QString &videoId,
                                          const QString &resolution);

    // Reactive warm-403 cure: a clock-fresh cached URL can still 403 (the
    // signature binds to the minting session/IP, not just expire=). When the
    // Player's warm launch dies before first position, it evicts that entry
    // and re-enters the cold resolve — costs network only on actual failure.
    // Drop the cached stream entry for one video+resolution after a playback
    // failure. allowRetry=false (default): the eviction also marks the key's
    // in-process Innertube attempt spent — a URL that failed to play must not
    // be re-minted by the same client in a resolve -> 403 -> evict -> resolve
    // loop. allowRetry=true (the warm-403 cure): the eviction exists to make
    // a SAME-SESSION re-resolve mint a fresh URL — spending the key here would
    // turn the cure's resolve into an instant RESOLVE_BUSY (the 2026-11-22
    // stuck-on-Loading defect), so the key stays eligible.
    Q_INVOKABLE void invalidateCachedUrl(const QString &videoId,
                                         const QString &resolution,
                                         bool allowRetry = false);

    // Plan A warm-up: one pass over watch history finds the newest video with
    // an unfinished resume position and fires its background resolve, so the
    // resume-into-a-warm-stream case costs zero at play time.
    Q_INVOKABLE void warmResumePlayback(const QString &resolution);

    // Plan A round 4: speculative warm-up of the CURRENT list. Whenever a view
    // presents a video list (feed, channel, playlist, watch-later, history),
    // the front entries are resolved in the background — one chained resolve
    // at a time — so picking any of them plays with the direct-URL path at the
    // mpv-init floor instead of paying a cold resolve. Entries already fresh or
    // in flight are skipped; queue caps bound the backlog.
    Q_INVOKABLE void warmListPlayback(const QStringList &videoIds,
                                      const QString &resolution);

    // SponsorBlock: ask for the k-anonymous segment window for a video. Cached
    // hits re-emit sponsorReady synchronously; a transient failure is
    // negative-cached for ytsb::kFailureTtlMs so a dead sponsor.ajay.app costs
    // one request per video per window. Also fired for every prefetched row so
    // list pages arrive with segments pre-cached before the first pick.
    Q_INVOKABLE void requestSponsorSegments(const QString &videoId);
    // Idle fill driver — wired to IdleTracker in main.cpp. `idle` true starts a
    // paced, low-priority walk of every known video id; any input activity stops
    // it immediately so a foreground press never queues behind it.
    void setIdleFillActive(bool idle);
    void noteKnownVideoIds(const QStringList &videoIds);
    // Launch-instant truth for the Player's yt-sponsor state log: was the
    // segment window already on file when the EDL was frozen?
    Q_INVOKABLE bool hasSponsorSegments(const QString &videoId);
    Q_INVOKABLE bool sponsorFetchInFlight(const QString &videoId) const;
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
    // signed twin of load_playlist_videos (browsePlaylist). Emits the same
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
    // A permanent negative-cache result suppresses the Player's timed mpv/
    // ytdl_hook fallback as well as backend resolver work. Reason is the
    // canonical LOGIN_REQUIRED or UNPLAYABLE class, never a token/response body.
    void playbackResolveFailed(const QString &videoId, const QString &reason);
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

    // Resolve telemetry for the Pi state log: which resolver served the pair
    // (innertube / ytdlp / cache) and how long it took. Wired to
    // AppCore::note_state in main.cpp; note_state is compiled out of release
    // builds, so this costs a signal emission and nothing else.
    void resolveTelemetry(const QString &videoId, const QString &source, qint64 ms);

private:
    // SponsorBlock state: normalized segments per videoId (an empty entry is a
    // confirmed "no segments" and suppresses re-fetch; transient failures live
    // in m_sponsorFailedAt with a short TTL so the next play retries after it,
    // in-flight set, lazy file cache.
    QHash<QString, QVector<yt::Segment>> m_sponsorCache;
    QHash<QString, qint64> m_sponsorFailedAt;   // videoId -> transient-failure stamp (ms)
    QSet<QString> m_sponsorFetching;
    QHash<QString, qint64> m_sponsorStartMs;
    // Recency index for the sponsor pair (MRU front). ONE index co-evicts
    // m_sponsorCache + m_sponsorFetchedAt so their key sets stay identical;
    // touched on cache hit as well as insert (bounded eviction).
    QStringList m_sponsorLru;
    bool m_sponsorCacheLoaded = false;
    // Debounced persistence: an idle-fill burst lands one answer per pace tick,
    // and rewriting the (bounded) array per answer is redundant SD-card wear.
    // A dirty flag + restarting single-shot timer coalesces the burst into one
    // write; the destructor flushes so app exit never loses a confirmed window.
    QTimer *m_sponsorSaveTimer = nullptr;
    bool m_sponsorSaveDirty = false;
    QString sponsorCacheFilePath() const;
    void ensureSponsorCacheLoaded();
    void saveSponsorCache();
    void markSponsorCacheDirty();

    // Idle SponsorBlock fill. The Pi is on all day; while nobody is pressing
    // keys we walk every video id we have ever seen and confirm its segment
    // window, so a later play finds the EDL already on disk and never waits.
    // Strictly lower priority than live navigation: the pump yields whenever a
    // foreground warm queue or resolve is busy, and aborts on input activity.
    QHash<QString, qint64> m_sponsorFetchedAt;  // videoId -> confirm stamp (ms)
    QSet<QString>          m_knownVideoIds;     // every id the UI has surfaced
    // Independent recency index for the known pool (MRU front); bounded
    // separately from the sponsor pool, sized <= it.
    QStringList            m_knownLru;
    QStringList            m_idleQueue;
    QTimer                *m_idleTimer = nullptr;
    bool                   m_idleFilling = false;
    QString knownIdsFilePath() const;
    void ensureKnownIdsLoaded();
    void saveKnownIds();
    bool m_knownIdsLoaded = false;
    void pumpIdleFill();
    // Pure selection: which of `candidates` still need a window, newest-first
    // input order preserved, capped. Split out so it is unit-testable.
    QStringList idleCandidates(const QStringList &candidates, qint64 nowMs) const;

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
        QString      fetchedTitle;     // first video title from the browse walk
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
    // Permanent resolve failures live beside the URL cache and share no key
    // namespace with signed stream pairs.
    QString      negativeCacheFilePath() const;
    void         ensureNegativeCacheLoaded();
    void         saveNegativeCache();
    QString      storeNegativeCacheEntry(const QString &videoId, const QString &reason);
    void         clearNegativeCacheEntry(const QString &videoId);
    bool         negativeCacheBlocks(const QString &videoId);
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
    void         browsePlaylist(const QString &playlistId, const QString &cursor,
                                int page, const QVariantList &acc);
    void         assemblePlaylistFromBrowse(const QString &playlistId, const QVariantList &rows);
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

    // Playlist mirror of the channel cache/refresh state, fed by the
    // Innertube browse walk instead of RSS requests.
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

    // youtube_resolve_failures.json: videoId -> {failedAt, reason}. Permanent
    // failures cover every resolution and persist across app restarts.
    QJsonObject m_negativeCache;
    bool        m_negativeCacheLoaded = false;

    // Speculative warm-up state (issue #28 plan A round 4). m_resolveKeys
    // tracks every cache key with a live resolve (press-initiated or warm),
    // so warm passes and a Player never duplicate work on one video.
    // m_warmChainKey is the warm entry currently owning the single chained
    // resolve; its completion pumps the queue to the next entry.
    QStringList         m_warmQueue;    // "videoId<US>resolution" tokens
    QSet<QString>       m_resolveKeys;
    QString             m_warmChainKey;
    QHash<QString, qint64> m_resolveStartMs; // key -> launch ms, for the journal elapsed figure
    ytui::PendingReplay m_pendingReplay; // requests deferred behind an OAuth swap
    QString             m_accountVideosEmitId;    // id echoed back in playlistVideosLoaded
    bool                hasFreshUrlPair(const QString &key) const;
    QString             warmToken(const QString &videoId,
                                 const QString &resolution) const; // US-separator form
    // One in-process Innertube resolver (overnight torture t_7ffb1fd1). Cheap
    // enough to warm a whole list; no subprocess fallback remains (NEG-01).
    YtInnertubeResolver *m_itResolver = nullptr;
    // Keys with a spent in-process attempt. A key gets exactly one Innertube
    // try; after that it surfaces RESOLVE_BUSY on warm paths, so a failed
    // resolve never loops.
    QSet<QString> m_itTried;
    void                launchUrlResolve(const QString &videoId, const QString &resolution,
                                          const QString &key);
    void                launchHlsResolve(const QString &videoId, const QString &resolution,
                                         const QString &key, const QString &hlsUrl);
    void                finishUrlResolve(const QString &key);
    void                pumpWarmQueue();

    // Plan D auth/device-flow state (tokens NEVER appear in telemetry/logs).
    QString m_accessToken, m_refreshToken;
    qint64 m_accessExpiresEpoch = 0;   // epoch seconds; 0 = unknown
    bool   m_refreshInFlight = false;  // one token swap at a time (replay flags ride it)
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
    static constexpr int    kMaxConcurrentPlaylistFetches = 2;   // browse walks are cheap; keep it parallel
    static constexpr int kPlaylistFetchTimeoutMs = 60000;
    static constexpr qint64 kUrlSafetyMarginS   = yturl::kSafetyMarginS; // buffer+launch slack
    static constexpr int    kUrlCacheMaxEntries = 80;
    // The in-process resolver is ~20x cheaper than a subprocess spawn, so one
    // warm pass can cover a whole list page instead of the front dozen rows.
    static constexpr int    kWarmListMax  = 50;  // entries warmed per list view
    static constexpr int    kWarmQueueMax = 64;  // backlog cap across rapid browsing
    // Segment prefetch is cheaper than a URL resolve and covers the whole list
    // page, not just the warmed front; the cap only bounds pathological lists —
    // the view's focused-row prefetch covers anything past it while scrolling.
    static constexpr int    kWarmSponsorPrefetchMax = 50;
    // Idle fill: one request per tick, paced so the community SponsorBlock
    // service is never hammered, and capped per idle window so a long idle
    // spell cannot turn into an unbounded crawl.
    static constexpr int    kIdleFillMax    = 400;
    static constexpr int    kIdleFillPaceMs = 300;
    // Longer than the idle pace so a whole burst coalesces into one bounded
    // rewrite; a lone foreground answer still persists within half a second.
    static constexpr int    kSponsorSaveDebounceMs = 500;
    // Bounded eviction: hard LRU caps on the ID-keyed state, layered on
    // top of the existing success/failure TTLs (which are unchanged). Known is
    // sized <= sponsor so idle fill reaches quiescence instead of churning.
    static constexpr int    kSponsorCacheMaxEntries    = 512;
    static constexpr int    kKnownVideoIdMaxEntries    = 512;
    static constexpr int    kSponsorFailedAtMaxEntries = 512;

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
