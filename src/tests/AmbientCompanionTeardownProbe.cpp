// AMBIENT-GHOST-AUDIO-01 checked-in two-sided gate probe.
//
// Loads a Player.qml view offscreen through a real QML Loader with a counting
// wrapper around the SHIPPED AmbientModeBackend and a fake looping mpv on PATH.
// Teardown is a Loader source reset with NO playbackEnded — the path that used
// to orphan the companion audio process. The probe always exits 0 when it ran;
// run_ambient_companion_teardown_case.py turns the JSON measurements into a
// verdict (FIXED / DEFECT / PASS) so the two arms share one assertion layer.
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "AmbientModeBackend.h"

namespace {

// Stub for the video-side mpv controller referenced by Player.qml. Only the
// surface the view touches; it never spawns a process itself.
class StubMpv : public QObject
{
    Q_OBJECT
public:
    int loadCalls = 0;

    Q_INVOKABLE void loadAndPlay(const QString &, float, int, int,
                                 const QStringList &, const QStringList &,
                                 bool, int, float, const QString &, bool,
                                 const QString &, bool, const QStringList &,
                                 float, bool, const QStringList &,
                                 const QString &, const QStringList &)
    {
        ++loadCalls;
    }
    Q_INVOKABLE void sendKey(const QString &) {}
    void fireEnded() { emit playbackEnded(0, 0, QStringLiteral("stopped")); }

signals:
    void playbackEnded(int finalPositionMs, int finalDurationMs, const QString &reason);
    void positionChanged(int);
    void durationChanged(int);
    void playlistPosChanged(int);
};

// Counting wrapper around the verbatim shipped backend. The redeclared
// Q_INVOKABLEs are what QML resolves; the base class's internal stopAudio()
// call from startAudio() binds statically to the base and is deliberately NOT
// counted, so the count reflects only view-driven (or explicit test) calls.
class CountingBackend : public AmbientModeBackend
{
    Q_OBJECT
public:
    using AmbientModeBackend::AmbientModeBackend;
    int startCalls = 0;
    int stopCalls = 0;

    Q_INVOKABLE void startAudio(const QStringList &paths, bool shuffle = false)
    {
        ++startCalls;
        AmbientModeBackend::startAudio(paths, shuffle);
    }
    Q_INVOKABLE void stopAudio()
    {
        ++stopCalls;
        AmbientModeBackend::stopAudio();
    }
};

QString g_pidFile;
QString g_registry;
QJsonArray g_notes;

void note(const QString &s) { g_notes.append(s); }

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (!t.hasExpired(ms)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

long readPid()
{
    QFile f(g_pidFile);
    if (!f.open(QIODevice::ReadOnly)) return -1;
    bool ok = false;
    const long pid = f.readAll().trimmed().toLong(&ok);
    return ok ? pid : -1;
}

bool pidAlive(long pid)
{
    if (pid <= 0) return false;
    if (::kill(static_cast<pid_t>(pid), 0) != 0) return false;
#ifdef Q_OS_LINUX
    QFile st(QStringLiteral("/proc/%1/stat").arg(pid));
    if (st.open(QIODevice::ReadOnly)) {
        const QByteArray d = st.readAll();
        const int r = d.lastIndexOf(')');
        if (r >= 0 && d.size() > r + 2) {
            const char c = d[r + 2];
            if (c == 'Z' || c == 'X' || c == 'x') return false;
        }
    }
#endif
    return true;
}

QJsonArray aliveRegistry()
{
    QJsonArray out;
    QFile f(g_registry);
    if (f.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : f.readAll().split('\n')) {
            bool ok = false;
            const long pid = line.trimmed().toLong(&ok);
            if (ok && pidAlive(pid)) out.append(static_cast<qint64>(pid));
        }
    }
    return out;
}

void killRegistry(bool hard)
{
    QFile f(g_registry);
    if (!f.open(QIODevice::ReadOnly)) return;
    for (const QByteArray &line : f.readAll().split('\n')) {
        bool ok = false;
        const long pid = line.trimmed().toLong(&ok);
        if (ok && pid > 0 && pidAlive(pid))
            ::kill(static_cast<pid_t>(pid), hard ? SIGKILL : SIGTERM);
    }
}

}  // namespace

int main(int argc, char **argv)
{
    QString caseName, viewFile, fakeMpv, resultPath, viewLabel = QStringLiteral("product");
    int cycles = 6;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a.startsWith(QStringLiteral("--case="))) caseName = a.mid(7);
        else if (a.startsWith(QStringLiteral("--view-file="))) viewFile = a.mid(12);
        else if (a.startsWith(QStringLiteral("--fake-mpv="))) fakeMpv = a.mid(11);
        else if (a.startsWith(QStringLiteral("--result="))) resultPath = a.mid(9);
        else if (a.startsWith(QStringLiteral("--view-label="))) viewLabel = a.mid(13);
        else if (a.startsWith(QStringLiteral("--cycles="))) cycles = a.mid(9).toInt();
    }
    if (caseName.isEmpty() || viewFile.isEmpty() || fakeMpv.isEmpty() || resultPath.isEmpty()) {
        std::fprintf(stderr, "usage: probe --case=C --view-file=P --fake-mpv=P --result=P "
                             "[--view-label=product|prefix] [--cycles=N]\n");
        return 2;
    }
    if (cycles < 1) cycles = 1;

    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        std::fprintf(stderr, "probe: could not create scratch dir\n");
        return 3;
    }
    g_pidFile = scratch.path() + QStringLiteral("/pid.txt");
    g_registry = scratch.path() + QStringLiteral("/registry.txt");

    // The backend's mpvbin::locate() checks <appDir>/mpv first, then PATH.
    // Put the compiled fake at <scratch>/bin/mpv and prepend that dir.
    const QDir binDir(scratch.path() + QStringLiteral("/bin"));
    binDir.mkpath(QStringLiteral("."));
    const QString fakeDest = binDir.filePath(QStringLiteral("mpv"));
    if (!QFile::copy(fakeMpv, fakeDest)) {
        std::fprintf(stderr, "probe: could not stage fake mpv at %s\n", qPrintable(fakeDest));
        return 4;
    }
    QFile::setPermissions(fakeDest, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                    QFile::ReadGroup | QFile::ExeGroup);

    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", (binDir.path() + QStringLiteral(":") +
                     QString::fromLocal8Bit(oldPath)).toLocal8Bit());
    qputenv("FAKE_MPV_PIDFILE", g_pidFile.toLocal8Bit());
    qputenv("FAKE_MPV_REGISTRY", g_registry.toLocal8Bit());
    qputenv("FAKE_MPV_MODE", "cooperative");

    // Copy the chosen view into a clean directory so the QML import sees only
    // Player.qml (the product view dir also holds Root.qml and other imports).
    const QString viewDirPath = scratch.path() + QStringLiteral("/views/") + viewLabel;
    QDir().mkpath(viewDirPath);
    const QString viewDest = viewDirPath + QStringLiteral("/Player.qml");
    if (!QFile::copy(viewFile, viewDest)) {
        std::fprintf(stderr, "probe: could not stage view %s\n", qPrintable(viewFile));
        return 5;
    }
    const QString viewDirUrl = QUrl::fromLocalFile(viewDirPath).toString();
    QFile hostQml(viewDirPath + QStringLiteral("/PlayerHost.qml"));
    if (hostQml.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        hostQml.write(QStringLiteral("import QtQuick\nimport \"%1\"\nPlayer { navParams: initialNavParams }\n")
                      .arg(viewDirUrl).toUtf8());
        hostQml.close();
    } else {
        std::fprintf(stderr, "probe: could not write PlayerHost.qml\n");
        return 6;
    }

    QQmlEngine engine;
    StubMpv mpv;
    CountingBackend backend(scratch.path() + QStringLiteral("/data"));
    QQmlContext *ctx = engine.rootContext();
    ctx->setContextProperty(QStringLiteral("mpvController"), QVariant::fromValue(&mpv));
    ctx->setContextProperty(QStringLiteral("ambientModeBackend"), QVariant::fromValue(&backend));

    const bool withAudio = caseName == QStringLiteral("teardown") ||
                           caseName == QStringLiteral("control-ended") ||
                           caseName == QStringLiteral("control-direct-stop") ||
                           caseName == QStringLiteral("control-repeat") ||
                           caseName == QStringLiteral("repeat-teardown");
    QVariantMap nav;
    nav[QStringLiteral("videoPath")] = scratch.path() + QStringLiteral("/fake-video.mp4");
    if (withAudio) nav[QStringLiteral("audioPath")] = scratch.path() + QStringLiteral("/fake-audio.mp3");
    ctx->setContextProperty(QStringLiteral("initialNavParams"), nav);

    const QUrl wrapperUrl = QUrl::fromLocalFile(viewDirPath + QStringLiteral("/PlayerHost.qml"));
    QQmlComponent wrapper(&engine, wrapperUrl);
    const bool wrapperOk = wrapper.status() == QQmlComponent::Ready;
    if (!wrapperOk)
        for (const auto &e : wrapper.errors()) note(QStringLiteral("wrapper-error: ") + e.description());
    ctx->setContextProperty(QStringLiteral("initialPlayerSource"),
                            wrapperOk ? wrapper.url().toString() : QString());

    QQmlComponent hostComp(&engine);
    hostComp.setData(QStringLiteral("import QtQuick\nItem { property url playerSource: initialPlayerSource; "
                                    "Loader { source: playerSource } }\n").toUtf8(),
                     QUrl::fromLocalFile(scratch.path() + QStringLiteral("/Host.qml")));
    QObject *host = hostComp.create();
    const bool hostOk = host != nullptr;
    if (!hostOk)
        for (const auto &e : hostComp.errors()) note(QStringLiteral("host-error: ") + e.description());

    const QString src = wrapper.url().toString();
    bool skipFinalTeardown = false;
    // A QML load failure (e.g. missing WorkerScript module on an older lane)
    // leaves host null; keep the drive loops safe so the JSON verdict still
    // reports the failed arm instead of segfaulting.
    auto setSource = [host](const QVariant &v) {
        if (host) host->setProperty("playerSource", v);
    };

    // Loader instantiates Player -> Component.onCompleted -> startAudio -> fake mpv.
    if (host) host->setProperty("playerSource", src);
    pump(400);
    const long pidAfterLoad = readPid();
    const bool aliveAfterLoad = pidAlive(pidAfterLoad);
    const int startAfterLoad = backend.startCalls;
    const int loadCalls = mpv.loadCalls;
    const int stopAtMeasure = backend.stopCalls;
    const QJsonArray regAfterLoad = aliveRegistry();

    if (caseName == QStringLiteral("control-ended")) {
        mpv.fireEnded();
        pump(500);
    } else if (caseName == QStringLiteral("control-direct-stop")) {
        backend.stopAudio();
        pump(500);
    } else if (caseName == QStringLiteral("control-empty-audio")) {
        backend.startAudio(QStringList(), false);
        pump(200);
    } else if (caseName == QStringLiteral("control-repeat")) {
        for (int i = 0; i < cycles; ++i) {
            mpv.fireEnded();
            pump(120);
            setSource(QVariant(QString()));
            pump(150);
            if (i < cycles - 1) { setSource(QVariant(src)); pump(300); }
        }
        skipFinalTeardown = true;
    } else if (caseName == QStringLiteral("repeat-teardown")) {
        // Repeated load -> Loader source reset WITHOUT playbackEnded. This is
        // the exact path that leaks a companion child on the pre-fix view.
        for (int i = 0; i < cycles; ++i) {
            setSource(QVariant(QString()));
            pump(450);
            if (i < cycles - 1) { setSource(QVariant(src)); pump(300); }
        }
        skipFinalTeardown = true;
    }

    // Teardown by Loader source reset, WITHOUT emitting playbackEnded.
    if (host && !skipFinalTeardown) {
        host->setProperty("playerSource", QVariant(QString()));
        pump(700);
    }

    const int stopFinal = backend.stopCalls;
    const bool aliveFinal = pidAlive(pidAfterLoad);
    const QJsonArray regFinal = aliveRegistry();
    int spawnedTotal = 0;
    {
        QFile reg(g_registry);
        if (reg.open(QIODevice::ReadOnly))
            for (const QByteArray &line : reg.readAll().split('\n'))
                if (!line.trimmed().isEmpty()) ++spawnedTotal;
    }

    QJsonObject r;
    r.insert(QStringLiteral("case"), caseName);
    r.insert(QStringLiteral("view"), viewLabel);
    r.insert(QStringLiteral("cycles"), cycles);
    r.insert(QStringLiteral("wrapperOk"), wrapperOk);
    r.insert(QStringLiteral("hostOk"), hostOk);
    r.insert(QStringLiteral("loadCalls"), loadCalls);
    r.insert(QStringLiteral("startCalls"), startAfterLoad);
    r.insert(QStringLiteral("stopCallsAtMeasure"), stopAtMeasure);
    r.insert(QStringLiteral("stopCallsFinal"), stopFinal);
    r.insert(QStringLiteral("pidAfterLoad"), static_cast<qint64>(pidAfterLoad));
    r.insert(QStringLiteral("aliveAfterLoad"), aliveAfterLoad);
    r.insert(QStringLiteral("aliveRegistryAfterLoad"), regAfterLoad);
    r.insert(QStringLiteral("aliveFinal"), aliveFinal);
    r.insert(QStringLiteral("aliveRegistryFinal"), regFinal);
    r.insert(QStringLiteral("spawnedTotal"), spawnedTotal);
    r.insert(QStringLiteral("notes"), g_notes);

    QFile out(resultPath);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        out.write(QJsonDocument(r).toJson(QJsonDocument::Indented));

    std::printf("RESULT case=%s view=%s load=%d start=%d stopMeasure=%d stopFinal=%d "
                "pid=%ld aliveLoad=%d aliveFinal=%d regFinal=%lld\n",
                qPrintable(caseName), qPrintable(viewLabel), loadCalls, startAfterLoad,
                stopAtMeasure, stopFinal, pidAfterLoad, aliveAfterLoad ? 1 : 0,
                aliveFinal ? 1 : 0, static_cast<long long>(regFinal.size()));

    // Leave the rig clean: reap any ghost child the pre-fix view orphaned.
    // All measurements were taken before this point.
    killRegistry(true);
    delete host;
    return 0;
}

#include "AmbientCompanionTeardownProbe.moc"
