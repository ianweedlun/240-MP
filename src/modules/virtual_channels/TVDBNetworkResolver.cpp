#include "TVDBNetworkResolver.h"

#include "Catalog.h"
#include "MediaServerSource.h"

#include <QEventLoop>
#include <QDebug>
#include <QJsonArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QStringList>

namespace vchan {

const QString TVDBNetworkResolver::kBaseURL = QStringLiteral("https://api4.thetvdb.com/v4");

TVDBNetworkResolver::TVDBNetworkResolver(const QString &apiKey)
    : m_apiKey(apiKey)
    , m_nam() {}

void TVDBNetworkResolver::useIndex(vchan::LibraryDb *db) {
    m_db = db;
}

void TVDBNetworkResolver::noteNetworkCatalog(const QVector<Subject> &networks) {
    m_networkIds.clear();
    m_reverseNetworks.clear();
    for (const Subject &s : networks) {
        // A network's catalog row carries both halves of the join: the name
        // the shelves use and the id TheTVDB answers with. Feeding both
        // directions keeps either lookup a single hash hit.
        m_networkIds.insert(s.name.toLower(), s.id);
        m_reverseNetworks.insert(s.id, s.name);
    }
}

QString TVDBNetworkResolver::networkIdFor(const MediaItem &item) const {
    const QString name = item.series.trimmed();
    const QString query = name.isEmpty() ? item.title : name;
    if (query.isEmpty()) return {};
    if (m_cache.contains(query)) return m_cache.value(query);

    // The shelves answer before TheTVDB does. A presence walk that saw the
    // server's Networks field for this show means no HTTP call and no rate
    // limit for it — and a show the shelves answered for never re-pays, even
    // across boots, because the index is a file.
    // A name is not an id: the join needs a catalog row, so a network the
    // shelves carry but the catalog can't name keeps the old path rather than
    // inventing an identity.
    if (m_db) {
        const QStringList nets = m_db->metadataFor(query, QStringLiteral("tv"),
                                                   QStringLiteral("networks"));
        for (const QString &net : nets) {
            const QString id = m_networkIds.value(net.toLower());
            if (!id.isEmpty()) {
                m_cache.insert(query, id);
                return id;
            }
        }
    }

    const QString netId = resolveByTitle(query);
    m_cache.insert(query, netId);
    // A fresh answer is worth keeping in the index too: next session's walk
    // may miss it, and the show still never re-pays TheTVDB. The reverse map
    // (id -> label) comes from the same catalog listing; without it the
    // resolver has no name to write, so it says nothing.
    if (m_db && !netId.isEmpty() && m_reverseNetworks.contains(netId))
        m_db->noteMetadata(query, QStringLiteral("tv"), QStringLiteral("networks"),
                           { m_reverseNetworks.value(netId) });
    static int freshLookups = 0;
    ++freshLookups;
    qInfo("[VirtualChannels] tvdb search #%d '%s' -> '%s' (empty means no match or a rate-limit miss)",
          freshLookups, qPrintable(query), qPrintable(netId));
    return netId;
}

QString TVDBNetworkResolver::sessionToken() const {
    if (!m_token.isEmpty()) return m_token;

    QNetworkRequest req(QUrl(kBaseURL + QStringLiteral("/login")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QByteArray body = QStringLiteral("{\"apikey\":\"%1\"}").arg(m_apiKey).toUtf8();

    QEventLoop loop;
    QNetworkReply *reply = m_nam.post(req, body);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const QByteArray data = reply->readAll();
    reply->deleteLater();

    const QJsonObject root = QJsonDocument::fromJson(data).object();
    const QString token = root.value(QStringLiteral("data")).toObject()
                              .value(QStringLiteral("token")).toString();
    if (!token.isEmpty()) m_token = token;
    return m_token;
}

QVector<QJsonObject> TVDBNetworkResolver::searchSeries(const QString &title) const {
    const QString token = sessionToken();
    if (token.isEmpty()) return {};

    const QString encoded = QUrl::toPercentEncoding(title);
    const QUrl url(kBaseURL + QStringLiteral("/search?query=%1&type=series&country=usa")
                       .arg(encoded));
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(token).toUtf8());

    QEventLoop loop;
    QNetworkReply *reply = m_nam.get(req);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const QByteArray data = reply->readAll();
    reply->deleteLater();

    QVector<QJsonObject> results;
    const QJsonObject root = QJsonDocument::fromJson(data).object();
    for (const QJsonValue &v : root.value(QStringLiteral("data")).toArray()) {
        results.append(v.toObject());
    }
    return results;
}

QString TVDBNetworkResolver::resolveByTitle(const QString &title) const {
    const QVector<QJsonObject> results = searchSeries(title);
    const int idx = bestMatchIndex(results, title);
    if (idx < 0) return {};

    const QJsonObject series = results.at(idx);
    const QString tvdbId = series.value(QStringLiteral("tvdb_id")).toString();
    if (tvdbId.isEmpty()) return {};

    const QString token = sessionToken();
    const QUrl url(kBaseURL + QStringLiteral("/series/%1/extended").arg(tvdbId));
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(token).toUtf8());

    QEventLoop loop;
    QNetworkReply *reply = m_nam.get(req);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const QByteArray data = reply->readAll();
    reply->deleteLater();

    const QJsonObject root = QJsonDocument::fromJson(data).object();
    return parseNetworkId(root.value(QStringLiteral("data")).toObject());
}

int TVDBNetworkResolver::bestMatchIndex(const QVector<QJsonObject> &results,
                                        const QString &query) {
    const QString q = query.toLower();
    int best = -1;
    int bestScore = 0;   // a hit that scores zero matches NOTHING: adopting
    for (int i = 0; i < results.size(); ++i) {   // result[0] made every show
        const QString name = results.at(i).value(QStringLiteral("name")).toString().toLower();  // missing from TheTVDB claim
        if (name.isEmpty()) continue;            // the first search hit's
        int score = 0;                           // network identity — the
        if (name == q) {                         // ABC channel harvested
            score = 100;                         // his whole library because
        } else if (name.contains(q) || q.contains(name)) {  // Private Snafu resolved to
            score = 50;                          // network id 5 and nothing
        }                                      // else ever refused
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

QString TVDBNetworkResolver::parseNetworkId(const QJsonObject &series) {
    for (const QString &key : {QStringLiteral("latestNetwork"),
                               QStringLiteral("originalNetwork")}) {
        const QJsonObject net = series.value(key).toObject();
        const QJsonValue idVal = net.value(QStringLiteral("id"));
        // TheTVDB returns the network id as a JSON integer; tolerate strings too.
        if (idVal.isString()) {
            const QString id = idVal.toString();
            if (!id.isEmpty()) return id;
        } else if (idVal.isDouble()) {
            return QString::number(idVal.toInt());
        }
    }
    return {};
}

QJsonObject TVDBNetworkResolver::fetch(const QString &path) const {
    const QString token = sessionToken();
    if (token.isEmpty()) return {};

    QNetworkRequest req(QUrl(kBaseURL + path));
    req.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(token).toUtf8());

    QEventLoop loop;
    QNetworkReply *reply = m_nam.get(req);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const QByteArray data = reply->readAll();
    reply->deleteLater();
    return QJsonDocument::fromJson(data).object();
}

// --- persistent cache ---
void TVDBNetworkResolver::loadCache(const QString &path) {
    m_cachePath = path;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;   // absent cache = cold start
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it)
        if (it.value().isString()) m_cache.insert(it.key(), it.value().toString());
}

void TVDBNetworkResolver::saveCache() const {
    if (m_cachePath.isEmpty()) return;
    QFile f(m_cachePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    QJsonObject root;
    for (auto it = m_cache.constBegin(); it != m_cache.constEnd(); ++it)
        root.insert(it.key(), it.value());
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    f.close();
}

QVector<Subject> TVDBNetworkResolver::listGenres() const {
    return Catalog::parseGenres(fetch(QStringLiteral("/genres")));
}

QVector<Subject> TVDBNetworkResolver::listNetworks() const {
    return Catalog::parseNetworks(fetch(QStringLiteral("/companies")),
                                  fetch(QStringLiteral("/companies/types")));
}

} // namespace vchan
