#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVector>
#include "../MpvBasicOptions.h"

// basicArgs reads config.json["app"] from a dataRoot we control per-case, so
// every case is a pure file-in/args-out check with no mpv spawning.
namespace {
QString tmpBase = QDir::tempPath() + "/mpvs-basic-options-t";
QVector<QString> madeDirs;

QString makeDataRoot(const QJsonObject &app)
{
    static int n = 0;
    const QString name = "mpvs-basic-options-t" + QString::number(++n);
    QDir base(QDir::tempPath());
    base.mkdir(name);
    QDir d(QDir::tempPath() + "/" + name);
    QFile f(d.filePath("config.json"));
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(QJsonObject{{"app", app}}).toJson());
    madeDirs.push_back(name);
    return d.path();
}

void cleanupTmpRoots()
{
    QDir base(QDir::tempPath());
    for (const QString &name : madeDirs)
        base.rmdir(name);
    madeDirs.clear();
}
}

class MpvsBasicOptionsTest : public QObject {
    Q_OBJECT

private slots:
    void absentKeysEmitNothing();
    void mpvConfTokenEmitsNothing();
    void unknownTokensAreSkipped();
    void replaygainMapsUiTokens();
    void numericGainBoundsAreEnforced();
    void tableOrderIsStable();
};

void MpvsBasicOptionsTest::absentKeysEmitNothing()
{
    QString root = makeDataRoot({{"color_scheme", QString("Video 1")}});
    QVERIFY(mpvopts::basicArgs(root).isEmpty());
    cleanupTmpRoots();
}

void MpvsBasicOptionsTest::mpvConfTokenEmitsNothing()
{
    QString root = makeDataRoot({{"replaygain", "Mpv Conf"},
                                 {"deinterlace", "Mpv Conf"}});
    QVERIFY(mpvopts::basicArgs(root).isEmpty());
    cleanupTmpRoots();
}

void MpvsBasicOptionsTest::unknownTokensAreSkipped()
{
    QString root = makeDataRoot({{"replaygain", "bananas"},
                                 {"audio_gain", "loud"}});
    QVERIFY(mpvopts::basicArgs(root).isEmpty());
    cleanupTmpRoots();
}

void MpvsBasicOptionsTest::replaygainMapsUiTokens()
{
    QString root = makeDataRoot({{"replaygain", "Track"}});
    QVERIFY(mpvopts::basicArgs(root) == (QVector<QString>{"--replaygain=track"}));
    cleanupTmpRoots();
}

void MpvsBasicOptionsTest::numericGainBoundsAreEnforced()
{
    QString ok = makeDataRoot({{"audio_gain", "8"}});
    QVERIFY(mpvopts::basicArgs(ok) == (QVector<QString>{"--audio-gain=8"}));
    QString neg = makeDataRoot({{"audio_gain", "-4"}});
    QVERIFY(mpvopts::basicArgs(neg) == (QVector<QString>{"--audio-gain=-4"}));
    QString out = makeDataRoot({{"audio_gain", "99"}}); // out of range -> skipped
    QVERIFY(mpvopts::basicArgs(out).isEmpty());
    cleanupTmpRoots();
}

void MpvsBasicOptionsTest::tableOrderIsStable()
{
    // Choice rows in table order (replaygain, deinterlace, audio-channels),
    // then numeric rows — deterministic CLI for logs and testability.
    QString root = makeDataRoot({{"audio_gain", "2"},
                                 {"audio_channels", "Stereo"},
                                 {"replaygain", "Album"}});
    QVector<QString> want{"--replaygain=album", "--audio-channels=stereo",
                         "--audio-gain=2"};
    QVERIFY(mpvopts::basicArgs(root) == want);
    cleanupTmpRoots();
}

QTEST_MAIN(MpvsBasicOptionsTest)
#include "MpvBasicOptionsTest.moc"
