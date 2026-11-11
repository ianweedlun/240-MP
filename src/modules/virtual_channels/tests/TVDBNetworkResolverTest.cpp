#include "TestHarness.h"
#include "../TVDBNetworkResolver.h"
#include <QJsonObject>
#include <QVector>
#include <QFile>
#include <QJsonDocument>

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

    // --- bestMatchIndex: unrelated hits are NOT adopted ---
    // The old fallback returned result[0] whatever the score: every show
    // missing from TheTVDB inherited an unrelated show's network identity,
    // so a whole-library harvest with 0 refuses made the ABC preset air
    // everything (Private Snafu resolved to network id 5).
    {
        QVector<QJsonObject> hits;
        hits.append(series("Private Snafu", "111"));
        hits.append(series("The Banana Stand", "222"));
        vtest::checkEq(TVDBNetworkResolver::bestMatchIndex(hits, "Futurama"),
                       -1, "no real match -> -1 (first hit not adopted)");
    }

    // --- persistent cache: file -> map -> file, no network ---
    // Seeds re-open channels every boot; lookups already paid must persist
    // (loadCache on an absent file = silent cold start; a cache hit answers
    // without the wire — the bogus key would fail login if a call ran).
    {
        const QString p = QStringLiteral("tvdb-cache-test.json");
        {
            QFile f(p);
            QJsonObject seed; seed.insert("Firefly", "15");
            if (f.open(QIODevice::WriteOnly))
                f.write(QJsonDocument(seed).toJson(QJsonDocument::Compact));
            else vtest::checkEq(f.open(QIODevice::WriteOnly), true, "cache fixture writable");
        }
        TVDBNetworkResolver r2("00000000-0000-0000-0000-000000000000");
        r2.loadCache(p);
        {
            MediaItem m; m.series = "Firefly";
            vtest::checkStr(r2.networkIdFor(m), "15",
                            "cache hit answers without the network");
        }
        r2.saveCache();
        TVDBNetworkResolver r3("00000000-0000-0000-0000-000000000000");
        r3.loadCache(p);
        {
            MediaItem m; m.series = "Firefly";
            vtest::checkStr(r3.networkIdFor(m), "15", "saved cache round-trips");
        }
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

    // --- construction / destruction: RAII owns the QNAM (no parent double-free) ---
    // Constructing with a key and destroying without issuing any network call
    // exercises the full object lifecycle. If QNAM were parent-owned AND
    // manually deleted, this would double-free.
    {
        TVDBNetworkResolver resolver("00000000-0000-0000-0000-000000000000");
    }

    return 0;
}
