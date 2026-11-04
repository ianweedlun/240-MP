#include <QtTest/QtTest>

#include "../YtAuthGate.h"

// Pure clock math for the OAuth access-token expiry gate: inside the slack
// band counts as expired (refresh-first semantics), unknown TTL never expires.
// No processes, no network.

class YtAuthGateTest : public QObject {
    Q_OBJECT

private slots:
    void slackBand()
    {
        const qint64 exp = 1000000;
        // exactly at the slack edge: expired (>= boundary).
        QVERIFY(ytauth::accessExpired(exp, exp - ytauth::kAuthSlackS));
        // one second inside the band: expired.
        QVERIFY(ytauth::accessExpired(exp, exp - ytauth::kAuthSlackS + 1));
        // one second outside the band: still alive.
        QVERIFY(!ytauth::accessExpired(exp, exp - ytauth::kAuthSlackS - 1));
    }

    void unknownTtlNeverExpires()
    {
        QVERIFY(!ytauth::accessExpired(0, 12345));  // legacy "no expiry" docs
        QVERIFY(!ytauth::accessExpired(-5, 12345));
    }

    void pastVsFuture()
    {
        QVERIFY(!ytauth::accessExpired(2000000, 1000000)); // still ahead
        QVERIFY(ytauth::accessExpired(1000000, 2000000));  // long past
    }
};

QTEST_MAIN(YtAuthGateTest)
#include "YtAuthGateTest.moc"
