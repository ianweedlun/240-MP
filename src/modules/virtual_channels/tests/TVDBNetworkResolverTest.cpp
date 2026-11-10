#include "TestHarness.h"
#include "../TVDBNetworkResolver.h"

#include <QJsonObject>
#include <QVector>

using namespace vchan;

static QJsonObject series(const QString &name, const QString &tvdbId = {}) {
    QJsonObject o;
    o.insert("name", name);
    if (!tvdbId.isEmpty()) o.insert("tvdb_id", tvdbId);
    return o;
}

static QJsonObject extendedWithNetwork(int netId, const QString &netName,
                                       const QString &latestKey) {
    QJsonObject net;
    net.insert("id", netId);
    net.insert("name", netName);
    QJsonObject ext;
    ext.insert(latestKey, net);
    return ext;
}

int runTVDBNetworkResolverTests() {
    vtest::section("TVDBNetworkResolver");

    // --- base URL ---
    vtest::checkStr(TVDBNetworkResolver::kBaseURL, "https://api4.thetvdb.com/v4",
                    "base URL");

    // --- bestMatchIndex: exact wins ---
    {
        QVector<QJsonObject> r;
        r.append(series("The Walking Dead", "153021"));
        r.append(series("Walking Dead: Origins", "999999"));
        r.append(series("The Walking With Dead", "888888"));
        vtest::checkEq(TVDBNetworkResolver::bestMatchIndex(r, "The Walking Dead"), 0,
                       "exact match index");
    }

    // --- bestMatchIndex: substring wins over unrelated ---
    {
        QVector<QJsonObject> r;
        r.append(series("Star Trek", "1"));
        r.append(series("Star Trek: The Original Series", "2"));
        r.append(series("Star Trek: Discovery", "3"));
        // "Trek" is a substring of all three; the first match wins.
        vtest::checkEq(TVDBNetworkResolver::bestMatchIndex(r, "Trek"), 0,
                       "first substring match");
    }

    // --- bestMatchIndex: empty query / no results ---
    {
        vtest::checkEq(TVDBNetworkResolver::bestMatchIndex({}, ""), -1,
                       "no results -> -1");
    }

    // --- parseNetworkId: latestNetwork ---
    {
        QJsonObject ext = extendedWithNetwork(389, "AMC", "latestNetwork");
        vtest::checkStr(TVDBNetworkResolver::parseNetworkId(ext), "389",
                        "latestNetwork id");
    }

    // --- parseNetworkId: falls back to originalNetwork ---
    {
        QJsonObject ext;
        ext.insert("originalNetwork", QJsonObject{{"id", 407}, {"name", "NBC"}});
        vtest::checkStr(TVDBNetworkResolver::parseNetworkId(ext), "407",
                        "originalNetwork fallback");
    }

    // --- parseNetworkId: neither present ---
    {
        vtest::checkStr(TVDBNetworkResolver::parseNetworkId(QJsonObject()), "",
                        "no network -> empty");
    }

    return 0;
}
