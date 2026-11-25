#include <QtTest/QtTest>
#include <QTemporaryDir>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

#include "../JsonStateFile.h"

using namespace jsonstate;

// Behaviour tests for the shared JSON state-file policy: four-way load status,
// quarantine-before-rewrite, owner-only atomic saves, and the routine-save
// refusal that stops a corrupt read from becoming a silent sign-out.
class JsonStateFileTest : public QObject {
    Q_OBJECT

private slots:
    void missingIsNotAnEmptyParse();
    void validRoundTripWritesOwnerOnlyMode();
    void wideExistingModeIsTightened();
    void ownerOnlyModeIsPreserved();
    void parseErrorQuarantinesRawBytesAndBlocksRoutineSave();
    void nonObjectQuarantinesRawBytes();
    void explicitSaveMayReplaceQuarantine();
    void repeatCorruptionKeepsFirstQuarantine();
    void readOnlyFileSaveFailsWithoutClobber();
    void saveIntoMissingDirectoryFails();
};

static void writeSeed(const QString &path, const QByteArray &bytes,
                      QFileDevice::Permissions perms = QFileDevice::ReadOwner |
                                                       QFileDevice::WriteOwner)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), qint64(bytes.size()));
    file.close();
    QVERIFY(QFile::setPermissions(path, perms));
}

#ifdef Q_OS_UNIX
static int posixMode(const QString &path)
{
    struct stat st{};
    if (::stat(path.toUtf8().constData(), &st) != 0)
        return -1;
    return st.st_mode & 0777;
}
#endif

void JsonStateFileTest::missingIsNotAnEmptyParse()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/state.json";

    const LoadResult loaded = load(path);
    QCOMPARE(int(loaded.status), int(Status::Missing));
    QVERIFY(loaded.object.isEmpty());
    QVERIFY(!loaded.quarantined());
    QVERIFY(loaded.allowsRoutineSave());
    QVERIFY(!QFileInfo::exists(quarantinePathFor(path)));
}

void JsonStateFileTest::validRoundTripWritesOwnerOnlyMode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/state.json";
    QJsonObject object;
    object[QStringLiteral("deviceId")] = QStringLiteral("DEV-KEEP-42");
    object[QStringLiteral("count")] = 7;

    const SaveResult saved = save(path, object);
    QVERIFY2(saved.ok, qPrintable(saved.errorString));
    QVERIFY(!saved.refusedByQuarantine);

    const LoadResult loaded = load(path);
    QCOMPARE(int(loaded.status), int(Status::Valid));
    QVERIFY(loaded.object == object);
    QVERIFY(loaded.allowsRoutineSave());
    QVERIFY(!loaded.isEmptyObject());
#ifdef Q_OS_UNIX
    QCOMPARE(posixMode(path), 0600);
#endif
}

void JsonStateFileTest::wideExistingModeIsTightened()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/state.json";
    writeSeed(path, "{}", QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ReadGroup | QFileDevice::ReadOther);
#ifdef Q_OS_UNIX
    QCOMPARE(posixMode(path), 0644);
#endif

    QJsonObject object;
    object[QStringLiteral("v")] = 1;
    const SaveResult saved = save(path, object);
    QVERIFY2(saved.ok, qPrintable(saved.errorString));
#ifdef Q_OS_UNIX
    QCOMPARE(posixMode(path), 0600);
#endif
}

void JsonStateFileTest::ownerOnlyModeIsPreserved()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/state.json";
    writeSeed(path, "{}", QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ExeOwner);
#ifdef Q_OS_UNIX
    QCOMPARE(posixMode(path), 0700);
#endif

    QJsonObject object;
    object[QStringLiteral("v")] = 1;
    const SaveResult saved = save(path, object);
    QVERIFY2(saved.ok, qPrintable(saved.errorString));
#ifdef Q_OS_UNIX
    QCOMPARE(posixMode(path), 0700);
#endif
}

void JsonStateFileTest::parseErrorQuarantinesRawBytesAndBlocksRoutineSave()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/emby_auth.json";
    const QByteArray truncated =
        "{\"serverUrl\":\"http://srv\",\"accessToken\":\"KEEP-TOKEN\","
        "\"deviceId\":\"DEVICE-KEEP-77\",\"userName\":\"user\""; // no closing brace
    writeSeed(path, truncated);

    const LoadResult loaded = load(path);
    QCOMPARE(int(loaded.status), int(Status::ParseError));
    QVERIFY(loaded.isCorrupt());
    QVERIFY(loaded.quarantined());
    QVERIFY(!loaded.allowsRoutineSave());
    QCOMPARE(loaded.quarantinePath, quarantinePathFor(path));

    QFile quarantined(quarantinePathFor(path));
    QVERIFY(quarantined.open(QIODevice::ReadOnly));
    QCOMPARE(quarantined.readAll(), truncated);
    quarantined.close();
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(recoverStringField(loaded.raw, QStringLiteral("deviceId")),
             QStringLiteral("DEVICE-KEEP-77"));

    QJsonObject replacement;
    replacement[QStringLiteral("deviceId")] = QStringLiteral("MINTED-BY-MISTAKE");
    const SaveResult refused = save(path, replacement, SaveMode::Routine);
    QVERIFY(!refused.ok);
    QVERIFY(refused.refusedByQuarantine);
    QVERIFY(!QFileInfo::exists(path)); // routine save must not resurrect it
}

void JsonStateFileTest::nonObjectQuarantinesRawBytes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/config.json";
    const QByteArray array = "[1,2,3]";
    writeSeed(path, array);

    const LoadResult loaded = load(path);
    QCOMPARE(int(loaded.status), int(Status::NotObject));
    QVERIFY(loaded.quarantined());
    QVERIFY(!loaded.allowsRoutineSave());
    QFile quarantined(quarantinePathFor(path));
    QVERIFY(quarantined.open(QIODevice::ReadOnly));
    QCOMPARE(quarantined.readAll(), array);
}

void JsonStateFileTest::explicitSaveMayReplaceQuarantine()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/state.json";
    writeSeed(path, "{\"v\":1", QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QCOMPARE(int(load(path).status), int(Status::ParseError));

    QJsonObject fresh;
    fresh[QStringLiteral("deviceId")] = QStringLiteral("NEW-AFTER-REAUTH");
    const SaveResult saved = save(path, fresh, SaveMode::Explicit);
    QVERIFY2(saved.ok, qPrintable(saved.errorString));
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(QFileInfo::exists(quarantinePathFor(path))); // evidence retained
    QCOMPARE(int(load(path).status), int(Status::Valid));
#ifdef Q_OS_UNIX
    QCOMPARE(posixMode(path), 0600);
#endif
}

void JsonStateFileTest::repeatCorruptionKeepsFirstQuarantine()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/state.json";
    writeSeed(path, "{\"first\":1");
    QCOMPARE(int(load(path).status), int(Status::ParseError));

    writeSeed(path, "{\"second\":2");
    QCOMPARE(int(load(path).status), int(Status::ParseError));

    QFile first(quarantinePathFor(path));
    QVERIFY(first.open(QIODevice::ReadOnly));
    QCOMPARE(first.readAll(), QByteArray("{\"first\":1"));

    const QString primary = quarantinePathFor(path);
    const QStringList siblings = QDir(dir.path()).entryList(
        QStringList{QFileInfo(primary).fileName() + QStringLiteral(".*")}, QDir::Files);
    QCOMPARE(siblings.size(), 1);
    QFile second(dir.path() + "/" + siblings.first());
    QVERIFY(second.open(QIODevice::ReadOnly));
    QCOMPARE(second.readAll(), QByteArray("{\"second\":2"));
}

void JsonStateFileTest::readOnlyFileSaveFailsWithoutClobber()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/state.json";
    const QByteArray original = "{\"token\":\"KEEP\"}";
    writeSeed(path, original, QFileDevice::ReadOwner);
    QCOMPARE(int(load(path).status), int(Status::Valid));

    QJsonObject replacement;
    replacement[QStringLiteral("token")] = QStringLiteral("WIPED");
    const SaveResult saved = save(path, replacement);
    QVERIFY(!saved.ok);
    QVERIFY(!saved.errorString.isEmpty());

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
}

void JsonStateFileTest::saveIntoMissingDirectoryFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/nope/state.json";
    QJsonObject object;
    object[QStringLiteral("v")] = 1;
    const SaveResult saved = save(path, object);
    QVERIFY(!saved.ok);
    QVERIFY(!QFileInfo::exists(path));
}

QTEST_GUILESS_MAIN(JsonStateFileTest)
#include "JsonStateFileTest.moc"
