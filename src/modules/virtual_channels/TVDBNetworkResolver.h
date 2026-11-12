#ifndef VCHAN_TVDB_NETWORK_RESOLVER_H
#define VCHAN_TVDB_NETWORK_RESOLVER_H

#include "NetworkResolver.h"

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QVector>

#include "Subject.h"
#include "LibraryDb.h"

namespace vchan {

// Resolves a show to its TheTVDB network id via the v4 API:
//   login (apikey -> session token) -> search by title -> pick best match ->
//   fetch the extended series -> parse originalNetwork/latestNetwork id.
//
// The apikey is a parameter (never baked in; supplied via TVDBAuth). The
// session token and resolved networks are cached. The matching/parsing logic is
// static and unit-tested with fixture JSON; the HTTP plumbing is exercised
// against the live API.
class TVDBNetworkResolver : public NetworkResolver {
public:
    static const QString kBaseURL;

    explicit TVDBNetworkResolver(const QString &apiKey);

    QString networkIdFor(const MediaItem &item) const override;

    // --- catalog listing (TheTVDB v4) ---
    // Enumerate the subject values a preset recipe can be built from. These do
    // a blocking GET (same QEventLoop pattern as searchSeries); the add-flow
    // loads a catalog once on view open, so a short block is acceptable.
    //   listGenres()      -> GET /genres
    //   listNetworks()    -> GET /companies joined against /companies/types,
    //                        kept only where the company type is "network".
    // Both return an empty list if the apikey is missing or the call fails.
    QVector<Subject> listGenres() const;
    QVector<Subject> listNetworks() const;

    // --- testable pure logic ---

    // Index of the search result whose name best matches the query:
    //   exact (case-insensitive) > substring; NO match -> -1.
    // Adopting an unrelated first hit is a bug, not a convenience: a show
    // missing from TheTVDB would inherit some other show's network identity,
    // and a whole-channel recipe built that way thought every show in the
    // library belonged to its network (the ABC channel adopted Private Snafu
    // and harvested the entire library).
    // Returns -1 if there are no results.
    static int bestMatchIndex(const QVector<QJsonObject> &results, const QString &query);

    // Network id from an extended-series object: latestNetwork, then
    // originalNetwork. Returns an empty string if neither is present.
    static QString parseNetworkId(const QJsonObject &series);

    // --- persistent title -> network-id cache (JSON object file) ---
    // The dial's lookups are stable across boots: loadCache fills m_cache and
    // records the path; saveCache writes the whole map back there (called at
    // seed end). Absent/unreadable file = silent cold start. Seeds never
    // re-pay TheTVDB for titles a previous session already resolved.
    void loadCache(const QString &path);
    void saveCache() const;

    // The module's presence index, when it has one. The resolver uses it to
    // skip TheTVDB for shows the shelves already named a network for, and to
    // teach the index what a fresh lookup answered. A resolver without an
    // index (the unit tests, a bare caller) keeps the old HTTP-only path.
    void useIndex(vchan::LibraryDb *db);
    // The network names TheTVDB's own listing carries, in lower-case join form,
    // plus the id behind each. The add-flow pays this listing once per view,
    // so the resolver borrows it instead of fetching a second time. Feeding
    // both directions at once keeps the join a single hash lookup per side.
    void noteNetworkCatalog(const QVector<Subject> &networks);

private:
    mutable QString m_token;
    mutable QHash<QString, QString> m_cache;
    mutable QString m_cachePath;   // set by loadCache; saveCache writes back
    QString m_apiKey;
    mutable QNetworkAccessManager m_nam;
    // The presence index and the catalog's network rows, when the module has
    // them (borrowed, never owned — the module keeps both alive).
    vchan::LibraryDb *m_db = nullptr;
    QHash<QString, QString> m_networkIds;       // lower(name) -> tvdb id
    QHash<QString, QString> m_reverseNetworks;  // tvdb id -> label

    QString sessionToken() const;
    QVector<QJsonObject> searchSeries(const QString &title) const;
    QString resolveByTitle(const QString &title) const;

    // GET a v4 path and return its response body object ({"data": ...}), or an
    // empty object if the token is missing or the call failed.
    QJsonObject fetch(const QString &path) const;
};

} // namespace vchan

#endif // VCHAN_TVDB_NETWORK_RESOLVER_H
