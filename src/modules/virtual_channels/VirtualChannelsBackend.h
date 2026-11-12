#pragma once
#include <functional>
#include <optional>
#include <QHash>
#include <QJsonArray>
#include <QPair>
#include <QSet>
#include <QObject>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include "ChannelSchedule.h"
#include "LocalLibrary.h"
#include "ChannelTuner.h"
#include "DurationProbe.h"
#include "MediaServerSource.h"
#include "LibraryDb.h"
#include "ScheduleGenerator.h"
#include "TrackChoice.h"

class QTimer;

namespace vchan {
class TVDBNetworkResolver; // forward: used as a pointer member only
} // namespace vchan

class VirtualChannelsBackend : public QObject {
    Q_OBJECT
public:
    explicit VirtualChannelsBackend(const QString &appRoot,
                                    const QString &dataRoot,
                                    QObject *plexBackend = nullptr,
                                    QObject *jellyfinBackend = nullptr,
                                    QObject *embyBackend = nullptr,
                                    QObject *parent = nullptr);

    Q_INVOKABLE QVariantList get_channels();

    Q_INVOKABLE int current_slot(int channelNumber);

    Q_INVOKABLE QVariantMap preview_source(int channelNumber,
                                           int artWidth = 480, int artHeight = 270);
    Q_INVOKABLE void preview_stream(int channelNumber);
    Q_INVOKABLE void cancel_preview_stream();

    Q_INVOKABLE QVariantMap now_next(int channelNumber);

    Q_INVOKABLE QVariantList upcoming(int channelNumber, int count = 40);

    static constexpr qint64 kMaxGuideSpanMs = 7LL * 24 * 3600 * 1000;
    Q_INVOKABLE QVariantList guide_grid(double fromMs, double spanMs);

    Q_INVOKABLE double now_ms() const;

    Q_INVOKABLE QVariantMap tune(int channelNumber);

    // Move to the next audio track of the programme now airing and hand back a
    // descriptor for it, exactly as tune() would. Where a channel is
    // transcoding, the track is chosen by the server and cannot be changed
    // without asking for the stream again -- so this asks again, at the offset
    // the clock says the programme has reached, which is why the viewer sees it
    // carry on rather than start over.
    Q_INVOKABLE QVariantMap cycle_audio(int channelNumber);

    // The same for subtitles; off is one of the stops.
    Q_INVOKABLE QVariantMap cycle_subtitle(int channelNumber);

    Q_INVOKABLE QVariantMap after_playback(int channelNumber,
                                           const QString &reason,
                                           int slotIndex,
                                           int consecutiveFailures);

    Q_INVOKABLE void regenerate(int channelNumber);
    Q_INVOKABLE bool is_generating() const { return m_genActive; }

    Q_INVOKABLE QString media_root() const { return m_mediaRoot; }
    Q_INVOKABLE QString prober_name() const { return m_probe.proberName(); }

    Q_INVOKABLE void get_resume_options();
    Q_INVOKABLE void get_schedule_days_options();
    Q_INVOKABLE void get_audio_language_options();
    Q_INVOKABLE void get_subtitle_language_options();
    Q_INVOKABLE void rebuild_all();

    static constexpr int kMaxBookingsPerChannel = 48;

    static constexpr int kMinScheduleDays     = 1;
    static constexpr int kMaxScheduleDays     = 7;
    static constexpr int kDefaultScheduleDays = 2;
    static constexpr int kStartupSweepDelayMs = 20000;
    // The library walk starts after the schedule sweep so a launch never makes
    // the add-flow wait on a listing.
    static constexpr int kPresenceWalkDelayMs = 30000;
    Q_INVOKABLE int schedule_days() const;

    Q_INVOKABLE void top_up_schedules();
    Q_INVOKABLE void release_tuner();

    static constexpr int kMinGridMinutes = 5;
    static constexpr int kMaxGridMinutes = 120;

    Q_INVOKABLE QVariantMap channel_timing(int channelNumber);
    Q_INVOKABLE bool set_channel_grid(int channelNumber, int minutes);
    Q_INVOKABLE bool set_channel_ads(int channelNumber, int adsPerBreak);
    Q_INVOKABLE bool set_channel_order(int channelNumber, const QString &order);
    // "tv" or "movies". A movie channel airs films as its programmes, has no
    // movie slots and no grid, and takes its running order from where the films
    // come from rather than from a setting.
    Q_INVOKABLE bool set_channel_kind(int channelNumber, const QString &kind);
    // "playlist" or "selection", on a movie channel: a playlist airs in its own
    // order, a selection is shuffled.
    Q_INVOKABLE bool set_channel_films_from(int channelNumber, const QString &from);

    // A channel's day plans, as written. Public because it reads a channel
    // object and returns plans -- it touches nothing else, and the alternative
    // is testing the parser only through a whole generation.
    static QVector<vchan::DayPlan> readPlans(const QJsonObject &channel);

    // The channel's programmes with each one kept once -- once per block on a
    // channel laid out in blocks. Public so the rule can be tested on a list
    // rather than through a whole generation.
    static QVector<vchan::MediaItem>
    keepEachProgrammeOnce(const QVector<vchan::MediaItem> &programmes);

    struct PoolJob {
        vchan::SlotKind pool = vchan::SlotKind::Programme;
        int  apptIndex = -1;
        int  pack = -1;
        vchan::MediaServerSource::Request::Wants wants =
            vchan::MediaServerSource::Request::Wants::Episodes;

        QString library;
        QStringList titles;
        QStringList genres;
        // Movie channels ask "by studio": the studio strings the servers
        // carry (Plex Studio attribute, Jellyfin Studios, Emby Studio).
        QStringList studios;
        QStringList collections;
        QStringList playlists;
        bool anyFilm = true;
        QStringList match;
        // The picked series' own ids on their source. Preferred over the name:
        // a show renamed on the server keeps its id.
        QStringList showIds;
        // Which block of a day plan this job gathers for, so the generator can
        // tell afterwards which stretch each programme belongs to.
        int planBlock = -1;
        QSet<QString> excludeSeasons;
        QSet<QString> excludeEpisodes;
        vchan::SlotSource src = vchan::SlotSource::Plex;
    };
    // What a channel asks its source for, worked out from its pools and its
    // blocks. Public for the same reason readPlans is: it reads a channel
    // object and returns jobs, and the alternative is testing it only through
    // a whole generation against a live server.
    QVector<PoolJob> readPools(const QJsonObject &channel, vchan::ChannelDef &def) const;

    // The plans as the screens want them: a list of maps, each with a name,
    // the days it runs, where it starts, and its blocks in order.
    // "free" (the grid decides, as today) or "day_plan". A channel switched
    // away from its plans keeps them, unread, the way it keeps its slots.
    Q_INVOKABLE bool set_channel_schedule(int channelNumber, const QString &schedule);

    // withCounts walks the media folder to count each block's own bumper
    // clips. Only the screen that shows the count asks for it: the screen
    // that lays out a day re-reads the plans on every move, and counting
    // on each of those is a directory listing per block for nothing.
    Q_INVOKABLE QVariantList channel_plans(int channelNumber, bool withCounts = false);
    // Written back whole. The screens hold the list they were given, change one
    // thing in it and hand it back, which is how the pool editor already works.
    Q_INVOKABLE bool set_channel_plans(int channelNumber, const QVariantList &plans);

    // Switch one season or episode off for a single block, rather than for the
    // whole channel: two blocks can run the same series and take different
    // parts of it. Same shape as a pool row's exclusions, kept on the block.
    // What one block draws on. A block holds one thing, so choosing names it and
    // choosing another replaces it; an empty name clears it. Narrowing goes with
    // the thing it narrowed, so changing what a block plays forgets which parts
    // of the last one it was skipping.
    Q_INVOKABLE bool set_block_source(int channelNumber, int planIndex, int blockIndex,
                                      const QString &name, const QString &ref);

    // Every episode of one season airs again, whatever had been picked out of
    // it. The twin of clear_episode_exclusions, for a block.
    Q_INVOKABLE bool clear_block_episode_exclusions(int channelNumber, int planIndex,
                                                    int blockIndex, const QString &seasonKey);

    Q_INVOKABLE bool set_block_excluded(int channelNumber, int planIndex, int blockIndex,
                                        const QString &kind, const QString &itemKey,
                                        bool excluded, const QString &seasonKey = QString());

    Q_INVOKABLE QVariantList list_logos();
    Q_INVOKABLE QString logos_dir() const { return m_dataRoot + QStringLiteral("/logos"); }
    Q_INVOKABLE QString logo_path(const QString &file) const;

    // The local library index (one SQLite file): what the shelves can actually
    // air. The add-flow's option lists read this instead of guessing categories
    // from TheTVDB, so a genre whose only show sits on the other side of the
    // dial stops being offered where it can never play. An index nobody has
    // walked reads empty, and the UI keeps its catalog fallback in that case —
    // presence darkens nothing before the walk has run.
    Q_INVOKABLE QVariantList library_presence(const QString &subjectKind,
                                              const QString &channel,
                                              int limit = 40) const;
    // The count behind one option row ("Comedy — 23 shows"), so a dial can
    // prefer a category the library fills richly over one it barely has.
    Q_INVOKABLE int library_count_for(const QString &subjectKind,
                                      const QString &value,
                                      const QString &channel) const;
    // False until the launch walk put something in the index; the UI falls back
    // to the catalog path while it is false.
    Q_INVOKABLE bool library_index_known() const;
    Q_INVOKABLE QString channel_logo(int channelNumber);
    Q_INVOKABLE bool    set_channel_logo(int channelNumber, const QString &file);
    // How a channel's logo is drawn where it differs from the module's Channel
    // Logo settings: any of size, opacity, offset_x, offset_y, each in the same
    // units as the module setting. Only the keys a channel has set are present.
    Q_INVOKABLE QVariantMap channel_logo_style(int channelNumber);
    Q_INVOKABLE bool set_channel_logo_style(int channelNumber, const QString &key, double value);
    Q_INVOKABLE bool clear_channel_logo_style(int channelNumber);
    // {min, max} for a style key, so the screen and the file agree on one range.
    Q_INVOKABLE QVariantMap logo_style_range(const QString &key) const;

    Q_INVOKABLE int create_channel(const QString &name);
    // Build a preset channel: identity (name) is derived from the subject, the
    // recipe is recorded under "preset" so the channel is recognisably a
    // genre/network/collection channel. `wants` says whether the channel airs
    // shows or films ("tv" | "movies"). Returns the new channel number, or -1.
    Q_INVOKABLE int create_preset_channel(int kind, const QString &id,
                                          const QString &name,
                                          const QString &wants = QStringLiteral("tv"));

    // Preset recipe kind as stored: channels written before the type fix carry
    // it as a JSON STRING ("1") and newer ones as a number — QJsonValue::toInt()
    // on a string is 0, which re-typed every Network/Collection preset into
    // Genre on reload (identity filter skipped, bogus server block written;
    // the ABC preset aired all 21,615 library items). Always read preset kind
    // through this, never a bare toInt().
    static int presetKindOf(const QJsonObject &preset);

    // Stand the preset's content up: ask the connected sources for what the
    // recipe names and write the picks into the channel's pools as concrete
    // series/film entries, so the builder opens with everything already ticked
    // and the viewer's work is removing what they don't want. Async: answers
    // through presetSeeded. The recipe itself stays on the channel under
    // "preset" so "Seed from recipe" can run it again after the viewer's
    // removals (seed_preset_channel again) without the recipe deciding what
    // airs by itself.
    Q_INVOKABLE void seed_preset_channel(int channelNumber);
    Q_INVOKABLE bool seed_active() const { return m_seedActive; }

    Q_INVOKABLE QVariantList channel_interstitials(int channelNumber);
    Q_INVOKABLE QVariantList media_folders(int maxDepth = 3);

    Q_INVOKABLE QVariantList channel_bookings(int channelNumber);
    Q_INVOKABLE int  add_booking(int channelNumber);
    Q_INVOKABLE bool delete_booking(int channelNumber, int index);
    Q_INVOKABLE bool set_booking_name(int channelNumber, int index, const QString &name);
    Q_INVOKABLE bool set_booking_time(int channelNumber, int index, const QString &hhmm);
    Q_INVOKABLE bool set_booking_days(int channelNumber, int index, const QStringList &days);
    Q_INVOKABLE QStringList booking_list(int channelNumber, int index,
                                         const QString &field);
    Q_INVOKABLE bool set_booking_list(int channelNumber, int index,
                                      const QString &field, const QStringList &values);
    Q_INVOKABLE bool set_booking_any_film(int channelNumber, int index, bool any);
    Q_INVOKABLE bool set_booking_folder(int channelNumber, int index, const QString &folder);

    Q_INVOKABLE QStringList available_sources() const { return availableSources(); }
    // Asked per source rather than per channel: a break pool can draw on any
    // server that is connected, not only the one the channel is sourced from.
    Q_INVOKABLE bool source_supports_playlists(const QString &source) const;

    Q_INVOKABLE void browse_source(int channelNumber, const QString &kind,
                                   const QString &parentKey = QString());
    Q_INVOKABLE bool browse_busy() const;
    void browse_local(const QString &kind, const QString &parentKey);
    Q_INVOKABLE void browse_from(const QString &source, const QString &kind,
                                 const QString &parentKey = QString());

    Q_INVOKABLE QVariantList channel_pool(int channelNumber, const QString &pool);
    Q_INVOKABLE bool set_channel_pool(int channelNumber, const QString &pool,
                                      const QVariantList &entries);

    Q_INVOKABLE QVariantMap channel_source_config(int channelNumber);
    Q_INVOKABLE bool set_channel_source(int channelNumber, const QString &source);
    // `refs` runs alongside `values`: the id of each picked series on its
    // source, where the picker knew it. A blank entry leaves whatever id is
    // already stored for that name in place, so editing the list from a screen
    // that never saw the ids cannot throw them away.
    Q_INVOKABLE bool set_channel_list(int channelNumber, const QString &field,
                                      const QStringList &values,
                                      const QStringList &refs = {});
    // `seasonKey` says which season an episode belongs to, so that switching a
    // season back on can put all of its episodes on with it. Ignored for
    // seasons, which have no parent above the series.
    Q_INVOKABLE bool set_channel_excluded(int channelNumber, const QString &kind,
                                          const QString &itemKey, bool excluded,
                                          const QString &seasonKey = QString());
    // Every episode of one season airs again.
    Q_INVOKABLE bool clear_episode_exclusions(int channelNumber, const QString &seasonKey);

    Q_INVOKABLE QVariantList list_channels();

    // Load the subject values for a preset recipe. `kind` is "genre",
    // "network" or "collection": the first two come from TheTVDB (needs the
    // apikey set), collections from the connected media server. Delivers the
    // values via catalogReady; catalogFailed when the source is unavailable.
    Q_INVOKABLE void load_catalog(const QString &kind);

    // Pure keep rule for browse section enumeration (unit-testable, static):
    // which Plex library type feeds which browse kind. TV-side kinds ("shows",
    // "tvcollections") walk show libraries only, movie-side kinds (anything
    // starting "movie") walk movie libraries only, and the legacy "collections"
    // union takes both.
    static bool browseKeepsLibrary(const QString &browseKind,
                                   const QString &sectionType);

    Q_INVOKABLE int guide_channel_number() const;
    Q_INVOKABLE int weather_channel_number() const;
    Q_INVOKABLE int manage_channel_number() const;
    Q_INVOKABLE bool weather_channel_enabled() const;
    Q_INVOKABLE bool rename_channel(int number, const QString &name);
    Q_INVOKABLE bool move_channel(int number, int direction);
    Q_INVOKABLE bool delete_channel(int number);

    Q_INVOKABLE QVariantList get_menu_entries() { return {}; }

signals:
    void sourceBrowseReady(const QString &kind, const QVariantList &items);
    void sourceBrowseFailed(const QString &kind, const QString &reason);

    void dynamicOptionsReady(const QString &key, const QVariant &options);

    void playDescriptorReady(const QVariantMap &descriptor);
    void previewStreamReady(int channelNumber, const QString &url, double positionMs);

    void generationProgress(int channelNumber, int done, int total);
    void generationFinished(int channelNumber, bool ok, const QString &message);

    void actionStatus(const QString &message);

    // Catalog of preset-recipe subject values, delivered after load_catalog.
    // `kind` echoes the requested recipe; `options` is [{id, label}].
    void catalogReady(const QString &kind, const QVariantList &options);
    void catalogFailed(const QString &kind, const QString &message);

    // The seeding pass finished: ok says whether anything landed, and message
    // counts the shows/films written (or says why nothing did).
    void presetSeeded(int channelNumber, bool ok, const QString &message);

public slots:
    void onSettingChanged(const QString &moduleId, const QString &key, const QVariant &value);

private slots:
    void onPlexStreamUrlReady(const QString &url, const QString &plexToken);
    void onServerStreamUrlReady(const QString &url);
    void onServerItemLoaded(const QVariant &detail);
    void onPlexItemLoaded(const QVariant &detail);
    void onPlexLibrariesLoaded(const QVariant &libraries);
    void onPlexItemsLoaded(const QVariant &items);
    void onPlexChildrenLoaded(const QVariant &items);
    void onPlexCollectionsLoaded(const QVariant &collections);
    void onPlexPlaylistsLoaded(const QVariant &playlists);
    void onPlexSubtitleStreamSet(const QString &partId);
    void onPlexAudioStreamSet(const QString &partId);
    void onPlexTranscodeStopped(const QString &sessionId);
    void onGenerationTick();
    void onUrlTimeout();
    void onPlexEnumTimeout();
    // Bridges the media-server browse to the collection catalog: when a catalog
    // load is pending, the browse result (or failure) is delivered as
    // catalogReady/catalogFailed instead of to a source browser.
    void onCatalogBrowseReady(const QString &, const QVariantList &);
    void onCatalogBrowseFailed(const QString &, const QString &);

private:
    QString m_appRoot;
    QString m_dataRoot;
    QString m_mediaRoot;
    // Local files presented the way the servers present theirs, so one
    // browser can drive all four sources.
    mutable vchan::LocalLibrary m_localLibrary;

    QObject *m_plex = nullptr;
    QObject *m_jellyfin = nullptr;
    QObject *m_emby = nullptr;

    // The TVDB catalog resolver, built lazily once an apikey is known. A preset
    // genre/network channel enumerates its subject values from it.
    vchan::TVDBNetworkResolver *m_resolver = nullptr;
    // While a collection catalog is loading, this records what the caller asked
    // for so onPlexCollectionsLoaded can deliver it back through catalogReady.
    QString m_catalogPending;

    vchan::SlotSource channelSource(int channelNumber) const;
    // Builds (once) and returns the TVDB resolver, or nullptr if no apikey.
    vchan::TVDBNetworkResolver *resolver();
    static vchan::SlotSource sourceOf(const QJsonObject &channel);
    static bool    isMovieChannel(const QJsonObject &channel);
    static bool    usesDayPlan(const QJsonObject &channel);
    static bool    playsAPlaylist(const QJsonObject &channel);
    static bool usesEntryPools(const QJsonObject &channel);
    static QString    sourceBlockName(vchan::SlotSource src);
    QStringList       availableSources() const;
    bool              sourceSignedIn(QObject *backend) const;

    vchan::DurationProbe m_probe;

    struct CachedSchedule {
        vchan::ChannelSchedule schedule;
        qint64                 mtimeMs = 0;
        QString                error;
    };
    QHash<int, CachedSchedule> m_cache;

    QTimer *m_urlTimer = nullptr;
    bool    m_urlPending = false;
    QVariantMap m_urlDescriptor;
    QString m_plexAwaitingDetailFor;

    enum class PlexStage { Idle, Libraries, Shows, Seasons, Episodes,
                           ApptLibraries, ApptItems,
                           ApptCollections, ApptCollectionItems,
                           ApptPlaylists, ApptPlaylistItems,
                           Collections, CollectionItems,
                           Playlists, PlaylistItems,
                           BrowseLibraries, BrowseShows, BrowseMovies,
                           BrowseCollections, BrowsePlaylists, BrowseChildren };
    PlexStage   m_pgStage = PlexStage::Idle;
    QTimer     *m_pgTimer = nullptr;
    int         m_pgChannel = -1;
    vchan::ChannelDef m_pgDef;
    QString     m_pgLibraryName;
    QStringList m_pgMatch;
    QStringList m_pgShowIds;
    QStringList m_pgGenres;
    // How many things carry each label being browsed, so a picker can show it.
    QHash<QString, int> m_browseTally;
    QSet<QString> m_pgExcludeSeasons;
    QSet<QString> m_pgExcludeEpisodes;
    QStringList m_pgCollections;
    QStringList m_pgPlaylists;
    QStringList m_pgPendingCollections;
    QStringList m_pgPendingPlaylists;
    bool m_pgAskedCollections = false;
    bool m_pgAskedPlaylists   = false;
    QStringList m_pgSections;
    QStringList m_pgShows;
    QStringList m_pgSeasons;
    QVector<vchan::MediaItem> m_pgEpisodes;

    // Public: the same gate the harvest applies, so a test can ask the gate
    // directly beside the readPools tests.
public:
    static bool bookingWants(const PoolJob &job, const QString &title,
                             const QStringList &genres = QStringList(),
                             const QStringList &studios = QStringList());
private:
    void appendToPool(const PoolJob &job, const QVector<vchan::MediaItem> &items);
    static QVector<QPair<vchan::SlotKind, const char *>> poolFields();
    QVector<PoolJob> m_pgApptJobs;
    int              m_pgApptCursor = 0;
    QStringList      m_pgApptSections;
    int              m_pgApptAdded = 0;
    enum class ApptPhase { Items, Collections, Playlists, Done };
    ApptPhase        m_pgApptPhase = ApptPhase::Items;
    QStringList      m_pgApptAllSections;
    QStringList      m_pgApptSetKeys;
    QSet<QString>    m_pgApptSeen;
    void plexApptAdvance();
    int  plexApptCollect(const QVariantList &items, bool applyCriteria);

    void browse_plex_shows();
    void browse_plex_movies(const QString &kind);
    // kind names the browse ("collections" unions both libraries;
    // "tvcollections"/"moviecollections" walk only the matching type).
    void browse_plex_collections(const QString &kind);
    void browse_plex_playlists();
    void browse_plex_seasons(const QString &showRatingKey);
    void browse_plex_episodes(const QString &seasonRatingKey);

    void parseBookings(const QJsonObject &o, int channelNumber,
                       vchan::ChannelDef &def,
                       QVector<PoolJob> &plexJobs,
                       QVector<PoolJob> &serverJobs,
                       QVector<QPair<int, QStringList>> *folderPools,
                       QVector<QPair<int, QStringList>> *localTitles = nullptr);

    QJsonArray bookingsOf(int channelNumber);
    bool       editBooking(int channelNumber, int index,
                           const std::function<void(QJsonObject &)> &edit);
    QString    bookingPoolKey(int channelNumber) const;

    void ensurePlexTimer();

    void armNightlySweep();
    QTimer *m_sweepTimer = nullptr;
    QVector<int> m_topUpQueue;
    int  m_tunedChannel = -1;

    void plexApptNext();
    QVector<PoolJob> m_serverApptJobs;
    int              m_serverApptCursor = 0;
    PoolJob          m_serverJob;
    bool             m_serverJobActive = false;
    void serverApptNext();
    QString m_browseKind;
    QVariantList m_browseItems;
    void browseStart(const QString &kind);
    void browseFail(const QString &reason);
    void finishLocalGeneration();

    void plexEnumStart(int channelNumber, const vchan::ChannelDef &def,
                       const QString &library, const QStringList &match,
                       const QStringList &showIds = {},
                       const QStringList &genres = {});
    bool plexExcluded(const QString &ratingKey, bool isSeason) const;
    bool plexItemToMedia(const QVariantMap &m, vchan::MediaItem *out) const;
    void plexEnumNext();
    void plexEnumFail(const QString &why);
    void plexEnumFinish();

    QList<int> m_rebuildQueue;

    QVariant settingValue(const QString &key) const;
    int      nextFreeChannelNumber() const;
    bool     appendChannel(const QJsonObject &channel, QString *error);
    QJsonArray readChannels() const;
    bool     writeChannels(const QJsonArray &channels);
    bool     moveScheduleFile(int fromNumber, int toNumber);
    bool     setSpecialNumber(const QString &which, int number);
    void     repairSpecialNumbers();
    bool     renumberDial();

    QTimer *m_genTimer = nullptr;
    bool    m_genActive = false;
    int     m_genChannel = -1;
    vchan::ChannelDef m_genDef;
    // A seeding pass shares the generation machinery (same enumeration, same
    // busy state) and differs only in what happens when the items land: the
    // picks are written into the channel's pools instead of laid onto a clock.
    bool     m_seedActive = false;
    QString  m_seedRecipeKey;      // "<kind id>:<subject id>" marks the entries
    QJsonArray m_seedPriorProgrammes;   // the channel's own entries, put back
                                        // once the picks land (the seed pass
                                        // reads the recipe with them set aside)
    void     finishSeeding();
    struct PendingItem { QString absPath; QString rel; vchan::SlotKind kind;
                         int apptIndex = -1;
                         int pack = -1;
                         // Which block of a day plan this was gathered for. The
                         // server paths carry it on the job; a local file is
                         // read off disk here, so it has to be carried too --
                         // without it every block on a local channel gathers
                         // nothing it can find again, and airs the card.
                         int planBlock = -1;
                         // Known only for library items. A folder of clips has
                         // none of this and falls back to its directory name.
                         QString series = {};
                         qint64  airMs = 0;
                         int     seasonNo = -1;
                         int     episodeNo = -1; };
    QVector<PendingItem> m_genQueue;
    int     m_genCursor = 0;

    QString channelsFilePath() const;
    QString scheduleFilePath(int channelNumber) const;
    QString resolveMediaRoot() const;
    // Creates series/ and movies/ inside an existing media root.
    void ensureLibraryFolders() const;

    const vchan::ChannelSchedule &scheduleFor(int channelNumber);
    void invalidateCache(int channelNumber) { m_cache.remove(channelNumber); }

    QVariantMap  channelObject(int channelNumber) const;
    qint64       rotationAt(int channelNumber, qint64 t);
    void         marksAt(int channelNumber, qint64 t,
                         QHash<QString, QString> *perSeries, QString *overall);
    QStringList  mediaFilesUnder(const QString &relDir) const;
    QString      relativeMediaFolder(const QString &folder, QString *why = nullptr) const;

    QVariantMap buildPlayDescriptor(int channelNumber,
                                    const vchan::ChannelSchedule &schedule,
                                    qint64 nowMs,
                                    vchan::Decision start);
    QString resolveLocalSlot(const vchan::Slot &s) const;
    bool    requestPlexUrl(const vchan::Slot &s, qint64 offsetMs,
                           bool transcodeAllowed);
    bool    askPlexForStream(const QString &ratingKey, const QString &partKey,
                             const QString &sessionId, qint64 offsetMs,
                             bool transcodeAllowed);

    QString plexVideoQuality() const;

    QString m_plexTranscodeSession;
    // The audio tracks of the programme now tuned, as its source reported them
    // when the item was resolved, and which one is playing. Kept so the AUDIO
    // button can move between them without asking for the list again; cleared
    // when the programme changes, because they belong to that programme.
    QVariantList m_audioStreams;
    int          m_audioIndex = -1;
    QString      m_audioForRef;
    // Asked for on the next stream request. Empty means "whatever the source
    // would have chosen", which is what every channel gets until a press or a
    // setting says otherwise.
    QString      m_preferredAudioId;

    // The subtitle tracks of the programme now tuned, and which is showing.
    // Index 0 is the OFF pseudo-stream every Plex track list opens with.
    QVariantList m_subtitleStreams;
    int          m_subtitleIndex = 0;
    QString      m_preferredSubtitleId;
    QString      m_plexPartId;    // where the selection is written
    // Whether the settings, rather than the part, chose. A direct play is
    // told its tracks only then; otherwise mpv picks as it always has.
    bool         m_audioDecided    = false;
    bool         m_subtitleDecided = false;

    // A stream request held until the server has taken the subtitle selection
    // and let go of the last session -- one hold per answer still owed.
    struct HeldTranscode {
        QString ratingKey;
        QString partKey;
        QString sessionId;
        qint64  offsetMs = 0;
    };
    int                          m_transcodeHolds = 0;
    QString                      m_awaitAudioPart;      // the part whose audio write is owed
    QString                      m_awaitSubtitlePart;   // the part whose subtitle write is owed
    QString                      m_awaitStop;           // the session whose stop is owed
    std::optional<HeldTranscode> m_heldTranscode;
    QTimer                      *m_holdCap = nullptr;
    bool plexWillTranscode(bool transcodeAllowed) const;
    bool writeTrackToPart(const char *slot, const QString &streamId, QString &owed);
    bool sendTranscodeRequest(const QString &ratingKey, const QString &partKey,
                              const QString &sessionId, qint64 offsetMs);
    void releaseOneHold();
    void releaseHeldTranscode();
    vchan::TrackPreference trackPreference() const;

    qint64  m_plexPendingOffsetMs = 0;
    bool    m_plexPendingTranscodeOk = false;
    bool    requestServerUrl(const vchan::Slot &s, bool chooseTracks = false);
    bool    askServerForStream(const vchan::Slot &s);
    void    applyTrackChoice(const QString &ref, const QVariantList &audio,
                             const QVariantList &subtitles, const QString &partAudio,
                             const QString &partSubtitle);
    void    describeTracks(QVariantMap &m, bool transcoded) const;
    bool    deliverPreviewUrl(const QString &url, const QString &token = QString());
    int     m_previewChannel  = -1;
    int     m_previewSlot     = -1;
    qint64  m_previewPosition = 0;
    QObject *serverBackend(vchan::SlotSource src) const;
    vchan::SlotSource m_serverPendingSrc = vchan::SlotSource::Local;
    vchan::Slot       m_serverPendingSlot;        // asked for again once its detail is in
    QString           m_serverAwaitingDetailFor;
    vchan::MediaServerSource *m_server = nullptr;

    // The local library index and the launch walk that fills it. One walk per
    // source per side of the dial queues behind the other server work (the
    // source layer is single-flight), and the index commits once the queue
    // drains. An empty index is not an error: the UI keeps its catalog path
    // until the first walk puts rows in.
    vchan::LibraryDb m_libraryDb;
    QVector<QPair<vchan::SlotSource, QString>> m_presenceQueue;   // source, side
    QVector<vchan::LibraryRecord> m_walkRecords;
    QVariantList m_walkRows;
    bool                        m_presenceWalkActive = false;  // a job is in flight when true
    int                         m_presenceRetries = 0;   // per-job auth retries (bounded)
    QPair<vchan::SlotSource, QString> m_presenceRetriedJob;    // budget owner
    QSet<QString>                m_presenceWalkStarted;   // "Source/side" walked or dropped
    QTimer *m_walkTimer = nullptr;
    void    onServerEnumerationFinished(const QVector<vchan::MediaItem> &items);
    void    onServerEnumerationFailed(const QString &reason);
    void    onServerBrowseReady(const QString &kind, const QVariantList &items);
    void    onServerBrowseFailed(const QString &kind, const QString &reason);
    // A presence walk finished for one source and side: its rows fold into the
    // index as they arrive, and the queue moves on. Failures land here too with
    // nothing in hand, so one server timing out cannot stall the walk.
    void    onPresenceReady(const QString &provider,
                            const QVector<vchan::LibraryRecord> &records,
                            const QVariantList &collectionRows);
    // Launch-time library walks: each answered batch folds into the index;
    // a source whose walk never arrives keeps its rows standing (watchdog
    // hands an empty batch). The queue is seeded at walk time — construction
    // time is too early, the servers have not authenticated yet.
    void    seedPresenceWalks();
    void    startNextPresenceWalk();
    // The walk is the module's own setting (on unless the config says off).
    bool    libraryWalkEnabled() const;

    void finishGeneration(bool ok, const QString &message);

    void generationEnded(int channelNumber, bool ok, const QString &message);

    void rebuildNextOrFinish();

    int m_rebuildTotal   = 0;
    int m_rebuildOk      = 0;
    int m_rebuildFailed  = 0;

    static QVariantMap offAirDescriptor(const QString &message, bool needsRegeneration);
};
