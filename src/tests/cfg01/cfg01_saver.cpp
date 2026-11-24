// CFG-01 kill-sweep child: runs ONE real AppCore::save_setting against a
// prepared data dir, then exits. The parent (cfg01-killprobe) SIGKILLs it at
// controlled offsets to measure the real truncation window.
//
// Modes:
//   cfg01-saver genbig <datadir>   write the F2 template (68,410 B Indented).
//   cfg01-saver save <datadir>     touch ready marker, save_setting once.
// env CFG01_FSIZE_LIMIT=N: RLIMIT_FSIZE=N + ignore SIGXFSZ before the save
// (disk-full shape: write fails at N bytes; PRE ignores the error, FIX checks).
//
// Exit contract (§8 D-3): save mode exits 0 ONLY when save_setting succeeded.
// PRE's void surface always reports success (that silence is the defect);
// FIX's bool surface propagates refusal/failure as exit 1.
// Synthetic credentials only. No network.

#include "AppCore.h"
#include "cfg01_common.h"
#include "cfg01_fixtures.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: cfg01-saver <genbig|save> <datadir>\n"); return 1; }
    const QString mode = argv[1];
    const QString dataDir = argv[2];
    QDir(dataDir).mkpath(".");
    QDir(dataDir + "/..").mkpath("approot");

    if (mode == "genbig") {
        const QByteArray b = cfg01::bigFixtureBytes();
        if (!cfg01::writeFileBytes(dataDir + "/config.json", b)) return 1;
        printf("GENBIG bytes=%lld sha256=%s\n", (long long)b.size(), cfg01::sha(b).constData());
        return 0;
    }
    if (mode != "save") return 1;

    qInstallMessageHandler(cfg01::msgHandler);
    QGuiApplication app(argc, argv);
    AppCore core(dataDir + "/../approot", dataDir);

    const QByteArray limitEnv = qgetenv("CFG01_FSIZE_LIMIT");
    if (!limitEnv.isEmpty()) {
        signal(SIGXFSZ, SIG_IGN);
        struct rlimit rl;
        rl.rlim_cur = rl.rlim_max = (rlim_t)atoll(limitEnv.constData());
        if (setrlimit(RLIMIT_FSIZE, &rl) != 0) perror("setrlimit");
    }

    // Ready marker: parent starts the kill clock here. Written AFTER AppCore
    // construction, immediately before the save. Parent unlinks it BEFORE
    // spawn (fixture-ready race fix, mandatory).
    const QByteArray readyPath = (dataDir + "/../ready").toUtf8();
    int fd = ::open(readyPath.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { ssize_t ignored = ::write(fd, "1", 1); (void)ignored; ::close(fd); }

    // App-level key, same value length as fixture ("Video 2" -> "Video 3"):
    // a completed write is byte-size-identical to the template.
    const bool ok = cfg01::saveSettingShim(core, QString(), QStringLiteral("color_scheme"),
                                           QVariant("Video 3"));
    cfg01::writeMessages(dataDir + "/../saver-messages.log");
    _exit(ok ? 0 : 1);
}