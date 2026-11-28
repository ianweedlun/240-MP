#pragma once
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QTimer>
#include <QByteArray>

// Loopback plex.tv fixture for PLEX-JWT-REFRESH-LATCH-01. Healthy answers each
// auth hop; the stall modes accept and count the request but never complete a
// reply. Trickle modes write one byte every 100 ms, which keeps Qt's 5 s
// no-progress transfer timer reset — the exact shape that used to latch the
// guard forever because nothing ever bounded the TOTAL exchange time.
class PlexAuthFixture : public QObject {
    Q_OBJECT
public:
    enum Mode { Healthy, BlackholeNonce, BlackholeToken, TrickleNonce, TrickleToken };

    explicit PlexAuthFixture(QObject *parent = nullptr);
    bool start();
    quint16 port() const { return m_server.serverPort(); }
    void setMode(Mode mode) { m_mode = mode; }
    void resetCounters();

    int nonceGets = 0;
    int tokenPosts = 0;
    int resourceGets = 0;

    int nonceStatus = 200;
    QByteArray nonceBody = R"({"nonce":"fixture-nonce"})";
    int tokenStatus = 200;
    QByteArray tokenBody;
    int resourceStatus = 200;

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

private:
    void respond(QTcpSocket *s, int status, const QByteArray &body);
    void startTrickle(QTcpSocket *s);
    void stopTrickle(QTcpSocket *s);

    QTcpServer m_server;
    Mode m_mode = Healthy;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QHash<QTcpSocket *, bool> m_counted;
    QHash<QTcpSocket *, QTimer *> m_trickles;
};
