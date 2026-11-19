// T2 (issue #111): per-instance mpv temp paths — pure-logic contract tests.
//
// Uniqueness across instances (pid and per-process seq) and the sockaddr_un
// sun_path bound: an over-limit socket path must disable IPC (empty socket,
// ipcUsable=false, rejected path recorded) — never silently truncate or
// relocate. The known-bad padded-TMPDIR case mirrors the RG-3 regression arm
// and FAILS pre-fix (the old ctor used the over-limit fixed path unguarded).
// Regular files (log/input-conf/subinfo) carry no sun_path bound.
#include <QTest>
#include <QString>
#include <QByteArray>

#include "util/MpvInstancePaths.h"

#ifndef Q_OS_WIN
#include <sys/un.h>
#endif

class MpvInstancePathsTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void platformLimit() {
#ifndef Q_OS_WIN
        QCOMPARE(mpvipc::sunPathLimit(),
                 static_cast<int>(sizeof(sockaddr_un::sun_path)) - 1);
#ifdef Q_OS_MACOS
        QCOMPARE(mpvipc::sunPathLimit(), 103);
#else
        QCOMPARE(mpvipc::sunPathLimit(), 107);
#endif
#endif
        QVERIFY(mpvipc::sunPathLimit() > 0);
    }

    void uniquenessAcrossInstances() {
        const QString dir = QStringLiteral("/tmp");
        const auto a = mpvipc::makeInstancePaths(dir, 1001, 1);
        const auto b = mpvipc::makeInstancePaths(dir, 1002, 1);   // other process
        const auto c = mpvipc::makeInstancePaths(dir, 1001, 2);   // same process, 2nd controller
        QVERIFY(a.ipcUsable && b.ipcUsable && c.ipcUsable);
        QVERIFY(a.socket != b.socket);
        QVERIFY(a.socket != c.socket);
        QVERIFY(a.log != b.log);
        QVERIFY(a.log != c.log);
        QVERIFY(a.inputConf != b.inputConf);
        QVERIFY(a.inputConf != c.inputConf);
        QVERIFY(a.subInfo != b.subInfo);
        QVERIFY(a.subInfo != c.subInfo);
        // The instance token is discoverable in every name.
        QVERIFY(a.socket.contains(QStringLiteral("1001-1")));
        QVERIFY(a.log.contains(QStringLiteral("1001-1")));
        QVERIFY(a.inputConf.contains(QStringLiteral("1001-1")));
        QVERIFY(a.subInfo.contains(QStringLiteral("1001-1")));
        QVERIFY(a.socket.startsWith(dir + QStringLiteral("/240mp-mpv-")));
        QVERIFY(a.socket.endsWith(QStringLiteral(".sock")));
    }

    // Socket byte length exactly at the limit -> usable; one byte over ->
    // IPC disabled with the rejected path recorded. Regular files stay set.
    void sunPathBoundary() {
        const qint64 pid = 1234567;   // 7 digits: worst realistic pid width
        const int seq = 1;
        const QString suffix = QStringLiteral("/240mp-mpv-%1-%2.sock").arg(pid).arg(seq);
        const int limit = mpvipc::sunPathLimit();

        const QString fitDir = QString(limit - suffix.size(), QLatin1Char('p'));
        const auto fit = mpvipc::makeInstancePaths(fitDir, pid, seq);
        QCOMPARE(fit.socket.toUtf8().size(), limit);
        QVERIFY(fit.ipcUsable);
        QVERIFY(!fit.socket.isEmpty());

        const QString overDir = fitDir + QStringLiteral("p");
        const auto over = mpvipc::makeInstancePaths(overDir, pid, seq);
        QCOMPARE(over.socket.toUtf8().size(), 0);
        QVERIFY(!over.ipcUsable);
        QCOMPARE(over.rejectedSocket.toUtf8().size(), limit + 1);
        QCOMPARE(over.rejectedLen, limit + 1);
        // Regular files are NOT bound by sun_path — always per-instance.
        QVERIFY(!over.log.isEmpty());
        QVERIFY(!over.inputConf.isEmpty());
        QVERIFY(!over.subInfo.isEmpty());
        QVERIFY(over.log.startsWith(overDir));
    }

    // The RG-3 regression arm's known-bad case: a padded private TMPDIR pushes
    // the socket past 107 usable bytes. Pre-fix the ctor used that path
    // unguarded (mpv: 'Could not create IPC socket'); post-fix IPC is disabled
    // explicitly and the rejected path is diagnosable.
    void knownBadPaddedTmpdir() {
        const QString padded = QStringLiteral("/home/ian/.hermes/cache/scratch/private-tmp-")
                             + QString(60, QLatin1Char('p'));
        const auto p = mpvipc::makeInstancePaths(padded, 4242, 1);
        QVERIFY(p.rejectedSocket.toUtf8().size() > 107);
        QVERIFY(!p.ipcUsable);
        QVERIFY(p.socket.isEmpty());
        QVERIFY(!p.log.isEmpty());   // mpv still logs; quit still works via forceStop
    }

    // The bound is UTF-8 BYTES, not QChar count: a multi-byte temp dir that
    // fits by character count must still be rejected when its bytes overflow.
    void boundCountsUtf8Bytes() {
        const int limit = mpvipc::sunPathLimit();
        const QString suffix = QStringLiteral("/240mp-mpv-7-1.sock");
        // Each 'é' is 2 UTF-8 bytes. Pick a char count that fits as chars but
        // not as bytes: dirChars = limit - suffix + 1 (one over by chars),
        // all multi-byte -> bytes = 2 * dirChars, far over the limit.
        const int dirChars = limit - suffix.size() + 1;
        const QString dir = QString(dirChars, QChar(0x00E9));
        QVERIFY(dir.size() <= limit);                       // fits by QChar count...
        QVERIFY(dir.toUtf8().size() > limit);               // ...not by bytes
        const auto p = mpvipc::makeInstancePaths(dir, 7, 1);
        QVERIFY(!p.ipcUsable);
        QVERIFY(p.socket.isEmpty());
        QCOMPARE(p.rejectedLen, dir.toUtf8().size() + suffix.toUtf8().size());
    }
};

QTEST_MAIN(MpvInstancePathsTest)
#include "MpvInstancePathsTest.moc"
