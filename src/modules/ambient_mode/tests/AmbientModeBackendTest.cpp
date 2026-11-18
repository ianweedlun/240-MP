// Regression tests for AmbientModeBackend (overnight torture, ambient_mode).
// Pure QtCore: filesystem scans use QTemporaryDir, playback uses a fake `mpv`
// on PATH, so no real audio, display or network is touched.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include "modules/ambient_mode/AmbientModeBackend.h"

namespace {

const char *kModuleId = "com.240mp.ambient_mode";

void writeFile(const QString &path, const QString &body = QStringLiteral("x")) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        qFatal("cannot write %s", qPrintable(path));
    f.write(body.toUtf8());
}

QString pidFromFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll()).trimmed();
}

bool pidAlive(const QString &pid) {
    if (pid.isEmpty())
        return false;
    QProcess p;
    p.start(QStringLiteral("kill"), {QStringLiteral("-0"), pid});
    p.waitForFinished(2000);
    return p.exitCode() == 0;
}

// Fake mpv writes its PID immediately, then sleeps. Restores PATH and reaps the
// child on destruction so a regression can never leak a process into the lab.
class FakeMpv {
public:
    FakeMpv() {
        m_bin.reset(new QTemporaryDir);
        m_pids.reset(new QTemporaryDir);
        m_pidFile = m_pids->path() + QStringLiteral("/mpv.pid");
        const QString script = m_bin->path() + QStringLiteral("/mpv");
        writeFile(script, QStringLiteral("#!/bin/sh\necho $$ > \"%1\"\nexec sleep 300\n")
                               .arg(m_pidFile));
        QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        m_oldPath = qgetenv("PATH");
        qputenv("PATH", (m_bin->path() + QLatin1Char(':') + QString::fromLocal8Bit(m_oldPath)).toLocal8Bit());
    }
    ~FakeMpv() {
        const QString currentPid = pid();
        if (pidAlive(currentPid))
            QProcess::execute(QStringLiteral("kill"), {QStringLiteral("-9"), currentPid});
        qputenv("PATH", m_oldPath);
    }

    QString pid() const { return pidFromFile(m_pidFile); }
    bool alive() const { return pidAlive(pid()); }

private:
    QScopedPointer<QTemporaryDir> m_bin;
    QScopedPointer<QTemporaryDir> m_pids;
    QString m_pidFile;
    QByteArray m_oldPath;
};

} // namespace

class AmbientModeBackendTest : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // Extensions are case-insensitive; lookalikes and hidden files are ignored.
    void testScanFiltersByExtension() {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        const QString media = data.path() + QStringLiteral("/ambient");
        QVERIFY(QDir().mkpath(media));
        writeFile(media + "/up.MP3");
        writeFile(media + "/low.mp3");
        writeFile(media + "/mixed.Mp4");
        writeFile(media + "/song.mp3.bak");
        writeFile(media + "/.hidden.mp3");

        AmbientModeBackend backend(data.path());
        QStringList audio;
        for (const QVariant &v : backend.getAudioFiles())
            audio << v.toMap().value(QStringLiteral("name")).toString();
        QStringList video;
        for (const QVariant &v : backend.getVideoFiles())
            video << v.toMap().value(QStringLiteral("name")).toString();
        audio.sort();
        video.sort();
        QCOMPARE(audio, QStringList({QStringLiteral("low.mp3"), QStringLiteral("up.MP3")}));
        QCOMPARE(video, QStringList({QStringLiteral("mixed.Mp4")}));
    }

    // A configured directory is picked up at construction; an empty setting
    // resets to dataRoot/ambient.
    void testConfiguredDirectoryAndReset() {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        const QString custom = data.path() + QStringLiteral("/custom");
        QVERIFY(QDir().mkpath(custom));
        QJsonObject cfg;
        QJsonObject module;
        module.insert(QStringLiteral("media_directory"), custom);
        QJsonObject modules;
        modules.insert(QLatin1String(kModuleId), module);
        cfg.insert(QStringLiteral("modules"), modules);
        QFile f(data.path() + QStringLiteral("/config.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(cfg).toJson());
        f.close();

        AmbientModeBackend backend(data.path());
        QCOMPARE(backend.mediaRoot(), custom);

        backend.onSettingChanged(kModuleId, "media_directory", QString());
        QCOMPARE(backend.mediaRoot(), data.path() + QStringLiteral("/ambient"));
        QVERIFY(QDir(backend.mediaRoot()).exists()); // default is created on demand
    }

    // A user-supplied (typo'd / unmounted) path must not be created on disk.
    void testUserPathIsNotCreated() {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        AmbientModeBackend backend(data.path());
        const QString bogus = data.path() + "/does/not/exist";

        backend.onSettingChanged(kModuleId, "media_directory", bogus);

        QCOMPARE(backend.mediaRoot(), bogus);
        QVERIFY2(!QDir(bogus).exists(), "a bogus media_directory must not be mkdir'd");
    }

    // Trailing separators are cosmetic: they must not count as a library swap.
    void testPathIsNormalized() {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        AmbientModeBackend backend(data.path());
        const QString base = data.path() + QStringLiteral("/lib");

        backend.onSettingChanged(kModuleId, "media_directory", base + QStringLiteral("/"));
        QCOMPARE(backend.mediaRoot(), base);
    }

    // Regression: changing media_directory while companion audio is playing
    // used to leave the old mpv process alive (ghost audio from the old library).
    void testMediaDirectoryChangeStopsAudio() {
        FakeMpv mpv;
        QTemporaryDir data;
        QVERIFY(data.isValid());
        AmbientModeBackend backend(data.path());
        backend.onSettingChanged(kModuleId, "media_directory", data.path() + "/first");
        backend.startAudio({QStringLiteral("/tmp/track.mp3")}, false);

        QTest::qWait(300);
        QVERIFY2(mpv.alive(), "fake mpv did not start");

        backend.onSettingChanged(kModuleId, "media_directory", data.path() + "/second");
        QTest::qWait(1200); // longer than the backend's 1s terminate wait
        QVERIFY2(!mpv.alive(), "companion audio survived a media_directory change");
    }

    // A cosmetic path change must not interrupt playback.
    void testCosmeticPathChangeKeepsAudio() {
        FakeMpv mpv;
        QTemporaryDir data;
        QVERIFY(data.isValid());
        AmbientModeBackend backend(data.path());
        const QString base = data.path() + "/lib";
        backend.onSettingChanged(kModuleId, "media_directory", base);
        backend.startAudio({QStringLiteral("/tmp/track.mp3")}, false);

        QTest::qWait(300);
        QVERIFY(mpv.alive());
        backend.onSettingChanged(kModuleId, "media_directory", base + "/");
        QTest::qWait(400);
        QVERIFY2(mpv.alive(), "a trailing-slash change restarted companion audio");
    }
};

QTEST_MAIN(AmbientModeBackendTest)
#include "AmbientModeBackendTest.moc"
