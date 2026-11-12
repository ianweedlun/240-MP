#include "MediaServerSource.h"

#include <QDebug>
#include <QMetaObject>
#include <QTimer>
#include <QVariantMap>
#include <algorithm>

namespace vchan {

namespace {

const QLatin1String kTvType("tvshows");
const QLatin1String kMovieType("movies");

const QLatin1String kEpisode("Episode");
const QLatin1String kMovie("Movie");
const QLatin1String kSeries("Series");
const QLatin1String kBoxSet("BoxSet");

bool listContainsCI(const QStringList &haystack, const QString &needle) {
    for (const QString &h : haystack)
        if (h.compare(needle, Qt::CaseInsensitive) == 0) return true;
    return false;
}

void sortForPlayout(QVector<MediaItem> &items) {
    std::stable_sort(items.begin(), items.end(),
                     [](const MediaItem &a, const MediaItem &b) {
        const int s = a.series.compare(b.series, Qt::CaseInsensitive);
        if (s != 0) return s < 0;
        if (a.ep != b.ep) return a.ep < b.ep;
        return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
    });
}
}

MediaServerSource::MediaServerSource(QObject *parent) : QObject(parent) {
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(kServerTimeoutMs);
    connect(m_timer, &QTimer::timeout, this, &MediaServerSource::onTimeout);
}

QString MediaServerSource::providerName(SlotSource src) {
    switch (src) {
    case SlotSource::Jellyfin: return QStringLiteral("Jellyfin");
    case SlotSource::Emby:     return QStringLiteral("Emby");
    case SlotSource::Plex:     return QStringLiteral("Plex");
    case SlotSource::Local:    break;
    }
    return QStringLiteral("Local");
}

QString MediaServerSource::seasonKey(const QString &seriesId, int seasonNumber) {
    return seriesId + QLatin1Char(':') + QString::number(seasonNumber);
}

bool MediaServerSource::parseSeasonKey(const QString &key, QString *seriesId, int *seasonNumber) {
    const int at = key.lastIndexOf(QLatin1Char(':'));
    if (at <= 0 || at + 1 >= key.size()) return false;
    bool ok = false;
    const int n = key.mid(at + 1).toInt(&ok);
    if (!ok) return false;
    if (seriesId)     *seriesId = key.left(at);
    if (seasonNumber) *seasonNumber = n;
    return true;
}

void MediaServerSource::setPresencePlex(QObject *plex) {
    m_presencePlex = nullptr;
    if (!plex) return;
    const QMetaObject *mo = plex->metaObject();
    // Presence-only: the walk needs the section pass and the shelf listing.
    // The browse machine's verbs are NOT demanded here — that is the door
    // that keeps Plex out of browsing, and this check keeps it honest.
    static const char *required[] = {
        "load_libraries()",
        "load_library_all(QString)",
        "load_collections(QString)",
        "get_auth_state()",
    };
    for (const char *m : required) {
        if (mo->indexOfMethod(m) < 0) {
            qWarning("[VirtualChannels] Plex backend has no %s — the walk skips it", m);
            return;
        }
    }
    if (mo->indexOfSignal("librariesLoaded(QVariant)") < 0
        || mo->indexOfSignal("itemsLoaded(QVariant)") < 0) {
        qWarning("[VirtualChannels] Plex does not report its listings — the walk skips it");
        return;
    }
    connect(plex, SIGNAL(librariesLoaded(QVariant)), this, SLOT(onLibrariesLoaded(QVariant)),
            Qt::UniqueConnection);
    connect(plex, SIGNAL(itemsLoaded(QVariant)), this, SLOT(onItemsLoaded(QVariant)),
            Qt::UniqueConnection);
    if (mo->indexOfSignal("collectionsLoaded(QVariant)") >= 0)
        connect(plex, SIGNAL(collectionsLoaded(QVariant)), this, SLOT(onItemsLoaded(QVariant)),
                Qt::UniqueConnection);
    if (mo->indexOfSignal("errorOccurred(QString)") >= 0)
        connect(plex, SIGNAL(errorOccurred(QString)), this, SLOT(onBackendError(QString)),
                Qt::UniqueConnection);
    m_presencePlex = plex;
    if (!m_wired.contains(plex)) {
        m_wired.insert(plex);
        connect(plex, &QObject::destroyed, this, [this, plex]() {
            m_wired.remove(plex);
            if (m_active == plex) fail(QStringLiteral("Server went away"));
            if (m_presencePlex == plex) m_presencePlex = nullptr;
        });
    }
}

void MediaServerSource::setBackend(SlotSource src, QObject *backend) {
    QObject **slotPtr = (src == SlotSource::Jellyfin) ? &m_jellyfin
                      : (src == SlotSource::Emby)     ? &m_emby
                                                      : nullptr;
    if (!slotPtr) return;
    *slotPtr = nullptr;
    if (!backend) return;

    const QMetaObject *mo = backend->metaObject();
    static const char *required[] = {
        "load_libraries()",
        "load_items(QString,QString,QString)",
        "load_seasons(QString)",
        "get_playback_url(QString,QString,int,int,bool)",
    };
    for (const char *m : required) {
        if (mo->indexOfMethod(m) < 0) {
            qWarning("[VirtualChannels] %s backend has no %s — that source is unavailable",
                     qPrintable(providerName(src)), m);
            return;
        }
    }
    if (mo->indexOfSignal("itemsLoaded(QVariant)") < 0
        || mo->indexOfSignal("librariesLoaded(QVariant)") < 0) {
        qWarning("[VirtualChannels] %s backend does not report its listings — that source is unavailable",
                 qPrintable(providerName(src)));
        return;
    }

    connect(backend, SIGNAL(librariesLoaded(QVariant)), this, SLOT(onLibrariesLoaded(QVariant)),
            Qt::UniqueConnection);
    connect(backend, SIGNAL(itemsLoaded(QVariant)), this, SLOT(onItemsLoaded(QVariant)),
            Qt::UniqueConnection);
    if (mo->indexOfSignal("seasonsLoaded(QVariant)") >= 0)
        connect(backend, SIGNAL(seasonsLoaded(QVariant)), this, SLOT(onSeasonsLoaded(QVariant)),
                Qt::UniqueConnection);
    if (mo->indexOfSignal("boxsetChildrenLoaded(QVariant)") >= 0)
        connect(backend, SIGNAL(boxsetChildrenLoaded(QVariant)),
                this, SLOT(onBoxsetChildrenLoaded(QVariant)), Qt::UniqueConnection);
    if (mo->indexOfSignal("collectionsLoaded(QVariant)") >= 0)
        connect(backend, SIGNAL(collectionsLoaded(QVariant)),
                this, SLOT(onItemsLoaded(QVariant)), Qt::UniqueConnection);
    if (mo->indexOfSignal("errorOccurred(QString)") >= 0)
        connect(backend, SIGNAL(errorOccurred(QString)), this, SLOT(onBackendError(QString)),
                Qt::UniqueConnection);

    if (!m_wired.contains(backend)) {
        m_wired.insert(backend);
        connect(backend, &QObject::destroyed, this, [this, slotPtr, backend]() {
            m_wired.remove(backend);
            if (m_active == backend) fail(QStringLiteral("Server went away"));
            if (*slotPtr == backend) *slotPtr = nullptr;
        });
    }

    *slotPtr = backend;
}

QObject *MediaServerSource::backend(SlotSource src) const {
    if (src == SlotSource::Jellyfin) return m_jellyfin;
    if (src == SlotSource::Emby)     return m_emby;
    // The browse machine never grew a Plex transport and never will through
    // this door. The presence walk holds Plex in its own slot and only sees
    // it while a walk is running — a browse asking for Plex still comes back
    // "not handled", which is what the machine's test guards.
    if (src == SlotSource::Plex && m_presence) return m_presencePlex;
    return nullptr;
}

bool MediaServerSource::busy() const { return m_stage != Stage::Idle; }

bool MediaServerSource::fromActive() const {
    return m_active && sender() == m_active && m_stage != Stage::Idle;
}

bool MediaServerSource::callBackend(const char *method, const QStringList &args) {
    if (!m_active) return false;
    bool ok = false;
    switch (args.size()) {
    case 0: ok = QMetaObject::invokeMethod(m_active, method); break;
    case 1: ok = QMetaObject::invokeMethod(m_active, method, Q_ARG(QString, args[0])); break;
    case 2: ok = QMetaObject::invokeMethod(m_active, method, Q_ARG(QString, args[0]),
                                           Q_ARG(QString, args[1])); break;
    default: ok = QMetaObject::invokeMethod(m_active, method, Q_ARG(QString, args[0]),
                                            Q_ARG(QString, args[1]),
                                            Q_ARG(QString, args[2])); break;
    }
    if (ok) m_timer->start();
    return ok;
}

void MediaServerSource::reset() {
    m_timer->stop();
    m_stage   = Stage::Idle;
    m_active  = nullptr;
    m_req             = Request{};
    m_expandingSeries = false;
    m_libs.clear();
    m_pendingSeries.clear();
    m_pendingBoxsets.clear();
    m_presenceSeenShows.clear();
    m_currentLib.clear();
    m_askedBoxsetsForLib = false;
    m_truncated = false;
    m_rejected  = 0;
    m_collected.clear();
    m_seenIds.clear();
    m_expandedSeries.clear();
    m_browseKind.clear();
    m_browseParent.clear();
    m_browseSeason = -1;
    m_browseSeries.clear();
    m_browseItems.clear();
    m_presence = false;
    m_presenceSide.clear();
    m_presencePhase = 0;
    m_presenceRecords.clear();
    m_presenceSubjectRows.clear();
}

void MediaServerSource::cancel() { reset(); }

void MediaServerSource::fail(const QString &reason) {
    if (m_stage == Stage::Idle) return;
    if (m_presence) {
        // A walk that could not finish hands back what it collected and stops:
        // the index keeps the rows it already has for this source, so an
        // unreachable server cannot dark-room a shelf the viewer still has.
        presenceFinish();
        return;
    }
    const QString kind = m_browseKind;
    reset();
    qWarning("[VirtualChannels] media server request failed: %s", qPrintable(reason));
    if (kind.isEmpty()) emit enumerationFailed(reason);
    else                emit browseFailed(kind, reason);
}

bool MediaServerSource::itemToMedia(const QVariantMap &m, SlotSource src, MediaItem *out) {
    if (!out) return false;
    const QString id = m.value(QStringLiteral("itemId")).toString();
    const qint64 dur = static_cast<qint64>(m.value(QStringLiteral("duration")).toDouble());
    if (id.isEmpty() || dur <= 0) return false;

    out->src   = src;
    out->ref   = id;
    out->partKey = m.value(QStringLiteral("mediaSourceId")).toString();
    out->durMs = dur;
    out->title = m.value(QStringLiteral("title")).toString();
    out->series= m.value(QStringLiteral("grandparentTitle")).toString();
    out->desc  = m.value(QStringLiteral("overview")).toString();
    out->art   = m.value(QStringLiteral("imageTag")).toString();

    const int season  = m.value(QStringLiteral("parentIndex")).toInt();
    const int episode = m.value(QStringLiteral("index")).toInt();
    out->seasonNo  = season  > 0 ? season  : -1;
    out->episodeNo = episode > 0 ? episode : -1;
    out->ep = (season > 0 && episode > 0)
                  ? QStringLiteral("S%1E%2")
                        .arg(season,  2, 10, QLatin1Char('0'))
                        .arg(episode, 2, 10, QLatin1Char('0'))
                  : QString();

    // Both backends already fold PremiereDate into "releaseDate" and
    // ProductionYear into "year". Emby asks for those fields explicitly;
    // Jellyfin does not, so an empty date here is expected rather than wrong,
    // and the year carries the ordering.
    out->airMs = airedAtMs(m.value(QStringLiteral("releaseDate")).toString(),
                           m.value(QStringLiteral("year")));
    return true;
}

QStringList MediaServerSource::eligibleLibraries(const QVariant &libraries, bool films,
                                                 const QStringList &wanted) const {
    QStringList out;
    const bool bothKinds = !m_req.collections.isEmpty()
                           || m_req.wants == Request::Wants::Anything;
    const QLatin1String need = films ? kMovieType : kTvType;

    for (const QVariant &v : libraries.toList()) {
        const QVariantMap m = v.toMap();
        // Plex pages its sections under its own labels ("show"/"movie" in
        // "sectionType"); the Jellyfin/Emby trio answers with "tvshows"/
        // "movies" in "collectionType". One eligibility, two spellings —
        // the machine asks the same question of all three transports.
        QString type = m.value(QStringLiteral("collectionType")).toString();
        if (type.isEmpty())
            type = m.value(QStringLiteral("sectionType")).toString();
        if (type == QStringLiteral("show"))
            type = kTvType;
        else if (type == QStringLiteral("movie"))
            type = kMovieType;
        if (bothKinds ? (type != kTvType && type != kMovieType) : (type != need))
            continue;
        if (!wanted.isEmpty() && !listContainsCI(wanted, m.value(QStringLiteral("title")).toString()))
            continue;
        QString id = m.value(QStringLiteral("itemId")).toString();
        if (id.isEmpty()) id = m.value(QStringLiteral("key")).toString();
        if (!id.isEmpty() && !out.contains(id)) out << id;
    }
    return out;
}

bool MediaServerSource::enumerate(const Request &req) {
    if (busy()) return false;
    QObject *b = backend(req.src);
    if (!b) return false;

    reset();
    m_active = b;
    m_req    = req;
    m_stage  = Stage::GenLibraries;
    if (!callBackend("load_libraries")) { reset(); return false; }
    return true;
}

void MediaServerSource::genNext() {
    if (!m_pendingBoxsets.isEmpty()) {
        m_stage = Stage::GenBoxsetChildren;
        if (!callBackend("load_boxset_children", { m_pendingBoxsets.takeFirst() }))
            fail(QStringLiteral("Server cannot list collection contents"));
        return;
    }
    if (!m_pendingSeries.isEmpty()) {
        m_stage = Stage::GenItems;
        m_expandingSeries = true;
        if (!callBackend("load_items", { m_pendingSeries.takeFirst(), kEpisode, QString() }))
            fail(QStringLiteral("Server cannot list episodes"));
        return;
    }

    const bool named = !m_req.match.isEmpty() || !m_req.titles.isEmpty()
                       || !m_req.genres.isEmpty() || !m_req.showIds.isEmpty()
                       || !m_req.studios.isEmpty();
    const bool takesAll = m_req.collections.isEmpty()
                          && (!wantsFilms(m_req.wants) || m_req.anyFilm);
    const bool wantsLibraryItems = named || takesAll;

    while (!m_libs.isEmpty()) {
        const QString lib = m_libs.first();
        if (!m_req.collections.isEmpty() && !m_askedBoxsetsForLib) {
            m_askedBoxsetsForLib = true;
            m_stage = Stage::GenBoxsets;
            if (!callBackend("load_items", { lib, kBoxSet, QStringLiteral("SortName") }))
                fail(QStringLiteral("Server cannot list collections"));
            return;
        }
        m_libs.removeFirst();
        m_askedBoxsetsForLib = false;
        if (wantsLibraryItems) {
            m_stage = Stage::GenItems;
            m_expandingSeries = false;
            QString wantType;
            if (m_req.wants != Request::Wants::Anything)
                wantType = QString(wantsFilms(m_req.wants) ? kMovie : kEpisode);
            // A genre lives on the SERIES item, not on its episodes, so the
            // episode scan can only narrow by show once it has seen the show
            // rows themselves. Ask for both when a genre filter is in play.
            if (!m_req.genres.isEmpty() && wantType == QString(kEpisode))
                wantType = QStringLiteral("Series,Episode");
            if (!callBackend("load_items", { lib, wantType, QStringLiteral("SortName") }))
                fail(QStringLiteral("Server cannot list items"));
            return;
        }
    }
    genFinish();
}

void MediaServerSource::genFinish() {
    QVector<MediaItem> items = m_collected;
    const bool truncated = m_truncated;
    const int  rejected  = m_rejected;
    const SlotSource src = m_req.src;
    reset();
    sortForPlayout(items);
    if (items.isEmpty() && rejected > 0)
        qWarning("[VirtualChannels] %s returned %d item(s), none of them schedulable — "
                 "items with no duration cannot be placed on a timeline",
                 qPrintable(providerName(src)), rejected);
    if (truncated)
        qWarning("[VirtualChannels] library listing capped at %d items — the channel uses the first %lld",
                 kMaxServerItems, static_cast<long long>(items.size()));
    emit enumerationFinished(items);
}

bool MediaServerSource::browse(SlotSource src, const QString &kind, const QString &parentKey) {
    if (busy()) return false;
    QObject *b = backend(src);
    if (!b) return false;

    reset();
    m_active      = b;
    m_req         = Request{};
    m_req.src     = src;
    m_browseKind  = kind;
    m_browseParent= parentKey;
    if (kind == QLatin1String("seasons")) {
        m_stage = Stage::BrowseSeasons;
        if (!callBackend("load_seasons", { parentKey })) { reset(); return false; }
        return true;
    }
    if (kind == QLatin1String("episodes")) {
        QString seriesId;
        if (!parseSeasonKey(parentKey, &seriesId, &m_browseSeason)) { reset(); return false; }
        m_browseSeries = seriesId;
        m_stage = Stage::BrowseEpisodes;
        if (!callBackend("load_items", { seriesId, kEpisode, QStringLiteral("SortName") })) {
            reset();
            return false;
        }
        return true;
    }
    if (kind != QLatin1String("shows") && kind != QLatin1String("collections")
        && kind != QLatin1String("movies") && kind != QLatin1String("moviegenres")
        && kind != QLatin1String("moviestudios")
        && kind != QLatin1String("moviecollections")) {
        reset();
        return false;
    }
    m_req.wants = (kind == QLatin1String("movies")
                   || kind == QLatin1String("moviegenres")
                   || kind == QLatin1String("moviestudios")
                   || kind == QLatin1String("moviecollections"))
                  ? Request::Wants::Films : Request::Wants::Episodes;
    if (kind == QLatin1String("collections") || kind == QLatin1String("moviecollections"))
        m_req.collections << QStringLiteral("*");
    m_stage = Stage::BrowseLibraries;
    if (!callBackend("load_libraries")) { reset(); return false; }
    return true;
}

void MediaServerSource::finishBrowse() {
    const QString kind = m_browseKind;
    QVariantList items = m_browseItems;
    if (kind == QLatin1String("moviegenres")) {
        std::sort(items.begin(), items.end(), [](const QVariant &a, const QVariant &b) {
            return a.toMap().value(QStringLiteral("label")).toString().compare(
                       b.toMap().value(QStringLiteral("label")).toString(),
                       Qt::CaseInsensitive) < 0;
        });
    } else if (kind == QLatin1String("moviestudios")) {
        std::sort(items.begin(), items.end(), [](const QVariant &a, const QVariant &b) {
            return a.toMap().value(QStringLiteral("label")).toString().compare(
                       b.toMap().value(QStringLiteral("label")).toString(),
                       Qt::CaseInsensitive) < 0;
        });
    }
    reset();
    emit browseReady(kind, items);
}

// ---- the presence walk ------------------------------------------------------
// Feeds the library index once per launch. It walks SHOW rows for tv (a genre
// and a network belong to the show, so an episode pass would report them
// nowhere) and FILM rows for movies, and it reads only what the index needs
// — titles and the metadata fields the servers already carry on those rows.
// Movie collections come from the boxset listing beside the film pass: a boxset
// row already knows how many films it holds, which is the honest count of what
// a collection channel would draw.
bool MediaServerSource::presence(SlotSource src, const QStringList &libraries,
                                 const QString &side) {
    if (busy()) return false;
    if (side != QLatin1String("tv") && side != QLatin1String("movies")) return false;
    // The presence flag is raised BEFORE the lookup: Plex answers only while
    // a walk runs, so the door order is what keeps the browse machine's
    // "not handled" guard honest rather than accidental.
    m_presence = true;
    QObject *b = backend(src);
    if (!b) { m_presence = false; return false; }
    // The dial does not walk through a handshake: a backend that has not
    // authenticated yet would answer with an empty library, and an empty
    // answer merged into the index would wipe everything the dial knows.
    // has_auth() is the backend's own truth (Jellyfin/Emby carry it; Plex's
    // token path is gated where its own browse pays, so Plex has no such
    // method and its walks stay as they are — the retry loop keeps its
    // jobs until auth lands).
    // The dial does not walk through a handshake: a backend that has not
    // authenticated yet answers with an empty library, and an empty answer
    // merged into the index would wipe everything the dial knows. get_auth_state()
    // is the trio's uniform truth ("authed"; Plex also says "none"/"needs_user"
    // while its boot handshake is in flight) — a source that is not authed
    // simply keeps its walk jobs queued until the state lands.
    {
        QString authState;
        const bool answered = QMetaObject::invokeMethod(
            b, "get_auth_state", Q_RETURN_ARG(QString, authState));
        if (answered && authState != QLatin1String("authed"))
            return false;   // auth still in flight — the job retries, it does not walk dark
    }

    reset();
    m_active = b;
    m_req = Request{};
    m_req.src = src;
    m_req.libraries = libraries;
    m_req.wants = (side == QLatin1String("movies"))
                      ? Request::Wants::Films : Request::Wants::Episodes;
    m_presence = true;
    m_presenceSide = side;
    m_presencePhase = 0;      // 0 = boxsets first (movies side), 1 = the rows
    m_stage = Stage::PresenceLibraries;
    if (!callBackend("load_libraries")) { reset(); return false; }
    return true;
}

void MediaServerSource::presenceNextLibrary() {
    if (m_libs.isEmpty()) { presenceFinish(); return; }
    const bool plex = (m_req.src == SlotSource::Plex);
    // The boxset listing for a movie library runs before that library's film
    // pass, so the collection names and their shelf counts arrive together.
    if (m_presenceSide == QLatin1String("movies") && m_presencePhase == 0) {
        m_presencePhase = 1;
        m_stage = Stage::PresenceItems;
        if (plex) {
            if (!callBackend("load_collections", { m_libs.first() }))
                fail(QStringLiteral("Server cannot list collections"));
        } else if (!callBackend("load_items", { m_libs.first(), QString(kBoxSet), QString() })) {
            fail(QStringLiteral("Server cannot list collections"));
        }
        return;
    }
    const QString lib = m_libs.takeFirst();
    m_presencePhase = 0;
    m_stage = Stage::PresenceItems;
    if (plex) {
        // Plex pages a whole section at a time and the type rides on the
        // section itself, so the walk asks it once per shelf.
        if (!callBackend("load_library_all", { lib }))
            fail(QStringLiteral("Server cannot walk that library"));
        return;
    }
    // The backends' type map has no bare "Series", so the tv pass asks for
    // both row kinds and the handler below keeps only the show rows — that is
    // where genres and networks live.
    const QString wantType = (m_presenceSide == QLatin1String("movies"))
                                 ? QString(kMovie) : QStringLiteral("Series,Episode");
    if (!callBackend("load_items", { lib, wantType, QStringLiteral("SortName") }))
        fail(QStringLiteral("Server cannot walk that library"));
}

void MediaServerSource::presenceFinish() {
    const QString provider = providerName(m_req.src);
    QVector<LibraryRecord> records;
    records.swap(m_presenceRecords);
    QVariantList rows;
    rows.swap(m_presenceSubjectRows);
    reset();
    emit presenceReady(provider, records, rows);
}

void MediaServerSource::browseNextLibrary() {
    if (m_libs.isEmpty()) { finishBrowse(); return; }
    const QString lib = m_libs.takeFirst();

    QLatin1String type = kBoxSet;
    if (m_browseKind == QLatin1String("shows")) {
        m_stage = Stage::BrowseSeries; type = kSeries;
    } else if (m_browseKind == QLatin1String("movies")
               || m_browseKind == QLatin1String("moviegenres")
               || m_browseKind == QLatin1String("moviestudios")) {
        m_stage = Stage::BrowseMovies; type = kMovie;
    } else {
        m_stage = Stage::BrowseCollections;
    }

    if (!callBackend("load_items", { lib, type, QStringLiteral("SortName") }))
        fail(QStringLiteral("Server cannot list that library"));
}

void MediaServerSource::onLibrariesLoaded(const QVariant &libraries) {
    if (!fromActive()) return;

    if (m_stage == Stage::GenLibraries) {
        m_libs = eligibleLibraries(libraries, wantsFilms(m_req.wants), m_req.libraries);
        if (m_libs.isEmpty()) { fail(QStringLiteral("No matching libraries")); return; }
        genNext();
        return;
    }
    if (m_stage == Stage::BrowseLibraries) {
        m_libs = eligibleLibraries(libraries, wantsFilms(m_req.wants), QStringList());
        if (m_libs.isEmpty()) { fail(QStringLiteral("No matching libraries")); return; }
        browseNextLibrary();
        return;
    }
    if (m_stage == Stage::PresenceLibraries) {
        m_libs = eligibleLibraries(libraries, wantsFilms(m_req.wants), m_req.libraries);
        if (m_libs.isEmpty()) { fail(QStringLiteral("No matching libraries")); return; }
        presenceNextLibrary();
        return;
    }
}

void MediaServerSource::onItemsLoaded(const QVariant &items) {
    if (!fromActive()) return;
    const QVariantList list = items.toList();

    switch (m_stage) {
    case Stage::GenBoxsets: {
        for (const QVariant &v : list) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("type")).toString() != QLatin1String("boxset")) continue;
            if (!listContainsCI(m_req.collections, m.value(QStringLiteral("title")).toString()))
                continue;
            const QString id = m.value(QStringLiteral("itemId")).toString();
            if (!id.isEmpty() && !m_pendingBoxsets.contains(id)) m_pendingBoxsets << id;
        }
        genNext();
        return;
    }
    case Stage::GenItems: {
        // Genre narrows TV by SHOW: genres live on the series item, so the pass
        // first collects which series carry a wanted genre, and the episode
        // pass below keeps every episode of a collected show — whole series in,
        // never episode-by-episode cherry-picking.
        QSet<QString> genreShows;
        if (!m_req.genres.isEmpty() && !wantsFilms(m_req.wants)) {
            for (const QVariant &sv : list) {
                const QVariantMap sm = sv.toMap();
                if (sm.value(QStringLiteral("type")).toString() != QLatin1String("series"))
                    continue;
                for (const QString &g : sm.value(QStringLiteral("genres")).toStringList()) {
                    if (listContainsCI(m_req.genres, g)) {
                        genreShows.insert(sm.value(QStringLiteral("itemId")).toString());
                        break;
                    }
                }
            }
        }
        for (const QVariant &v : list) {
            if (m_collected.size() >= kMaxServerItems) { m_truncated = true; break; }
            const QVariantMap m = v.toMap();
            const QString type = m.value(QStringLiteral("type")).toString();

            MediaItem mi;
            if (!itemToMedia(m, m_req.src, &mi)) { ++m_rejected; continue; }

            if (m_req.wants == Request::Wants::Anything) {
                if (type != QLatin1String("movie") && type != QLatin1String("episode"))
                    continue;
            } else if (wantsFilms(m_req.wants)) {
                if (type != QLatin1String("movie")) continue;
                if (!m_req.titles.isEmpty() || !m_req.genres.isEmpty()
                    || !m_req.match.isEmpty() || !m_req.studios.isEmpty()) {
                    bool wanted = listContainsCI(m_req.titles, mi.title);
                    if (!wanted) {
                        for (const QString &g : m.value(QStringLiteral("genres")).toStringList())
                            if (listContainsCI(m_req.genres, g)) { wanted = true; break; }
                    }
                    if (!wanted) {
                        for (const QString &s : m.value(QStringLiteral("studios")).toStringList())
                            if (listContainsCI(m_req.studios, s)) { wanted = true; break; }
                    }
                    if (!wanted) {
                        for (const QString &needle : m_req.match)
                            if (!needle.isEmpty()
                                && mi.title.contains(needle, Qt::CaseInsensitive)) {
                                wanted = true;
                                break;
                            }
                    }
                    if (!wanted) continue;
                }
            } else {
                if (type != QLatin1String("episode")) continue;
                const QString seriesId = m.value(QStringLiteral("seriesId")).toString();
                if (!m_expandingSeries
                    && (!m_req.match.isEmpty() || !m_req.showIds.isEmpty()
                        || !m_req.genres.isEmpty())) {
                    const bool byId   = !seriesId.isEmpty()
                                        && m_req.showIds.contains(seriesId);
                    const bool byName = listContainsCI(m_req.match, mi.series);
                    // Show-level genre: the series carried the genre, so every
                    // one of its episodes is in play.
                    const bool byShow = genreShows.contains(seriesId);
                    if (!byId && !byName && !byShow) continue;
                }
                const int season = m.value(QStringLiteral("parentIndex")).toInt();
                if (!seriesId.isEmpty()
                    && m_req.excludeSeasons.contains(seasonKey(seriesId, season))) continue;
                if (m_req.excludeEpisodes.contains(mi.ref)) continue;
            }
            if (m_seenIds.contains(mi.ref)) continue;
            m_seenIds.insert(mi.ref);
            m_collected.append(mi);
        }
        genNext();
        return;
    }
    case Stage::BrowseSeries: {
        for (const QVariant &v : list) {
            if (m_browseItems.size() >= kMaxServerItems) break;
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("type")).toString() != QLatin1String("series")) continue;
            const QString id    = m.value(QStringLiteral("itemId")).toString();
            const QString title = m.value(QStringLiteral("title")).toString();
            if (id.isEmpty() || title.isEmpty()) continue;
            if (m_seenIds.contains(id)) continue;
            m_seenIds.insert(id);
            m_browseItems.append(QVariantMap{{"id", id}, {"label", title}, {"sub", QString()}});
        }
        browseNextLibrary();
        return;
    }
    case Stage::PresenceItems: {
        // The walk's rows: show rows carry what a tv channel can be programmed
        // by, film rows what a movie channel can. Boxset rows are not titles —
        // they answer the collection question with a shelf count already on the
        // row, so they go to the index as their own rows.
        const bool tvSide = (m_presenceSide == QLatin1String("tv"));
        const bool plex = (m_req.src == SlotSource::Plex);
        for (const QVariant &v : list) {
            const QVariantMap m = v.toMap();
            const QString type = m.value(QStringLiteral("type")).toString();
            // Plex's section pass flattens seasons into episode rows, so its
            // show-level facts ride on every episode row — the show's title,
            // genres and collections are the row's grandparent fields. One
            // record per show, not one per episode.
            if (plex && tvSide && type == QLatin1String("episode")) {
                const QString show = m.value(QStringLiteral("grandparentTitle")).toString();
                if (show.isEmpty()) continue;
                if (m_presenceSeenShows.contains(show)) continue;
                m_presenceSeenShows.insert(show);
                LibraryRecord rec;
                rec.src = providerName(m_req.src);
                rec.channel = m_presenceSide;
                rec.title = show;
                rec.genres = m.value(QStringLiteral("genres")).toStringList();
                rec.collections = m.value(QStringLiteral("collections")).toStringList();
                m_presenceRecords.append(rec);
                continue;
            }
            if (type == QLatin1String("boxset") || type == QLatin1String("collection")) {
                const QString name = m.value(QStringLiteral("title")).toString();
                if (name.isEmpty()) continue;
                int shelf = m.value(QStringLiteral("childCount")).toInt();
                if (shelf <= 0) shelf = m.value(QStringLiteral("leafCount")).toInt();
                m_presenceSubjectRows.append(QVariantMap{
                    {QStringLiteral("kind"), QStringLiteral("collection")},
                    {QStringLiteral("channel"), QStringLiteral("movies")},
                    {QStringLiteral("value"), name},
                    {QStringLiteral("count"), qMax(1, shelf)}});
                continue;
            }
            if (tvSide ? (type != QLatin1String("series") && type != QLatin1String("show"))
                       : (type != QLatin1String("movie")))
                continue;
            LibraryRecord rec;
            rec.src = providerName(m_req.src);
            rec.channel = m_presenceSide;
            rec.title = m.value(QStringLiteral("title")).toString();
            if (rec.title.isEmpty()) continue;
            rec.updatedMs = m.value(QStringLiteral("addedAtMs")).toLongLong();
            rec.genres = m.value(QStringLiteral("genres")).toStringList();
            rec.networks = m.value(QStringLiteral("networks")).toStringList();
            rec.studios = m.value(QStringLiteral("studios")).toStringList();
            rec.collections = m.value(QStringLiteral("collections")).toStringList();
            m_presenceRecords.append(rec);
        }
        presenceNextLibrary();
        return;
    }
    case Stage::BrowseMovies: {
        const bool wantGenres = (m_browseKind == QLatin1String("moviegenres"));
        const bool wantStudios = (m_browseKind == QLatin1String("moviestudios"));
        for (const QVariant &v : list) {
            if (m_browseItems.size() >= kMaxServerItems) break;
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("type")).toString() != QLatin1String("movie")) continue;

            QStringList values;
            if (wantStudios)     values = m.value(QStringLiteral("studios")).toStringList();
            else if (wantGenres) values = m.value(QStringLiteral("genres")).toStringList();
            else                 values << m.value(QStringLiteral("title")).toString();

            for (const QString &value : std::as_const(values)) {
                if (value.isEmpty() || m_seenIds.contains(value)) continue;
                m_seenIds.insert(value);
                m_browseItems.append(QVariantMap{{"id", value}, {"label", value},
                                                 {"sub", QString()}});
            }
        }
        browseNextLibrary();
        return;
    }
    case Stage::BrowseCollections: {
        for (const QVariant &v : list) {
            if (m_browseItems.size() >= kMaxServerItems) break;
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("type")).toString() != QLatin1String("boxset")) continue;
            const QString title = m.value(QStringLiteral("title")).toString();
            if (title.isEmpty()) continue;
            if (m_seenIds.contains(title)) continue;
            m_seenIds.insert(title);
            m_browseItems.append(QVariantMap{{"id", title}, {"label", title}, {"sub", QString()}});
        }
        browseNextLibrary();
        return;
    }
    case Stage::BrowseEpisodes: {
        for (const QVariant &v : list) {
            if (m_browseItems.size() >= kMaxServerItems) break;
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("type")).toString() != QLatin1String("episode")) continue;
            if (m.value(QStringLiteral("parentIndex")).toInt() != m_browseSeason) continue;
            const QString owner = m.value(QStringLiteral("seriesId")).toString();
            if (!owner.isEmpty() && owner != m_browseSeries) continue;
            const QString id = m.value(QStringLiteral("itemId")).toString();
            if (id.isEmpty()) continue;
            const int ep = m.value(QStringLiteral("index")).toInt();
            m_browseItems.append(QVariantMap{
                {"id", id},
                {"label", m.value(QStringLiteral("title")).toString()},
                {"sub", ep > 0 ? QStringLiteral("E%1").arg(ep, 2, 10, QLatin1Char('0')) : QString()}});
        }
        finishBrowse();
        return;
    }
    default:
        return;
    }
}

void MediaServerSource::onSeasonsLoaded(const QVariant &seasons) {
    if (!fromActive() || m_stage != Stage::BrowseSeasons) return;
    for (const QVariant &v : seasons.toList()) {
        if (m_browseItems.size() >= kMaxServerItems) break;
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("type")).toString() != QLatin1String("season")) continue;
        const int number = m.value(QStringLiteral("index")).toInt();
        m_browseItems.append(QVariantMap{
            {"id", seasonKey(m_browseParent, number)},
            {"label", m.value(QStringLiteral("title")).toString()},
            {"sub", QString()}});
    }
    finishBrowse();
}

void MediaServerSource::onBoxsetChildrenLoaded(const QVariant &children) {
    if (!fromActive() || m_stage != Stage::GenBoxsetChildren) return;
    for (const QVariant &v : children.toList()) {
        const QVariantMap m = v.toMap();
        const QString type = m.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("series")) {
            if (wantsFilms(m_req.wants)) continue;
            const QString id = m.value(QStringLiteral("itemId")).toString();
            if (!id.isEmpty() && !m_expandedSeries.contains(id)) {
                m_expandedSeries.insert(id);
                m_pendingSeries << id;
            }
            continue;
        }
        if (m_req.wants == Request::Wants::Episodes) continue;
        if (type != QLatin1String("movie")) continue;
        if (m_collected.size() >= kMaxServerItems) { m_truncated = true; break; }
        MediaItem mi;
        if (!itemToMedia(m, m_req.src, &mi)) { ++m_rejected; continue; }
        if (m_req.excludeEpisodes.contains(mi.ref)) continue;
        if (m_seenIds.contains(mi.ref)) continue;
        m_seenIds.insert(mi.ref);
        m_collected.append(mi);
    }
    genNext();
}

void MediaServerSource::onBackendError(const QString &message) {
    if (!fromActive()) return;
    fail(message.isEmpty() ? QStringLiteral("Server error") : message);
}

void MediaServerSource::onTimeout() {
    if (m_stage == Stage::Idle) return;
    fail(QStringLiteral("%1 did not respond").arg(providerName(m_req.src)));
}
}
