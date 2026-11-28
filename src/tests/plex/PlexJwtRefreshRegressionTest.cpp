#include "PlexAuthFixture.h"
#include "modules/plex/PlexBackend.h"

#include <QtTest>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QTemporaryDir>

// PLEX-JWT-REFRESH-LATCH-01 product regression suite.
//
// Built twice: default (product source, fixed) is GREEN; -DPLEXJWT_SOURCE_ROOT
// builds the same cases against a pinned pre-fix tree and must show the latch.
// Assertions are behavioral (guard, callbacks, counters, saved auth), never
// source text. Watchdog 300 ms vs 1200 ms stall window gives a 4x margin.
class PlexJwtRefreshRegressionTest : public QObject {
    Q_OBJECT

private slots:
    void controlHealthyRefresh();
    void controlNonce4xx();
    void controlToken4xx();
    void controlEmptyNonce();
    void controlEmptyAuthToken();
    void controlMissingPrivateKey();
    void controlConcurrentSingleFlight();
    void control498RetryAfterSuccess();
    void controlGlobalTimeoutUnchanged();

    void trickleNonceHardDeadline();
    void trickleTokenHardDeadline();
    void abortNoDoubleCallback();
    void concurrent498SingleFlight();
    void endToEndRefreshAfterTimeout();
    void repeatedStallNoPermanentLatch();
    void authRevokedAfterFailedRecovery();
    void startupStalledThenRecovered();
    void resources401EmitsAuthRevoked();
    void noRetryStackingDuringStall();
};

// ---------------------------------------------------------------------------

static void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

static QByteArray makeTokenBody(QString *tokenOut) {
    auto b64 = [](const QByteArray &b) {
        return b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    };
    QJsonObject header{{QStringLiteral("alg"), QStringLiteral("EdDSA")}};
    QJsonObject payload;
    payload[QStringLiteral("exp")] = 4102444800.0; // 2100, safely in the future
    payload[QStringLiteral("user")] = QJsonObject{{QStringLiteral("id"), 7}};
    const QByteArray token =
        b64(QJsonDocument(header).toJson(QJsonDocument::Compact)) + "." +
        b64(QJsonDocument(payload).toJson(QJsonDocument::Compact)) + ".sig";
    if (tokenOut) *tokenOut = QString::fromLatin1(token);
    return QByteArray("{\"auth_token\":\"") + token + "\"}";
}

struct Rig {
    QTemporaryDir dir;
    PlexAuthFixture fx;
    PlexBackend be;
    QString expectedToken;
    int cb = 0;
    bool lastOk = false;
    bool guardSeenInCallback = false;
    int retry = 0;
    int revoked = 0;
    int startup = 0;

    explicit Rig(bool withKey = true) : be(QString(), dir.path()) {
        if (withKey) be.generateAndSaveKeyPair(QStringLiteral("regression-kid"));
        fx.tokenBody = makeTokenBody(&expectedToken);
        QVERIFY2(QFile::exists(dir.path() + "/plex_key.pem") || !withKey,
                 "fixture key file missing");
        QFile auth(dir.path() + "/plex_auth.json");
        if (auth.open(QIODevice::WriteOnly)) {
            QJsonObject a;
            a[QStringLiteral("auth_token")] = QStringLiteral("expired-token");
            a[QStringLiteral("jwt_exp")] = 1;
            a[QStringLiteral("jwt_key_id")] = QStringLiteral("regression-kid");
            auth.write(QJsonDocument(a).toJson(QJsonDocument::Compact));
            auth.close();
        }
        QVERIFY2(fx.start(), "loopback fixture failed to listen");
        PlexBackend::setPlexTvUrlForTests(
            QStringLiteral("http://127.0.0.1:%1").arg(fx.port()));
        be.setAuthWatchdogMsForTests(300);
        QObject::connect(&be, &PlexBackend::authRevoked, [this]() { ++revoked; });
    }

    std::function<void(bool)> recorder() {
        return [this](bool ok) {
            ++cb;
            lastOk = ok;
            guardSeenInCallback |= be.m_refreshInFlight;
        };
    }

    ~Rig() {
        const auto replies = be.findChildren<QNetworkReply *>();
        for (QNetworkReply *r : replies) r->abort();
        spin(60);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
};

// --------------------------- controls ---------------------------

void PlexJwtRefreshRegressionTest::controlHealthyRefresh() {
    Rig r;
    r.fx.setMode(PlexAuthFixture::Healthy);
    r.be.refreshJwt(r.recorder());
    spin(700);
    QCOMPARE(r.cb, 1);
    QVERIFY(r.lastOk);
    QVERIFY(!r.be.m_refreshInFlight);
    QVERIFY(!r.guardSeenInCallback);
    QCOMPARE(r.fx.nonceGets, 1);
    QCOMPARE(r.fx.tokenPosts, 1);
    QCOMPARE(r.be.loadAuth()[QStringLiteral("auth_token")].toString(), r.expectedToken);
}

void PlexJwtRefreshRegressionTest::controlNonce4xx() {
    Rig r;
    r.fx.nonceStatus = 404;
    r.be.refreshJwt(r.recorder());
    spin(700);
    QCOMPARE(r.cb, 1);
    QVERIFY(!r.lastOk);
    QVERIFY(!r.be.m_refreshInFlight);
    QCOMPARE(r.fx.nonceGets, 1);
    QCOMPARE(r.fx.tokenPosts, 0);
}

void PlexJwtRefreshRegressionTest::controlToken4xx() {
    Rig r;
    r.fx.tokenStatus = 500;
    r.be.refreshJwt(r.recorder());
    spin(700);
    QCOMPARE(r.cb, 1);
    QVERIFY(!r.lastOk);
    QVERIFY(!r.be.m_refreshInFlight);
    QCOMPARE(r.fx.tokenPosts, 1);
}

void PlexJwtRefreshRegressionTest::controlEmptyNonce() {
    Rig r;
    r.fx.nonceBody = R"({"nonce":""})";
    r.be.refreshJwt(r.recorder());
    spin(700);
    QCOMPARE(r.cb, 1);
    QVERIFY(!r.lastOk);
    QVERIFY(!r.be.m_refreshInFlight);
    QCOMPARE(r.fx.tokenPosts, 0);
}

void PlexJwtRefreshRegressionTest::controlEmptyAuthToken() {
    Rig r;
    r.fx.tokenBody = R"({"auth_token":""})";
    r.be.refreshJwt(r.recorder());
    spin(700);
    QCOMPARE(r.cb, 1);
    QVERIFY(!r.lastOk);
    QVERIFY(!r.be.m_refreshInFlight);
    QCOMPARE(r.fx.tokenPosts, 1);
}

void PlexJwtRefreshRegressionTest::controlMissingPrivateKey() {
    Rig r(false);
    r.be.refreshJwt(r.recorder());
    spin(700);
    QCOMPARE(r.cb, 1);
    QVERIFY(!r.lastOk);
    QVERIFY(!r.be.m_refreshInFlight);
    QCOMPARE(r.fx.tokenPosts, 0);
}

void PlexJwtRefreshRegressionTest::controlConcurrentSingleFlight() {
    Rig r;
    r.be.refreshJwt(r.recorder());
    int second = 0;
    r.be.refreshJwt([&second](bool) { ++second; });
    QCOMPARE(second, 1); // immediate false, guard untouched
    QVERIFY(r.be.m_refreshInFlight);
    spin(60);
    QCOMPARE(r.fx.nonceGets, 1);
    spin(700);
    QCOMPARE(r.cb, 1);
    QVERIFY(r.lastOk);
    QVERIFY(!r.be.m_refreshInFlight);
    QCOMPARE(r.fx.nonceGets, 1);
}

void PlexJwtRefreshRegressionTest::control498RetryAfterSuccess() {
    Rig r;
    r.be.handle498([&r]() { ++r.retry; });
    spin(700);
    QCOMPARE(r.retry, 1);
    QCOMPARE(r.revoked, 0);
    QVERIFY(!r.be.m_refreshInFlight);
    QCOMPARE(r.fx.nonceGets, 1);
}

void PlexJwtRefreshRegressionTest::controlGlobalTimeoutUnchanged() {
    Rig r;
    const QNetworkRequest req = r.be.plexRequest(QUrl(QStringLiteral("http://example.test")), {});
    QCOMPARE(req.transferTimeout(), 5000); // kPlexTransferTimeoutMs unchanged
}

// --------------------------- trickle / watchdog ---------------------------

void PlexJwtRefreshRegressionTest::trickleNonceHardDeadline() {
    Rig r;
    r.fx.setMode(PlexAuthFixture::TrickleNonce);
    r.be.refreshJwt(r.recorder());
    spin(1200);
    QCOMPARE(r.cb, 1);                 // hard deadline fired, not the 5 s timer
    QVERIFY(!r.lastOk);
    QVERIFY(!r.be.m_refreshInFlight);  // no permanent latch
    QVERIFY(!r.guardSeenInCallback);   // guard cleared before callback
    QCOMPARE(r.fx.nonceGets, 1);

    int second = 0;
    r.be.refreshJwt([&second](bool) { ++second; });
    QCOMPARE(second, 0); // accepted; no immediate false
    QVERIFY(r.be.m_refreshInFlight);
    spin(1200);
    QCOMPARE(r.fx.nonceGets, 2); // a fresh nonce GET after the timeout
    QCOMPARE(second, 1);         // and exactly one false callback

    r.fx.setMode(PlexAuthFixture::Healthy);
    r.be.handle498([&r]() { ++r.retry; });
    spin(700);
    QCOMPARE(r.retry, 1);
}

void PlexJwtRefreshRegressionTest::trickleTokenHardDeadline() {
    Rig r;
    r.fx.setMode(PlexAuthFixture::TrickleToken);
    r.be.refreshJwt(r.recorder());
    spin(1200);
    QCOMPARE(r.cb, 1);
    QVERIFY(!r.lastOk);
    QVERIFY(!r.be.m_refreshInFlight);
    QCOMPARE(r.fx.nonceGets, 1);
    QCOMPARE(r.fx.tokenPosts, 1);

    r.fx.setMode(PlexAuthFixture::Healthy);
    r.be.handle498([&r]() { ++r.retry; });
    spin(700);
    QCOMPARE(r.retry, 1);
}

void PlexJwtRefreshRegressionTest::abortNoDoubleCallback() {
    Rig r;
    r.fx.setMode(PlexAuthFixture::TrickleNonce);
    const QString before = r.be.loadAuth()[QStringLiteral("auth_token")].toString();
    r.be.refreshJwt(r.recorder());
    spin(1200);
    QCOMPARE(r.cb, 1);
    spin(400); // the aborted reply's finished signal must not call back again
    QCOMPARE(r.cb, 1);
    QCOMPARE(r.be.loadAuth()[QStringLiteral("auth_token")].toString(), before);
    QVERIFY(!r.be.m_refreshInFlight);
}

void PlexJwtRefreshRegressionTest::concurrent498SingleFlight() {
    Rig r;
    r.fx.setMode(PlexAuthFixture::TrickleNonce);
    r.be.handle498([&r]() { ++r.retry; });
    r.be.handle498([&r]() { ++r.retry; });
    spin(60); // let the first request reach the fixture before counting
    QCOMPARE(r.fx.nonceGets, 1); // second 498 did not start another exchange
    spin(1200);
    QCOMPARE(r.retry, 0);
    QCOMPARE(r.revoked, 1); // the first 498 saw the timeout via authRevoked
    r.fx.setMode(PlexAuthFixture::Healthy);
    r.be.handle498([&r]() { ++r.retry; });
    spin(700);
    QCOMPARE(r.retry, 1); // exactly one retry after recovery
}

void PlexJwtRefreshRegressionTest::endToEndRefreshAfterTimeout() {
    Rig r;
    r.fx.setMode(PlexAuthFixture::TrickleNonce);
    r.be.refreshJwt(r.recorder());
    spin(1200);
    QCOMPARE(r.cb, 1);
    QVERIFY(!r.lastOk);
    QVERIFY(!r.be.m_refreshInFlight);

    r.fx.setMode(PlexAuthFixture::Healthy);
    int ok = 0;
    r.be.refreshJwt([&](bool good) { ++ok; r.lastOk = good; });
    spin(700);
    QCOMPARE(ok, 1);
    QVERIFY(r.lastOk);
    QCOMPARE(r.fx.nonceGets, 2);
    QCOMPARE(r.fx.tokenPosts, 1);
    QCOMPARE(r.be.loadAuth()[QStringLiteral("auth_token")].toString(), r.expectedToken);
    QVERIFY(!r.be.m_refreshInFlight);
}

void PlexJwtRefreshRegressionTest::repeatedStallNoPermanentLatch() {
    Rig r;
    r.fx.setMode(PlexAuthFixture::TrickleNonce);
    for (int i = 0; i < 2; ++i) {
        r.be.refreshJwt(r.recorder());
        spin(1200);
        QVERIFY(!r.be.m_refreshInFlight);
    }
    QCOMPARE(r.cb, 2);
    QCOMPARE(r.fx.nonceGets, 2);
    r.fx.setMode(PlexAuthFixture::Healthy);
    r.be.refreshJwt(r.recorder());
    spin(700);
    QCOMPARE(r.cb, 3);
    QVERIFY(r.lastOk);
    QCOMPARE(r.fx.nonceGets, 3);
}

void PlexJwtRefreshRegressionTest::authRevokedAfterFailedRecovery() {
    Rig r;
    r.fx.setMode(PlexAuthFixture::TrickleNonce);
    r.be.refreshJwt(r.recorder());
    spin(1200);
    QCOMPARE(r.cb, 1);
    QVERIFY(!r.be.m_refreshInFlight);

    r.fx.setMode(PlexAuthFixture::Healthy);
    r.fx.nonceStatus = 404;
    r.be.handle498([&r]() { ++r.retry; });
    spin(700);
    QCOMPARE(r.retry, 0);
    QCOMPARE(r.revoked, 1);
    QVERIFY(!r.be.m_refreshInFlight);
}

void PlexJwtRefreshRegressionTest::startupStalledThenRecovered() {
    Rig r;
    r.fx.setMode(PlexAuthFixture::TrickleNonce);
    r.be.checkAndRefreshOnStartup([&r]() { ++r.startup; });
    spin(1200);
    QCOMPARE(r.startup, 1);
    QCOMPARE(r.fx.resourceGets, 1); // proceed() ran after the timed-out refresh
    QCOMPARE(r.revoked, 0);
    QVERIFY(!r.be.m_refreshInFlight);
}

void PlexJwtRefreshRegressionTest::resources401EmitsAuthRevoked() {
    Rig r;
    r.fx.resourceStatus = 401;
    r.be.checkAndRefreshOnStartup([&r]() { ++r.startup; });
    spin(700);
    QCOMPARE(r.revoked, 1);
    QCOMPARE(r.startup, 0);
    QCOMPARE(r.fx.resourceGets, 1);
}

void PlexJwtRefreshRegressionTest::noRetryStackingDuringStall() {
    Rig r;
    r.fx.setMode(PlexAuthFixture::TrickleToken);
    for (int i = 0; i < 3; ++i)
        r.be.handle498([&r]() { ++r.retry; });
    spin(1200);
    QCOMPARE(r.fx.nonceGets, 1);
    QCOMPARE(r.fx.tokenPosts, 1);
    QCOMPARE(r.retry, 0);
    r.fx.setMode(PlexAuthFixture::Healthy);
    r.be.handle498([&r]() { ++r.retry; });
    spin(700);
    QCOMPARE(r.retry, 1);
}

QTEST_GUILESS_MAIN(PlexJwtRefreshRegressionTest)
#include "PlexJwtRefreshRegressionTest.moc"
