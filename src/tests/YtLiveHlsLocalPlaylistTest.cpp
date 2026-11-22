// CRT repro gate for #145: the resolver takes the HLS route for a finished
// live event, shapes the master to a LOCAL .m3u8, and hands that file to mpv.
// ffmpeg's whitelist for a local top-level playlist is only
// file,crypto,data — every https variant is rejected, mpv demuxes zero
// streams, and Player.qml shows "PLAYBACK FAILED ... reason=launch-failed".
//
// The original contract cases proved the routing decision with a canned
// Player response but never launched mpv, so this exact failure sailed
// through green. This gate runs the REAL mpv binary against a real local
// master whose variants are served over loopback HTTP. It builds the launch
// args from MpvController::extraArgsForLocalHls — the same helper production
// uses — so reverting the production fix turns the playback case RED.
//
// No YouTube network traffic: the master and its single TS segment come
// from a local QTcpServer. ffmpeg generates the segment once at init.
#include <QCoreApplication>
#include <QHostAddress>
#include <QScopedPointer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include "player/MpvController.h"

namespace {

// Minimal HLS origin: one variant playlist and one mpegts segment. mpv asks
// for both over loopback http; a local master would be whitelisted but http
// is not, which is the exact defect shape.
class FixtureHttp final : public QTcpServer {
public:
    QByteArray variant;
    QByteArray segment;

protected:
    void incomingConnection(qintptr fd) override
    {
        auto *s = new QTcpSocket(this);
        s->setSocketDescriptor(fd);
        connect(s, &QTcpSocket::readyRead, s, [this, s]() {
            const QByteArray req = s->readAll();
            QByteArray body;
            QByteArray type;
            if (req.startsWith("GET /variant.m3u8")) {
                body = variant;
                type = "application/vnd.apple.mpegurl";
            } else if (req.startsWith("GET /seg.ts")) {
                body = segment;
                type = "video/mp2t";
            } else {
                s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
                s->flush();
                s->disconnectFromHost();
                return;
            }
            s->write("HTTP/1.1 200 OK\r\nContent-Type: " + type
                     + "\r\nContent-Length: " + QByteArray::number(body.size())
                     + "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n" + body);
            s->flush();
            s->disconnectFromHost();
        });
    }
};

// Run the real mpv with the production arg set and return everything it said.
// Pump the event loop so the fixture origin serves mpv's requests.
QByteArray runMpv(const QString &mpv, const QString &master, const QString &log,
                  const QStringList &extraArgs, int timeoutMs)
{
    QProcess proc;
    QStringList args{QStringLiteral("--no-config"),
                     QStringLiteral("--vo=null"),
                     QStringLiteral("--ao=null"),
                     QStringLiteral("--hwdec=no"),
                     QStringLiteral("--msg-level=all=v"),
                     QStringLiteral("--log-file=%1").arg(log)};
    args += extraArgs;
    args << master;
    proc.start(mpv, args, QIODevice::ReadOnly);

    QByteArray out;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        out += proc.readAllStandardOutput();
        out += proc.readAllStandardError();
        if (out.contains("playback restart complete")
            || out.contains("first video frame after restart shown")
            || out.contains("not on whitelist"))
            break;
        QThread::msleep(10);
    }
    if (proc.state() != QProcess::NotRunning) {
        proc.kill();
        proc.waitForFinished(2000);
    }
    return out;
}

} // namespace

class YtLiveHlsLocalPlaylistTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void localMasterPlaysWithProductionArgs();
    void localMasterFailsWithoutWhitelist();

private:
    QScopedPointer<QTemporaryDir> m_dir;
    QString m_mpv;
    QString m_master;
    FixtureHttp m_origin;
};

void YtLiveHlsLocalPlaylistTest::initTestCase()
{
    m_mpv = QStandardPaths::findExecutable(QStringLiteral("mpv"));
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (m_mpv.isEmpty() || ffmpeg.isEmpty())
        QSKIP("mpv/ffmpeg not available — the mpv playback repro needs both");

    m_dir.reset(new QTemporaryDir());
    QVERIFY(m_dir->isValid());

    QProcess ff;
    const QString segment = m_dir->path() + QStringLiteral("/seg.ts");
    ff.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                      QStringLiteral("-f"), QStringLiteral("lavfi"),
                      QStringLiteral("-i"), QStringLiteral("testsrc=duration=3:size=320x240:rate=15"),
                      QStringLiteral("-f"), QStringLiteral("lavfi"),
                      QStringLiteral("-i"), QStringLiteral("sine=frequency=440:duration=3"),
                      QStringLiteral("-c:v"), QStringLiteral("mpeg2video"),
                      QStringLiteral("-c:a"), QStringLiteral("mp2"),
                      QStringLiteral("-f"), QStringLiteral("mpegts"),
                      QStringLiteral("-y"), segment});
    QVERIFY2(ff.waitForFinished(30000) && ff.exitCode() == 0 && QFile::exists(segment),
             "ffmpeg could not build the fixture TS segment");
    QFile f(segment);
    QVERIFY(f.open(QIODevice::ReadOnly));
    m_origin.segment = f.readAll();
    QVERIFY(!m_origin.segment.isEmpty());

    QVERIFY(m_origin.listen(QHostAddress::LocalHost, 0));
    const QByteArray port = QByteArray::number(m_origin.serverPort());
    m_origin.variant = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
                       "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:2.0,\n"
                       "http://127.0.0.1:" + port + "/seg.ts\n#EXT-X-ENDLIST\n";

    m_master = m_dir->path() + QStringLiteral("/master.m3u8");
    QFile master(m_master);
    QVERIFY(master.open(QIODevice::WriteOnly | QIODevice::Truncate));
    master.write("#EXTM3U\n#EXT-X-VERSION:3\n"
                 "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=320x240\n"
                 "http://127.0.0.1:" + port + "/variant.m3u8\n");
}

void YtLiveHlsLocalPlaylistTest::localMasterPlaysWithProductionArgs()
{
    // No structural assertion here on purpose: the gate must fail on mpv's
    // actual "not on whitelist" / no-playback behavior when production stops
    // passing the option, not merely on a changed helper signature.
    const QStringList args = MpvController::extraArgsForLocalHls(m_master);
    const QByteArray out = runMpv(m_mpv, m_master, m_dir->path() + "/with.log", args, 20000);
    QVERIFY2(out.contains("playback restart complete")
             || out.contains("first video frame after restart shown"),
             out.left(1500).constData());
    QVERIFY2(!out.contains("not on whitelist"), out.left(1500).constData());
}

void YtLiveHlsLocalPlaylistTest::localMasterFailsWithoutWhitelist()
{
    // RED control: the exact failure seen on the CRT (PLAYBACK FAILED from
    // launch-failed) with no whitelist option. Proves the gate reproduces the
    // defect rather than just passing on any local playlist.
    const QByteArray out = runMpv(m_mpv, m_master, m_dir->path() + "/without.log", {}, 12000);
    QVERIFY2(out.contains("not on whitelist"), out.left(1500).constData());
    QVERIFY2(!out.contains("playback restart complete"), out.left(1500).constData());
}

QTEST_MAIN(YtLiveHlsLocalPlaylistTest)
#include "YtLiveHlsLocalPlaylistTest.moc"
