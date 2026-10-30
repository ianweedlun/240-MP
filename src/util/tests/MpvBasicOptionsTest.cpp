#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QVariantMap>
#include "../MpvBasicOptions.h"

// All cases write mpv.conf fixtures under a temp dir and check effectiveTokens
// / setUiValue against them: pure file-in, tokens-out. No mpv spawning.
namespace {
QString makeConf(const QString &name, const QString &content)
{
    const QString path = QDir::tempPath() + "/mpv-conf-direct-edit-t/" + name;
    QDir(QDir::tempPath()).mkpath("mpv-conf-direct-edit-t");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    f.write(content.toUtf8());
    f.close();
    return path;
}
void rmTmpTree()
{
    QDir(QDir::tempPath() + "/mpv-conf-direct-edit-t").removeRecursively();
}
}

class MpvsBasicOptionsTest : public QObject {
    Q_OBJECT

private slots:
    void absentFileKeepsBuiltInDefaults();
    void defaultsThatAreMenuValuesDisplayAsTokens();
    void commentedKeysAreIgnored();
    void rawValuesMapToMenuTokens();
    void lastAssignmentWins();
    void includeOneLevelIsFollowed();
    void offMenuStepShowsCustom();
    void underscoreSpellingMatches();
    void setUiValueReplacesInPlace();
    void setUiValueAppendsWhenAbsent();
    void unknownUiTokenIsRejected();
};

void MpvsBasicOptionsTest::absentFileKeepsBuiltInDefaults()
{
    const QString path = QDir::tempPath() + "/mpv-conf-direct-edit-t/does-not-exist.conf";
    const QVariantMap eff = mpvopts::effectiveTokens(path);
    QCOMPARE(eff.value(QStringLiteral("deinterlace")).toString(), QStringLiteral("Off")); // no = default
    QCOMPARE(eff.value(QStringLiteral("audio_gain")).toString(), QStringLiteral("0"));
    QCOMPARE(eff.value(QStringLiteral("replaygain")).toString(), QStringLiteral("Custom (auto)"));
    QCOMPARE(eff.value(QStringLiteral("audio_channels")).toString(), QStringLiteral("Custom (auto)"));
}

void MpvsBasicOptionsTest::defaultsThatAreMenuValuesDisplayAsTokens()
{
    // deinterlace's built-in default (no) IS a menu value -> shows Off, not Custom.
    const QString path = makeConf("empty.conf", "# nothing set\n");
    const QVariantMap eff = mpvopts::effectiveTokens(path);
    QCOMPARE(eff.value(QStringLiteral("deinterlace")).toString(), QStringLiteral("Off"));
    rmTmpTree();
}

void MpvsBasicOptionsTest::commentedKeysAreIgnored()
{
    const QString path = makeConf("commented.conf",
        "#audio-channels=stereo\n# deinterlace=yes\n");
    const QVariantMap eff = mpvopts::effectiveTokens(path);
    QCOMPARE(eff.value(QStringLiteral("audio_channels")).toString(), QStringLiteral("Custom (auto)"));
    QCOMPARE(eff.value(QStringLiteral("deinterlace")).toString(), QStringLiteral("Off"));
    rmTmpTree();
}

void MpvsBasicOptionsTest::rawValuesMapToMenuTokens()
{
    const QString path = makeConf("set.conf",
        "replaygain=album\naudio-channels=stereo\n");
    const QVariantMap eff = mpvopts::effectiveTokens(path);
    QCOMPARE(eff.value(QStringLiteral("replaygain")).toString(), QStringLiteral("Album"));
    QCOMPARE(eff.value(QStringLiteral("audio_channels")).toString(), QStringLiteral("Stereo"));
    rmTmpTree();
}

void MpvsBasicOptionsTest::lastAssignmentWins()
{
    const QString path = makeConf("dup.conf",
        "deinterlace=yes\ndeinterlace=no\n");
    const QVariantMap eff = mpvopts::effectiveTokens(path);
    QCOMPARE(eff.value(QStringLiteral("deinterlace")).toString(), QStringLiteral("Off"));
    rmTmpTree();
}

void MpvsBasicOptionsTest::includeOneLevelIsFollowed()
{
    QString dir = QDir::tempPath() + "/mpv-conf-direct-edit-t/inc";
    QDir().mkpath(dir);
    { QFile inc(dir + "/more.conf");
      if (inc.open(QIODevice::WriteOnly | QIODevice::Truncate)) { inc.write("audio-gain=4\n"); inc.close(); } }
    const QString path = dir + "/main.conf";
    { QFile m(path); if (m.open(QIODevice::WriteOnly | QIODevice::Truncate)) { m.write("# include style\ninclude <more.conf>\n"); m.close(); } }
    const QVariantMap eff = mpvopts::effectiveTokens(path);
    QCOMPARE(eff.value(QStringLiteral("audio_gain")).toString(), QStringLiteral("4"));
    rmTmpTree();
}

void MpvsBasicOptionsTest::offMenuStepShowsCustom()
{
    const QString path = makeConf("odd.conf", "audio-gain=-7.5\n");
    const QVariantMap eff = mpvopts::effectiveTokens(path);
    QCOMPARE(eff.value(QStringLiteral("audio_gain")).toString(), QStringLiteral("Custom (-7.5)"));
    rmTmpTree();
}

void MpvsBasicOptionsTest::underscoreSpellingMatches()
{
    const QString path = makeConf("alias.conf", "audio_channels=max\n");
    const QVariantMap eff = mpvopts::effectiveTokens(path);
    QCOMPARE(eff.value(QStringLiteral("audio_channels")).toString(), QStringLiteral("Auto"));
    rmTmpTree();
}

void MpvsBasicOptionsTest::setUiValueReplacesInPlace()
{
    // (fixture below is the real content under test)
    const QString p2 = makeConf("edit2.conf",
        "audio-gain=4\ncontrast=1.2 # inline? no\n");
    QVERIFY(mpvopts::setUiValue(p2, "audio_gain", "8"));
    QFile f(p2); f.open(QIODevice::ReadOnly);
    const QString after = f.readAll(); f.close();
    QCOMPARE(after, QStringLiteral("audio-gain=8\ncontrast=1.2 # inline? no\n"));
    rmTmpTree();
}

void MpvsBasicOptionsTest::setUiValueAppendsWhenAbsent()
{
    const QString path = makeConf("append.conf", "contrast=1.2\n");
    QVERIFY(mpvopts::setUiValue(path, "deinterlace", "Auto"));
    QFile f(path); f.open(QIODevice::ReadOnly);
    const QString after = f.readAll(); f.close();
    QCOMPARE(after, QStringLiteral("contrast=1.2\ndeinterlace=auto\n"));
    rmTmpTree();
}

void MpvsBasicOptionsTest::unknownUiTokenIsRejected()
{
    const QString path = makeConf("reject.conf", "contrast=1.2"); // no trailing NL preserved
    QVERIFY(!mpvopts::setUiValue(path, "deinterlace", "Bananas"));
    QFile f(path); f.open(QIODevice::ReadOnly);
    QCOMPARE(QString::fromUtf8(f.readAll()), QStringLiteral("contrast=1.2")); // untouched
    f.close();
    rmTmpTree();
}

QTEST_MAIN(MpvsBasicOptionsTest)
#include "MpvBasicOptionsTest.moc"
