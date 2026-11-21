#include <QtTest/QtTest>
#include <QTemporaryDir>

#include "../YtNegativeCache.h"

// Behavior tests for durable permanent YouTube resolve failures. The cache is
// keyed by video id (not resolution), only evidenced permanent classes stick,
// and a restart reads the same on-disk format the running backend writes.
class YtNegativeCacheTest : public QObject {
    Q_OBJECT

private slots:
    void permanentFailuresAreCached();
    void transientFailuresAreNotCached();
    void ttlExpiryAllowsFreshAttempt();
    void persistenceRoundTrip();
};

void YtNegativeCacheTest::permanentFailuresAreCached()
{
    const qint64 now = 1800000000;
    QJsonObject cache;

    QCOMPARE(ytneg::recordFailure(cache, QStringLiteral("Fi2d7mN-EzU"),
                                  QStringLiteral("exception:LOGIN_REQUIRED: Sign in"), now),
             QStringLiteral("LOGIN_REQUIRED"));
    QCOMPARE(ytneg::freshFailureClass(cache, QStringLiteral("Fi2d7mN-EzU"), now + 1),
             QStringLiteral("LOGIN_REQUIRED"));

    QCOMPARE(ytneg::recordFailure(cache, QStringLiteral("-zphhfHon_I"),
                                  QStringLiteral("exception:UNPLAYABLE: claimed content"), now),
             QStringLiteral("UNPLAYABLE"));
    QCOMPARE(ytneg::freshFailureClass(cache, QStringLiteral("-zphhfHon_I"), now + 1),
             QStringLiteral("UNPLAYABLE"));
}

void YtNegativeCacheTest::transientFailuresAreNotCached()
{
    const qint64 now = 1800000000;
    const QStringList transient{
        QStringLiteral("timeout"),
        QStringLiteral("exception:network unreachable"),
        QStringLiteral("exception:HTTP 503"),
        QStringLiteral("no-playable-pair"),
        QStringLiteral("no-expiry"),
    };

    for (const QString &reason : transient) {
        QJsonObject cache;
        QVERIFY(ytneg::recordFailure(cache, QStringLiteral("video-id"), reason, now).isEmpty());
        QVERIFY(cache.isEmpty());
        QVERIFY(ytneg::freshFailureClass(cache, QStringLiteral("video-id"), now).isEmpty());
    }
}

void YtNegativeCacheTest::ttlExpiryAllowsFreshAttempt()
{
    const qint64 now = 1800000000;
    QJsonObject cache;
    QVERIFY(!ytneg::recordFailure(cache, QStringLiteral("Fi2d7mN-EzU"),
                                  QStringLiteral("LOGIN_REQUIRED"), now).isEmpty());

    QVERIFY(!ytneg::freshFailureClass(cache, QStringLiteral("Fi2d7mN-EzU"),
                                      now + ytneg::kNegativeCacheTtlS - 1).isEmpty());
    QVERIFY(ytneg::freshFailureClass(cache, QStringLiteral("Fi2d7mN-EzU"),
                                     now + ytneg::kNegativeCacheTtlS).isEmpty());

    ytneg::pruneNegativeCache(cache, now + ytneg::kNegativeCacheTtlS);
    QVERIFY(cache.isEmpty());
}

void YtNegativeCacheTest::persistenceRoundTrip()
{
    const qint64 now = 1800000000;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("youtube_resolve_failures.json"));

    QJsonObject beforeRestart;
    QVERIFY(!ytneg::recordFailure(beforeRestart, QStringLiteral("Fi2d7mN-EzU"),
                                  QStringLiteral("LOGIN_REQUIRED"), now).isEmpty());
    QVERIFY(ytneg::writeNegativeCache(path, beforeRestart));

    const QJsonObject afterRestart = ytneg::readNegativeCache(path, now + 60);
    QCOMPARE(ytneg::freshFailureClass(afterRestart, QStringLiteral("Fi2d7mN-EzU"), now + 60),
             QStringLiteral("LOGIN_REQUIRED"));

    const QJsonObject afterTtl = ytneg::readNegativeCache(
        path, now + ytneg::kNegativeCacheTtlS);
    QVERIFY(ytneg::freshFailureClass(afterTtl, QStringLiteral("Fi2d7mN-EzU"),
                                     now + ytneg::kNegativeCacheTtlS).isEmpty());
    QVERIFY(afterTtl.isEmpty());
}

QTEST_MAIN(YtNegativeCacheTest)
#include "YtNegativeCacheTest.moc"
