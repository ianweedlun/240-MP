// Contract probe for the one-terminal-result invariant (ROB-07 regression).
//
// Drives the REAL YtInnertubeResolver against the loopback fixture proxy
// (fixture_proxy.py) and records EVERY finished() callback for the accepted
// resolve() key over a fixed observation window — not just the first result.
// Pure measurement instrument: it always exits 0 when it ran to completion;
// the contract assertions live in run_contract_case.py, which also owns the
// proxy lifecycle and the request-log checks. No live network, no mpv, no
// display (QCoreApplication only).
//
//   yt-resolver-contract-probe PORT CERT_PATH KEY OBSERVE_MS
//
// Output lines (comma-separated key=value, reason last so a reason containing
// commas cannot corrupt the other fields):
//   meta,timeout_ms=<kTimeoutMs>
//   accepted,key=<KEY>            (or refused,key=<KEY> and exit 66)
//   event,wall_ms=N,resolve_ms=N,ok=0|1,key=<KEY>,reason=<text>
//   summary,events=N
//   keycount,key=<KEY>,n=N,ok=N   (one line per key that produced callbacks)
//
// Exit codes: 0 ran the full window; 64 bad usage; 65 cert load failed;
// 66 resolve() refused. OBSERVE_MS is measured from resolve() acceptance.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkProxy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTextStream>
#include <QTimer>
#include <map>
#include <vector>

#include "../YtInnertubeResolver.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    if (argc != 5) {
        out << "usage: yt-resolver-contract-probe PORT CERT_PATH KEY OBSERVE_MS\n";
        return 64;
    }
    const int port = QString::fromLocal8Bit(argv[1]).toInt();
    const QString certPath = QString::fromLocal8Bit(argv[2]);
    const QString key = QString::fromLocal8Bit(argv[3]);
    const int observeMs = QString::fromLocal8Bit(argv[4]).toInt();
    if (port <= 0 || port > 65535 || key.isEmpty() || observeMs <= 0) {
        out << "usage: yt-resolver-contract-probe PORT CERT_PATH KEY OBSERVE_MS\n";
        return 64;
    }

    // Trust the ephemeral fixture cert (generated per run by the orchestrator)
    // on top of the system roots, and route every request through the loopback
    // CONNECT proxy — the resolver and the vendored library both honour the
    // application proxy, so no endpoint override seam is needed.
    const QList<QSslCertificate> certs = QSslCertificate::fromPath(certPath);
    if (certs.isEmpty()) {
        out << "certificate-load-failed\n";
        return 65;
    }
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    QList<QSslCertificate> roots = ssl.caCertificates();
    roots.append(certs);
    ssl.setCaCertificates(roots);
    QSslConfiguration::setDefaultConfiguration(ssl);
    QNetworkProxy::setApplicationProxy(
        QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), port));

    out << "meta,timeout_ms=" << YtInnertubeResolver::kTimeoutMs << "\n";
    out.flush();

    YtInnertubeResolver resolver;
    QElapsedTimer wall;
    struct Event { qint64 wallMs; qint64 resolveMs; bool ok; QString reason; QString key; };
    std::vector<Event> events;
    struct Count { int n = 0; int ok = 0; };
    std::map<QString, Count> perKey;

    QObject::connect(&resolver, &YtInnertubeResolver::finished, &app,
        [&](const YtResolveResult &r) {
            events.push_back({wall.elapsed(), r.elapsedMs, r.ok, r.reason, r.key});
            Count &c = perKey[r.key];
            ++c.n;
            if (r.ok)
                ++c.ok;
            out << "event,wall_ms=" << wall.elapsed() << ",resolve_ms=" << r.elapsedMs
                << ",ok=" << (r.ok ? 1 : 0) << ",key=" << r.key
                << ",reason=" << r.reason << "\n";
            out.flush();
        });

    wall.start();
    if (!resolver.resolve(QStringLiteral("fixturevid"), QStringLiteral("480p"), key)) {
        out << "refused,key=" << key << "\n";
        out.flush();
        return 66;
    }
    out << "accepted,key=" << key << "\n";
    out.flush();

    // Run the FULL window even after a terminal callback: the invariant is
    // "exactly one result per accepted resolve()", so a phantom second result
    // arriving late must still be observed and counted.
    QTimer::singleShot(observeMs, &app, &QCoreApplication::quit);
    app.exec();

    out << "summary,events=" << int(events.size()) << "\n";
    for (const auto &kv : perKey) {
        out << "keycount,key=" << kv.first << ",n=" << kv.second.n
            << ",ok=" << kv.second.ok << "\n";
    }
    out.flush();
    return 0;
}
