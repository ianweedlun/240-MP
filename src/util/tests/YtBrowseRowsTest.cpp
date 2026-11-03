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

QJsonObject wrapTile(const QJsonObject &tile)
{
    QJsonObject wrapper;
    wrapper.insert(QStringLiteral("tileRenderer"), tile);
    return wrapper;
}

QJsonObject makeTile(const QString &videoId, const QString &title,
                     const QString &owner, const QString &lengthOverlay,
                     const QString &contentType =
                         QStringLiteral("TILE_CONTENT_TYPE_VIDEO"))
{
    QJsonObject t;
    t.insert(QStringLiteral("contentId"), videoId);
    if (!contentType.isEmpty())
        t.insert(QStringLiteral("contentType"), contentType);
    // header: duration lives as a time-status overlay on the thumbnail
    if (!lengthOverlay.isEmpty()) {
        QJsonObject textObj;
        textObj.insert(QStringLiteral("simpleText"), lengthOverlay);
        QJsonObject overlayInner;
        overlayInner.insert(QStringLiteral("text"), textObj);
        QJsonObject overlay;
        overlay.insert(QStringLiteral("thumbnailOverlayTimeStatusRenderer"), overlayInner);
        QJsonArray overlays;
        overlays.append(overlay);
        QJsonObject headerInner;
        headerInner.insert(QStringLiteral("thumbnailOverlays"), overlays);
        QJsonObject header;
        header.insert(QStringLiteral("tileHeaderRenderer"), headerInner);
        t.insert(QStringLiteral("header"), header);
    }
    // metadata: simpleText title + lines where the first plain text item is
    // the channel and badge/view-count items must be skipped
    QJsonObject titleObj;
    titleObj.insert(QStringLiteral("simpleText"), title);
    QJsonObject metaInner;
    metaInner.insert(QStringLiteral("title"), titleObj);
    QJsonArray items;
    QJsonObject ownerItem;
    ownerItem.insert(QStringLiteral("text"), runsText(owner));
    QJsonObject ownerWrap;
    ownerWrap.insert(QStringLiteral("lineItemRenderer"), ownerItem);
    items.append(ownerWrap);
    QJsonObject badgeItem;
    QJsonObject badge;
    badge.insert(QStringLiteral("label"), QStringLiteral("4K"));
    badgeItem.insert(QStringLiteral("badge"), badge);
    QJsonObject badgeWrap;
    badgeWrap.insert(QStringLiteral("lineItemRenderer"), badgeItem);
    items.append(badgeWrap);
    QJsonObject viewsItem;
    viewsItem.insert(QStringLiteral("text"),
                     simpleText(QStringLiteral("39K views")));
    QJsonObject viewsWrap;
    viewsWrap.insert(QStringLiteral("lineItemRenderer"), viewsItem);
    items.append(viewsWrap);
    QJsonObject lineInner;
    lineInner.insert(QStringLiteral("items"), items);
    QJsonObject lineWrap;
    lineWrap.insert(QStringLiteral("lineRenderer"), lineInner);
    QJsonArray lines;
    lines.append(lineWrap);
    metaInner.insert(QStringLiteral("lines"), lines);
    QJsonObject meta;
    meta.insert(QStringLiteral("tileMetadataRenderer"), metaInner);
    t.insert(QStringLiteral("metadata"), meta);
    return t;
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
    void tileRowsFromTvPages();
    void tileContentTypeGate();
    void watchLaterCursorTvChip();
    void watchLaterCursorDesktopPlaylist();
    void watchLaterCursorMiss();
    void playlistRowsDesktopShelf();
    void playlistRowsTvTiles();
    void playlistsTabCursorTvChip();
    void playlistsTabCursorDesktopUc();
    void playlistsTabCursorMiss();
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

void YtBrowseRowsTest::tileRowsFromTvPages()
{
    // The verified TVHTML5 account-page shape: identity is contentId +
    // contentType, title is a simpleText under tileMetadataRenderer, the
    // owner is the first plain line item (badges and view counts skipped),
    // and the duration is a thumbnail-overlay time status on the header.
    QJsonArray items;
    items.append(wrapTile(makeTile(QStringLiteral("1W8PGKl8gUg"),
                                   QStringLiteral("The Forgotten Operating System"),
                                   QStringLiteral("NT to Now"),
                                   QStringLiteral("29:52"))));
    const QJsonObject root = sectionOf(items);

    const QVariantList rows = ytui::extractBrowseRows(root, 0);
    QCOMPARE(rows.size(), 1);
    const QVariantMap row = rows.at(0).toMap();
    QCOMPARE(row.value(QStringLiteral("videoId")), QStringLiteral("1W8PGKl8gUg"));
    QCOMPARE(row.value(QStringLiteral("title")),
             QStringLiteral("The Forgotten Operating System"));
    QCOMPARE(row.value(QStringLiteral("channelName")), QStringLiteral("NT to Now"));
    QCOMPARE(row.value(QStringLiteral("lengthText")), QStringLiteral("29:52"));
    QVERIFY(row.value(QStringLiteral("url"))
            == QStringLiteral("https://www.youtube.com/watch?v=1W8PGKl8gUg"));
}

void YtBrowseRowsTest::tileContentTypeGate()
{
    // Channel tiles share the tile shape but are NOT video rows; tiles
    // without the contentType field (older payloads) stay accepted.
    QJsonArray items;
    items.append(wrapTile(makeTile(QStringLiteral("CHANNNNNNNN"),
                                   QStringLiteral("Some Channel"),
                                   QStringLiteral("Owner"),
                                   QString(),
                                   QStringLiteral("TILE_CONTENT_TYPE_CHANNEL"))));
    items.append(wrapTile(makeTile(QStringLiteral("NOTYPEEEEEE"),
                                   QStringLiteral("No Type Row"),
                                   QStringLiteral("Chan"),
                                   QString(),
                                   QString())));
    const QJsonObject root = sectionOf(items);

    const QVariantList rows = ytui::extractBrowseRows(root, 0);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("videoId")),
             QStringLiteral("NOTYPEEEEEE"));
}

void YtBrowseRowsTest::watchLaterCursorTvChip()
{
    // The account home's Watch-later tab chip carries NO playlist id: its
    // cursor is a raw reloadContinuationData continuation string.
    QJsonObject rcd;
    rcd.insert(QStringLiteral("continuation"), QStringLiteral("4qmFsgTOKEN"));
    QJsonObject cont;
    cont.insert(QStringLiteral("reloadContinuationData"), rcd);
    QJsonObject tv;
    tv.insert(QStringLiteral("continuation"), cont);
    QJsonObject content;
    content.insert(QStringLiteral("tvSurfaceContentRenderer"), tv);
    QJsonObject chip;
    chip.insert(QStringLiteral("title"), QStringLiteral("Watch later")); // raw string on TV
    chip.insert(QStringLiteral("content"), content);
    QJsonObject tabWrap;
    tabWrap.insert(QStringLiteral("tabRenderer"), chip);
    QJsonArray contents;
    contents.append(tabWrap);
    QJsonObject root;
    root.insert(QStringLiteral("contents"), contents);

    const QJsonValue cur = ytui::findWatchLaterCursor(root);
    QVERIFY(!cur.isNull());
    QCOMPARE(cur.toString(), QStringLiteral("4qmFsgTOKEN"));
}

void YtBrowseRowsTest::watchLaterCursorDesktopPlaylist()
{
    // Desktop shape: a playlistId whose subtree names the shelf title; the
    // browse cursor is the VL-prefixed playlist id.
    const QJsonValue titleObj = runsText(QStringLiteral("Watch Later"));
    QJsonObject playlistInner;
    playlistInner.insert(QStringLiteral("playlistId"), QStringLiteral("PLmywatchlater"));
    playlistInner.insert(QStringLiteral("title"), titleObj);
    QJsonObject wrapper;
    wrapper.insert(QStringLiteral("playlistRenderer"), playlistInner);
    QJsonArray contents;
    contents.append(wrapper);
    QJsonObject root;
    root.insert(QStringLiteral("contents"), contents);

    const QJsonValue cur = ytui::findWatchLaterCursor(root);
    QVERIFY(!cur.isNull());
    QCOMPARE(cur.toString(), QStringLiteral("VLPLmywatchlater"));
}

void YtBrowseRowsTest::watchLaterCursorMiss()
{
    // Decoys only: playlists without the shelf title, and an unrelated
    // continuation token elsewhere — neither may produce a cursor.
    QJsonArray items;
    QJsonObject playlistInner;
    playlistInner.insert(QStringLiteral("playlistId"), QStringLiteral("PLrandom"));
    playlistInner.insert(QStringLiteral("title"), runsText(QStringLiteral("Funny Cats")));
    QJsonObject wrapper;
    wrapper.insert(QStringLiteral("playlistRenderer"), playlistInner);
    items.append(wrapper);
    QJsonObject root = sectionOf(items);

    const QJsonValue cur = ytui::findWatchLaterCursor(root);
    QVERIFY(cur.isNull());
}

void YtBrowseRowsTest::playlistRowsDesktopShelf()
{
    // playlistRenderer rows: id + title + videoCountText, kept in document
    // order (YouTube's tab order — never re-sorted).
    QJsonArray items;
    QJsonObject p1;
    p1.insert(QStringLiteral("playlistId"), QStringLiteral("PLalpha000001"));
    p1.insert(QStringLiteral("title"), runsText(QStringLiteral("Alpha Mix")));
    p1.insert(QStringLiteral("videoCountText"), simpleText(QStringLiteral("12 videos")));
    QJsonObject w1;
    w1.insert(QStringLiteral("playlistRenderer"), p1);
    items.append(w1);
    QJsonObject p2;
    p2.insert(QStringLiteral("playlistId"), QStringLiteral("PLbeta0000002"));
    p2.insert(QStringLiteral("title"), runsText(QStringLiteral("Beta List")));
    QJsonObject w2;
    w2.insert(QStringLiteral("playlistRenderer"), p2);
    items.append(w2);
    // No id -> not a row.
    QJsonObject p3;
    p3.insert(QStringLiteral("title"), runsText(QStringLiteral("Idless")));
    QJsonObject w3;
    w3.insert(QStringLiteral("playlistRenderer"), p3);
    items.append(w3);

    QJsonObject section;
    section.insert(QStringLiteral("contents"), items);
    QJsonObject wrapper;
    wrapper.insert(QStringLiteral("sectionListRenderer"), section);
    QJsonArray outer;
    outer.append(wrapper);
    QJsonObject root;
    root.insert(QStringLiteral("contents"), outer);

    const QVariantList rows = ytui::extractPlaylistRows(root, -1);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("playlistId")),
             QStringLiteral("PLalpha000001"));
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("title")),
             QStringLiteral("Alpha Mix"));
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("videoCountText")),
             QStringLiteral("12 videos"));
    QCOMPARE(rows.at(1).toMap().value(QStringLiteral("title")),
             QStringLiteral("Beta List"));
}

void YtBrowseRowsTest::playlistRowsTvTiles()
{
    // TV playlists tab: playlist tiles share the tile shape with video
    // tiles but carry TILE_CONTENT_TYPE_PLAYLIST; channel tiles must not
    // sneak in. Empty-ctype tiles fall back to the PL/RD/UU/VL id prefix.
    QJsonArray items;
    QJsonObject t1;
    t1.insert(QStringLiteral("contentId"), QStringLiteral("PLtvlist00001"));
    t1.insert(QStringLiteral("contentType"), QStringLiteral("TILE_CONTENT_TYPE_PLAYLIST"));
    QJsonObject title;
    title.insert(QStringLiteral("simpleText"), QStringLiteral("TV Watchlist"));
    QJsonObject meta;
    meta.insert(QStringLiteral("title"), title);
    QJsonObject tileMeta;
    tileMeta.insert(QStringLiteral("tileMetadataRenderer"), meta);
    t1.insert(QStringLiteral("metadata"), tileMeta);
    QJsonObject w1;
    w1.insert(QStringLiteral("tileRenderer"), t1);
    items.append(w1);

    QJsonObject t2;
    t2.insert(QStringLiteral("contentId"), QStringLiteral("CHANnnnnnnnn"));
    t2.insert(QStringLiteral("contentType"), QStringLiteral("TILE_CONTENT_TYPE_CHANNEL"));
    QJsonObject w2;
    w2.insert(QStringLiteral("tileRenderer"), t2);
    items.append(w2);

    QJsonObject t3;
    t3.insert(QStringLiteral("contentId"), QStringLiteral("RDmix0000001")); // empty-ctype
    QJsonObject title3;
    title3.insert(QStringLiteral("simpleText"), QStringLiteral("Mixtape"));
    QJsonObject meta3;
    meta3.insert(QStringLiteral("title"), title3);
    QJsonObject tileMeta3;
    tileMeta3.insert(QStringLiteral("tileMetadataRenderer"), meta3);
    t3.insert(QStringLiteral("metadata"), tileMeta3);
    QJsonObject w3;
    w3.insert(QStringLiteral("tileRenderer"), t3);
    items.append(w3);

    QJsonObject section;
    section.insert(QStringLiteral("contents"), items);
    QJsonObject wrapper;
    wrapper.insert(QStringLiteral("sectionListRenderer"), section);
    QJsonArray outer;
    outer.append(wrapper);
    QJsonObject root;
    root.insert(QStringLiteral("contents"), outer);

    const QVariantList rows = ytui::extractPlaylistRows(root, 0);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).toMap().value(QStringLiteral("playlistId")),
             QStringLiteral("PLtvlist00001"));
    QCOMPARE(rows.at(1).toMap().value(QStringLiteral("playlistId")),
             QStringLiteral("RDmix0000001"));
}

void YtBrowseRowsTest::playlistsTabCursorTvChip()
{
    // Account home TV chip: raw reloadContinuationData cursor keyed beside a
    // "Playlists" title — same chip shape as the watch-later cursor hunt.
    QJsonObject rcd;
    rcd.insert(QStringLiteral("continuation"), QStringLiteral("4qmFsgPLAYLISTTOKEN"));
    QJsonObject cont;
    cont.insert(QStringLiteral("reloadContinuationData"), rcd);
    QJsonObject tv;
    tv.insert(QStringLiteral("continuation"), cont);
    QJsonObject content;
    content.insert(QStringLiteral("tvSurfaceContentRenderer"), tv);
    QJsonObject chip;
    chip.insert(QStringLiteral("title"), QStringLiteral("Playlists"));
    chip.insert(QStringLiteral("content"), content);
    QJsonObject tabWrap;
    tabWrap.insert(QStringLiteral("tabRenderer"), chip);
    QJsonArray contents;
    contents.append(tabWrap);
    QJsonObject root;
    root.insert(QStringLiteral("contents"), contents);

    const QJsonValue cur = ytui::findPlaylistsTabCursor(root);
    QVERIFY(!cur.isNull());
    QCOMPARE(cur.toString(), QStringLiteral("4qmFsgPLAYLISTTOKEN"));
}

void YtBrowseRowsTest::playlistsTabCursorDesktopUc()
{
    // Desktop tab chip: browseEndpoint browseId UC<channelId>. A bare
    // playlistId chip (named playlist, not the tab) must not match.
    QJsonObject be;
    be.insert(QStringLiteral("browseId"), QStringLiteral("UCUCchannelid1"));
    QJsonObject ep;
    ep.insert(QStringLiteral("browseEndpoint"), be);
    QJsonObject chip;
    chip.insert(QStringLiteral("title"), runsText(QStringLiteral("Playlists")));
    chip.insert(QStringLiteral("endpoint"), ep);
    QJsonArray contents;
    QJsonObject decoy;
    QJsonObject playlistInner;
    playlistInner.insert(QStringLiteral("playlistId"), QStringLiteral("PLnope0000001"));
    playlistInner.insert(QStringLiteral("title"), runsText(QStringLiteral("Watch later")));
    decoy.insert(QStringLiteral("playlistRenderer"), playlistInner);
    contents.append(decoy);
    contents.append(chip);
    QJsonObject root;
    root.insert(QStringLiteral("contents"), contents);

    const QJsonValue cur = ytui::findPlaylistsTabCursor(root);
    QVERIFY(!cur.isNull());
    QCOMPARE(cur.toString(), QStringLiteral("UCUCchannelid1"));
}

void YtBrowseRowsTest::playlistsTabCursorMiss()
{
    // Decoys only: watch-later chips and video rows, no tab cursor anywhere.
    QJsonArray items;
    QJsonObject playlistInner;
    playlistInner.insert(QStringLiteral("playlistId"), QStringLiteral("PLwatchlaterstuff"));
    playlistInner.insert(QStringLiteral("title"), runsText(QStringLiteral("Watch later")));
    QJsonObject wrapper;
    wrapper.insert(QStringLiteral("playlistRenderer"), playlistInner);
    items.append(wrapper);
    QJsonObject root = sectionOf(items);

    const QJsonValue cur = ytui::findPlaylistsTabCursor(root);
    QVERIFY(cur.isNull());
}

QTEST_MAIN(YtBrowseRowsTest)
#include "YtBrowseRowsTest.moc"
