// Unit tests for live HLS master shaping (#128). Pure fixtures modeled on the
// measured Kofie 0.5 live table (m3u8 variants 144p..1080p + two audio groups).
// Behavior assertions — variant eligibility, audio pruning, URI absolutization,
// fallbacks — not source-text snapshots.
#include <QtTest>
#include "util/YtHlsSelect.h"

class YtHlsSelectTest : public QObject {
    Q_OBJECT
private slots:
    void cap480KeepsFourVariantsAndReferencedAudio();
    void cap144PrunesUnreferencedAudioGroup();
    void capBelowAllFallsBackToLowestKnown();
    void absoluteUrisPassThrough();
    void relativeUrisResolveAgainstMaster();
    void garbageReturnsEmpty();
    void headerWithoutVariantsReturnsEmpty();
    void unknownResolutionFallsBackToBandwidth();
    void invalidCapReturnsEmpty();
    void muxedVariantsWithoutAudioGroup();
    void crlfInputParses();
    void codecsCommaDoesNotSplitAttrs();
};

static const char *kMaster =
    "#EXTM3U\n"
    "#EXT-X-VERSION:3\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"English\",DEFAULT=YES,URI=\"audio_234.m3u8\"\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac_low\",NAME=\"English low\",URI=\"audio_233.m3u8\"\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=269056,RESOLUTION=256x144,CODECS=\"avc1.4d400c,mp4a.40.2\",AUDIO=\"aac_low\"\n"
    "v144.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=426x240,CODECS=\"avc1.4d401e\",AUDIO=\"aac_low\"\n"
    "v240.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=640x360,AUDIO=\"aac\"\n"
    "v360.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=1300000,RESOLUTION=854x480,AUDIO=\"aac\"\n"
    "v480.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720,AUDIO=\"aac\"\n"
    "v720.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=4500000,RESOLUTION=1920x1080,AUDIO=\"aac\"\n"
    "v1080.m3u8\n";

static const QUrl kBase(QStringLiteral(
    "https://manifest.googlevideo.com/api/manifest/hls_playlist/abc/master.m3u8"));

static int countOccurrences(const QString &hay, const QString &needle)
{
    return hay.count(needle);
}

void YtHlsSelectTest::cap480KeepsFourVariantsAndReferencedAudio()
{
    const QString out = ythls::filterMaster(QString::fromLatin1(kMaster), kBase, 480);
    QVERIFY(!out.isEmpty());
    QVERIFY(out.startsWith(QStringLiteral("#EXTM3U\n")));
    QVERIFY(out.contains(QStringLiteral("#EXT-X-VERSION:3")));
    for (const char *v : {"v144.m3u8", "v240.m3u8", "v360.m3u8", "v480.m3u8"})
        QVERIFY2(out.contains(QString::fromLatin1(v)), v);
    QVERIFY(!out.contains(QStringLiteral("v720.m3u8")));
    QVERIFY(!out.contains(QStringLiteral("v1080.m3u8")));
    // Both audio groups are referenced by kept variants.
    QVERIFY(out.contains(QStringLiteral("audio_234.m3u8")));
    QVERIFY(out.contains(QStringLiteral("audio_233.m3u8")));
    // Exactly four STREAM-INF entries survive.
    QCOMPARE(countOccurrences(out, QStringLiteral("#EXT-X-STREAM-INF:")), 4);
}

void YtHlsSelectTest::cap144PrunesUnreferencedAudioGroup()
{
    const QString out = ythls::filterMaster(QString::fromLatin1(kMaster), kBase, 144);
    QVERIFY(out.contains(QStringLiteral("v144.m3u8")));
    QVERIFY(!out.contains(QStringLiteral("v240.m3u8")));
    QVERIFY(out.contains(QStringLiteral("audio_233.m3u8")));   // aac_low referenced
    QVERIFY(!out.contains(QStringLiteral("audio_234.m3u8")));  // aac only on dropped variants
    QCOMPARE(countOccurrences(out, QStringLiteral("#EXT-X-STREAM-INF:")), 1);
}

void YtHlsSelectTest::capBelowAllFallsBackToLowestKnown()
{
    // A live event must still play when every variant exceeds the cap.
    const QString out = ythls::filterMaster(QString::fromLatin1(kMaster), kBase, 100);
    QVERIFY(!out.isEmpty());
    QVERIFY(out.contains(QStringLiteral("v144.m3u8")));
    QCOMPARE(countOccurrences(out, QStringLiteral("#EXT-X-STREAM-INF:")), 1);
}

void YtHlsSelectTest::absoluteUrisPassThrough()
{
    const QString master = QStringLiteral(
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1000,RESOLUTION=640x360\n"
        "https://cdn.example.net/live/v360.m3u8\n");
    const QString out = ythls::filterMaster(master, kBase, 480);
    QVERIFY(out.contains(QStringLiteral("https://cdn.example.net/live/v360.m3u8")));
}

void YtHlsSelectTest::relativeUrisResolveAgainstMaster()
{
    const QString out = ythls::filterMaster(QString::fromLatin1(kMaster), kBase, 144);
    QVERIFY(out.contains(QStringLiteral(
        "https://manifest.googlevideo.com/api/manifest/hls_playlist/abc/v144.m3u8")));
    QVERIFY(out.contains(QStringLiteral(
        "https://manifest.googlevideo.com/api/manifest/hls_playlist/abc/audio_233.m3u8")));
}

void YtHlsSelectTest::garbageReturnsEmpty()
{
    QCOMPARE(ythls::filterMaster(QStringLiteral("not a playlist"), kBase, 480), QString());
    QCOMPARE(ythls::filterMaster(QString(), kBase, 480), QString());
    QCOMPARE(ythls::filterMaster(QStringLiteral("<html>403</html>"), kBase, 480), QString());
}

void YtHlsSelectTest::headerWithoutVariantsReturnsEmpty()
{
    QCOMPARE(ythls::filterMaster(QStringLiteral("#EXTM3U\n#EXT-X-VERSION:3\n"), kBase, 480),
             QString());
}

void YtHlsSelectTest::unknownResolutionFallsBackToBandwidth()
{
    const QString master = QStringLiteral(
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=4500000\n"
        "high.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=269056\n"
        "low.m3u8\n");
    const QString out = ythls::filterMaster(master, kBase, 480);
    QVERIFY(out.contains(QStringLiteral("low.m3u8")));
    QVERIFY(!out.contains(QStringLiteral("high.m3u8")));
}

void YtHlsSelectTest::invalidCapReturnsEmpty()
{
    QCOMPARE(ythls::filterMaster(QString::fromLatin1(kMaster), kBase, 0), QString());
    QCOMPARE(ythls::filterMaster(QString::fromLatin1(kMaster), QUrl(), 480), QString());
}

void YtHlsSelectTest::muxedVariantsWithoutAudioGroup()
{
    const QString master = QStringLiteral(
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1300000,RESOLUTION=854x480,CODECS=\"avc1.4d401f,mp4a.40.2\"\n"
        "muxed480.m3u8\n");
    const QString out = ythls::filterMaster(master, kBase, 480);
    QVERIFY(out.contains(QStringLiteral("muxed480.m3u8")));
    QVERIFY(!out.contains(QStringLiteral("#EXT-X-MEDIA:")));
}

void YtHlsSelectTest::crlfInputParses()
{
    QString crlf = QString::fromLatin1(kMaster);
    crlf.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    const QString out = ythls::filterMaster(crlf, kBase, 480);
    QCOMPARE(countOccurrences(out, QStringLiteral("#EXT-X-STREAM-INF:")), 4);
    QVERIFY(out.contains(QStringLiteral("v480.m3u8")));
}

void YtHlsSelectTest::codecsCommaDoesNotSplitAttrs()
{
    // The quoted CODECS comma must not break AUDIO group detection: the 144p
    // variant carries CODECS="avc1...,mp4a..." AND AUDIO="aac_low".
    const QString out = ythls::filterMaster(QString::fromLatin1(kMaster), kBase, 144);
    QVERIFY(out.contains(QStringLiteral("AUDIO=\"aac_low\"")));
    QVERIFY(out.contains(QStringLiteral("audio_233.m3u8")));
    QVERIFY(out.contains(QStringLiteral("CODECS=\"avc1.4d400c,mp4a.40.2\"")));
}

QTEST_MAIN(YtHlsSelectTest)
#include "YtHlsSelectTest.moc"
