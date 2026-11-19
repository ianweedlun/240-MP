// Lab gate probe: resolve through the REAL YtInnertubeResolver, then make mpv
// decode the result. Not a ctest test — it needs the network. Run manually:
//   ./build-test/yt-resolver-probe <videoId> [required audio xtags substring]
//
// Why this exists: a curl Range probe is not a playability proof. IOS-client
// URLs answer bounded ranges with 206 while 403ing the open-ended
// `Range: bytes=0-` that ffmpeg actually sends, so a curl-only gate passed
// while every real playback failed. This probe closes that hole in the lab.
#include <QCoreApplication>
#include <QProcess>
#include <QTimer>
#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <QUrl>
#include <QUrlQuery>

#include "../YtInnertubeResolver.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString vid = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                 : QStringLiteral("dh2nQsbuzrc");
    const QString expectedXtags = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    YtInnertubeResolver resolver;
    int exitCode = 1;

    QObject::connect(&resolver, &YtInnertubeResolver::finished, &app,
                     [&](const YtResolveResult &r) {
        QTextStream out(stdout);
        out << "resolve ok=" << r.ok << " ms=" << r.elapsedMs
            << " reason=" << r.reason << "\n";
        if (!r.ok) { out << "GATE: FAIL (resolve)\n"; exitCode = 1; app.exit(1); return; }
        out << "video=" << r.videoUrl.left(90) << "...\n";
        out << "audio=" << (r.audioUrl.isEmpty() ? "(muxed)" : r.audioUrl.left(90) + "...") << "\n";
        const QString xtags = QUrlQuery(QUrl(r.audioUrl))
                                  .queryItemValue(QStringLiteral("xtags"),
                                                  QUrl::FullyDecoded);
        out << "audio-xtags=" << (xtags.isEmpty() ? QStringLiteral("(none)") : xtags) << "\n";

        QStringList args{ "--vo=null", "--ao=null", "--no-config", "--length=6",
                          "--msg-level=all=v", "--log-file=/tmp/yt-probe-mpv.log" };
        if (!r.audioUrl.isEmpty())
            args << QStringLiteral("--audio-file=%1").arg(r.audioUrl);
        args << r.videoUrl;

        QProcess mpv;
        mpv.start(QStringLiteral("mpv"), args);
        mpv.waitForFinished(120000);
        QFile f(QStringLiteral("/tmp/yt-probe-mpv.log"));
        f.open(QIODevice::ReadOnly);
        const QString log = QString::fromUtf8(f.readAll());
        const bool started = log.contains(QStringLiteral("Starting playback"));
        const bool vdec = log.contains(QStringLiteral("Selected decoder: h264"));
        const bool adec = log.contains(QStringLiteral("Selected decoder: aac")) || r.single;
        const bool no403 = !log.contains(QStringLiteral("403"));
        const bool expectedTrack = expectedXtags.isEmpty() || xtags.contains(expectedXtags);
        out << "mpv: started=" << started << " h264=" << vdec
            << " aac=" << adec << " no403=" << no403
            << " expectedTrack=" << expectedTrack << "\n";
        const bool pass = started && vdec && adec && no403 && expectedTrack;
        out << "GATE: " << (pass ? "PASS" : "FAIL") << "\n";
        exitCode = pass ? 0 : 1;
        app.exit(exitCode);
    });

    if (!resolver.resolve(vid, QStringLiteral("480p"), QStringLiteral("probe"))) {
        QTextStream(stdout) << "GATE: FAIL (resolve refused)\n";
        return 1;
    }
    QTimer::singleShot(150000, &app, [&]() { app.exit(1); });
    app.exec();
    return exitCode;
}