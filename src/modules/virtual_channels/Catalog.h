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
    static QVector<Subject> parseGenres(const QJsonObject &body);

    // Subjects from a /companies body joined against a /companies/types body.
    // Only companies whose type name is exactly "network" are kept.
    static QVector<Subject> parseNetworks(const QJsonObject &companies,
                                          const QJsonObject &types);
};

} // namespace vchan

#endif // VCHAN_CATALOG_H
