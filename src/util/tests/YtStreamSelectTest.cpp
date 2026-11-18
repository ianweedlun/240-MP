#include <QtTest/QtTest>

#include "../YtStreamSelect.h"

// Pure-logic tests for the Innertube stream picker (overnight torture
// t_7ffb1fd1): cap enforcement, the H.264 preference, the itag-140 audio pick,
// the muxed single-stream fallback, and expiry derivation. No network, no Qt
// event loop, no vendored library.

namespace {

ytstream::Format video(int itag, int height, const QString &codec, bool withUrl = true)
{
    ytstream::Format f;
    f.itag = itag;
    f.height = height;
    f.mimeType = QStringLiteral("video/mp4; codecs=\"%1\"").arg(codec);
    f.url = withUrl ? QStringLiteral("https://v/%1").arg(itag) : QString();
    return f;
}

ytstream::Format audio(int itag, const QString &codec)
{
    ytstream::Format f;
    f.itag = itag;
    f.mimeType = QStringLiteral("audio/mp4; codecs=\"%1\"").arg(codec);
    f.url = QStringLiteral("https://a/%1").arg(itag);
    return f;
}

} // anonymous namespace

class YtStreamSelectTest : public QObject {
    Q_OBJECT

private slots:
    void picksTallestAvc1UnderTheCap();
    void prefersAvc1OverVp9AtTheSameHeight();
    void fallsBackToNonAvc1WhenNothingFits();
    void rejectsEverythingAboveTheCap();
    void prefersItag140Audio();
    void muxedSingleWhenNoAudioTrack();
    void muxedIsNotInventedFromVideoOnly();
    void emptyInputIsNotOk();
    void resolutionCaps();
    void expiryDerivation();
};

void YtStreamSelectTest::picksTallestAvc1UnderTheCap()
{
    const QVector<ytstream::Format> formats{
        video(137, 1080, QStringLiteral("avc1.640028")),
        video(135, 480, QStringLiteral("avc1.4d401f")),
        video(134, 360, QStringLiteral("avc1.4d401e")),
        audio(140, QStringLiteral("mp4a.40.2")),
    };
    const ytstream::Pair p = ytstream::selectPair(formats, 480);
    QVERIFY(p.ok);
    QCOMPARE(p.videoUrl, QStringLiteral("https://v/135"));
    QCOMPARE(p.audioUrl, QStringLiteral("https://a/140"));
    QVERIFY(!p.single);
}

void YtStreamSelectTest::prefersAvc1OverVp9AtTheSameHeight()
{
    // A VP9 480p track listed FIRST must lose to the H.264 480p track.
    const QVector<ytstream::Format> formats{
        video(244, 480, QStringLiteral("vp9")),
        video(135, 480, QStringLiteral("avc1.4d401f")),
        audio(140, QStringLiteral("mp4a.40.2")),
    };
    const ytstream::Pair p = ytstream::selectPair(formats, 480);
    QVERIFY(p.ok);
    QCOMPARE(p.videoUrl, QStringLiteral("https://v/135"));
}

void YtStreamSelectTest::fallsBackToNonAvc1WhenNothingFits()
{
    const QVector<ytstream::Format> formats{
        video(244, 480, QStringLiteral("vp9")),
        video(137, 1080, QStringLiteral("avc1.640028")), // avc1 but over the cap
        audio(140, QStringLiteral("mp4a.40.2")),
    };
    const ytstream::Pair p = ytstream::selectPair(formats, 480);
    QVERIFY(p.ok);
    QCOMPARE(p.videoUrl, QStringLiteral("https://v/244"));
}

void YtStreamSelectTest::rejectsEverythingAboveTheCap()
{
    const QVector<ytstream::Format> formats{
        video(137, 1080, QStringLiteral("avc1.640028")),
        video(136, 720, QStringLiteral("avc1.4d401f")),
        audio(140, QStringLiteral("mp4a.40.2")),
    };
    const ytstream::Pair p = ytstream::selectPair(formats, 480);
    QVERIFY(!p.ok);
    QVERIFY(p.videoUrl.isEmpty());
}

void YtStreamSelectTest::prefersItag140Audio()
{
    // 251 (opus/webm, no seek index) listed first must lose to 140 (m4a).
    const QVector<ytstream::Format> formats{
        video(135, 480, QStringLiteral("avc1.4d401f")),
        audio(251, QStringLiteral("opus")),
        audio(140, QStringLiteral("mp4a.40.2")),
    };
    const ytstream::Pair p = ytstream::selectPair(formats, 480);
    QVERIFY(p.ok);
    QCOMPARE(p.audioUrl, QStringLiteral("https://a/140"));
}

void YtStreamSelectTest::muxedSingleWhenNoAudioTrack()
{
    // Live/combined progressive format: one mimeType names both codecs.
    ytstream::Format muxed = video(18, 360, QStringLiteral("avc1.42001E, mp4a.40.2"));
    const QVector<ytstream::Format> formats{muxed};
    const ytstream::Pair p = ytstream::selectPair(formats, 480);
    QVERIFY(p.ok);
    QVERIFY(p.single);
    QCOMPARE(p.videoUrl, QStringLiteral("https://v/18"));
    QVERIFY(p.audioUrl.isEmpty());
}

void YtStreamSelectTest::muxedIsNotInventedFromVideoOnly()
{
    // A video-only track with no audio sibling is NOT playable as single.
    const QVector<ytstream::Format> formats{video(135, 480, QStringLiteral("avc1.4d401f"))};
    const ytstream::Pair p = ytstream::selectPair(formats, 480);
    QVERIFY(!p.ok);
    QVERIFY(!p.single);
}

void YtStreamSelectTest::emptyInputIsNotOk()
{
    QVERIFY(!ytstream::selectPair({}, 480).ok);
    QVERIFY(!ytstream::selectPair({video(135, 480, QStringLiteral("avc1"))}, 0).ok);
    // A track with no URL is unusable even when its metadata fits.
    QVERIFY(!ytstream::selectPair({video(135, 480, QStringLiteral("avc1"), false)}, 480).ok);
}

void YtStreamSelectTest::resolutionCaps()
{
    QCOMPARE(ytstream::maxHeightForResolution(QStringLiteral("480p")), 480);
    QCOMPARE(ytstream::maxHeightForResolution(QStringLiteral("720p")), 720);
    QCOMPARE(ytstream::maxHeightForResolution(QStringLiteral("1080p")), 1080);
    // Unknown / empty / junk all land on the safe default.
    QCOMPARE(ytstream::maxHeightForResolution(QString()), 480);
    QCOMPARE(ytstream::maxHeightForResolution(QStringLiteral("8k")), 480);
}

void YtStreamSelectTest::expiryDerivation()
{
    const qint64 now = 1000000;
    // The URL's own epoch wins, even when it is farther out than the claim.
    QCOMPARE(ytstream::expiryEpoch(now + 21540, now, 21540), now + 21540);
    // No usable URL epoch (0 / already past): fall back to the claim.
    QCOMPARE(ytstream::expiryEpoch(0, now, 21540), now + 21540);
    QCOMPARE(ytstream::expiryEpoch(now - 5, now, 21540), now + 21540);
    // Neither source: reject (caller must not cache it).
    QCOMPARE(ytstream::expiryEpoch(0, now, 0), 0);
    QCOMPARE(ytstream::expiryEpoch(0, now, -1), 0);
}

QTEST_MAIN(YtStreamSelectTest)
#include "YtStreamSelectTest.moc"
