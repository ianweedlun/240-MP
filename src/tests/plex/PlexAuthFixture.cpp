#include "PlexAuthFixture.h"

#include <QTimer>
#include <QDebug>

PlexAuthFixture::PlexAuthFixture(QObject *parent) : QObject(parent) {}

bool PlexAuthFixture::start() {
    connect(&m_server, &QTcpServer::newConnection, this, &PlexAuthFixture::onNewConnection);
    return m_server.listen(QHostAddress::LocalHost, 0);
}

void PlexAuthFixture::resetCounters() {
    nonceGets = tokenPosts = resourceGets = 0;
}

void PlexAuthFixture::onNewConnection() {
    while (m_server.hasPendingConnections()) {
        QTcpSocket *s = m_server.nextPendingConnection();
        m_buffers.insert(s, QByteArray());
        m_counted.insert(s, false);
        connect(s, &QTcpSocket::readyRead, this, &PlexAuthFixture::onReadyRead);
        connect(s, &QTcpSocket::disconnected, this, &PlexAuthFixture::onDisconnected);
    }
}

void PlexAuthFixture::onDisconnected() {
    auto *s = qobject_cast<QTcpSocket *>(sender());
    if (!s) return;
    stopTrickle(s);
    m_buffers.remove(s);
    m_counted.remove(s);
    s->deleteLater();
}

void PlexAuthFixture::onReadyRead() {
    auto *s = qobject_cast<QTcpSocket *>(sender());
    if (!s) return;
    QByteArray &buf = m_buffers[s];
    buf += s->readAll();

    const int lineEnd = buf.indexOf("\r\n");
    if (lineEnd < 0) return;
    const QByteArray line = buf.left(lineEnd);

    if (!m_counted.value(s, false)) {
        m_counted.insert(s, true);
        if (line.startsWith("GET /api/v2/auth/nonce")) ++nonceGets;
        else if (line.startsWith("POST /api/v2/auth/token")) ++tokenPosts;
        else if (line.startsWith("GET /api/v2/resources")) ++resourceGets;
    }

    // Stall decisions apply to every chunk on the socket, not just the first
    // line: a POST body can arrive in a later readyRead, and answering it
    // would turn a trickle stall into a corrupt-but-complete HTTP reply.
    const bool stallNonce = line.startsWith("GET /api/v2/auth/nonce");
    const bool stallToken = line.startsWith("POST /api/v2/auth/token");
    if ((m_mode == TrickleNonce && stallNonce) ||
        (m_mode == TrickleToken && stallToken)) {
        startTrickle(s);
        return;
    }
    if ((m_mode == BlackholeNonce && stallNonce) ||
        (m_mode == BlackholeToken && stallToken)) return;

    const int headerEnd = buf.indexOf("\r\n\r\n");
    if (headerEnd < 0) return;
    int contentLength = 0;
    const QByteArray headers = buf.left(headerEnd);
    int pos = 0;
    while (pos <= headers.size()) {
        const int nl = headers.indexOf('\n', pos);
        const QByteArray h = headers.mid(pos, (nl < 0 ? headers.size() : nl) - pos);
        if (h.toLower().startsWith("content-length:")) {
            contentLength = h.mid(h.indexOf(':') + 1).trimmed().toInt();
            break;
        }
        if (nl < 0) break;
        pos = nl + 1;
    }
    if (buf.size() < headerEnd + 4 + contentLength) return;

    if (line.startsWith("GET /api/v2/auth/nonce")) respond(s, nonceStatus, nonceBody);
    else if (line.startsWith("POST /api/v2/auth/token")) respond(s, tokenStatus, tokenBody);
    else if (line.startsWith("GET /api/v2/resources")) respond(s, resourceStatus, "{}");
    else respond(s, 404, "{}");
}

void PlexAuthFixture::startTrickle(QTcpSocket *s) {
    if (m_trickles.contains(s)) return;
    if (s->state() != QAbstractSocket::ConnectedState) return;
    // A syntactically valid response whose declared body never arrives. Qt
    // accepts the status line, then keeps the reply open draining the body;
    // each trickled byte resets the 5 s no-progress timer. Only a hard total
    // deadline can end this exchange.
    s->write("HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 100000\r\n"
             "Connection: close\r\n\r\n");
    s->flush();
    auto *t = new QTimer(s);
    t->setInterval(100); // far below the shipped 5000 ms no-progress deadline
    connect(t, &QTimer::timeout, s, [this, s, t]() {
        if (s->state() != QAbstractSocket::ConnectedState) { stopTrickle(s); return; }
        s->write("x");
        s->flush();
        Q_UNUSED(t);
    });
    t->start();
    m_trickles.insert(s, t);
}

void PlexAuthFixture::stopTrickle(QTcpSocket *s) {
    if (auto *t = m_trickles.take(s)) { t->stop(); t->deleteLater(); }
}

void PlexAuthFixture::respond(QTcpSocket *s, int status, const QByteArray &body) {
    if (!s || s->state() != QAbstractSocket::ConnectedState) return;
    stopTrickle(s);
    QByteArray out;
    out += "HTTP/1.1 " + QByteArray::number(status) + " Status\r\n";
    out += "Content-Type: application/json\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += body;
    s->write(out);
    s->flush();
    QTimer::singleShot(0, s, &QTcpSocket::disconnectFromHost);
}
