// T2 (issue #111): real MpvController per-instance socket contract.
//
// The controller's input-conf/log/subinfo paths are private; the socket path is
// published via socketPath(). This test verifies the contract without spawning
// mpv (CI lanes without a media stack still run it). It runs offscreen only so
// QGuiApplication exists and detectVideoProfile() can query screens.
//
// Boundary arms use a SHORT base dir (/tmp/240mp-t2-XXXXXX): the default
// QTemporaryDir location on macOS (/var/folders/.../T/<test>-XXXXXX) is already
// ~84 bytes — near/over the 103-byte macOS sun_path limit — so a probe
// controller there can have IPC disabled, and boundary padding must not be
// derived from an empty probe socket.
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QTest>
#include <QDir>
#include <QFileInfo>
#include "AppCore.h"
#include "player/MpvController.h"

#ifndef Q_OS_WIN
#include <sys/un.h>
#endif

namespace {
// Restore TMPDIR even when an assertion aborts the slot mid-test: a leaked
// padded TMPDIR poisons every later QTemporaryDir in the process (the padded
// dir may also be auto-removed with its parent, leaving TMPDIR dangling).
struct TmpdirGuard {
    QByteArray old = qgetenv("TMPDIR");
    ~TmpdirGuard() {
        if (!old.isEmpty()) qputenv("TMPDIR", old);
        else                qunsetenv("TMPDIR");
    }
};
} // namespace

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
        TmpdirGuard guard;
        // Short, platform-independent base: the default temp location on macOS
        // is too long for the probe controller to have IPC enabled at all.
        QTemporaryDir shortDir(QStringLiteral("/tmp/240mp-t2-XXXXXX"));
        QVERIFY(shortDir.isValid());
        qputenv("TMPDIR", shortDir.path().toUtf8());
        {
            // First, measure the suffix length with a short TMPDIR for THIS
            // controller. We then derive a fit/over directory to hit the
            // limit exactly and exceed it by one byte.
            AppCore core(QString::fromUtf8(MP240_REPO_ROOT), shortDir.path());
            MpvController probe(QString::fromUtf8(MP240_REPO_ROOT), shortDir.path(), &core, nullptr);
            const QString probeSocket = probe.socketPath();
            // Guard the measurement: an empty probe socket (base already over
            // the limit) would yield a negative suffix and bogus padding.
            QVERIFY(!probeSocket.isEmpty());
            const int suffixLen = probeSocket.toUtf8().size() - shortDir.path().toUtf8().size();

            const int limit = static_cast<int>(sizeof(sockaddr_un::sun_path)) - 1;
            const int padding = limit - suffixLen - 4 - shortDir.path().toUtf8().size(); // -4 for "/fit"
            QVERIFY(padding >= 0);
            const QString fitDir = shortDir.path() + QStringLiteral("/fit")
                                   + QString(padding, QLatin1Char('p'));
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
            // macOS QDir::tempPath() resolves /tmp -> /private/tmp: compare
            // against the canonical effective temp dir, not the raw env string.
            QCOMPARE(QDir::tempPath(), QFileInfo(overDir).canonicalFilePath());
            QVERIFY(over.logFilePath().startsWith(QDir::tempPath()));
        }
        // The guard restores the original TMPDIR at function exit. Prove the
        // disabled state is per-instance, not sticky: a fresh controller under
        // the known-short base gets a usable socket again.
        qputenv("TMPDIR", shortDir.path().toUtf8());
        AppCore core4(QString::fromUtf8(MP240_REPO_ROOT), shortDir.path());
        MpvController restored(QString::fromUtf8(MP240_REPO_ROOT), shortDir.path(), &core4, nullptr);
        QVERIFY(!restored.socketPath().isEmpty());
    }

    // The documented RG-3 padded-TMPDIR arm: a private temp directory padded
    // to >100 chars yields an unusable socket. Pre-fix the controller used the
    // fixed path unguarded; post-fix it disables IPC cleanly.
    void knownBadPaddedTmpdirDisablesIpc() {
        TmpdirGuard guard;
        // Short base keeps the padded length deterministic across platforms
        // (macOS' default temp base alone is ~84 bytes).
        QTemporaryDir base(QStringLiteral("/tmp/240mp-t2-XXXXXX"));
        QVERIFY(base.isValid());
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
            QCOMPARE(QDir::tempPath(), QFileInfo(padded).canonicalFilePath());
            QVERIFY(mpv.logFilePath().startsWith(QDir::tempPath()));
        }
    }
};

QTEST_MAIN(MpvInstanceSocketTest)
#include "MpvInstanceSocketTest.moc"