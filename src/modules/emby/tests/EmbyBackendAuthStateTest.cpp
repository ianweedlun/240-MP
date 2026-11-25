#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

// Access control is compile-time only; Qt headers are already included above.
// The persistence methods under test are private implementation details, and
// this suite drives them directly the same way the app's auth flow does.
#define private public
#include "modules/emby/EmbyBackend.h"
#undef private

using namespace Qt::Literals::StringLiterals;

// Behaviour gate for emby_auth.json: a corrupt read must be quarantined and
// distinguishable from a missing file, and a routine save must never turn it
// into an empty credential file.
class EmbyBackendAuthStateTest : public QObject {
    Q_OBJECT

private slots:
    void corruptLoadQuarantinesAndRoutineSaveDoesNotClobber();
    void missingFileCreatesDeviceIdAndSaves();
    void validAuthRoundTripsOwnerOnly();
    void nonObjectQuarantinesAndBlocksRoutineSave();
    void readOnlyFileSaveFailsWithoutClobber();
    void reauthRequiredIsReported();
    void explicitReauthWritesAndReusesRecoveredDeviceId();
    void explicitSaveRefusesEmptyAuth();
    void unreadableFileBlocksRoutineSave();
};

static const char kToken[] = "TOKEN-EMBY-KEEP-4f81";
static const char kDevice[] = "DEVICE-EMBY-SEED-77c2";

static QByteArray validAuthBytes(const QByteArray &token = QByteArray(kToken),
                                 const QByteArray &device = QByteArray(kDevice))
{
    QJsonObject auth;
    auth[u"serverUrl"_s]   = u"http://127.0.0.1:8096/"_s;
    auth[u"accessToken"_s] = QString::fromLatin1(token);
    auth[u"userId"_s]      = u"user-emby"_s;
    auth[u"userName"_s]    = u"ian"_s;
    auth[u"serverName"_s]  = u"lab"_s;
    auth[u"deviceId"_s]    = QString::fromLatin1(device);
    return QJsonDocument(auth).toJson(QJsonDocument::Indented);
}

static bool writeSeed(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = f.write(bytes) == bytes.size();
    f.close();
    return ok;
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = f.readAll();
    f.close();
    return bytes;
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

void EmbyBackendAuthStateTest::corruptLoadQuarantinesAndRoutineSaveDoesNotClobber()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/emby_auth.json";
    const QByteArray corrupt = validAuthBytes().left(validAuthBytes().size() - 11);
    QVERIFY(writeSeed(path, corrupt));

    EmbyBackend backend(QString(), dir.path());
    QVERIFY(backend.get_access_token().isEmpty());
    QCOMPARE(backend.get_auth_state(), u"reauth-required"_s);
    // Recovered identity, not a freshly minted QUuid.
    QCOMPARE(backend.m_deviceId, QString::fromLatin1(kDevice));

    const QString quarantine = path + ".corrupt";
    QVERIFY(QFileInfo::exists(quarantine));
    QCOMPARE(readFile(quarantine), corrupt);

    backend.saveAuthState();
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(readFile(quarantine), corrupt);
    QVERIFY(!readFile(path).contains(kToken));
}

void EmbyBackendAuthStateTest::missingFileCreatesDeviceIdAndSaves()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/emby_auth.json";

    EmbyBackend backend(QString(), dir.path());
    QVERIFY(!backend.m_deviceId.isEmpty());
    QCOMPARE(backend.get_auth_state(), u"none"_s);
    QVERIFY(!QFileInfo::exists(path + ".corrupt"));

    backend.saveAuthState();
    QVERIFY(QFileInfo::exists(path));
    const QJsonObject saved = QJsonDocument::fromJson(readFile(path)).object();
    QCOMPARE(saved[u"deviceId"_s].toString(), backend.m_deviceId);
    QVERIFY(!QFileInfo::exists(path + ".corrupt"));
}

void EmbyBackendAuthStateTest::validAuthRoundTripsOwnerOnly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/emby_auth.json";
    QVERIFY(writeSeed(path, validAuthBytes()));
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ReadGroup | QFileDevice::ReadOther);

    EmbyBackend backend(QString(), dir.path());
    QCOMPARE(backend.get_access_token(), QString::fromLatin1(kToken));
    QCOMPARE(backend.m_deviceId, QString::fromLatin1(kDevice));
    QCOMPARE(backend.get_auth_state(), u"authed"_s);
    QVERIFY(!QFileInfo::exists(path + ".corrupt"));

    backend.saveAuthState();
    const QJsonObject saved = QJsonDocument::fromJson(readFile(path)).object();
    QCOMPARE(saved[u"accessToken"_s].toString(), QString::fromLatin1(kToken));
    QCOMPARE(saved[u"deviceId"_s].toString(), QString::fromLatin1(kDevice));
#ifdef Q_OS_UNIX
    QCOMPARE(posixMode(path), 0600);
#endif
    QVERIFY(!QFileInfo::exists(path + ".corrupt"));
}

void EmbyBackendAuthStateTest::nonObjectQuarantinesAndBlocksRoutineSave()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/emby_auth.json";
    const QByteArray wrongType = "[\"not\", \"an\", \"object\"]";
    QVERIFY(writeSeed(path, wrongType));

    EmbyBackend backend(QString(), dir.path());
    QVERIFY(backend.get_access_token().isEmpty());
    QCOMPARE(backend.get_auth_state(), u"reauth-required"_s);
    QVERIFY(backend.m_deviceId.isEmpty());
    QCOMPARE(readFile(path + ".corrupt"), wrongType);

    backend.saveAuthState();
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(readFile(path + ".corrupt"), wrongType);
}

void EmbyBackendAuthStateTest::readOnlyFileSaveFailsWithoutClobber()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/emby_auth.json";
    const QByteArray seed = validAuthBytes();
    QVERIFY(writeSeed(path, seed));
    QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner));

    EmbyBackend backend(QString(), dir.path());
    QCOMPARE(backend.get_access_token(), QString::fromLatin1(kToken));

    backend.m_accessToken = u"WIPED"_s;
    backend.saveAuthState();
    QCOMPARE(readFile(path), seed);

    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

void EmbyBackendAuthStateTest::reauthRequiredIsReported()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    EmbyBackend backend(QString(), dir.path());
    QSignalSpy spy(&backend, &EmbyBackend::errorOccurred);
    QVERIFY(spy.isValid());

    const QByteArray corrupt = validAuthBytes().left(validAuthBytes().size() - 7);
    QVERIFY(writeSeed(dir.path() + "/emby_auth.json", corrupt));
    backend.loadAuthState();
    QCoreApplication::processEvents();

    QCOMPARE(spy.count(), 1);
    QVERIFY(spy.first().at(0).toString().contains(u"RE-AUTHENTICATION REQUIRED"_s));
    QCOMPARE(backend.get_auth_state(), u"reauth-required"_s);
}

void EmbyBackendAuthStateTest::explicitReauthWritesAndReusesRecoveredDeviceId()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/emby_auth.json";
    const QByteArray corrupt = validAuthBytes().left(validAuthBytes().size() - 11);
    QVERIFY(writeSeed(path, corrupt));

    EmbyBackend backend(QString(), dir.path());
    QCOMPARE(backend.get_auth_state(), u"reauth-required"_s);
    const QString recovered = backend.m_deviceId;
    QCOMPARE(recovered, QString::fromLatin1(kDevice));

    // Simulate the successful auth response that calls saveAuthenticatedState().
    backend.m_serverUrl = u"http://127.0.0.1:8096"_s;
    backend.m_accessToken = u"TOKEN-NEW-AFTER-REAUTH"_s;
    backend.m_userId = u"user-emby-new"_s;
    backend.saveAuthenticatedState();

    QVERIFY(QFileInfo::exists(path));
    const QJsonObject saved = QJsonDocument::fromJson(readFile(path)).object();
    QCOMPARE(saved[u"accessToken"_s].toString(), u"TOKEN-NEW-AFTER-REAUTH"_s);
    QCOMPARE(saved[u"deviceId"_s].toString(), recovered); // grant not orphaned
    QCOMPARE(backend.get_auth_state(), u"authed"_s);
    QCOMPARE(readFile(path + ".corrupt"), corrupt); // evidence kept

    // A later routine save still cannot rewrite while the marker exists.
    backend.m_accessToken = u"WIPED"_s;
    backend.saveAuthState();
    const QJsonObject afterRoutine = QJsonDocument::fromJson(readFile(path)).object();
    QCOMPARE(afterRoutine[u"accessToken"_s].toString(), u"TOKEN-NEW-AFTER-REAUTH"_s);
}

void EmbyBackendAuthStateTest::explicitSaveRefusesEmptyAuth()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/emby_auth.json";
    const QByteArray corrupt = validAuthBytes().left(validAuthBytes().size() - 11);
    QVERIFY(writeSeed(path, corrupt));

    EmbyBackend backend(QString(), dir.path());
    backend.saveAuthenticatedState(); // no token/user/url yet
    QVERIFY(!QFileInfo::exists(path));
    QCOMPARE(backend.get_auth_state(), u"reauth-required"_s);
}

void EmbyBackendAuthStateTest::unreadableFileBlocksRoutineSave()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/emby_auth.json";
    const QByteArray seed = validAuthBytes();
    QVERIFY(writeSeed(path, seed));
    QVERIFY(QFile::setPermissions(path, QFileDevice::Permission(0)));

    EmbyBackend backend(QString(), dir.path());
    QCOMPARE(backend.get_auth_state(), u"reauth-required"_s);
    QVERIFY(backend.m_deviceId.isEmpty()); // no bytes to recover, no fresh mint
    backend.saveAuthState();

    QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QCOMPARE(readFile(path), seed);
    QVERIFY(!QFileInfo::exists(path + ".corrupt"));
}

QTEST_GUILESS_MAIN(EmbyBackendAuthStateTest)
#include "EmbyBackendAuthStateTest.moc"
