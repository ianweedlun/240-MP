#include "TestHarness.h"
#include "../TVDBNetworkResolver.h"
#include <QJsonObject>
#include <QVector>
#include <QFile>
#include <QJsonDocument>
#include <QDir>

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
        const QString p = QDir::tempPath() + QStringLiteral("/tvdb-cache-test.json");
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
        QFile::remove(p);
    }

    // --- the presence index answers before TheTVDB does ---
    {
        const QString dbPath = QDir::tempPath() + QStringLiteral("/tvdb-index-test.db");
        LibraryDb db;
        vtest::check(db.open(dbPath), "index opens for the join test");
        {
            // One show, walked with its network; the catalog row supplies the
            // id the join needs. A resolver pointed at this index must answer
            // without paying the fake network at all — the fixture below has
            // no HTTP handler, so any attempt would come back empty.
            QVector<vchan::LibraryRecord> recs(1);
            vchan::LibraryRecord &r = recs.first();
            r.src = QStringLiteral("Jellyfin");
            r.channel = QStringLiteral("tv");
            r.title = QStringLiteral("Firefly");
            r.networks = QStringList{ QStringLiteral("Comedy Central") };
            r.updatedMs = 1;
            vtest::check(db.replaceAll(recs), "the walked show lands in the index");
            vtest::checkEq(db.metadataFor(QStringLiteral("Firefly"),
                                          QStringLiteral("tv"),
                                          QStringLiteral("networks")).size(), 1,
                           "and its network reads back");

            TVDBNetworkResolver res("00000000-0000-0000-0000-000000000000");
            res.useIndex(&db);
            QVector<Subject> cat(1);
            cat.first().kind = SubjectKind::Network;
            cat.first().id = QStringLiteral("15");
            cat.first().name = QStringLiteral("Comedy Central");
            res.noteNetworkCatalog(cat);
            {
                MediaItem m; m.series = QStringLiteral("Firefly");
                vtest::checkStr(res.networkIdFor(m), QStringLiteral("15"),
                                "the shelves answer the network with no search at all");
            }
            {
                // The teaching side stays honest: with no HTTP fixture, a
                // fresh lookup answers empty, and an empty answer must NOT be
                // taught (no phantom network written into the index).
                MediaItem m; m.series = QStringLiteral("Cargo");
                vtest::checkStr(res.networkIdFor(m), QString(),
                                "no search means no answer");
                vtest::checkEq(db.metadataFor(QStringLiteral("Cargo"),
                                              QStringLiteral("tv"),
                                              QStringLiteral("networks")).size(), 0,
                               "and the empty answer was not taught to the index");
            }
            {
                // replaceAll is wipe-all by contract: a launch that walked
                // replaces the whole index from the fresh rows (tallies
                // recomputed). Teaching survives the launches that DID NOT
                // walk — a source that never reached auth writes nothing, and
                // its last known lineup keeps standing rows (the retry chain
                // logs exactly this). Pin the wipe side here; the standing
                // side is pinned where the retry chain lands on the Pi.
                QVector<vchan::LibraryRecord> more(1);
                vchan::LibraryRecord &e = more.first();
                e.src = QStringLiteral("Emby");
                e.channel = QStringLiteral("tv");
                e.title = QStringLiteral("Other Show");
                e.networks = QStringList{ QStringLiteral("NBC") };
                e.updatedMs = 2;
                vtest::check(db.replaceAll(more), "a walked launch replaces the index");
                vtest::checkEq(db.metadataFor(QStringLiteral("Firefly"),
                                              QStringLiteral("tv"),
                                              QStringLiteral("networks")).size(), 0,
                               "and a show the fresh walk did not see is gone (wipe-all)");
                vtest::checkEq(db.metadataFor(QStringLiteral("Other Show"),
                                              QStringLiteral("tv"),
                                              QStringLiteral("networks")).size(), 1,
                               "while the fresh row reads back");
            }
            {
                // The join is keyed by NAME (shelves speak names, TheTVDB
                // answers ids), so the catalog listing answers for the
                // walked show. A listing that carries the same label twice
                // — two ids, one umbrella company — is first-wins, not
                // listing-order luck: the walked network must keep the first
                // row's id even when a later row says otherwise.
                QVector<Subject> dup(2);
                dup[0].kind = SubjectKind::Network; dup[0].id = QStringLiteral("2109");
                dup[0].name = QStringLiteral("Cargo Media");
                dup[1].kind = SubjectKind::Network; dup[1].id = QStringLiteral("9999");
                dup[1].name = QStringLiteral("Cargo Media");
                QVector<vchan::LibraryRecord> walked(1);
                vchan::LibraryRecord &w = walked.first();
                w.src = QStringLiteral("Jellyfin"); w.channel = QStringLiteral("tv");
                w.title = QStringLiteral("Dupped Show");
                w.networks = QStringList{ QStringLiteral("Cargo Media") };
                w.updatedMs = 3;
                vtest::check(db.replaceAll(walked), "the walked show is indexed");
                TVDBNetworkResolver joiner("00000000-0000-0000-0000-000000000000");
                joiner.useIndex(&db);
                joiner.noteNetworkCatalog(dup);
                MediaItem m; m.series = QStringLiteral("Dupped Show");
                vtest::checkStr(joiner.networkIdFor(m), QStringLiteral("2109"),
                                "a duplicated label joins first-wins");
            }
        }
        db.close();
        QFile::remove(dbPath);
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
