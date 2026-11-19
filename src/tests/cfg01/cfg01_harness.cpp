// CFG-01 Tier-1 gate harness — links the REAL AppCore.cpp from whichever
// snapshot the build targets (PRE ab00d606 / P2 atomic-only / FIX). Same source
// compiles against all three: save_setting's void->bool change is bridged by
// cfg01::saveSettingShim; no FIX-only symbol is referenced here.
//
// Modes (fixed-behavior predicates; exit 0 pass / 2 product fail / 1 rig / 3 rig-validity):
//   healthy <rundir>          G4 — healthy save preserves every fixture key.
//   corrupt-refusal <rundir>  G2 — F1-CUT, no bak: 5 saves + restart + 2 saves,
//                             live byte-identical throughout, loud warnings,
//                             zero change signals, defaults served in memory.
//   wipe-signature <rundir>   G2 PRE control — the durable armA defect signature
//                             (wipe sha 02f732d6…, 0 warnings, modules=={weather},
//                             scheme Video 1, idle_timeout gone). Exit 0 = defect
//                             confirmed (required-red on PRE/P2), 2 = not reproduced.
//   storm <rundir>            G7 — 100 saves, read-your-writes, final doc, bak
//                             == second-to-last when the policy maintains one.
//   missing-unreadable <rundir> G9 — missing != corrupt; unreadable = corrupt policy.
//   open-failure <rundir>     G3b — chmod 0555 data dir: false + warning + intact.
//   armB <rundir>             lab-only (Linux /proc): QSaveFile partial write +
//                             process death WITHOUT commit -> original intact.
//
// Synthetic credentials only (FIXTURE-TOKEN-123). No network.

#include "AppCore.h"
#include "cfg01_common.h"
#include "cfg01_fixtures.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSignalSpy>
#include <QStringList>
#include <QVariant>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace cfg01;

static bool hasBoolSave()
{
    // Compile-time surface probe mirrored at runtime for conditional asserts.
    if constexpr (requires(AppCore &c) { { c.save_setting(QString(), QString(), QVariant()) } -> std::same_as<bool>; })
        return true;
    else
        return false;
}

static QStringList dirEntries(const QString &dir)
{
    return QDir(dir).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
}

static bool tempLitter(const QString &dir)
{
    for (const QString &n : dirEntries(dir)) {
        if (n == "config.json" || n == "config.json.bak"
            || n.startsWith("config.json.corrupt-"))
            continue;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- G4
static int runHealthy(const QString &rundir)
{
    const QString cfg = setupDataDir(rundir, fixtureBytes());
    const QByteArray before = readFileBytes(cfg);
    AppCore core(rundir + "/approot", rundir + "/data");
    check("precheck healthy read plex.token",
          core.get_setting("com.240mp.plex", "token").toString() == "FIXTURE-TOKEN-123");

    const bool ok = saveSettingShim(core, "com.240mp.weather", "units", QVariant("metric"));
    const QByteArray after = readFileBytes(cfg);
    dumpFile(cfg, rundir + "/config-after.json");

    QJsonObject intended = fixtureObject();
    QJsonObject mods = intended["modules"].toObject();
    QJsonObject wx = mods["com.240mp.weather"].toObject();
    wx["units"] = "metric";
    mods["com.240mp.weather"] = wx;
    intended["modules"] = mods;
    const QByteArray intendedBytes = QJsonDocument(intended).toJson(QJsonDocument::Indented);

    check("save reported success", ok);
    check("live == intended doc (sha)", sha(after) == sha(intendedBytes));
    check("readback weather.units == metric",
          core.get_setting("com.240mp.weather", "units").toString() == "metric");
    check("plex token preserved on disk",
          QJsonDocument::fromJson(after).object()["modules"].toObject()
              .contains("com.240mp.plex"));
    check("all 5 fixture modules preserved",
          QJsonDocument::fromJson(after).object()["modules"].toObject().keys().size() == 5);
    check("app.color_scheme preserved (Video 2)",
          QJsonDocument::fromJson(after).object()["app"].toObject()["color_scheme"].toString()
              == "Video 2");
    check("zero quarantine files",
          !dirEntries(rundir + "/data").filter("config.json.corrupt-").size());
    check("zero temp litter", !tempLitter(rundir + "/data"));
    // R7 bak semantics: FIX preserves prior-live bytes; PRE/P2 maintain none.
    const QByteArray bak = readFileBytes(rundir + "/data/config.json.bak");
    check("bak absent or == prior live (R7)", bak.isEmpty() || sha(bak) == sha(before));
    printf("HEALTHY after bytes=%lld sha=%s bak=%s\n", (long long)after.size(),
           sha(after).constData(), bak.isEmpty() ? "<absent>" : sha(bak).constData());
    writeMessages(rundir + "/messages.log");
    return verdict("G4-healthy");
}

// ---------------------------------------------------------------- G2
static int runCorruptRefusal(const QString &rundir)
{
    const QString cfg = setupDataDir(rundir, fixtureBytes());
    const QString dataDir = rundir + "/data";
    AppCore *core = new AppCore(rundir + "/approot", dataDir);
    check("precheck healthy read plex.token",
          core->get_setting("com.240mp.plex", "token").toString() == "FIXTURE-TOKEN-123");

    // Inject the power-cut corpse: F1 truncated mid-token (467 B).
    const QByteArray cut = fixtureCutBytes();
    check("F1-CUT pin (467 B / a1535fcd…)",
          cut.size() == kF1CutSize && sha(cut) == kF1CutSha);
    if (!writeFileBytes(cfg, cut)) { fprintf(stderr, "RIG ERROR: inject failed\n"); return 1; }
    dumpFile(cfg, rundir + "/config-injected.json");
    check("no bak exists (no-bak variant)", !QFile::exists(dataDir + "/config.json.bak"));

    QSignalSpy modSpy(core, &AppCore::moduleSettingChanged);
    QSignalSpy appSpy(core, &AppCore::appSettingChanged);
    g_msgs.clear(); g_warnings = 0;

    int attempts = 0;
    bool allIntact = true, allRefused = true;
    for (int i = 0; i < 5; ++i) {
        const bool ok = saveSettingShim(*core, "com.240mp.weather", "units", QVariant("metric"));
        const QByteArray now = readFileBytes(cfg);
        ++attempts;
        if (sha(now) != kF1CutSha || now.size() != kF1CutSize) allIntact = false;
        if (hasBoolSave() && ok) allRefused = false;
        if (sha(now) == kWipeSha || now.size() == 152 || now.size() == 252) allIntact = false;
    }
    check("5 attempts: live sha == a1535fcd… byte-identical", allIntact);
    check("5 attempts: every save refused (bool surface)", allRefused || !hasBoolSave());
    check("zero moduleSettingChanged emissions", modSpy.count() == 0);
    check("zero appSettingChanged emissions", appSpy.count() == 0);
    check("get_setting serves defaults (token absent) while disk keeps corpse",
          variantEmpty(core->get_setting("com.240mp.plex", "token")));
    check("read-your-writes probe: weather.units NOT metric in memory",
          core->get_setting("com.240mp.weather", "units").toString() != "metric");

    // Loud warning: path + parse error + offset (R12/R20).
    bool loud = false;
    for (const QString &m : g_msgs)
        if (m.contains("corrupt at") && m.contains(dataDir) && m.contains("offset"))
            loud = true;
    check("corrupt-load warning carries path + parse error + offset", loud);
    check(">=1 warning during corrupt load", g_warnings >= 1);

    // Restart re-detection (R18): degraded state is on DISK, not latched in RAM.
    delete core;
    g_msgs.clear(); g_warnings = 0;
    core = new AppCore(rundir + "/approot", dataDir);
    QSignalSpy modSpy2(core, &AppCore::moduleSettingChanged);
    bool restartIntact = true, restartRefused = true;
    for (int i = 0; i < 2; ++i) {
        const bool ok = saveSettingShim(*core, "com.240mp.weather", "units", QVariant("metric"));
        const QByteArray now = readFileBytes(cfg);
        ++attempts;
        if (sha(now) != kF1CutSha) restartIntact = false;
        if (hasBoolSave() && ok) restartRefused = false;
    }
    check("restart: live still byte-identical corpse", restartIntact);
    check("restart: saves refused again", restartRefused || !hasBoolSave());
    check("restart: corrupt warning re-emitted (re-detection)", g_warnings >= 1);
    check("restart: zero change signals", modSpy2.count() == 0);
    check("all 7 attempts intact", allIntact && restartIntact);
    check("wipe sha 02f732d6… never appeared", sha(readFileBytes(cfg)) != kWipeSha);
    check("zero quarantine litter growth beyond policy (<=3)",
          dirEntries(dataDir).filter("config.json.corrupt-").size() <= 3);
    check("zero temp litter", !tempLitter(dataDir));
    dumpFile(cfg, rundir + "/config-after.json");
    delete core;
    writeMessages(rundir + "/messages.log");
    printf("CORRUPT-REFUSAL attempts=%d\n", attempts);
    return verdict("G2-corrupt-refusal");
}

// ------------------------------------------- G2 PRE control (required-red)
static int runWipeSignature(const QString &rundir)
{
    const QString cfg = setupDataDir(rundir, fixtureBytes());
    AppCore core(rundir + "/approot", rundir + "/data");
    check("precheck healthy read plex.token",
          core.get_setting("com.240mp.plex", "token").toString() == "FIXTURE-TOKEN-123");
    const QByteArray cut = fixtureCutBytes();
    if (!writeFileBytes(cfg, cut)) return 1;
    g_msgs.clear(); g_warnings = 0;
    saveSettingShim(core, "com.240mp.weather", "units", QVariant("metric"));
    const QByteArray after = readFileBytes(cfg);
    dumpFile(cfg, rundir + "/config-after.json");
    const QJsonObject obj = QJsonDocument::fromJson(after).object();
    const QJsonObject mods = obj["modules"].toObject();
    const QJsonObject app = obj["app"].toObject();
    printf("WIPE-SIGNATURE after bytes=%lld sha=%s\n",
           (long long)after.size(), sha(after).constData());
    check("wipe sha == 02f732d6… (152 B defaults-over-corpse)", sha(after) == kWipeSha);
    check("ZERO warnings (silent defect)", g_warnings == 0);
    check("modules == {weather} only", mods.keys() == QStringList{"com.240mp.weather"});
    check("app.color_scheme reset to Video 1", app["color_scheme"].toString() == "Video 1");
    check("app.idle_timeout GONE", !app.contains("idle_timeout"));
    check("plex token GONE from disk", !mods.contains("com.240mp.plex"));
    check("the one new setting survived",
          mods["com.240mp.weather"].toObject()["units"].toString() == "metric");
    writeMessages(rundir + "/messages.log");
    // Exit 0 = defect signature CONFIRMED (the required-red control passes its
    // own expectation); exit 2 = signature not reproduced (rig or fix present).
    return verdict("G2-PRE-wipe-signature");
}

// ---------------------------------------------------------------- G7
static int runStorm(const QString &rundir)
{
    const QString cfg = setupDataDir(rundir, fixtureBytes());
    AppCore core(rundir + "/approot", rundir + "/data");
    QByteArray prevLive = readFileBytes(cfg);
    QByteArray secondToLast;
    bool ryw = true, savesOk = true;
    for (int i = 0; i < 100; ++i) {
        const QString val = (i % 2 == 0) ? "metric" : "imperial";
        const bool ok = saveSettingShim(core, "com.240mp.weather", "units", QVariant(val));
        if (hasBoolSave() && !ok) savesOk = false;
        if (core.get_setting("com.240mp.weather", "units").toString() != val) ryw = false;
        secondToLast = prevLive;
        prevLive = readFileBytes(cfg);
    }
    check("100 saves all reported success", savesOk || !hasBoolSave());
    check("read-your-writes after EVERY save", ryw);
    QJsonObject intended = fixtureObject();
    QJsonObject mods = intended["modules"].toObject();
    QJsonObject wx = mods["com.240mp.weather"].toObject();
    wx["units"] = "imperial";   // i=99 odd -> imperial
    mods["com.240mp.weather"] = wx;
    intended["modules"] = mods;
    check("final live == last intended doc",
          sha(readFileBytes(cfg)) == sha(QJsonDocument(intended).toJson(QJsonDocument::Indented)));
    const QByteArray bak = readFileBytes(rundir + "/data/config.json.bak");
    check("bak == second-to-last doc (when maintained)",
          bak.isEmpty() || sha(bak) == sha(secondToLast));
    check("zero temp litter after storm", !tempLitter(rundir + "/data"));
    check("zero quarantine files", dirEntries(rundir + "/data").filter("config.json.corrupt-").isEmpty());
    writeMessages(rundir + "/messages.log");
    return verdict("G7-storm");
}

// ---------------------------------------------------------------- G9
static int runMissingUnreadable(const QString &rundir)
{
    if (::geteuid() == 0) {
        fprintf(stderr, "RIG ERROR: G9 unreadable arm needs non-root euid\n");
        return 1;
    }
    // Arm 1: MISSING — first run, defaults, fully writable, no litter.
    const QString rd1 = rundir + "/missing";
    QDir().mkpath(rd1 + "/data");
    QDir().mkpath(rd1 + "/approot");
    {
        AppCore core(rd1 + "/approot", rd1 + "/data");
        check("missing: defaults served (token absent)",
              variantEmpty(core.get_setting("com.240mp.plex", "token")));
        const bool ok = saveSettingShim(core, "com.240mp.weather", "units", QVariant("metric"));
        check("missing: first save writable", ok);
        const QByteArray b = readFileBytes(rd1 + "/data/config.json");
        QJsonObject o = QJsonDocument::fromJson(b).object();
        check("missing: saved doc parseable + guard-passing",
              !b.isEmpty() && passesGuard(o));
        check("missing: no bak on first-ever save (R7)",
              !QFile::exists(rd1 + "/data/config.json.bak"));
        check("missing: no quarantine", dirEntries(rd1 + "/data").filter("config.json.corrupt-").isEmpty());
    }
    // Arm 2: UNREADABLE — I/O error is CORRUPT policy, not missing.
    const QString rd2 = rundir + "/unreadable";
    const QString cfg = setupDataDir(rd2, fixtureBytes());
    ::chmod(cfg.toUtf8().constData(), 0000);
    {
        g_msgs.clear(); g_warnings = 0;
        AppCore core(rd2 + "/approot", rd2 + "/data");
        check("unreadable: defaults served in memory",
              variantEmpty(core.get_setting("com.240mp.plex", "token")));
        const bool ok = saveSettingShim(core, "com.240mp.weather", "units", QVariant("metric"));
        if (hasBoolSave())
            check("unreadable: save refused (bool surface)", !ok);
        check("unreadable: loud warning", g_warnings >= 1);
        ::chmod(cfg.toUtf8().constData(), 0644);
        check("unreadable: live bytes untouched", sha(readFileBytes(cfg)) == sha(fixtureBytes()));
    }
    writeMessages(rundir + "/messages.log");
    return verdict("G9-missing-unreadable");
}

// ---------------------------------------------------------------- G3b
static int runOpenFailure(const QString &rundir)
{
    if (::geteuid() == 0) {
        fprintf(stderr, "RIG ERROR: G3b needs non-root euid (chmod 0555 must bite)\n");
        return 1;
    }
    const QString cfg = setupDataDir(rundir, fixtureBytes());
    const QByteArray before = readFileBytes(cfg);
    AppCore core(rundir + "/approot", rundir + "/data");
    ::chmod((rundir + "/data").toUtf8().constData(), 0555);
    g_msgs.clear(); g_warnings = 0;
    const bool ok = saveSettingShim(core, "com.240mp.weather", "units", QVariant("metric"));
    const QByteArray after = readFileBytes(cfg);
    ::chmod((rundir + "/data").toUtf8().constData(), 0755);
    check("open-failure: live byte-identical", sha(after) == sha(before));
    if (hasBoolSave())
        check("open-failure: save returned false", !ok);
    check("open-failure: loud warning with path", g_warnings >= 1
          && g_msgs.join('\n').contains(rundir + "/data"));
    check("open-failure: zero temp litter", !tempLitter(rundir + "/data"));
    writeMessages(rundir + "/messages.log");
    return verdict("G3b-open-failure");
}

// ---------------------------------------------------------------- armB (lab)
// Child: real QSaveFile open + PARTIAL write + flush, pause so the parent can
// inspect the inode via /proc/<pid>/fd, then SIGKILL from parent — the exact
// state a power cut between write and commit leaves. Runs before any
// QGuiApplication/thread exists (QSaveFile is pure QtCore file I/O).
static int runArmB(const QString &rundir)
{
#ifndef __linux__
    fprintf(stderr, "RIG ERROR: armB needs Linux /proc\n");
    return 1;
#else
    const QString cfg = setupDataDir(rundir, fixtureBytes());
    const QByteArray before = readFileBytes(cfg);
    const QString dataDir = rundir + "/data";
    struct stat dst;
    if (::stat(dataDir.toUtf8().constData(), &dst) != 0) return 1;

    pid_t pid = ::fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        QSaveFile sf(cfg);
        if (!sf.open(QIODevice::WriteOnly)) _exit(1);
        const QByteArray payload = intendedBytes();
        sf.write(payload.constData(), 1024);   // partial write, mid-document
        sf.flush();
        // Pause until killed — NO commit, NO destructor cleanup.
        for (;;) ::usleep(10000);
        _exit(0);
    }
    ::usleep(200000);   // let the child open + write + flush

    // Inspect the child's fds: the temp inode must live on the DATA DIR's
    // filesystem (same st_dev), never /tmp.
    bool sameFs = false, foundTemp = false;
    const QString fdDir = QStringLiteral("/proc/%1/fd").arg(pid);
    for (const QString &fd : QDir(fdDir).entryList(QDir::Files | QDir::NoDotAndDotDot)) {
        struct stat st;
        const QByteArray link = QFile::symLinkTarget(fdDir + "/" + fd).toUtf8();
        if (::stat((fdDir + "/" + fd).toUtf8().constData(), &st) == 0
            && st.st_dev == dst.st_dev && link.contains("config")) {
            sameFs = true;
            foundTemp = true;
        }
        // O_TMPFILE: symlink target is "/path/#inode (deleted)" — unnamed.
        if (link.contains(dataDir) && link.contains("deleted")) {
            sameFs = true;
            foundTemp = true;
        }
    }
    check("armB: partial-write inode on data-dir filesystem (no /tmp)", foundTemp && sameFs);
    check("armB: original config intact while child holds partial write",
          sha(readFileBytes(cfg)) == sha(before));

    ::kill(pid, SIGKILL);
    int ws; ::waitpid(pid, &ws, 0);
    check("armB: child killed by SIGKILL", WIFSIGNALED(ws) && WTERMSIG(ws) == SIGKILL);
    check("armB: original STILL intact after process death",
          sha(readFileBytes(cfg)) == sha(before));
    // Fresh process reads the REAL settings, not a corpse.
    AppCore core(rundir + "/approot", dataDir);
    check("armB: real settings readable after abandoned write",
          core.get_setting("com.240mp.plex", "token").toString() == "FIXTURE-TOKEN-123");
    const QStringList entries = dirEntries(dataDir);
    printf("armB: post-death dir entries: %s\n", qPrintable(entries.join(", ")));
    writeMessages(rundir + "/messages.log");
    return verdict("armB-abandoned-write");
#endif
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: cfg01-harness <healthy|corrupt-refusal|wipe-signature|storm|"
                        "missing-unreadable|open-failure|armB> <rundir>\n");
        return 1;
    }
    const QString mode = argv[1];
    const QString rundir = argv[2];
    QDir().mkpath(rundir);

    if (mode == "armB")
        return runArmB(rundir);   // before QGuiApplication — fork must be single-threaded

    qInstallMessageHandler(msgHandler);
    QGuiApplication app(argc, argv);
    if (mode == "healthy")            return runHealthy(rundir);
    if (mode == "corrupt-refusal")    return runCorruptRefusal(rundir);
    if (mode == "wipe-signature")     return runWipeSignature(rundir);
    if (mode == "storm")              return runStorm(rundir);
    if (mode == "missing-unreadable") return runMissingUnreadable(rundir);
    if (mode == "open-failure")       return runOpenFailure(rundir);
    fprintf(stderr, "unknown mode: %s\n", qPrintable(mode));
    return 1;
}