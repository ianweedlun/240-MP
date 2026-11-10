#include "TestHarness.h"
#include "../TVDBAuth.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>

using namespace vchan;

int runTVDBAuthTests() {
    vtest::section("TVDBAuth");

    // --- default path lives under the data root ---
    vtest::checkStr(TVDBAuth::defaultPath("/home/ian/.local/share/240-MP"),
                    "/home/ian/.local/share/240-MP/thetvdb_apikey",
                    "default path");

    QString path = QDir::tempPath() + "/vchan-tvdbauth-test.txt";
    QFile::remove(path);

    TVDBAuth auth(path);
    vtest::check(!auth.hasApikey(), "no key before set");
    vtest::checkStr(auth.apikey(), "", "apikey empty before set");

    // --- set: writes the file, round-trips ---
    const QString key = "11111111-2222-3333-4444-555555555555";
    vtest::check(auth.setApikey(key), "setApikey returns true");
    vtest::check(auth.hasApikey(), "hasApikey after set");
    vtest::checkStr(auth.apikey(), key, "apikey round-trips");

    // --- permissions are 0600 (owner read/write only) ---
    QFileInfo fi(path);
    vtest::check(fi.isFile(), "token file is a regular file");
    auto perms = fi.permissions();
    vtest::check(perms & QFile::ReadOwner, "owner read set");
    vtest::check(perms & QFile::WriteOwner, "owner write set");
    vtest::check(!(perms & QFile::ReadOther), "no world read");
    vtest::check(!(perms & QFile::WriteOther), "no world write");

    // --- empty key is rejected ---
    vtest::check(!auth.setApikey(""), "empty key rejected");

    QFile::remove(path);
    return 0;
}
