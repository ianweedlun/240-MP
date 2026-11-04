#include <QtTest/QtTest>

#include "../YtSponsorEdl.h"

// Pure-logic tests for the SponsorBlock -> mpv EDL layer (issue #28 plan E):
// collision filtering by videoUID, dead-range guards, range merging, EDL
// escaping and the virtual-timeline gap arithmetic. No processes, no network
// (the live mpv behavior was proven by IPC probe on the real binary).

namespace {

QJsonObject row(const QString &cat, const QString &action, double a, double b)
{
    QJsonObject o;
    QJsonArray seg;
    seg.append(a);
    seg.append(b);
    o.insert(QStringLiteral("category"), cat);
    o.insert(QStringLiteral("actionType"), action);
    o.insert(QStringLiteral("segment"), seg);
    return o;
}

QJsonObject group(const QString &uid, const QJsonArray &segments)
{
    QJsonObject o;
    o.insert(QStringLiteral("videoID"), uid);
    o.insert(QStringLiteral("segments"), segments);
    return o;
}

} // anonymous namespace

class YtSponsorEdlTest : public QObject {
    Q_OBJECT

private slots:
    void collisionFilterKeepsExactVideoUidOnly();
    void deadRangesDropped();
    void mergeOverlappingSkipRanges();
    void edlValueEscapeRoundtrip();
    void buildEdlSkipOnlyComputesVirtualGaps();
    void buildEdlMarkBoundariesCarryTitles();
    void buildEdlSkipAndMarkMix();
    void buildEdlEmptyWhenNothingActive();
};

void YtSponsorEdlTest::collisionFilterKeepsExactVideoUidOnly()
{
    QJsonArray payload;
    QJsonArray mineSegs;
    mineSegs.append(row(QStringLiteral("sponsor"), QStringLiteral("skip"), 5.0, 15.0));
    QJsonArray otherSegs;
    otherSegs.append(row(QStringLiteral("intro"), QStringLiteral("skip"), 100.0, 105.0));
    payload.append(group(QStringLiteral("ABCD1234efgh"), mineSegs));
    payload.append(group(QStringLiteral("ZZZZ0000yyyy"), otherSegs));

    const auto segs = yt::sponsorSegmentsFromJson(payload, QStringLiteral("ABCD1234efgh"));
    QCOMPARE(segs.size(), 1);
    QCOMPARE(segs[0].start, 5.0);
    QCOMPARE(segs[0].end, 15.0);
    QCOMPARE(segs[0].category, QStringLiteral("sponsor"));
}

void YtSponsorEdlTest::deadRangesDropped()
{
    QJsonArray rows;
    rows.append(row(QStringLiteral("sponsor"), QStringLiteral("full"), 0.0, 0.0)); // [0,0] guard
    rows.append(row(QStringLiteral("filler"), QStringLiteral("skip"), 7.0, 7.0));  // zero length
    rows.append(row(QStringLiteral("intro"), QStringLiteral("skip"), -1.0, 3.0));  // negative
    rows.append(row(QStringLiteral("outro"), QStringLiteral("skip"), 60.0, 50.0)); // inverted
    rows.append(row(QStringLiteral("preview"), QStringLiteral("skip"), 1.0, 2.0)); // valid
    const auto segs = yt::segmentsFromRows(rows);
    QCOMPARE(segs.size(), 1);
    QCOMPARE(segs[0].category, QStringLiteral("preview"));
}

void YtSponsorEdlTest::mergeOverlappingSkipRanges()
{
    QVector<yt::Segment> segs;
    segs.append({QStringLiteral("sponsor"), QStringLiteral("skip"), 10.0, 20.0});
    segs.append({QStringLiteral("selfpromo"), QStringLiteral("mute"), 15.0, 25.0});
    segs.append({QStringLiteral("sponsor"), QStringLiteral("skip"), 30.0, 40.0});
    const QSet<QString> drop{QStringLiteral("skip"), QStringLiteral("mute")};
    const auto merged = yt::mergeRanges(segs, drop);
    QCOMPARE(merged.size(), 2);
    QCOMPARE(merged[0].first, 10.0);
    QCOMPARE(merged[0].second, 25.0);
    QCOMPARE(merged[1].first, 30.0);
    QCOMPARE(merged[1].second, 40.0);
}

void YtSponsorEdlTest::edlValueEscapeRoundtrip()
{
    // Clean URL characters pass through untouched.
    QCOMPARE(yt::edlValue(QStringLiteral("https://x/play?v=1&a=b&sig=c%2Fd")),
             QStringLiteral("https://x/play?v=1&a=b&sig=c%2Fd"));
    // Comma in a title forces the %<len>%<value> form.
    QCOMPARE(yt::edlValue(QStringLiteral("a,b")), QStringLiteral("%3%a,b"));
    // EDL FILE fields always carry the %<len>% prefix (mpv 0.40 refuses unescaped
    // https:// filenames outright).
    QCOMPARE(yt::edlFile(QStringLiteral("https://x/play?v=1")),
             QStringLiteral("%18%https://x/play?v=1"));
}

void YtSponsorEdlTest::buildEdlSkipOnlyComputesVirtualGaps()
{
    const QString u = QStringLiteral("https://gitvd/play?v=1");
    QVector<QPair<double, double>> skip;
    skip.append(QPair<double, double>(5.0, 15.0));
    skip.append(QPair<double, double>(20.0, 25.0));
    const QString edl = yt::buildEdl(u, skip, {});
    // play 0-5, jump to 15 for 5s (omitting 5-15 and 20-25), then rest open.
    const QString e = QStringLiteral("%%1%").arg(u.size()) + u;
    QCOMPARE(edl, QStringLiteral("edl://!no_chapters;") + e + ",0,5;" + e + ",15,5;" + e + ",25");
    // Skip from t=0: only the open tail remains.
    const QString headSkip = yt::buildEdl(u, {QPair<double, double>(0.0, 10.0)}, {});
    QCOMPARE(headSkip, QStringLiteral("edl://!no_chapters;") + e + ",10");
}

void YtSponsorEdlTest::buildEdlMarkBoundariesCarryTitles()
{
    const QString u = QStringLiteral("https://gitvd/play?v=2");
    QVector<yt::Segment> marks;
    marks.append({QStringLiteral("sponsor"), QStringLiteral("skip"), 10.0, 20.0});
    const QString edl = yt::buildEdl(u, {}, marks);
    // No content cut; boundary cuts make the 10-20 segment titled for OSC chapters.
    const QString e = QStringLiteral("%%1%").arg(u.size()) + u;
    QCOMPARE(edl, QStringLiteral("edl://") + e + ",0,10;" + e + ",10,10,title=sponsor;" + e + ",20");
}

void YtSponsorEdlTest::buildEdlSkipAndMarkMix()
{
    const QString u = QStringLiteral("https://gitvd/play?v=3");
    QVector<QPair<double, double>> skip;
    skip.append(QPair<double, double>(5.0, 15.0));
    QVector<yt::Segment> marks;
    marks.append({QStringLiteral("sponsor"), QStringLiteral("skip"), 10.0, 20.0});
    const QString edl = yt::buildEdl(u, skip, marks);
    // 5-10 and 10-15 are cut (skip wins the space), 15-20 is the marked remainder.
    const QString e = QStringLiteral("%%1%").arg(u.size()) + u;
    QCOMPARE(edl, QStringLiteral("edl://") + e + ",0,5;" + e + ",15,5,title=sponsor;" + e + ",20");
}

void YtSponsorEdlTest::buildEdlEmptyWhenNothingActive()
{
    const QString edl = yt::buildEdl(QStringLiteral("https://x/a"), {}, {});
    QVERIFY(edl.isEmpty());
}

QTEST_MAIN(YtSponsorEdlTest)
#include "YtSponsorEdlTest.moc"
