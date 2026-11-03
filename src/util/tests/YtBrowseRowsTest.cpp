#include <QtTest/QtTest>

#include "../YtBrowseRows.h"

// Pure-logic tests for the InnerTube browse-row parser (issue #28 plan D):
// nested videoRenderer extraction in document order, the two InnerTube text
// shapes (runs[] and simpleText), non-video renderers ignored, row caps, and
// continuation cursors at any depth. Fixtures are schema-faithful to InnerTube
// browse pages; live paired-token responses are re-verified on the Pi in the
// testing round. No processes, no network.

namespace {

QJsonValue runsText(const QString &text)
{
    QJsonObject run;
    run.insert(QStringLiteral("text"), text);
    QJsonArray runs;
    runs.append(run);
    QJsonObject out;
    out.insert(QStringLiteral("runs"), runs);
    return out;
}

QJsonValue simpleText(const QString &text)
{
    QJsonObject out;
    out.insert(QStringLiteral("simpleText"), text);
    return out;
}

QJsonObject makeRenderer(const QString &id, const QString &title,
                         const QString &channel, const bool bylineLong)
{
    QJsonObject r;
    r.insert(QStringLiteral("videoId"), id);
    r.insert(QStringLiteral("title"), runsText(title));
    if (bylineLong)
        r.insert(QStringLiteral("longBylineText"), runsText(channel));
    return r;
}

QJsonObject wrapRenderer(const QJsonObject &renderer)
{
    QJsonObject wrapper;
    wrapper.insert(QStringLiteral("videoRenderer"), renderer);
    return wrapper;
}

QJsonObject sectionOf(const QJsonArray &items)   // sectionListRenderer nesting
{
    QJsonObject section;
    section.insert(QStringLiteral("contents"), items);
    QJsonObject wrapper;
    wrapper.insert(QStringLiteral("sectionListRenderer"), section);
    QJsonArray outer;
    outer.append(wrapper);
    QJsonObject root;
    root.insert(QStringLiteral("contents"), outer);
    return root;
}

} // anonymous namespace

class YtBrowseRowsTest : public QObject {
    Q_OBJECT

private slots:
    void extractRowsInDocumentOrder();
    void ignoreNonVideoRenderers();
    void simpleTextAndOwnerTextShapes();
    void rowCapsStopEarly();
    void continuationTokenAtAnyDepth();
    void singlePageNoToken();
};

void YtBrowseRowsTest::extractRowsInDocumentOrder()
{
    QJsonArray items;
    QJsonObject decoy;                 // channelRenderer: not a row
    decoy.insert(QStringLiteral("channelRenderer"),
                 simpleText(QStringLiteral("Not A Video")));
    items.append(decoy);
    items.append(wrapRenderer(makeRenderer(QStringLiteral("AAAAAAAAAAA"),
                                          QStringLiteral("First Row"),
                                          QStringLiteral("Channel One"), true)));
    items.append(wrapRenderer(makeRenderer(QStringLiteral("BBBBBBBBBBB"),
                                          QStringLiteral("Second Row"),
                                          QStringLiteral("Channel Two"), true)));
    const QJsonObject root = sectionOf(items);

    const QVariantList rows = ytui::extractBrowseRows(root, 0);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("videoId")),
             QStringLiteral("AAAAAAAAAAA"));
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("title")),
             QStringLiteral("First Row"));
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("channelName")),
             QStringLiteral("Channel One"));
    QCOMPARE(rows.at(1).toMap().value(QStringLiteral("videoId")),
             QStringLiteral("BBBBBBBBBBB"));
    QVERIFY(rows.at(0).toMap().value(QStringLiteral("url"))
            == QStringLiteral("https://www.youtube.com/watch?v=AAAAAAAAAAA"));
    // browse never yields absolute publish times; the fields stay empty.
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("publishedAt")), QString());
}

void YtBrowseRowsTest::ignoreNonVideoRenderers()
{
    QJsonArray items;
    QJsonObject playlist;              // playlistRenderer without videoId
    QJsonObject playlistInner;
    playlistInner.insert(QStringLiteral("playlistId"), QStringLiteral("PLnotavideo"));
    playlist.insert(QStringLiteral("playlistRenderer"), playlistInner);
    items.append(playlist);
    QJsonObject idless;                // videoRenderer shape but no videoId
    QJsonObject idlessInner;
    idlessInner.insert(QStringLiteral("title"), simpleText(QStringLiteral("No ID")));
    idless.insert(QStringLiteral("videoRenderer"), idlessInner);
    items.append(idless);
    QJsonObject realInner = makeRenderer(QStringLiteral("CCCCCCCCCCC"),
                                         QStringLiteral("Real Row"),
                                         QStringLiteral("Channel"), true);
    items.append(wrapRenderer(realInner));
    const QJsonObject root = sectionOf(items);

    const QVariantList rows = ytui::extractBrowseRows(root, 0);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("videoId")),
             QStringLiteral("CCCCCCCCCCC"));
}

void YtBrowseRowsTest::simpleTextAndOwnerTextShapes()
{
    QJsonArray items;
    QJsonObject ownerInner = makeRenderer(QStringLiteral("DDDDDDDDDDD"),
                                          QStringLiteral("Owner Text Row"),
                                          QStringLiteral("Owner Channel"), false);
    ownerInner.insert(QStringLiteral("ownerText"),
                      runsText(QStringLiteral("Owner Channel")));
    items.append(wrapRenderer(ownerInner));
    QJsonObject simpleInner;
    simpleInner.insert(QStringLiteral("videoId"), QStringLiteral("EEEEEEEEEEE"));
    simpleInner.insert(QStringLiteral("title"),
                       simpleText(QStringLiteral("Simple Title Row")));
    items.append(wrapRenderer(simpleInner));
    const QJsonObject root = sectionOf(items);

    const QVariantList rows = ytui::extractBrowseRows(root, 0);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("channelName")),
             QStringLiteral("Owner Channel"));
    QVERIFY(rows.at(1).toMap().value(QStringLiteral("title"))
            == QStringLiteral("Simple Title Row"));
    // absent byline maps to an empty name; the row itself is kept.
    QVERIFY(rows.at(1).toMap().value(QStringLiteral("channelName")).toString().isEmpty());
}

void YtBrowseRowsTest::rowCapsStopEarly()
{
    QJsonArray items;
    items.append(wrapRenderer(makeRenderer(QStringLiteral("AAAAAAAAAAA"),
                                           QStringLiteral("Row 1"), QStringLiteral("C"), true)));
    items.append(wrapRenderer(makeRenderer(QStringLiteral("BBBBBBBBBBB"),
                                           QStringLiteral("Row 2"), QStringLiteral("C"), true)));
    items.append(wrapRenderer(makeRenderer(QStringLiteral("CCCCCCCCCCC"),
                                           QStringLiteral("Row 3"), QStringLiteral("C"), true)));
    const QJsonObject root = sectionOf(items);

    QCOMPARE(ytui::extractBrowseRows(root, 2).size(), 2);
    QCOMPARE(ytui::extractBrowseRows(root, -1).size(), 3);
}

void YtBrowseRowsTest::continuationTokenAtAnyDepth()
{
    // continuationItemRenderer nests under continuationItems, not contents.
    QJsonObject command;
    command.insert(QStringLiteral("token"), QStringLiteral("TOKEN123"));
    QJsonObject continuationCommandObj;
    continuationCommandObj.insert(QStringLiteral("continuationCommand"), command);
    QJsonObject endpoint;
    endpoint.insert(QStringLiteral("continuationEndpoint"), continuationCommandObj);
    QJsonObject contRenderer;
    contRenderer.insert(QStringLiteral("continuationItemRenderer"), endpoint);

    QJsonObject section;
    QJsonArray sectionItems;
    sectionItems.append(contRenderer);
    section.insert(QStringLiteral("contents"), sectionItems);
    QJsonObject wrapper;
    wrapper.insert(QStringLiteral("sectionListRenderer"), section);
    QJsonArray outer;
    outer.append(wrapper);
    QJsonObject root;
    root.insert(QStringLiteral("contents"), outer);

    QCOMPARE(ytui::continuationToken(root), QStringLiteral("TOKEN123"));
}

void YtBrowseRowsTest::singlePageNoToken()
{
    QJsonArray items;
    items.append(wrapRenderer(makeRenderer(QStringLiteral("FFFFFFFFFFF"),
                                            QStringLiteral("Only Row"),
                                            QStringLiteral("C"), true)));
    const QJsonObject root = sectionOf(items);
    QVERIFY(ytui::continuationToken(root).isEmpty());
}

QTEST_MAIN(YtBrowseRowsTest)
#include "YtBrowseRowsTest.moc"
