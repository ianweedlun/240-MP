#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "../../../util/YtNegativeCache.h"
#include "../../../util/YtUrlFresh.h"
#include "../YouTubeBackend.h"
#include "../YtInnertubeResolver.h"

// Integration gate: a cached permanent failure must stop all three resolve
// entry points (list warm, focus warm and press) before Innertube or yt-dlp.
// The second backend instance proves the suppression survives an app restart.
class YouTubeNegativeCacheIntegrationTest : public QObject {
    Q_OBJECT

private slots:
    void permanentResultPersistsWithoutYtdlpFallback();
    void transientResultKeepsYtdlpFallback();
    void cachedFailureSkipsWarmFocusAndPressAcrossRestart();

private:
    static int telemetryCount(const QSignalSpy &spy, const QString &source)
    {
        int count = 0;
        for (const QList<QVariant> &row : spy)
            if (row.at(1).toString() == source)
                ++count;
        return count;
    }

    static void seedSponsorHit(const QString &dataRoot, const QString &videoId)
    {
        QJsonObject row;
        row.insert(QStringLiteral("videoId"), videoId);
        row.insert(QStringLiteral("segments"), QJsonArray());
        row.insert(QStringLiteral("fetchedAt"),
                   double(QDateTime::currentMSecsSinceEpoch()));
        QFile file(dataRoot + QStringLiteral("/youtube_sponsor.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonArray{row}).toJson(QJsonDocument::Compact));
    }

    static QString installYtdlpSentinel(const QString &dataRoot)
    {
        QDir().mkpath(dataRoot + QStringLiteral("/bin"));
        const QString marker = dataRoot + QStringLiteral("/ytdlp-called");
        QFile script(dataRoot + QStringLiteral("/bin/yt-dlp"));
        if (!script.open(QIODevice::WriteOnly))
            return QString();
        script.write("#!/bin/sh\nprintf called >> '" + marker.toUtf8() + "'\n");
        script.close();
        script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                              | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                              | QFileDevice::ExeGroup | QFileDevice::ReadOther
                              | QFileDevice::ExeOther);
        return marker;
    }
};

void YouTubeNegativeCacheIntegrationTest::permanentResultPersistsWithoutYtdlpFallback()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString marker = installYtdlpSentinel(dir.path());
    QVERIFY(!marker.isEmpty());
    const QString videoId = QStringLiteral("Fi2d7mN-EzU");
    const QString resolution = QStringLiteral("480p");
    const QString key = yturl::cacheKeyFor(videoId, QStringLiteral("fmt"));

    YouTubeBackend backend(QString(), dir.path());
    auto *resolver = backend.findChild<YtInnertubeResolver *>();
    QVERIFY(resolver != nullptr);
    QSignalSpy failed(&backend, &YouTubeBackend::playbackResolveFailed);

    YtResolveResult result;
    result.videoId = videoId;
    result.resolution = resolution;
    result.key = key;
    result.ok = false;
    result.reason = QStringLiteral("exception:LOGIN_REQUIRED: age-restricted");
    result.elapsedMs = 150;
    emit resolver->finished(result);

    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 250);
    QCOMPARE(failed.at(0).at(0).toString(), videoId);
    QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("LOGIN_REQUIRED"));
    QVERIFY(!QFile::exists(marker));

    const QJsonObject persisted = ytneg::readNegativeCache(
        dir.filePath(QStringLiteral("youtube_resolve_failures.json")),
        QDateTime::currentSecsSinceEpoch());
    QCOMPARE(ytneg::freshFailureClass(persisted, videoId,
                                      QDateTime::currentSecsSinceEpoch()),
             QStringLiteral("LOGIN_REQUIRED"));
}

void YouTubeNegativeCacheIntegrationTest::transientResultKeepsYtdlpFallback()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString marker = installYtdlpSentinel(dir.path());
    QVERIFY(!marker.isEmpty());
    const QString videoId = QStringLiteral("transient-video");

    YouTubeBackend backend(QString(), dir.path());
    auto *resolver = backend.findChild<YtInnertubeResolver *>();
    QVERIFY(resolver != nullptr);
    QSignalSpy failed(&backend, &YouTubeBackend::playbackResolveFailed);

    YtResolveResult result;
    result.videoId = videoId;
    result.resolution = QStringLiteral("480p");
    result.key = yturl::cacheKeyFor(videoId, QStringLiteral("fmt"));
    result.ok = false;
    result.reason = QStringLiteral("timeout");
    result.elapsedMs = 4000;
    emit resolver->finished(result);

    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(marker), 1000);
    QCOMPARE(failed.count(), 0);
    const QJsonObject persisted = ytneg::readNegativeCache(
        dir.filePath(QStringLiteral("youtube_resolve_failures.json")),
        QDateTime::currentSecsSinceEpoch());
    QVERIFY(persisted.isEmpty());
}

void YouTubeNegativeCacheIntegrationTest::cachedFailureSkipsWarmFocusAndPressAcrossRestart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString marker = installYtdlpSentinel(dir.path());
    QVERIFY(!marker.isEmpty());
    const QString videoId = QStringLiteral("Fi2d7mN-EzU");
    const QString resolution = QStringLiteral("480p");
    seedSponsorHit(dir.path(), videoId);

    QJsonObject cache;
    QVERIFY(!ytneg::recordFailure(cache, videoId, QStringLiteral("UNPLAYABLE"),
                                  QDateTime::currentSecsSinceEpoch()).isEmpty());
    QVERIFY(ytneg::writeNegativeCache(
        dir.filePath(QStringLiteral("youtube_resolve_failures.json")), cache));

    {
        YouTubeBackend backend(QString(), dir.path());
        QSignalSpy telemetry(&backend, &YouTubeBackend::resolveTelemetry);
        QSignalSpy failed(&backend, &YouTubeBackend::playbackResolveFailed);

        backend.warmListPlayback({videoId}, resolution);
        QCOMPARE(telemetryCount(telemetry, QStringLiteral("negative-cache")), 1);

        backend.resolvePlayback(videoId, resolution); // focus warm
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 2, 250);
        backend.resolvePlayback(videoId, resolution); // press
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 3, 250);
        QCOMPARE(telemetryCount(telemetry, QStringLiteral("negative-cache")), 3);
        QCOMPARE(telemetryCount(telemetry, QStringLiteral("innertube")), 0);
        QCOMPARE(telemetryCount(telemetry, QStringLiteral("ytdlp-empty")), 0);
        QVERIFY(!QFile::exists(marker));
    }

    // A new backend has no in-memory cache: only the persisted entry can
    // explain an immediate negative-cache hit and zero resolver processes.
    YouTubeBackend restarted(QString(), dir.path());
    QSignalSpy telemetry(&restarted, &YouTubeBackend::resolveTelemetry);
    QSignalSpy failed(&restarted, &YouTubeBackend::playbackResolveFailed);
    restarted.resolvePlayback(videoId, resolution);
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 250);
    QCOMPARE(telemetryCount(telemetry, QStringLiteral("negative-cache")), 1);
    QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("UNPLAYABLE"));
    QVERIFY(!QFile::exists(marker));
}

QTEST_GUILESS_MAIN(YouTubeNegativeCacheIntegrationTest)
#include "YouTubeNegativeCacheIntegrationTest.moc"
