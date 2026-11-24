// CFG-01 N4b — two-sided gate for atomic mpv.conf persistence.
//
// The checked-in assertions describe the FIXED behaviour. Point
// MPVCONF_ATOMIC_SRC at a pre-fix MpvBasicOptions.cpp (via cmake) and the SAME
// instrument fails BY DESIGN with the defect signature: the kill sweep sees
// ZEROED/PARTIAL writes, RLIMIT leaves a truncated live file with a "success"
// return, and the unreadable-file case silently wipes the conf to one line.
// Fixtures are synthetic and hermetic (QTemporaryDir); no real data touched.
#include "util/MpvBasicOptions.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QVariantMap>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_checks = 0;
int g_fail = 0;

void check(bool ok, const char *label)
{
    ++g_checks;
    if (!ok) {
        ++g_fail;
        qCritical().noquote() << "FAIL" << label;
    } else {
        qInfo().noquote() << "ok  " << label;
    }
}

QByteArray readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

QString sha(const QByteArray &bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// A large synthetic conf widens the pre-fix truncate+write window enough for
// a SIGKILL sweep to land inside it. The writer must replace ONLY the
// replaygain line; every padding byte is preserved verbatim.
QByteArray synthConf(const QByteArray &replayValue)
{
    QByteArray out;
    out.append("# synthetic mpv.conf — CFG-01 N4b gate\n");
    out.append("replaygain=");
    out.append(replayValue);
    out.append("\n");
    out.append("deinterlace=no\naudio-gain=0\n");
    int i = 0;
    while (out.size() < 60000) {
        out.append("# pad ");
        out.append(QByteArray::number(i++));
        out.append(" abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz\n");
    }
    return out;
}

bool writeAll(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(bytes) == bytes.size();
}

bool onlyFile(const QString &dir, const QString &name)
{
    const QStringList entries = QDir(dir).entryList(QDir::Files | QDir::Hidden);
    return entries.size() == 1 && entries.first() == name;
}

// ---- healthy control (PASS-BOTH): format, mode, read-back, no litter ----
void testHealthy()
{
    QTemporaryDir td;
    check(td.isValid(), "healthy: temp dir");
    const QString p = td.path() + QStringLiteral("/mpv.conf");
    const QByteArray before = synthConf("album");
    check(writeAll(p, before), "healthy: fixture written");
    QFile::setPermissions(p, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ReadGroup);
    const auto modeBefore = QFileInfo(p).permissions();

    check(mpvopts::setUiValue(p, QStringLiteral("replaygain"), QStringLiteral("Track")),
          "healthy: setUiValue returns true");
    const QByteArray after = readAll(p);
    QByteArray intended = before;
    intended.replace("replaygain=album", "replaygain=track");
    check(after == intended, "healthy: only the managed line changed (byte-exact)");
    check(QFileInfo(p).permissions() == modeBefore, "healthy: existing mode preserved");
    check(mpvopts::effectiveTokens(p).value(QStringLiteral("replaygain")).toString()
              == QStringLiteral("Track"),
          "healthy: load side reads back the new token");
    check(onlyFile(td.path(), QStringLiteral("mpv.conf")), "healthy: no temp litter");
    qInfo().noquote() << "healthy: sha" << sha(after) << "bytes" << after.size();
}

// ---- SIGKILL sweep: live file is always old-or-new COMPLETE -------------
enum class Cls { Intact, Complete, Zeroed, Partial };

Cls classify(const QByteArray &live, const QByteArray &before, const QByteArray &intended)
{
    if (live.isEmpty())
        return Cls::Zeroed;
    if (live == before)
        return Cls::Intact;
    if (live == intended)
        return Cls::Complete;
    return Cls::Partial;
}

// Spawn this same binary in a child mode via fork()+execv(). Calling Qt/CF
// APIs directly in a bare fork() child after QCoreApplication construction
// aborts on macOS (CoreFoundation/ObjC fork safety), so the child performs
// ONLY async-signal-safe execv()/_exit().
pid_t spawnSelf(const QByteArray &selfExe, const char *mode,
                const char *arg1, const char *arg2 = nullptr)
{
    const pid_t pid = ::fork();
    if (pid == 0) {
        char *const argv[] = {
            const_cast<char *>(selfExe.constData()),
            const_cast<char *>(mode),
            const_cast<char *>(arg1),
            const_cast<char *>(arg2),
            nullptr
        };
        ::execv(selfExe.constData(), argv);
        ::_exit(127);
    }
    return pid;
}

void testKillSweep(const QByteArray &selfExe)
{
    QTemporaryDir td;
    check(td.isValid(), "kill: temp dir");
    const QString p = td.path() + QStringLiteral("/mpv.conf");
    const QString ready = td.path() + QStringLiteral("/ready");
    const QByteArray before = synthConf("album");
    QByteArray intended = before;
    intended.replace("replaygain=album", "replaygain=track");

    int zeroed = 0, partial = 0, intact = 0, complete = 0, rig = 0;
    const QByteArray pathBytes = QFile::encodeName(p);
    const QByteArray readyBytes = QFile::encodeName(ready);
    // Sparse tail reaches past the commit on slower filesystems (macOS fsync
    // is ~20x slower than this Linux host): 0..2000 us step 25 then a tail.
    QList<int> offsets;
    for (int off = 0; off <= 2000; off += 25) offsets.append(off);
    for (int off = 2500; off <= 16000; off += 500) offsets.append(off);
    offsets << 32000 << 64000 << 128000;
    for (int rep = 0; rep < 2; ++rep) {
        for (int off : offsets) {
            if (!writeAll(p, before)) { ++rig; continue; }
            ::unlink(readyBytes.constData());
            const pid_t pid = spawnSelf(
                selfExe, "--child-write", pathBytes.constData(), readyBytes.constData());
            if (pid < 0) { ++rig; continue; }
            // Poll the ready marker written by the exec'd child right before
            // setUiValue(), with 100 us granularity (up to 10 s for dyld load).
            bool seen = false;
            for (int t = 0; t < 100000; ++t) {
                if (::access(readyBytes.constData(), F_OK) == 0) { seen = true; break; }
                ::usleep(100);
            }
            if (!seen) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, nullptr, 0);
                ++rig;
                continue;
            }
            ::usleep(static_cast<useconds_t>(off));
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            switch (classify(readAll(p), before, intended)) {
            case Cls::Intact:   ++intact;   break;
            case Cls::Complete: ++complete; break;
            case Cls::Zeroed:   ++zeroed;   break;
            case Cls::Partial:  ++partial;  break;
            }
        }
    }
    qInfo().noquote() << QStringLiteral("kill: intact=%1 complete=%2 zeroed=%3 partial=%4 rig=%5")
                         .arg(intact).arg(complete).arg(zeroed).arg(partial).arg(rig);
    check(rig == 0, "kill: rig-valid (every trial reached the write window)");
    check(intact + complete + zeroed + partial >= 200, "kill: >=200 valid trials");
    check(zeroed == 0, "kill: ZEROED == 0");
    check(partial == 0, "kill: PARTIAL == 0 (never truncated or torn)");

    // Deterministic coverage proof using the exact same exec'd child path,
    // un-killed, in a clean directory: proves the spawned child genuinely
    // commits the new document and leaves no stray temp files on every OS.
    QTemporaryDir ctlTd;
    check(ctlTd.isValid(), "kill: control temp dir");
    const QString ctlPath = ctlTd.path() + QStringLiteral("/mpv.conf");
    const QByteArray ctlPathBytes = QFile::encodeName(ctlPath);
    check(writeAll(ctlPath, before), "kill: control fixture written");
    const pid_t ctl = spawnSelf(selfExe, "--child-write", ctlPathBytes.constData(), "");
    if (ctl < 0) {
        check(false, "kill: control fork+exec");
    } else {
        int status = 0;
        ::waitpid(ctl, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "kill: un-killed control child exited 0");
        check(readAll(ctlPath) == intended,
              "kill: un-killed control child committed the new doc");
        check(onlyFile(ctlTd.path(), QStringLiteral("mpv.conf")),
              "kill: no stray QSaveFile temp beside the live file");
    }
}

// ---- RLIMIT_FSIZE: checked short write, live file untouched -------------
void testRlimit(const QByteArray &selfExe)
{
    QTemporaryDir td;
    check(td.isValid(), "rlimit: temp dir");
    const QString p = td.path() + QStringLiteral("/mpv.conf");
    const QByteArray before = synthConf("album");
    check(writeAll(p, before), "rlimit: fixture written");
    const QByteArray pathBytes = QFile::encodeName(p);

    const pid_t pid = spawnSelf(selfExe, "--child-rlimit", pathBytes.constData());
    if (pid < 0) { check(false, "rlimit: fork+exec"); return; }
    int status = 0;
    ::waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "rlimit: setUiValue reported failure (clean child exit 1)");
    check(readAll(p) == before, "rlimit: live file byte-identical to template");
    check(onlyFile(td.path(), QStringLiteral("mpv.conf")), "rlimit: no temp litter");
}

// ---- seam failure matrix: every checked return + warning ----------------
QStringList g_messages;
void capture(QtMsgType, const QMessageLogContext &, const QString &msg)
{
    g_messages.append(msg);
}

void testSeamFailures()
{
    const char *stages[] = { "open", "write", "flush", "fsync", "commit", "dirfsync", "readopen" };
    for (const char *stage : stages) {
        QTemporaryDir td;
        const QString p = td.path() + QStringLiteral("/mpv.conf");
        const QByteArray before = synthConf("album");
        writeAll(p, before);
        QByteArray intended = before;
        intended.replace("replaygain=album", "replaygain=track");

        g_messages.clear();
        ::setenv("MPVCONF_SEAM_FAIL", stage, 1);
        QtMessageHandler prev = qInstallMessageHandler(capture);
        const bool ok = mpvopts::setUiValue(
            p, QStringLiteral("replaygain"), QStringLiteral("Track"));
        qInstallMessageHandler(prev);
        ::unsetenv("MPVCONF_SEAM_FAIL");

        const QByteArray live = readAll(p);
        check(!ok, (std::string(stage) + ": returns false").c_str());
        if (std::strcmp(stage, "dirfsync") == 0)
            check(live == intended, "dirfsync: live is the NEW COMPLETE doc");
        else
            check(live == before, (std::string(stage) + ": live file untouched").c_str());
        bool warned = false;
        for (const QString &m : g_messages)
            if (m.contains(QStringLiteral("[mpvopts]"))) warned = true;
        check(warned, (std::string(stage) + ": qWarning emitted").c_str());
        check(onlyFile(td.path(), QStringLiteral("mpv.conf")),
              (std::string(stage) + ": no temp litter").c_str());
    }
}

// ---- first run (PASS-BOTH control): missing file creates a valid conf ----
void testMissing()
{
    QTemporaryDir td;
    const QString p = td.path() + QStringLiteral("/mpv.conf");
    check(mpvopts::setUiValue(p, QStringLiteral("replaygain"), QStringLiteral("Track")),
          "missing: setUiValue returns true");
    check(readAll(p) == QByteArray("replaygain=track\n"),
          "missing: creates a parseable one-line conf");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 4 && std::strcmp(argv[1], "--child-write") == 0) {
        const char *confPath = argv[2];
        const char *readyPath = argv[3];
        if (readyPath[0] != '\0') {
            const int fd = ::open(readyPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd >= 0) {
                const ssize_t n = ::write(fd, "1", 1);
                (void)n;
                ::close(fd);
            }
        }
        const bool ok = mpvopts::setUiValue(
            QFile::decodeName(confPath), QStringLiteral("replaygain"),
            QStringLiteral("Track"));
        return ok ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[1], "--child-rlimit") == 0) {
        ::signal(SIGXFSZ, SIG_IGN); // write() must return EFBIG, not kill us
        const struct rlimit lim = { 1024, 1024 };
        if (::setrlimit(RLIMIT_FSIZE, &lim) != 0)
            return 2;
        const bool ok = mpvopts::setUiValue(
            QFile::decodeName(argv[2]), QStringLiteral("replaygain"),
            QStringLiteral("Track"));
        return ok ? 0 : 1;
    }

    QCoreApplication app(argc, argv);
    const QByteArray selfExe = QFile::encodeName(QCoreApplication::applicationFilePath());
    check(!selfExe.isEmpty() && ::access(selfExe.constData(), X_OK) == 0,
          "harness: resolved self executable for fork+exec");
    testHealthy();
    testKillSweep(selfExe);
    testRlimit(selfExe);
    testSeamFailures();
    testMissing();
    qInfo().noquote() << QStringLiteral("mpvconf-atomic: %1 checks, %2 failures")
                         .arg(g_checks).arg(g_fail);
    return g_fail == 0 ? 0 : 1;
}
