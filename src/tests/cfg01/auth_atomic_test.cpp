// CFG-01 N4a gates: credential-file atomic writes.
//
// Every case drives the SAME `credfile::write` function the six production
// writers call (Plex/Jellyfin/Emby auth, plex_key.pem, TVDB apikey, YouTube
// auth). It is not a source snapshot: bytes on disk, warning text, return
// values and file modes are asserted.
//
// Failure stages use the shared writer's runtime seam (open/write/flush/
// fsync/commit/dirfsync). Invariant under EVERY stage: the live file is OLD or
// NEW-COMPLETE — never truncated, zeroed or partial. A legacy-write canary
// proves the gate would catch the pre-fix shape.
//
// Synthetic credentials only. No network, no real data dir.

#include "util/CredentialFile.h"
#include "modules/virtual_channels/TVDBAuth.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QStringList>

#include <cstdio>

namespace {

int g_failures = 0;
QStringList g_messages;

void capture(QtMsgType, const QMessageLogContext &, const QString &msg) {
    g_messages.append(msg);
}

void check(bool ok, const QString &what) {
    if (!ok) {
        ++g_failures;
        fprintf(stderr, "  FAIL: %s\n", qPrintable(what));
    } else {
        printf("  ok: %s\n", qPrintable(what));
    }
}

QByteArray sha(const QByteArray &b) {
    return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex();
}

QByteArray readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray b = f.readAll();
    f.close();
    return b;
}

bool is0600(const QString &path) {
    const QFile::Permissions p = QFileInfo(path).permissions();
    return (p & QFile::ReadOwner) && (p & QFile::WriteOwner)
        && !(p & QFile::ReadGroup) && !(p & QFile::WriteGroup)
        && !(p & QFile::ReadOther) && !(p & QFile::WriteOther);
}

// Pre-fix shape: QFile::Truncate then a short write. The gate must prove this
// produces a partial file — otherwise the old-or-new assertion proves nothing.
void legacyTruncatingWrite(const QString &path, const QByteArray &full) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(full.left(full.size() / 2));
    f.close();
}

QStringList tempEntries(const QString &dir) {
    QStringList out;
    for (const QFileInfo &fi : QDir(dir).entryInfoList(QDir::Files)) {
        const QString n = fi.fileName();
        if (n.contains(QStringLiteral("qt_temp")) || n.contains(QStringLiteral(".tmp")))
            out << n;
    }
    return out;
}

struct ShapeCase {
    const char *name;
    const char *file;
    credfile::Shape shape;
    QByteArray oldBytes;
    QByteArray newBytes;
};

QByteArray jsonDoc(const QString &token) {
    QJsonObject o;
    o.insert(QStringLiteral("accessToken"), token);
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

QList<ShapeCase> cases() {
    const QByteArray pem("-----BEGIN PRIVATE KEY-----\n"
                         "MC4CAQAwBQYDK2VwBCIEIA==\n"
                         "-----END PRIVATE KEY-----\n");
    return {
        {"plex_auth.json", "plex_auth.json", credfile::Shape::JsonObject,
         jsonDoc(QStringLiteral("OLD-synthetic-111")), jsonDoc(QStringLiteral("NEW-synthetic-222"))},
        {"jellyfin_auth.json", "jellyfin_auth.json", credfile::Shape::JsonObject,
         jsonDoc(QStringLiteral("OLD-synthetic-111")), jsonDoc(QStringLiteral("NEW-synthetic-222"))},
        {"emby_auth.json", "emby_auth.json", credfile::Shape::JsonObject,
         jsonDoc(QStringLiteral("OLD-synthetic-111")), jsonDoc(QStringLiteral("NEW-synthetic-222"))},
        {"youtube auth.json", "auth.json", credfile::Shape::JsonObject,
         QByteArray("{\"access\":\"OLD-synthetic-111\"}"),
         QByteArray("{\"access\":\"NEW-synthetic-222\"}")},
        {"plex_key.pem", "plex_key.pem", credfile::Shape::PemPrivateKey,
         pem, pem},
        {"thetvdb_apikey", "thetvdb_apikey", credfile::Shape::ApiKeyLine,
         QByteArray("11111111-2222-3333-4444-555555555555\n"),
         QByteArray("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\n")},
    };
}

void happyAndMode(const ShapeCase &c, const QString &dir) {
    printf("[%s] happy + mode\n", c.name);
    const QString path = dir + QLatin1Char('/') + c.file;
    QFile::remove(path);
    g_messages.clear();

    // FIRST write: no pre-existing target. The old QFile+setPermissions path
    // created 0644; credfile must enforce 0600 on the committed target.
    check(credfile::write(path, c.oldBytes, c.shape, c.name),
          QString::fromLatin1(c.name) + QStringLiteral(": first write succeeds"));
    check(readAll(path) == c.oldBytes, QString::fromLatin1(c.name) + QStringLiteral(": first bytes exact"));
    check(is0600(path), QString::fromLatin1(c.name) + QStringLiteral(": first write is 0600"));

    // Overwrite: old-or-new complete + mode preserved.
    check(credfile::write(path, c.newBytes, c.shape, c.name),
          QString::fromLatin1(c.name) + QStringLiteral(": overwrite succeeds"));
    check(readAll(path) == c.newBytes, QString::fromLatin1(c.name) + QStringLiteral(": overwrite bytes exact"));
    check(is0600(path), QString::fromLatin1(c.name) + QStringLiteral(": overwrite stays 0600"));
    check(tempEntries(dir).isEmpty(), QString::fromLatin1(c.name) + QStringLiteral(": no temp litter"));
}

void corruptRefusal(const ShapeCase &c, const QString &dir, int idx) {
    printf("[%s] corrupt-preserving guard\n", c.name);
    const QString path = dir + QLatin1Char('/') + c.file;
    QByteArray corrupt;
    if (idx == 4) corrupt = QByteArray("not a private key at all");        // PEM
    else if (idx == 5) corrupt = QByteArray("  \n");                       // empty apikey line
    else corrupt = QByteArray("{\"accessToken\":\"TRUNC");                // mid-token JSON
    QFile cf(path);
    if (cf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        cf.write(corrupt);
        cf.close();
    }
    const QByteArray before = readAll(path);
    g_messages.clear();

    // ApiKeyLine is an explicit user-supplied value: allowed with a warning.
    const bool allowCorrupt = (c.shape == credfile::Shape::ApiKeyLine);
    const bool ok = credfile::write(path, c.newBytes, c.shape, c.name, allowCorrupt);
    if (allowCorrupt) {
        check(ok, QString::fromLatin1(c.name) + QStringLiteral(": explicit apikey replacement succeeds"));
        check(readAll(path) == c.newBytes, QString::fromLatin1(c.name) + QStringLiteral(": new key exact"));
        bool warned = false;
        for (const QString &m : g_messages)
            if (m.contains(QStringLiteral("overwriting corrupt"))) warned = true;
        check(warned, QString::fromLatin1(c.name) + QStringLiteral(": loud warning on overwrite"));
    } else {
        check(!ok, QString::fromLatin1(c.name) + QStringLiteral(": corrupt live file refuses write"));
        check(readAll(path) == before, QString::fromLatin1(c.name) + QStringLiteral(": corrupt bytes preserved byte-identical"));
        bool warned = false;
        for (const QString &m : g_messages)
            if (m.contains(QStringLiteral("REFUSING")) && m.contains(QString::fromLatin1(c.name)))
                warned = true;
        check(warned, QString::fromLatin1(c.name) + QStringLiteral(": warning names file + refusal"));

        // A genuine re-auth (token present at the call site) must be able to
        // recover: allowCorrupt=true proceeds with a loud warning.
        check(credfile::write(path, c.newBytes, c.shape, c.name, /*allowCorrupt=*/true),
              QString::fromLatin1(c.name) + QStringLiteral(": explicit re-auth over corrupt succeeds"));
        check(readAll(path) == c.newBytes,
              QString::fromLatin1(c.name) + QStringLiteral(": re-auth bytes exact"));
        bool reauthWarn = false;
        for (const QString &m : g_messages)
            if (m.contains(QStringLiteral("overwriting corrupt"))) reauthWarn = true;
        check(reauthWarn, QString::fromLatin1(c.name) + QStringLiteral(": re-auth over corrupt warns"));
    }
}

void errorShapes(const ShapeCase &c, const QString &dir) {
    printf("[%s] failure-stage old-or-new\n", c.name);
    const QString path = dir + QLatin1Char('/') + c.file;
    const QByteArray oldHash = sha(c.oldBytes);
    const QByteArray newHash = sha(c.newBytes);
    const char *stages[] = {"open", "write", "flush", "fsync", "commit", "dirfsync"};
    for (const char *stage : stages) {
        QFile::remove(path);
        QFile seed(path);
        if (seed.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            seed.write(c.oldBytes);
            seed.close();
        }
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
        g_messages.clear();
        const bool ok = credfile::write(path, c.newBytes, c.shape, c.name,
                                        /*allowCorrupt=*/false, stage);
        check(!ok, QString::fromLatin1(c.name) + QStringLiteral(": ") + stage + QStringLiteral(": returns false"));
        const QByteArray live = sha(readAll(path));
        const bool oldOrNew = live == oldHash || live == newHash;
        check(oldOrNew, QString::fromLatin1(c.name) + QStringLiteral(": ") + stage + QStringLiteral(": live is OLD or NEW-COMPLETE"));
        bool warned = false;
        for (const QString &m : g_messages)
            if (m.contains(QString::fromLatin1(c.name))) warned = true;
        check(warned, QString::fromLatin1(c.name) + QStringLiteral(": ") + stage + QStringLiteral(": warning names file"));
        check(tempEntries(dir).isEmpty(), QString::fromLatin1(c.name) + QStringLiteral(": ") + stage + QStringLiteral(": no temp litter after graceful failure"));
    }
}

void legacyCanary(const QString &dir) {
    printf("[rig] legacy truncation canary\n");
    const QString path = dir + QStringLiteral("/canary.json");
    const QByteArray full = jsonDoc(QStringLiteral("a-long-synthetic-token-0123456789"));
    QFile::remove(path);
    legacyTruncatingWrite(path, full);
    const QByteArray live = readAll(path);
    check(!live.isEmpty() && live != full && live != QByteArray(),
          QStringLiteral("legacy QFile+Truncate produces a partial file (gate would FAIL on PRE)"));
    check(sha(live) != sha(full), QStringLiteral("legacy partial hash differs from complete"));
}

void tvdbEndToEnd(const QString &dir) {
    printf("[thetvdb_apikey] TVDBAuth end-to-end\n");
    const QString path = dir + QStringLiteral("/tvdb-e2e-apikey");
    QFile::remove(path);
    vchan::TVDBAuth auth(path);
    const QString key = QStringLiteral("99998888-7777-6666-5555-444433332222");
    check(auth.setApikey(key), QStringLiteral("TVDBAuth::setApikey succeeds"));
    check(auth.apikey() == key, QStringLiteral("TVDBAuth key round-trips"));
    check(is0600(path), QStringLiteral("TVDBAuth file is 0600"));
    check(readAll(path) == key.toUtf8() + "\n" || readAll(path) == key.toUtf8(),
          QStringLiteral("TVDBAuth file keeps single-line format"));
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    qInstallMessageHandler(capture);
    printf("=== CFG-01 N4a credential atomic gates ===\n");

    QTemporaryDir dir(QStringLiteral("/tmp/cfg01n4a-XXXXXX"));
    if (!dir.isValid()) {
        fprintf(stderr, "cannot create scratch dir\n");
        return 2;
    }
    const QList<ShapeCase> all = cases();
    for (int i = 0; i < all.size(); ++i) {
        QDir shapeDir(dir.path() + QStringLiteral("/") + QString::number(i));
        shapeDir.mkpath(QStringLiteral("."));
        happyAndMode(all.at(i), shapeDir.path());
        corruptRefusal(all.at(i), shapeDir.path(), i);
        errorShapes(all.at(i), shapeDir.path());
    }
    legacyCanary(dir.path());
    tvdbEndToEnd(dir.path());

    printf("=== %d failures ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
