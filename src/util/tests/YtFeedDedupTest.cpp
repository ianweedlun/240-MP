// Unit tests for subscription-union dedup (#128). Covers the measured live
// double-post (same channel, identical title, two ids, minutes apart), the
// cross-channel videoId repeat, and the safety arms: far-apart same-title
// daily uploads and missing metadata must NOT collapse.
#include <QtTest>
#include "util/YtFeedDedup.h"

class YtFeedDedupTest : public QObject {
    Q_OBJECT
private slots:
    void sameVideoIdTwiceKeepsNewest();
    void liveDoublePostCollapses();
    void dailyShowSameTitleFarApartKept();
    void windowBoundaryIsInclusive();
    void justOutsideWindowKept();
    void differentChannelsSameTitleKept();
    void crossChannelSameVideoIdCollapses();
    void missingPublishedDoesNotTitleCollapse();
    void missingChannelOrTitleDoesNotCollapse();
    void emptyInputAndOrderPreserved();
    void chainCollapseTracksNewestKept();
};

static QVariant row(const QString &id, const QString &channel, const QString &title,
                    qint64 publishedMs)
{
    QVariantMap v;
    v.insert(QStringLiteral("videoId"), id);
    v.insert(QStringLiteral("channelId"), channel);
    v.insert(QStringLiteral("title"), title);
    v.insert(QStringLiteral("publishedMs"), publishedMs);
    return v;
}

static constexpr qint64 kHour = 60 * 60 * 1000;

void YtFeedDedupTest::sameVideoIdTwiceKeepsNewest()
{
    const QVariantList in = {row(QStringLiteral("A"), QStringLiteral("C"), QStringLiteral("T"), 200),
                             row(QStringLiteral("A"), QStringLiteral("C"), QStringLiteral("T"), 100)};
    const QVariantList out = ytfeed::dedupFeed(in);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].toMap().value(QStringLiteral("publishedMs")).toLongLong(), 200);
}

void YtFeedDedupTest::liveDoublePostCollapses()
{
    // The measured Kofie 0.5 case: two ids, same channel + title, 5 min apart.
    const QVariantList in = {
        row(QStringLiteral("yOLpzNqq8cc"), QStringLiteral("Kofie"), QStringLiteral("KNICKS WATCH PARTY"), 1000000),
        row(QStringLiteral("12sxyyYCs00"), QStringLiteral("Kofie"), QStringLiteral("KNICKS WATCH PARTY"), 1000000 - 5 * 60 * 1000)};
    const QVariantList out = ytfeed::dedupFeed(in);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].toMap().value(QStringLiteral("videoId")).toString(), QStringLiteral("yOLpzNqq8cc"));
}

void YtFeedDedupTest::dailyShowSameTitleFarApartKept()
{
    const QVariantList in = {
        row(QStringLiteral("A"), QStringLiteral("C"), QStringLiteral("Morning News"), 2 * kHour),
        row(QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("Morning News"), 2 * kHour - 24 * kHour)};
    QCOMPARE(ytfeed::dedupFeed(in).size(), 2);
}

void YtFeedDedupTest::windowBoundaryIsInclusive()
{
    const QVariantList in = {
        row(QStringLiteral("A"), QStringLiteral("C"), QStringLiteral("T"), 20 * kHour),
        row(QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("T"), 20 * kHour - ytfeed::kLiveDuplicateWindowMs)};
    QCOMPARE(ytfeed::dedupFeed(in).size(), 1);
}

void YtFeedDedupTest::justOutsideWindowKept()
{
    const QVariantList in = {
        row(QStringLiteral("A"), QStringLiteral("C"), QStringLiteral("T"), 20 * kHour),
        row(QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("T"), 20 * kHour - ytfeed::kLiveDuplicateWindowMs - 1)};
    QCOMPARE(ytfeed::dedupFeed(in).size(), 2);
}

void YtFeedDedupTest::differentChannelsSameTitleKept()
{
    const QVariantList in = {
        row(QStringLiteral("A"), QStringLiteral("C1"), QStringLiteral("T"), 1000),
        row(QStringLiteral("B"), QStringLiteral("C2"), QStringLiteral("T"), 1000)};
    QCOMPARE(ytfeed::dedupFeed(in).size(), 2);
}

void YtFeedDedupTest::crossChannelSameVideoIdCollapses()
{
    const QVariantList in = {
        row(QStringLiteral("A"), QStringLiteral("C1"), QStringLiteral("T"), 2000),
        row(QStringLiteral("A"), QStringLiteral("C2"), QStringLiteral("T"), 1000)};
    const QVariantList out = ytfeed::dedupFeed(in);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].toMap().value(QStringLiteral("channelId")).toString(), QStringLiteral("C1"));
}

void YtFeedDedupTest::missingPublishedDoesNotTitleCollapse()
{
    const QVariantList in = {
        row(QStringLiteral("A"), QStringLiteral("C"), QStringLiteral("T"), 0),
        row(QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("T"), 1000)};
    QCOMPARE(ytfeed::dedupFeed(in).size(), 2);
    // videoId dedup still applies without timestamps.
    const QVariantList sameId = {
        row(QStringLiteral("A"), QStringLiteral("C"), QStringLiteral("T"), 0),
        row(QStringLiteral("A"), QStringLiteral("C"), QStringLiteral("T"), 0)};
    QCOMPARE(ytfeed::dedupFeed(sameId).size(), 1);
}

void YtFeedDedupTest::missingChannelOrTitleDoesNotCollapse()
{
    const QVariantList noChannel = {
        row(QStringLiteral("A"), QString(), QStringLiteral("T"), 1000),
        row(QStringLiteral("B"), QString(), QStringLiteral("T"), 1000)};
    QCOMPARE(ytfeed::dedupFeed(noChannel).size(), 2);
    const QVariantList noTitle = {
        row(QStringLiteral("A"), QStringLiteral("C"), QString(), 1000),
        row(QStringLiteral("B"), QStringLiteral("C"), QString(), 1000)};
    QCOMPARE(ytfeed::dedupFeed(noTitle).size(), 2);
}

void YtFeedDedupTest::emptyInputAndOrderPreserved()
{
    QCOMPARE(ytfeed::dedupFeed({}).size(), 0);
    const QVariantList in = {
        row(QStringLiteral("A"), QStringLiteral("C1"), QStringLiteral("T1"), 3000),
        row(QStringLiteral("B"), QStringLiteral("C2"), QStringLiteral("T2"), 2000),
        row(QStringLiteral("C"), QStringLiteral("C3"), QStringLiteral("T3"), 1000)};
    const QVariantList out = ytfeed::dedupFeed(in);
    QCOMPARE(out.size(), 3);
    QCOMPARE(out[0].toMap().value(QStringLiteral("videoId")).toString(), QStringLiteral("A"));
    QCOMPARE(out[2].toMap().value(QStringLiteral("videoId")).toString(), QStringLiteral("C"));
}

void YtFeedDedupTest::chainCollapseTracksNewestKept()
{
    // A at 0 h, B at 11 h (duplicate of A), C at 22 h (a NEW broadcast, 22 h
    // from the kept A): A and C survive, B collapses.
    const QVariantList in = {
        row(QStringLiteral("C"), QStringLiteral("CH"), QStringLiteral("T"), 22 * kHour),
        row(QStringLiteral("B"), QStringLiteral("CH"), QStringLiteral("T"), 11 * kHour),
        row(QStringLiteral("A"), QStringLiteral("CH"), QStringLiteral("T"), 0)};
    const QVariantList out = ytfeed::dedupFeed(in);
    QCOMPARE(out.size(), 2);
    QCOMPARE(out[0].toMap().value(QStringLiteral("videoId")).toString(), QStringLiteral("C"));
    QCOMPARE(out[1].toMap().value(QStringLiteral("videoId")).toString(), QStringLiteral("A"));
}

QTEST_MAIN(YtFeedDedupTest)
#include "YtFeedDedupTest.moc"
