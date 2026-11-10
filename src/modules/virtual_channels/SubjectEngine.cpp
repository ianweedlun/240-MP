#include "SubjectEngine.h"

#include <QStringList>

namespace vchan {

QString subjectKindName(SubjectKind k) {
    switch (k) {
        case SubjectKind::Genre:      return QStringLiteral("genre");
        case SubjectKind::Network:    return QStringLiteral("network");
        case SubjectKind::Collection: return QStringLiteral("collection");
    }
    return QStringLiteral("unknown");
}

QString SubjectEngine::showKey(const MediaItem &item) {
    const QString series = item.series.trimmed();
    const QString title  = item.title.trimmed();
    // Collapse internal whitespace runs so "The  Office" and "The Office"
    // dedup as the same show across servers.
    QString key = series.isEmpty() ? title : series;
    return key.simplified().toLower();
}

QVector<MediaItem> SubjectEngine::unionByTitle(const QVector<MediaItem> &a,
                                               const QVector<MediaItem> &b) {
    QVector<MediaItem> out = a;
    QHash<QString, bool> seen;
    for (const MediaItem &it : a) seen.insert(showKey(it), true);
    for (const MediaItem &it : b) {
        if (!seen.contains(showKey(it))) out.append(it);
    }
    return out;
}

QVector<MediaItem> SubjectEngine::unionAcrossServers(
    const QVector<QVector<MediaItem>> &servers) {
    QVector<MediaItem> out;
    QHash<QString, bool> seen;
    for (const QVector<MediaItem> &list : servers) {
        for (const MediaItem &it : list) {
            const QString key = showKey(it);
            if (!seen.contains(key)) {
                seen.insert(key, true);
                out.append(it);
            }
        }
    }
    return out;
}

bool SubjectEngine::belongsTo(const MediaItem &item, const Subject &subject,
                              const NetworkResolver &resolver) {
    switch (subject.kind) {
        case SubjectKind::Genre:
        case SubjectKind::Collection:
            // The server already filtered to this subject; every returned show
            // belongs.
            return true;
        case SubjectKind::Network:
            if (subject.id.trimmed().isEmpty()) return false;
            return resolver.networkIdFor(item) == subject.id;
    }
    return false;
}

QVector<MediaItem> SubjectEngine::itemsForSubject(const QVector<MediaItem> &items,
                                                  const Subject &subject,
                                                  const NetworkResolver &resolver) {
    QVector<MediaItem> out;
    out.reserve(items.size());
    for (const MediaItem &it : items) {
        if (belongsTo(it, subject, resolver)) out.append(it);
    }
    return out;
}

MediaServerSource::Request SubjectEngine::buildRequest(const Subject &subject) {
    MediaServerSource::Request req;
    switch (subject.kind) {
        case SubjectKind::Genre:
            req.genres << subject.name;
            break;
        case SubjectKind::Collection:
            req.collections << subject.name;
            break;
        case SubjectKind::Network:
            // No server-side network filter exists; the engine resolves the
            // network post-union. Leave the filter lists empty so the caller
            // scans broadly.
            break;
    }
    return req;
}

} // namespace vchan
