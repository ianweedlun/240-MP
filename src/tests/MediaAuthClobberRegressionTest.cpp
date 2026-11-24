/*
 * MEDIA-AUTH-CLOBBER-01 — two-sided, checked-in CTest regression.
 *
 * The legacy slots drive a file-local known-bad oracle that reproduces the
 * shipped truncate-in-place / empty-parse behaviour. They assert that the
 * oracle DOES destroy the credential, and that the no-clobber contract used by
 * the fixed slots reports the expected violations against it. If the oracle
 * stops clobbering, the negative control fails and the suite proves nothing.
 *
 * The fixed slots drive the real EmbyBackend / JellyfinBackend from this
 * checkout against a QTemporaryDir data root. No host path, fixture checkout,
 * network or display is involved. All persistence methods are private, so
 * access is granted at compile time only (Qt headers are included first).
 */
#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStringList>
#include <QUuid>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#define private public
#include "modules/emby/EmbyBackend.h"
#include "modules/jellyfin/JellyfinBackend.h"
#undef private

using namespace Qt::Literals::StringLiterals;

namespace {
constexpr char kEmbyToken[]    = "TOKEN-EMBY-KEEP-4f81";
constexpr char kEmbyDevice[]   = "DEVICE-EMBY-SEED-77c2";
constexpr char kJfToken[]      = "TOKEN-JELLYFIN-KEEP-91ae";
constexpr char kJfDevice[]     = "DEVICE-JELLYFIN-SEED-2b6d";
constexpr char kJfConfigMark[] = "CONFIG-JELLYFIN-KEEP-cc40";

QByteArray readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = f.readAll();
    f.close();
    return bytes;
}

bool writeAll(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = f.write(bytes) == bytes.size();
    f.close();
    return ok;
}

QByteArray authSeed(bool jellyfin)
{
    QJsonObject o;
    o[u"serverUrl"_s] = jellyfin ? u"http://127.0.0.1:8097/"_s
                                 : u"http://127.0.0.1:8096/"_s;
    o[u"accessToken"_s] = QString::fromLatin1(jellyfin ? kJfToken : kEmbyToken);
    o[u"userId"_s]      = jellyfin ? u"user-jf"_s : u"user-emby"_s;
    o[u"userName"_s]    = u"ian"_s;
    o[u"serverName"_s]  = u"lab"_s;
    o[u"deviceId"_s]    = QString::fromLatin1(jellyfin ? kJfDevice : kEmbyDevice);
    return QJsonDocument(o).toJson();
}

QByteArray configSeed()
{
    QJsonObject inner;
    inner[u"watch_marker"_s] = QString::fromLatin1(kJfConfigMark);
    QJsonObject modules;
    modules[u"com.240mp.jellyfin"_s] = inner;
    QJsonObject o;
    o[u"modules"_s] = modules;
    return QJsonDocument(o).toJson();
}

QByteArray interrupted(const QByteArray &bytes, int tailBytes)
{
    return bytes.left(bytes.size() - tailBytes);
}

bool isEmptyObjectBytes(const QByteArray &bytes)
{
    const QByteArray t = bytes.trimmed();
    return t == "{}" || t == "{\n}" || t == "{ }";
}

bool emptyTokenWritten(const QByteArray &bytes)
{
    return bytes.contains("\"accessToken\": \"\"") ||
           bytes.contains("\"accessToken\":\"\"");
}

bool emptyDeviceWritten(const QByteArray &bytes)
{
    return bytes.contains("\"deviceId\": \"\"") ||
           bytes.contains("\"deviceId\":\"\"");
}

struct AuthObservation {
    QByteArray before;
    QByteArray after;
    QByteArray quarantine;
    bool hasQuarantine = false;
    QString token;        // observed right after construction
    QString device;       // observed right after construction
    QString seededToken;
    QString seededDevice;
};

// The contract that MUST hold on the fixed arm. Applied unchanged to the
// legacy oracle it must fail, with the exact violation names asserted below.
QStringList authNoClobberViolations(const AuthObservation &o)
{
    QStringList bad;
    if (!o.hasQuarantine)
        bad << u"quarantine-missing"_s;
    else if (o.quarantine != o.before)
        bad << u"quarantine-not-raw-bytes"_s;
    if (o.device != o.seededDevice)
        bad << u"device-id-replaced"_s;
    if (!o.after.isEmpty()) {
        if (o.after != o.before)
            bad << u"main-file-rewritten"_s;
        if (!o.after.contains(o.seededToken.toLatin1()))
            bad << u"seeded-token-dropped"_s;
        if (emptyTokenWritten(o.after))
            bad << u"empty-token-written"_s;
        if (emptyDeviceWritten(o.after))
            bad << u"empty-device-written"_s;
    }
    return bad;
}

struct ConfigObservation {
    QByteArray before;
    QByteArray after;
    QByteArray quarantine;
    bool hasQuarantine = false;
};

QStringList configNoClobberViolations(const ConfigObservation &o)
{
    QStringList bad;
    if (!o.hasQuarantine)
        bad << u"quarantine-missing"_s;
    else if (o.quarantine != o.before)
        bad << u"quarantine-not-raw-bytes"_s;
    if (!o.after.isEmpty()) {
        if (o.after != o.before)
            bad << u"main-file-rewritten"_s;
        if (!o.after.contains(kJfConfigMark))
            bad << u"config-marker-dropped"_s;
        if (isEmptyObjectBytes(o.after))
            bad << u"reduced-to-empty-object"_s;
    }
    return bad;
}

AuthObservation observeAuth(const QString &path, const QByteArray &before,
                            const QString &token, const QString &device,
                            bool jellyfin)
{
    AuthObservation o;
    o.before = before;
    o.after = readAll(path);
    o.hasQuarantine = QFileInfo::exists(path + u".corrupt"_s);
    if (o.hasQuarantine)
        o.quarantine = readAll(path + u".corrupt"_s);
    o.token = token;
    o.device = device;
    o.seededToken = QString::fromLatin1(jellyfin ? kJfToken : kEmbyToken);
    o.seededDevice = QString::fromLatin1(jellyfin ? kJfDevice : kEmbyDevice);
    return o;
}

void expectViolations(const QStringList &observed, const QStringList &expected)
{
    for (const QString &name : expected) {
        QVERIFY2(observed.contains(name),
                 qPrintable(u"expected '%1' among violations [%2]"_s
                                .arg(name, observed.join(u", "_s))));
    }
}

// ---------------------------------------------------------------------------
// Known-bad oracle: the shipped read/write shape, kept inside the test binary.
// ---------------------------------------------------------------------------

struct LegacyAuthResult {
    QString token;
    QString device;
    QByteArray after;
};

LegacyAuthResult legacyAuthLoadThenTruncateSave(const QString &dataRoot,
                                                const QString &fileName,
                                                const QByteArray &corruptBytes,
                                                bool jellyfin)
{
    // Shipped loadAuthState(): parse error or non-object leaves all members
    // empty and reports nothing to the caller.
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(corruptBytes, &err);
    const QJsonObject parsed = (err.error == QJsonParseError::NoError && doc.isObject())
                                   ? doc.object() : QJsonObject{};
    QString token = parsed.value(u"accessToken"_s).toString();
    QString device = parsed.value(u"deviceId"_s).toString();
    // Shipped constructor: empty device id is indistinguishable from first run.
    if (device.isEmpty())
        device = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // Shipped saveAuthState(): plain QFile::WriteOnly | Truncate, no quarantine
    // check, no atomic commit.
    QJsonObject out;
    out[u"serverUrl"_s] = jellyfin ? u"http://127.0.0.1:8097/"_s
                                   : u"http://127.0.0.1:8096/"_s;
    out[u"accessToken"_s] = token;
    out[u"userId"_s] = parsed.value(u"userId"_s).toString();
    out[u"userName"_s] = parsed.value(u"userName"_s).toString();
    out[u"serverName"_s] = parsed.value(u"serverName"_s).toString();
    out[u"deviceId"_s] = device;
    const QString path = dataRoot + u"/"_s + fileName;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(out).toJson());
        f.close();
    }
    return {token, device, readAll(path)};
}

QByteArray legacyConfigLoadThenTruncateSave(const QString &dataRoot,
                                             const QByteArray &corruptBytes)
{
    // Shipped loadConfig(): parse failure returns {}; saveConfig() then
    // truncates config.json in place to that empty object.
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(corruptBytes, &err);
    const QJsonObject cfg = (err.error == QJsonParseError::NoError && doc.isObject())
                                ? doc.object() : QJsonObject{};
    const QString path = dataRoot + u"/config.json"_s;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        // A default-constructed QJsonObject serialises to "null"; the shipped
        // path produced the literal empty object, so write those bytes.
        const QByteArray written =
            (err.error == QJsonParseError::NoError && doc.isObject())
                ? QJsonDocument(cfg).toJson() : QByteArray("{\n}\n");
        f.write(written);
        f.close();
    }
    return readAll(path);
}

#ifdef Q_OS_UNIX
// Restores mode bits even when a QVERIFY returns early, so QTemporaryDir can
// always clean itself up.
class ModeRestore {
public:
    ModeRestore(const QString &file, const QString &dir)
        : m_file(file), m_dir(dir),
          m_fileMode(QFile::permissions(file)), m_dirMode(QFile::permissions(dir))
    {}
    ~ModeRestore()
    {
        QFile::setPermissions(m_file, m_fileMode);
        QFile::setPermissions(m_dir, m_dirMode);
    }
private:
    QString m_file;
    QString m_dir;
    QFileDevice::Permissions m_fileMode;
    QFileDevice::Permissions m_dirMode;
};
#endif

} // namespace

class MediaAuthClobberRegressionTest : public QObject {
    Q_OBJECT

private slots:
    // Negative controls: the shipped shape must clobber by design.
    void legacyEmbyAuthClobbersByDesign();
    void legacyJellyfinAuthClobbersByDesign();
    void legacyJellyfinConfigClobbersByDesign();

    // Fixed arms: real backends, one QTemporaryDir per case.
    void fixedEmbyAuthPreservesCorruptEvidence();
    void fixedJellyfinAuthPreservesCorruptEvidence();
    void fixedJellyfinConfigIsNotReducedToEmptyObject();

    // Controls: green on the fixed checkout, no .corrupt side files.
    void validEmbyAuthRoundTrips();
    void validJellyfinAuthRoundTrips();
    void missingEmbyAuthMintsIdentity();
    void missingJellyfinAuthMintsIdentity();
    void wrongTypeEmbyAuthQuarantines();
    void wrongTypeJellyfinAuthQuarantines();
    void readOnlyEmbyAuthSaveDoesNotClobber();
    void readOnlyJellyfinAuthSaveDoesNotClobber();
    void validJellyfinConfigRoundTrips();
};

void MediaAuthClobberRegressionTest::legacyEmbyAuthClobbersByDesign()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.path() + u"/emby_auth.json"_s;
    const QByteArray before = interrupted(authSeed(false), 11);
    QVERIFY(writeAll(path, before));

    const LegacyAuthResult r = legacyAuthLoadThenTruncateSave(
        root.path(), u"emby_auth.json"_s, before, false);
    const AuthObservation o = observeAuth(path, before, r.token, r.device, false);

    QVERIFY(r.token.isEmpty());
    QVERIFY(!r.device.isEmpty() && r.device != QString::fromLatin1(kEmbyDevice));
    QVERIFY(emptyTokenWritten(o.after));
    QVERIFY(!o.after.contains(kEmbyToken));
    QVERIFY(!o.hasQuarantine);

    // Same checker as the fixed arm: it must flag exactly the shipped damage.
    expectViolations(authNoClobberViolations(o),
                     {u"quarantine-missing"_s, u"device-id-replaced"_s,
                      u"main-file-rewritten"_s, u"seeded-token-dropped"_s,
                      u"empty-token-written"_s});
}

void MediaAuthClobberRegressionTest::legacyJellyfinAuthClobbersByDesign()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.path() + u"/jellyfin_auth.json"_s;
    const QByteArray before = interrupted(authSeed(true), 11);
    QVERIFY(writeAll(path, before));

    const LegacyAuthResult r = legacyAuthLoadThenTruncateSave(
        root.path(), u"jellyfin_auth.json"_s, before, true);
    const AuthObservation o = observeAuth(path, before, r.token, r.device, true);

    QVERIFY(r.token.isEmpty());
    QVERIFY(!r.device.isEmpty() && r.device != QString::fromLatin1(kJfDevice));
    QVERIFY(emptyTokenWritten(o.after));
    QVERIFY(!o.after.contains(kJfToken));
    QVERIFY(!o.hasQuarantine);

    expectViolations(authNoClobberViolations(o),
                     {u"quarantine-missing"_s, u"device-id-replaced"_s,
                      u"main-file-rewritten"_s, u"seeded-token-dropped"_s,
                      u"empty-token-written"_s});
}

void MediaAuthClobberRegressionTest::legacyJellyfinConfigClobbersByDesign()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.path() + u"/config.json"_s;
    const QByteArray before = interrupted(configSeed(), 9);
    QVERIFY(writeAll(path, before));

    const QByteArray after = legacyConfigLoadThenTruncateSave(root.path(), before);
    ConfigObservation o;
    o.before = before;
    o.after = after;
    o.hasQuarantine = QFileInfo::exists(path + u".corrupt"_s);
    if (o.hasQuarantine)
        o.quarantine = readAll(path + u".corrupt"_s);

    QVERIFY2(isEmptyObjectBytes(after),
             qPrintable(u"legacy config after=%1 (%2) before-tail=%3"_s
                            .arg(QString::fromLatin1(after),
                                 QString::fromLatin1(after.toHex()),
                                 QString::fromLatin1(before.right(24).toHex()))));
    QVERIFY(!after.contains(kJfConfigMark));
    QVERIFY(!o.hasQuarantine);
    expectViolations(configNoClobberViolations(o),
                     {u"quarantine-missing"_s, u"main-file-rewritten"_s,
                      u"config-marker-dropped"_s, u"reduced-to-empty-object"_s});
}

void MediaAuthClobberRegressionTest::fixedEmbyAuthPreservesCorruptEvidence()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.path() + u"/emby_auth.json"_s;
    const QByteArray before = interrupted(authSeed(false), 11);
    QVERIFY(writeAll(path, before));

    EmbyBackend backend(QString(), root.path());
    const QString token = backend.get_access_token();
    const QString device = backend.m_deviceId;
    backend.saveAuthState(); // routine save: must refuse quarantined state

    const AuthObservation o = observeAuth(path, before, token, device, false);
    const QStringList bad = authNoClobberViolations(o);
    QVERIFY2(bad.isEmpty(), qPrintable(u"clobber contract violated: [%1]"_s
                                           .arg(bad.join(u", "_s))));

    QVERIFY(token.isEmpty());
    QCOMPARE(device, QString::fromLatin1(kEmbyDevice));
    QVERIFY(o.hasQuarantine);
    QCOMPARE(o.quarantine, before); // raw bytes, byte for byte
    QVERIFY(o.after.isEmpty() || o.after == before); // no empty-parse rewrite
    QVERIFY(!emptyTokenWritten(o.after));
    QVERIFY(!emptyDeviceWritten(o.after));
    QCOMPARE(backend.get_auth_state(), u"reauth-required"_s);
}

void MediaAuthClobberRegressionTest::fixedJellyfinAuthPreservesCorruptEvidence()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.path() + u"/jellyfin_auth.json"_s;
    const QByteArray before = interrupted(authSeed(true), 11);
    QVERIFY(writeAll(path, before));

    JellyfinBackend backend(QString(), root.path());
    const QString token = backend.get_access_token();
    const QString device = backend.m_deviceId;
    backend.saveAuthState();

    const AuthObservation o = observeAuth(path, before, token, device, true);
    const QStringList bad = authNoClobberViolations(o);
    QVERIFY2(bad.isEmpty(), qPrintable(u"clobber contract violated: [%1]"_s
                                           .arg(bad.join(u", "_s))));

    QVERIFY(token.isEmpty());
    QCOMPARE(device, QString::fromLatin1(kJfDevice));
    QVERIFY(o.hasQuarantine);
    QCOMPARE(o.quarantine, before);
    QVERIFY(o.after.isEmpty() || o.after == before);
    QVERIFY(!emptyTokenWritten(o.after));
    QVERIFY(!emptyDeviceWritten(o.after));
    QCOMPARE(backend.get_auth_state(), u"reauth-required"_s);
}

void MediaAuthClobberRegressionTest::fixedJellyfinConfigIsNotReducedToEmptyObject()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.path() + u"/config.json"_s;
    const QByteArray before = interrupted(configSeed(), 9);
    QVERIFY(writeAll(path, before));

    JellyfinBackend backend(QString(), root.path());
    const QJsonObject loaded = backend.loadConfig();
    backend.saveConfig(loaded); // must refuse to rewrite from an empty parse

    ConfigObservation o;
    o.before = before;
    o.after = readAll(path);
    o.hasQuarantine = QFileInfo::exists(path + u".corrupt"_s);
    if (o.hasQuarantine)
        o.quarantine = readAll(path + u".corrupt"_s);

    const QStringList bad = configNoClobberViolations(o);
    QVERIFY2(bad.isEmpty(), qPrintable(u"config contract violated: [%1]"_s
                                           .arg(bad.join(u", "_s))));
    QVERIFY(loaded.isEmpty());
    QVERIFY(o.hasQuarantine);
    QCOMPARE(o.quarantine, before);
    QVERIFY(o.after.isEmpty() || o.after == before);
    QVERIFY(!isEmptyObjectBytes(o.after));
}

void MediaAuthClobberRegressionTest::validEmbyAuthRoundTrips()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.path() + u"/emby_auth.json"_s;
    const QByteArray seed = authSeed(false);
    QVERIFY(writeAll(path, seed));

    EmbyBackend backend(QString(), root.path());
    QCOMPARE(backend.get_access_token(), QString::fromLatin1(kEmbyToken));
    QCOMPARE(backend.m_deviceId, QString::fromLatin1(kEmbyDevice));
    backend.saveAuthState();

    const QByteArray after = readAll(path);
    QVERIFY(after.contains(kEmbyToken));
    QVERIFY(after.contains(kEmbyDevice));
    QVERIFY(!QFileInfo::exists(path + u".corrupt"_s));
}

void MediaAuthClobberRegressionTest::validJellyfinAuthRoundTrips()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.path() + u"/jellyfin_auth.json"_s;
    QVERIFY(writeAll(path, authSeed(true)));

    JellyfinBackend backend(QString(), root.path());
    QCOMPARE(backend.get_access_token(), QString::fromLatin1(kJfToken));
    QCOMPARE(backend.m_deviceId, QString::fromLatin1(kJfDevice));
    backend.saveAuthState();

    const QByteArray after = readAll(path);
    QVERIFY(after.contains(kJfToken));
    QVERIFY(after.contains(kJfDevice));
    QVERIFY(!QFileInfo::exists(path + u".corrupt"_s));
}

void MediaAuthClobberRegressionTest::missingEmbyAuthMintsIdentity()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.path() + u"/emby_auth.json"_s;

    EmbyBackend backend(QString(), root.path());
    QVERIFY(backend.get_access_token().isEmpty());
    QVERIFY(!backend.m_deviceId.isEmpty());
    QVERIFY(!QFileInfo::exists(path + u".corrupt"_s));
    backend.saveAuthState();

    const QByteArray after = readAll(path);
    QVERIFY(after.contains(backend.m_deviceId.toLatin1()));
    QVERIFY(!QFileInfo::exists(path + u".corrupt"_s));
}

void MediaAuthClobberRegressionTest::missingJellyfinAuthMintsIdentity()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.path() + u"/jellyfin_auth.json"_s;

    JellyfinBackend backend(QString(), root.path());
    QVERIFY(backend.get_access_token().isEmpty());
    QVERIFY(!backend.m_deviceId.isEmpty());
    QVERIFY(!QFileInfo::exists(path + u".corrupt"_s));
    backend.saveAuthState();

    const QByteArray after = readAll(path);
    QVERIFY(after.contains(backend.m_deviceId.toLatin1()));
    QVERIFY(!QFileInfo::exists(path + u".corrupt"_s));
}

void MediaAuthClobberRegressionTest::wrongTypeEmbyAuthQuarantines()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.path() + u"/emby_auth.json"_s;
    const QByteArray seed = "[]";
    QVERIFY(writeAll(path, seed));

    EmbyBackend backend(QString(), root.path());
    QVERIFY(backend.get_access_token().isEmpty());
    QVERIFY(QFileInfo::exists(path + u".corrupt"_s));
    QCOMPARE(readAll(path + u".corrupt"_s), seed);
    backend.saveAuthState();

    QVERIFY(readAll(path).isEmpty());
    QVERIFY(!QFileInfo::exists(path + u".corrupt"_s + u".corrupt"_s));
}

void MediaAuthClobberRegressionTest::wrongTypeJellyfinAuthQuarantines()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.path() + u"/jellyfin_auth.json"_s;
    const QByteArray seed = "[]";
    QVERIFY(writeAll(path, seed));

    JellyfinBackend backend(QString(), root.path());
    QVERIFY(backend.get_access_token().isEmpty());
    QVERIFY(QFileInfo::exists(path + u".corrupt"_s));
    QCOMPARE(readAll(path + u".corrupt"_s), seed);
    backend.saveAuthState();

    QVERIFY(readAll(path).isEmpty());
}

void MediaAuthClobberRegressionTest::readOnlyEmbyAuthSaveDoesNotClobber()
{
#ifndef Q_OS_UNIX
    QSKIP("POSIX mode bits required for the read-only control");
#else
    if (::geteuid() == 0)
        QSKIP("root bypasses POSIX mode bits; read-only control needs an unprivileged user");
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.path() + u"/emby_auth.json"_s;
    const QByteArray seed = authSeed(false);
    QVERIFY(writeAll(path, seed));

    ModeRestore restore(path, root.path());
    QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner));
    QVERIFY(QFile::setPermissions(root.path(),
                                  QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    EmbyBackend backend(QString(), root.path());
    QCOMPARE(backend.get_access_token(), QString::fromLatin1(kEmbyToken));
    backend.m_accessToken = u"NEW-TOKEN-MUST-NOT-LAND"_s;
    backend.saveAuthState();

    const QByteArray after = readAll(path);
    QCOMPARE(after, seed);
    QVERIFY(!after.contains("NEW-TOKEN-MUST-NOT-LAND"));
    QVERIFY(!QFileInfo::exists(path + u".corrupt"_s));
#endif
}

void MediaAuthClobberRegressionTest::readOnlyJellyfinAuthSaveDoesNotClobber()
{
#ifndef Q_OS_UNIX
    QSKIP("POSIX mode bits required for the read-only control");
#else
    if (::geteuid() == 0)
        QSKIP("root bypasses POSIX mode bits; read-only control needs an unprivileged user");
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.path() + u"/jellyfin_auth.json"_s;
    const QByteArray seed = authSeed(true);
    QVERIFY(writeAll(path, seed));

    ModeRestore restore(path, root.path());
    QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner));
    QVERIFY(QFile::setPermissions(root.path(),
                                  QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    JellyfinBackend backend(QString(), root.path());
    QCOMPARE(backend.get_access_token(), QString::fromLatin1(kJfToken));
    backend.m_accessToken = u"NEW-TOKEN-MUST-NOT-LAND"_s;
    backend.saveAuthState();

    const QByteArray after = readAll(path);
    QCOMPARE(after, seed);
    QVERIFY(!after.contains("NEW-TOKEN-MUST-NOT-LAND"));
    QVERIFY(!QFileInfo::exists(path + u".corrupt"_s));
#endif
}

void MediaAuthClobberRegressionTest::validJellyfinConfigRoundTrips()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.path() + u"/config.json"_s;
    QVERIFY(writeAll(path, configSeed()));

    JellyfinBackend backend(QString(), root.path());
    const QJsonObject loaded = backend.loadConfig();
    QVERIFY(loaded.contains(u"modules"_s));
    backend.saveConfig(loaded);

    const QByteArray after = readAll(path);
    QVERIFY(after.contains(kJfConfigMark));
    QVERIFY(!QFileInfo::exists(path + u".corrupt"_s));
}

QTEST_GUILESS_MAIN(MediaAuthClobberRegressionTest)
#include "MediaAuthClobberRegressionTest.moc"
