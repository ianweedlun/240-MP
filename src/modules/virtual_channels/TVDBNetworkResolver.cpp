#include "TVDBNetworkResolver.h"

#include "MediaServerSource.h"

#include <QEventLoop>
#include <QJsonArray>
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

QString TVDBNetworkResolver::networkIdFor(const MediaItem &item) const {
    const QString name = item.series.trimmed();
    const QString query = name.isEmpty() ? item.title : name;
    if (query.isEmpty()) return {};
    if (m_cache.contains(query)) return m_cache.value(query);

    const QString netId = resolveByTitle(query);
    m_cache.insert(query, netId);
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
    int bestScore = -1;
    for (int i = 0; i < results.size(); ++i) {
        const QString name = results.at(i).value(QStringLiteral("name")).toString().toLower();
        if (name.isEmpty()) continue;
        int score = 0;
        if (name == q) {
            score = 100;
        } else if (name.contains(q) || q.contains(name)) {
            score = 50;
        } else {
            score = 0; // fall through; keep the first non-empty as a last resort
        }
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

} // namespace vchan
