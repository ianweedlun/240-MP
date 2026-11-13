#include "TestHarness.h"
#include "../NetworkResolver.h"
#include "../Subject.h"
#include "../SubjectEngine.h"

#include <QVector>

using namespace vchan;

// A resolver that maps a show's title to a fixed network id. Lets the engine's
// network grouping be tested without any network I/O.
class MockResolver : public NetworkResolver {
public:
    QHash<QString, QString> byTitle; // showKey -> network id

    QString networkIdFor(const MediaItem &item) const override {
        return byTitle.value(SubjectEngine::showKey(item));
    }
};

static MediaItem item(const QString &title, const QString &series = {}) {
    MediaItem it;
    it.title = title;
    it.series = series;
    it.durMs = 240;
    return it;
}

int runSubjectEngineTests() {
    vtest::section("SubjectEngine");

    // --- showKey: series wins over episode title; movies fall back to title ---
    {
        MediaItem ep;
        ep.title = "The Walking Dead";
        ep.series = "The Walking Dead";
        vtest::checkStr(SubjectEngine::showKey(ep), "the walking dead", "episode showKey");

        MediaItem movie;
        movie.title = "Mad Max: Fury Road";
        vtest::checkStr(SubjectEngine::showKey(movie), "mad max: fury road", "movie showKey");

        MediaItem spaced;
        spaced.title = "x";
        spaced.series = "  The  Office  ";
        vtest::checkStr(SubjectEngine::showKey(spaced), "the office", "trimmed series showKey");
    }

    // --- unionByTitle: first occurrence wins, duplicates dropped ---
    {
        QVector<MediaItem> a;
        a.append(item("Show A", "Show A"));
        a.append(item("Show B", "Show B"));
        QVector<MediaItem> b;
        b.append(item("show a", "Show A")); // dup, different case
        b.append(item("Show C", "Show C")); // new
        auto u = SubjectEngine::unionByTitle(a, b);
        vtest::checkEq(u.size(), 3, "unionByTitle size");
        vtest::checkStr(u.at(0).title, "Show A", "first occurrence kept");
        vtest::checkStr(u.at(2).title, "Show C", "new item appended");
    }

    // --- unionAcrossServers: three servers, one shared show ---
    {
        QVector<QVector<MediaItem>> servers(3);
        servers[0] = {item("Alpha", "Alpha"), item("Beta", "Beta")};
        servers[1] = {item("beta", "Beta"), item("Gamma", "Gamma")};
        servers[2] = {item("Gamma", "Gamma"), item("Delta", "Delta")};
        auto u = SubjectEngine::unionAcrossServers(servers);
        vtest::checkEq(u.size(), 4, "unionAcrossServers size");
    }

    // --- belongsTo: genre/collection always true (server already filtered) ---
    {
        MockResolver r;
        Subject genre{SubjectKind::Genre, "1", "Action"};
        Subject collection{SubjectKind::Collection, "stark", "Star Trek"};
        MediaItem any = item("Whatever", "Whatever");
        vtest::check(SubjectEngine::belongsTo(any, genre, r), "genre belongs");
        vtest::check(SubjectEngine::belongsTo(any, collection, r), "collection belongs");
    }

    // --- belongsTo / itemsForSubject: network matches resolved id ---
    {
        MockResolver r;
        r.byTitle.insert("the walking dead", "389"); // AMC's tvdb network id
        r.byTitle.insert("brooklyn nine nine", "124"); // Fox
        Subject amc{SubjectKind::Network, "389", "AMC"};

        QVector<MediaItem> items;
        items.append(item("Episode 1", "The Walking Dead"));
        items.append(item("Episode 2", "The Walking Dead"));
        items.append(item("Episode 1", "Brooklyn Nine Nine"));

        auto amcItems = SubjectEngine::itemsForSubject(items, amc, r);
        vtest::checkEq(amcItems.size(), 2, "AMC keeps both episodes");
        vtest::checkStr(amcItems.at(0).series, "The Walking Dead", "AMC series");

        Subject fox{SubjectKind::Network, "124", "Fox"};
        auto foxItems = SubjectEngine::itemsForSubject(items, fox, r);
        vtest::checkEq(foxItems.size(), 1, "Fox keeps one");
    }

    // --- network with an empty subject id matches nothing ---
    {
        MockResolver r;
        r.byTitle.insert("the walking dead", "389");
        Subject empty{SubjectKind::Network, "", "???"};
        QVector<MediaItem> items{item("Ep", "The Walking Dead")};
        auto out = SubjectEngine::itemsForSubject(items, empty, r);
        vtest::checkEq(out.size(), 0, "empty network id matches none");
    }

    // --- buildRequest: genre/collection carry a server filter; network does not ---
    {
        MediaServerSource::Request g =
            SubjectEngine::buildRequest({SubjectKind::Genre, "1", "Action"});
        vtest::checkEq(g.genres.size(), 1, "genre filter set");
        vtest::checkStr(g.genres.first(), "Action", "genre value");
        vtest::check(g.collections.isEmpty(), "genre has no collection filter");

        MediaServerSource::Request c = SubjectEngine::buildRequest(
            {SubjectKind::Collection, "stark", "Star Trek"});
        vtest::checkEq(c.collections.size(), 1, "collection filter set");
        vtest::checkStr(c.collections.first(), "Star Trek", "collection value");

        MediaServerSource::Request n =
            SubjectEngine::buildRequest({SubjectKind::Network, "389", "AMC"});
        vtest::check(n.genres.isEmpty(), "network has no genre filter");
        vtest::check(n.collections.isEmpty(), "network has no collection filter");
    }

    return 0;
}
