// Permanent gate for the weather music IPC connect sequence. The old loop
// retried a missing mpv socket every 100 ms for the life of the process; these
// cases drive the real WeatherBackend against a fake mpv that stays alive but
// never listens, so the bound, backoff, one-shot failure signal and clean
// re-arm are checked as behavior rather than as source constants.
#include <QtTest/QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDevice>
#include <QLocalServer>
#include <QSignalSpy>
#include <QThread>

#include "modules/weather/WeatherBackend.h"

namespace {

template <class Predicate>
bool waitFor(Predicate predicate, int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    return predicate();
}

void pump(int ms) { waitFor([] { return false; }, ms); }

// A QVERIFY returns from the slot immediately; stop the fake mpv on the way
// out so a failed case cannot leave a process polling into the next case.
struct BackendGuard {
    WeatherBackend &backend;
    ~BackendGuard() {
        backend.stopMusic();
        // stopMusic() clears its process pointer immediately and schedules the
        // QProcess deleteLater(); let its 300 ms kill timer run, then flush the
        // deferred delete or QProcess warns about a live child at destruction.
        pump(400);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
};

} // namespace

class MusicIpcRetryTest : public QObject
{
    Q_OBJECT

private:
    QString m_root;
    QString m_dataRoot;
    QString m_socketPath;

    void writeFile(const QString &path, const QByteArray &bytes)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(bytes);
        file.close();
    }

    static QStringList failureReasons()
    {
        return QStringList{QStringLiteral("refused"), QStringLiteral("no_server"),
                           QStringLiteral("access"), QStringLiteral("timeout"),
                           QStringLiteral("socket"), QStringLiteral("unknown")};
    }

private slots:
    void initTestCase()
    {
        // The product derives the mpv IPC path from QDir::tempPath(). Give each
        // run its own temp root so this case cannot collide with another ctest
        // process or a leftover socket from a previous run.
        m_root = QStringLiteral("/tmp/240mp-wxretry-%1")
                     .arg(QCoreApplication::applicationPid());
        QVERIFY(QDir(m_root).removeRecursively() || !QDir(m_root).exists());
        QVERIFY(QDir().mkpath(m_root));
        qputenv("TMPDIR", m_root.toLocal8Bit());
        m_socketPath = QDir::tempPath() + QStringLiteral("/240mp-weather-music.sock");

        const QString binDir = m_root + QStringLiteral("/bin");
        QVERIFY(QDir().mkpath(binDir));
        const QString fakeMpv = binDir + QStringLiteral("/mpv");
        writeFile(fakeMpv, "#!/bin/sh\nexec sleep 30\n");
        QVERIFY(QFile::setPermissions(
            fakeMpv, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                         | QFileDevice::ExeOwner));
        qputenv("PATH", (binDir + QStringLiteral(":")
                         + QString::fromLocal8Bit(qgetenv("PATH"))).toLocal8Bit());

        m_dataRoot = m_root + QStringLiteral("/data");
        QVERIFY(QDir().mkpath(m_dataRoot));
        writeFile(m_dataRoot + QStringLiteral("/config.json"),
                  "{\"modules\":{\"com.240mp.weather\":{\"music\":true,"
                  "\"units\":\"US\"}}}");
        writeFile(m_dataRoot + QStringLiteral("/weather_music.txt"),
                  "track1.mp3\n");
    }

    void cleanupTestCase() { QDir(m_root).removeRecursively(); }

    // A peer that never listens must exhaust the bound, emit exactly one
    // failure, keep playing audio, and stop polling even if a listener shows
    // up later. The 2 s floor proves the backoff ran (a flat 100 ms loop is
    // ~1 s); the ceiling proves the retry timer really stopped.
    void neverListeningPeerExhaustsBoundAndSignalsOnce()
    {
        WeatherBackend wb(QString(), m_dataRoot);
        BackendGuard guard{wb};
        QSignalSpy spy(&wb, &WeatherBackend::musicIpcFailed);
        QVERIFY(!wb.musicIpcExhausted());
        QCOMPARE(wb.musicIpcAttempts(), 0);

        wb.startMusic();
        QVERIFY(wb.musicPlaying());

        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY2(waitFor([&] { return wb.musicIpcExhausted(); }, 9000),
                 "never-listening peer never reached the retry bound");
        const int exhaustMs = elapsed.elapsed();
        QVERIFY2(exhaustMs >= 2000,
                 "backoff did not stretch the sequence past a flat 100 ms loop");
        QVERIFY2(exhaustMs <= 9000, "retry sequence outlived the deadline");

        QCOMPARE(wb.musicIpcAttempts(), 10);
        pump(100); // the failure emit is deferred to the next event-loop turn
        QCOMPARE(spy.count(), 1);
        const QString reason = spy.at(0).at(0).toString();
        QVERIFY2(failureReasons().contains(reason),
                 qPrintable(QStringLiteral("unexpected reason: %1").arg(reason)));
        QVERIFY(wb.musicPlaying()); // audio survives; only IPC control is lost

        QLocalServer late;
        QVERIFY(late.listen(m_socketPath));
        int lateConnections = 0;
        QObject::connect(&late, &QLocalServer::newConnection,
                         [&lateConnections] { ++lateConnections; });
        pump(2600); // longer than the 2 s backoff cap
        QCOMPARE(lateConnections, 0);
        QCOMPARE(wb.musicIpcAttempts(), 10);
        QCOMPARE(spy.count(), 1);
        late.close();
        QFile::remove(m_socketPath);
    }

    // A listener present from the start connects without burning the budget or
    // raising a failure — the bound must not penalize the healthy case.
    void listenerAtStartConnectsWithoutFailure()
    {
        QFile::remove(m_socketPath);
        QLocalServer peer;
        QVERIFY(peer.listen(m_socketPath));
        int connections = 0;
        QObject::connect(&peer, &QLocalServer::newConnection,
                         [&connections] { ++connections; });

        WeatherBackend wb(QString(), m_dataRoot);
        BackendGuard guard{wb};
        QSignalSpy spy(&wb, &WeatherBackend::musicIpcFailed);
        wb.startMusic();
        QVERIFY(waitFor([&connections] { return connections >= 1; }, 3000));
        pump(100);

        QCOMPARE(wb.musicIpcAttempts(), 0);
        QVERIFY(!wb.musicIpcExhausted());
        QCOMPARE(spy.count(), 0);
        peer.close();
        QFile::remove(m_socketPath);
    }

    // Re-arm after exhaustion clears the counters and a late listener wins;
    // stopMusic()'s deliberate abort must not be counted as a failure.
    void rearmAfterExhaustionConnectsAndResets()
    {
        WeatherBackend wb(QString(), m_dataRoot);
        BackendGuard guard{wb};
        QSignalSpy spy(&wb, &WeatherBackend::musicIpcFailed);
        wb.startMusic();
        QVERIFY(waitFor([&] { return wb.musicIpcExhausted(); }, 9000));
        pump(100);
        QCOMPARE(spy.count(), 1);

        wb.stopMusic();
        QVERIFY(!wb.musicIpcExhausted());
        QCOMPARE(wb.musicIpcAttempts(), 0);

        QFile::remove(m_socketPath);
        QLocalServer peer;
        QVERIFY(peer.listen(m_socketPath));
        int connections = 0;
        QObject::connect(&peer, &QLocalServer::newConnection,
                         [&connections] { ++connections; });
        wb.startMusic();
        QVERIFY(waitFor([&connections] { return connections >= 1; }, 3000));
        pump(100);

        QCOMPARE(connections, 1);
        QCOMPARE(wb.musicIpcAttempts(), 0);
        QVERIFY(!wb.musicIpcExhausted());
        QCOMPARE(spy.count(), 1); // a successful connect emits no failure
        peer.close();
        QFile::remove(m_socketPath);

        wb.stopMusic();
        pump(300);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(wb.musicIpcAttempts(), 0);
    }
};

QTEST_MAIN(MusicIpcRetryTest)
#include "MusicIpcRetryTest.moc"
