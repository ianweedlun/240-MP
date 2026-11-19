// T2 (issue #111): real MpvController per-instance socket contract.
//
// The controller's input-conf/log/subinfo paths are private; the socket path is
// published via socketPath(). This test verifies the contract without spawning
// mpv (CI lanes without a media stack still run it). It runs offscreen only so
// QGuiApplication exists and detectVideoProfile() can query screens.
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QTest>
#include <QDir>
#include "AppCore.h"
#include "player/MpvController.h"

#ifndef Q_OS_WIN
#include <sys/un.h>
#endif

class MpvInstanceSocketTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() {
        // Avoid any leftover global state from prior test processes; keep only
        // today's per-instance files that belong to this app pid.
        const qint64 pid = QCoreApplication::applicationPid();
        QDir tmp(QDir::temp());
        const QStringList filters {
            QStringLiteral("240mp-mpv-%1-*.sock").arg(pid),
            QStringLiteral("240mp-mpv-%1-*.log").arg(pid),
            QStringLiteral("240mp-input-%1-*.conf").arg(pid),
            QStringLiteral("240mp-mpv-%1-*.subinfo.json").arg(pid)
        };
        for (const QString &pat : filters) {
            for (const QString &f : tmp.entryList({pat}, QDir::Files))
                tmp.remove(f);
        }
    }

    // Two controllers in one process get distinct tokens (per-process seq) and
    // both names include the app pid.
    void twoControllersDistinctPaths() {
        QTemporaryDir dataA; QVERIFY(dataA.isValid());
        QTemporaryDir dataB; QVERIFY(dataB.isValid());
        AppCore core(QString::fromUtf8(MP240_REPO_ROOT), dataA.path());
        MpvController a(QString::fromUtf8(MP240_REPO_ROOT), dataA.path(), &core, nullptr);
        MpvController b(QString::fromUtf8(MP240_REPO_ROOT), dataB.path(), &core, nullptr);

        QVERIFY(!a.socketPath().isEmpty());
        QVERIFY(!b.socketPath().isEmpty());
        QVERIFY(a.socketPath() != b.socketPath());
        QVERIFY(a.logFilePath() != b.logFilePath());

        const QString pidToken = QStringLiteral("-%1-").arg(QCoreApplication::applicationPid());
        QVERIFY(a.socketPath().contains(pidToken));
        QVERIFY(b.socketPath().contains(pidToken));
        QVERIFY(a.socketPath().startsWith(QDir::tempPath()));
    }

    // Boundary test: compute a TMPDIR such that the socket is exactly at the
    // platform sun_path limit; the controller must keep IPC enabled. Increase
    // TMPDIR by one byte and IPC must be disabled (empty socket), while the mpv
    // log file remains assigned.
    void longTmpdirDisablesIpc() {
        const QByteArray oldTmpdir = qgetenv("TMPDIR");
        QTemporaryDir shortDir; QVERIFY(shortDir.isValid());
        qputenv("TMPDIR", shortDir.path().toUtf8());
        {
            // First, measure the suffix length with a short TMPDIR for THIS
            // controller. We then derive a fit/over directory to hit the
            // limit exactly and exceed it by one byte.
            AppCore core(QString::fromUtf8(MP240_REPO_ROOT), shortDir.path());
            MpvController probe(QString::fromUtf8(MP240_REPO_ROOT), shortDir.path(), &core, nullptr);
            const QString probeSocket = probe.socketPath();
            const int suffixLen = probeSocket.toUtf8().size() - shortDir.path().toUtf8().size();

            const int limit = static_cast<int>(sizeof(sockaddr_un::sun_path)) - 1;
            const int padding = limit - suffixLen - 4 - shortDir.path().size(); // -4 for "/fit"
            const QString fitDir = shortDir.path() + QStringLiteral("/fit")
                                   + QString(qMax(0, padding), QLatin1Char('p'));
            QDir().mkpath(fitDir);
            QVERIFY(fitDir.toUtf8().size() + suffixLen == limit);

            qputenv("TMPDIR", fitDir.toUtf8());
            AppCore core2(QString::fromUtf8(MP240_REPO_ROOT), fitDir);
            MpvController fit(QString::fromUtf8(MP240_REPO_ROOT), fitDir, &core2, nullptr);
            QCOMPARE(fit.socketPath().toUtf8().size(), limit);
            QVERIFY(!fit.socketPath().isEmpty());

            // One byte over.
            const QString overDir = fitDir + QStringLiteral("p");
            QDir().mkpath(overDir);
            qputenv("TMPDIR", overDir.toUtf8());
            AppCore core3(QString::fromUtf8(MP240_REPO_ROOT), overDir);
            MpvController over(QString::fromUtf8(MP240_REPO_ROOT), overDir, &core3, nullptr);
            QVERIFY(over.socketPath().isEmpty());
            QVERIFY(!over.logFilePath().isEmpty());
            QVERIFY(over.logFilePath().startsWith(overDir));
        }
        if (!oldTmpdir.isEmpty()) qputenv("TMPDIR", oldTmpdir);
        else                      qunsetenv("TMPDIR");

        // After restoration a fresh controller gets a usable socket again.
        AppCore core4(QString::fromUtf8(MP240_REPO_ROOT), shortDir.path());
        MpvController restored(QString::fromUtf8(MP240_REPO_ROOT), shortDir.path(), &core4, nullptr);
        QVERIFY(!restored.socketPath().isEmpty());
    }

    // The documented RG-3 padded-TMPDIR arm: a private temp directory padded
    // to >100 chars yields an unusable socket. Pre-fix the controller used the
    // fixed path unguarded; post-fix it disables IPC cleanly.
    void knownBadPaddedTmpdirDisablesIpc() {
        const QByteArray oldTmpdir = qgetenv("TMPDIR");
        QTemporaryDir base; QVERIFY(base.isValid());
        const QString padded = base.path() + QStringLiteral("/private-tmp-")
                             + QString(60, QLatin1Char('p'));
        QDir().mkpath(padded);
        qputenv("TMPDIR", padded.toUtf8());
        {
            QTemporaryDir data; QVERIFY(data.isValid());
            AppCore core(QString::fromUtf8(MP240_REPO_ROOT), data.path());
            MpvController mpv(QString::fromUtf8(MP240_REPO_ROOT), data.path(), &core, nullptr);
            QVERIFY(mpv.socketPath().isEmpty());
            QVERIFY(!mpv.logFilePath().isEmpty());
            QVERIFY(mpv.logFilePath().startsWith(padded));
        }
        if (!oldTmpdir.isEmpty()) qputenv("TMPDIR", oldTmpdir);
        else                      qunsetenv("TMPDIR");
    }
};

QTEST_MAIN(MpvInstanceSocketTest)
#include "MpvInstanceSocketTest.moc"
