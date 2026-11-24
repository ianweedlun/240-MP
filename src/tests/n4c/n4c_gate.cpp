// N4c two-sided gate — exercises the SHARED atomic-write shape used by all
// data-dir persistence writers, plus its no-clobber variant, in a hermetic
// directory. QtCore only, synthetic fixtures, no network, no app boot.
//
// Exit taxonomy (same as the CFG-01 gates):
//   0 = gate PASS   2 = product fail   1 = rig error
//
// Modes:
//   failstages <rundir>   forced open|write|flush|fsync|commit|dirfsync: false
//                         + warning + old-or-new COMPLETE live file, no litter
//   formats <rundir>      every in-scope file format round-trips byte-identical
//                         and loads healthy (JSON parse / plain text)
//   newonly <rundir>      no-clobber create: happy path, pre-existing target,
//                         forced stages leave no partial target
//   saver-atomic <target> <payload> <ready>   kill-sweep child (atomic writer)
//   saver-legacy <target> <payload> <ready>   kill-sweep child (legacy in-place
//                         writer — the required-red control)

#include "AtomicFile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace n4c {

static QStringList g_msgs;
static int g_checks = 0;
static int g_failed = 0;

static void msgHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    g_msgs.append(msg);
    fprintf(stderr, "[%s] %s\n",
            type == QtWarningMsg ? "WARN" : "DEBUG", qPrintable(msg));
}

static void check(const char *label, bool ok)
{
    ++g_checks;
    if (!ok) ++g_failed;
    printf("CHECK %s: %s\n", label, ok ? "PASS" : "FAIL");
    fflush(stdout);
}

static QByteArray sha(const QByteArray &b)
{
    return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex();
}

static QByteArray readFile(const QString &p)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

static bool seedFile(const QString &p, const QByteArray &b)
{
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(b) == b.size();
}

static QStringList litter(const QString &dir, const QString &keep)
{
    QStringList out;
    for (const QString &n : QDir(dir).entryList(QDir::Files | QDir::Hidden)) {
        if (n != keep) out << n;
    }
    return out;
}

static int verdict(const char *gate)
{
    printf("GATE %s: %s (%d checks, %d failed)\n", gate,
           g_failed ? "FAIL" : "PASS", g_checks, g_failed);
    fflush(stdout);
    return g_failed ? 2 : 0;
}

// One representative document per in-scope writer site. These are the exact
// format families the real writers emit (JSON object, JSON array, plain text,
// script sidecar). Byte-compatibility of a healthy load is what we assert.
struct Format { const char *name; QByteArray bytes; bool json; bool array; };

static QList<Format> formats()
{
    return {
        {"volume.json",            "{\"level\":0.62,\"muted\":false}\n", true, false},
        {"youtube_history.json",   "[{\"id\":\"abc\",\"t\":1700000000,\"p\":12}]\n", true, true},
        {"youtube_watch_later.json","[{\"id\":\"def\",\"t\":1700000001}]\n", true, true},
        {"youtube_sponsor.json",   "{\"abc\":[[10,20]]}\n", true, false},
        {"youtube_url_cache.json", "{\"abc\":{\"url\":\"https://x/y\",\"expire\":1700003600}}\n", true, false},
        {"youtube_known_ids.json", "[\"abc\",\"def\"]\n", true, true},
        {"local_files_history.json","[{\"path\":\"/home/ian/ROMs/x.nes\",\"pos\":90}]\n", true, true},
        {"nfc_tag.txt",            "04:AB:CD:EF\n", false, false},
        {"weather_location.txt",   "Los Angeles\n34.05,-118.24\n", false, false},
        {"script.sidecar",         "# 240-MP script metadata\nname = Foo\nmode = console\n"
                                   "favorite = yes\nconfirm = no\nargs = --fullscreen\n"
                                   "wait = child\ntty = no\n", false, false},
        {"tvdb_cache.json",        "{\"123\":{\"name\":\"Some Show\",\"network\":\"NBC\"}}\n", true, false},
        {"update-staged.json",     "{\"version\":\"1.2.3\",\"asset\":\"240mp.tar.gz\","
                                   "\"sha256\":\"deadbeef\",\"size\":\"1234\"}\n", true, false},
        {"update-staged.sha256",   "deadbeef  240mp.tar.gz\n", false, false},
    };
}

static int runFailStages(const QString &rd)
{
    const QByteArray oldBytes = "{\"v\":1,\"keep\":\"original\"}\n";
    const QByteArray newBytes = "{\"v\":2,\"keep\":\"replacement\"}\n";
    const char *stages[] = {"open", "write", "flush", "fsync", "commit", "dirfsync"};

    for (const char *stage : stages) {
        const QString caseDir = rd + "/" + QString::fromLatin1(stage);
        QDir().mkpath(caseDir);
        const QString p = caseDir + "/doc.json";
        if (!seedFile(p, oldBytes)) { fprintf(stderr, "RIG ERROR seed %s\n", stage); return 1; }
        g_msgs.clear();
        const bool ok = atomicfile::writeBytes(p, newBytes, /*durable=*/true,
                                               "n4c-failstages", stage);
        const QByteArray live = readFile(p);
        char label[160];
        snprintf(label, sizeof(label), "%s: returned false", stage);
        check(label, !ok);
        snprintf(label, sizeof(label), "%s: warned", stage);
        bool warned = false;
        for (const QString &m : g_msgs)
            if (m.contains(QStringLiteral("failed")) || m.contains(QStringLiteral("injected")))
                warned = true;
        check(label, warned);
        // Every stage leaves a COMPLETE document: OLD for all pre-commit
        // stages, NEW for dirfsync (post-commit). Never a wipe or partial.
        snprintf(label, sizeof(label), "%s: live old-or-new COMPLETE", stage);
        const bool complete = sha(live) == sha(oldBytes) || sha(live) == sha(newBytes);
        check(label, complete);
        snprintf(label, sizeof(label), "%s: no temp litter", stage);
        check(label, litter(caseDir, QFileInfo(p).fileName()).isEmpty());
        if (std::strcmp(stage, "dirfsync") == 0) {
            snprintf(label, sizeof(label), "%s: live is NEW (post-commit)", stage);
            check(label, sha(live) == sha(newBytes));
        } else {
            snprintf(label, sizeof(label), "%s: live is OLD (pre-commit)", stage);
            check(label, sha(live) == sha(oldBytes));
        }
    }

    // durable=false must skip both fsync stages and succeed.
    const QString ndDir = rd + "/nodurable";
    QDir().mkpath(ndDir);
    const QString p = ndDir + "/doc.json";
    seedFile(p, oldBytes);
    check("durable=false: fsync stage skipped -> success",
          atomicfile::writeBytes(p, newBytes, /*durable=*/false,
                                 "n4c-failstages", "fsync"));
    check("durable=false: live NEW", sha(readFile(p)) == sha(newBytes));
    return verdict("N4C-failstages");
}

static int runFormats(const QString &rd)
{
    for (const Format &f : formats()) {
        const QString caseDir = rd + "/" + QLatin1String(f.name);
        QDir().mkpath(caseDir);
        const QString p = caseDir + "/doc";
        if (!seedFile(p, "PRE-EXISTING\n")) {
            fprintf(stderr, "RIG ERROR seed format %s\n", f.name);
            return 1;
        }
        g_msgs.clear();
        const bool ok = atomicfile::writeBytes(p, f.bytes, /*durable=*/true, "n4c-format");
        char label[160];
        snprintf(label, sizeof(label), "%s: write ok", f.name);
        check(label, ok);
        snprintf(label, sizeof(label), "%s: live byte-identical", f.name);
        check(label, readFile(p) == f.bytes);
        snprintf(label, sizeof(label), "%s: no temp litter", f.name);
        check(label, litter(caseDir, QFileInfo(p).fileName()).isEmpty());
        if (f.json) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(readFile(p), &err);
            snprintf(label, sizeof(label), "%s: healthy JSON load", f.name);
            check(label, err.error == QJsonParseError::NoError
                  && (f.array ? doc.isArray() : doc.isObject()));
        } else {
            snprintf(label, sizeof(label), "%s: healthy text load", f.name);
            check(label, !readFile(p).isEmpty());
        }
    }
    return verdict("N4C-formats");
}

static int runNewOnly(const QString &rd)
{
    const QByteArray stub = "04:AB:CD:EF\n";
    const QByteArray handEdited = "HAND-EDITED UID\n";

    // Happy path: creates when absent.
    const QString fresh = rd + "/fresh.txt";
    check("newonly: creates absent target",
          atomicfile::writeBytesNewOnly(fresh, stub, true, "n4c-newonly"));
    check("newonly: created bytes exact", readFile(fresh) == stub);
    check("newonly: no temp litter after create", litter(rd, "fresh.txt").isEmpty());

    // Pre-existing target: refuse, leave bytes untouched (no-clobber).
    const QString existing = rd + "/existing.txt";
    seedFile(existing, handEdited);
    check("newonly: refuses existing target",
          !atomicfile::writeBytesNewOnly(existing, stub, true, "n4c-newonly"));
    check("newonly: existing bytes untouched", readFile(existing) == handEdited);

    // Forced stages: never a partial target.
    const char *stages[] = {"open", "write", "flush", "fsync", "clobber", "dirfsync"};
    for (const char *stage : stages) {
        const QString p = rd + "/" + QString::fromLatin1(stage) + "-newonly.txt";
        seedFile(p, handEdited);
        g_msgs.clear();
        const bool ok = atomicfile::writeBytesNewOnly(p, stub, true,
                                                      "n4c-newonly", stage);
        char label[160];
        snprintf(label, sizeof(label), "newonly/%s: returns false", stage);
        check(label, !ok);
        snprintf(label, sizeof(label), "newonly/%s: pre-existing intact", stage);
        check(label, readFile(p) == handEdited);
        snprintf(label, sizeof(label), "newonly/%s: no temp litter", stage);
        check(label, litter(rd, QFileInfo(p).fileName())
                     .filter(QStringLiteral(".newonly-")).isEmpty());
    }

    // An absent target with an injected failure must stay ABSENT (never a
    // partial stub). Use a distinct dir to avoid the seeded files above.
    const QString sub = rd + "/absent";
    QDir().mkpath(sub);
    for (const char *stage : {"write", "flush", "fsync", "clobber"}) {
        const QString p = sub + "/" + QString::fromLatin1(stage) + ".txt";
        atomicfile::writeBytesNewOnly(p, stub, true, "n4c-newonly", stage);
        char label[160];
        snprintf(label, sizeof(label), "newonly/%s/absent: no partial target", stage);
        check(label, !QFile::exists(p));
    }
    return verdict("N4C-newonly");
}

// Kill-sweep children. Write a ready file before the write window so the
// parent can time the SIGKILL deterministically.
static bool readPayload(const QString &p, QByteArray &out)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return false;
    out = f.readAll();
    return !out.isEmpty();
}

static void signalReady(const QString &ready)
{
    QFile f(ready);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write("R", 1);
        f.flush();
        f.close();
    }
}

static int runSaver(const QString &mode, const QString &target,
                    const QString &payloadPath, const QString &ready)
{
    QByteArray payload;
    if (!readPayload(payloadPath, payload)) {
        fprintf(stderr, "RIG ERROR: payload unreadable\n");
        return 1;
    }
    signalReady(ready);
    if (mode == "saver-atomic") {
        atomicfile::writeBytes(target, payload, true, "n4c-killsweep");
        return 0;
    }
    // Legacy required-red control: truncate in place, write in chunks with a
    // small delay so a kill reliably lands mid-document and leaves it partial.
    QFile f(target);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
    const int chunk = 64 * 1024;
    for (int off = 0; off < payload.size(); off += chunk) {
        f.write(payload.constData() + off, qMin(chunk, payload.size() - off));
        ::usleep(80);
    }
    f.close();
    return 0;
}

} // namespace n4c

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        fprintf(stderr, "usage: n4c-gate <mode> <dir> | "
                        "n4c-gate <saver-atomic|saver-legacy> <target> <payload> <ready>\n");
        return 1;
    }
    const QString mode = argv[1];
    if (mode == "saver-atomic" || mode == "saver-legacy") {
        if (argc < 5) { fprintf(stderr, "saver needs target payload ready\n"); return 1; }
        return n4c::runSaver(mode, argv[2], argv[3], argv[4]);
    }
    const QString rd = argv[2];
    {
        QDir d(rd);
        if (d.exists() && !d.removeRecursively()) {
            fprintf(stderr, "RIG ERROR: could not clear rundir %s\n", qPrintable(rd));
            return 1;
        }
    }
    if (!QDir().mkpath(rd)) { fprintf(stderr, "RIG ERROR mkpath %s\n", qPrintable(rd)); return 1; }
    qInstallMessageHandler(n4c::msgHandler);
    if (mode == "failstages") return n4c::runFailStages(rd);
    if (mode == "formats")    return n4c::runFormats(rd);
    if (mode == "newonly")    return n4c::runNewOnly(rd);
    fprintf(stderr, "unknown mode: %s\n", qPrintable(mode));
    return 1;
}
