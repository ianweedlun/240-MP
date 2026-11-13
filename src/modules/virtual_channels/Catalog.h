#ifndef VCHAN_CATALOG_H
#define VCHAN_CATALOG_H

#include "Subject.h"

#include <QJsonObject>
#include <QVector>

namespace vchan {

// Pure parsing of TheTVDB v4 catalog responses into Subject recipes. Kept free
// of any HTTP so it is unit-tested with fixture JSON; the resolver does the
// network calls and hands the parsed bodies here.
//
//   Genres      -> GET /genres            -> SubjectKind::Genre
//   Networks    -> GET /companies (+ the
//                  /companies/types map)  -> SubjectKind::Network, filtered to
//                  companies whose primaryCompanyType names a "network".
//
// Collections have no TheTVDB list endpoint; they come from the media server,
// so they are not parsed here.
class Catalog {
public:
    // Subjects from a /genres response body. id may be an int or a string.
    //
    // A genre row is only usable by a recipe that airs the kind of content the
    // row was filed under: TheTVDB files the SAME name twice, once as a series
    // genre and once as a movie genre, and offering the movie rows on a TV
    // recipe (or the series rows on a films recipe) advertises a channel the
    // library cannot fill. `wants` narrows the list: "tv" keeps series rows,
    // "movies" keeps movie rows, empty keeps everything (the legacy union).
    // A row with NO type field belongs to both lists — dropping unlabelled
    // rows would silently empty a catalog on an API revision that stops
    // labelling them.
    static QVector<Subject> parseGenres(const QJsonObject &body,
                                        const QString &wants = QString());

    // Subjects from a /companies body joined against a /companies/types body.
    // Only companies whose type name is exactly "network" are kept.
    static QVector<Subject> parseNetworks(const QJsonObject &companies,
                                          const QJsonObject &types);
};

} // namespace vchan

#endif // VCHAN_CATALOG_H
