#ifndef VCHAN_TVDB_NETWORK_RESOLVER_H
#define VCHAN_TVDB_NETWORK_RESOLVER_H

#include "NetworkResolver.h"

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QVector>

#include "Subject.h"

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

private:
    mutable QString m_token;
    mutable QHash<QString, QString> m_cache;
    mutable QString m_cachePath;   // set by loadCache; saveCache writes back
    QString m_apiKey;
    mutable QNetworkAccessManager m_nam;

    QString sessionToken() const;
    QVector<QJsonObject> searchSeries(const QString &title) const;
    QString resolveByTitle(const QString &title) const;

    // GET a v4 path and return its response body object ({"data": ...}), or an
    // empty object if the token is missing or the call failed.
    QJsonObject fetch(const QString &path) const;
};

} // namespace vchan

#endif // VCHAN_TVDB_NETWORK_RESOLVER_H
