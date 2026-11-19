// MEM-02 regression probe: visitorData homepage size cap + bounded scan.
//
// Drives the REAL YtInnertubeResolver against the loopback CONNECT+TLS fixture
// (vd_cap_fixture_proxy.py) and records EVERY finished() callback for the
// accepted resolve() key over a fixed observation window — not just the first
// result — plus per-event RSS/HWM so the orchestrator can assert bounded
// memory. Pure measurement instrument: it always exits 0 when it ran to
// completion; the two-sided assertions live in run_vd_cap_case.py, which owns
// the proxy lifecycle, the request log, and the bytes-consumed accounting.
// No live network, no mpv, no display (QCoreApplication only).
//
//   yt-visitordata-cap-probe PORT CERT_PATH KEY OBSERVE_MS
//
// Output lines (comma-separated key=value; reason LAST so a reason containing
// commas cannot corrupt the other fields):
//   meta,timeout_ms=<kTimeoutMs>,cap_bytes=<kMaxHomepageBytes>
//   accepted,key=<KEY>                 (or refused,key=<KEY> and exit 66)
//   event,wall_ms=N,resolve_ms=N,ok=0|1,key=<KEY>,rss_kb=N,hwm_kb=N,reason=<text>
//   summary,events=N,rss_kb=N,hwm_kb=N
//
// rss_kb/hwm_kb come from /proc/self/status (Linux); -1 where unavailable
// (e.g. macOS) — the orchestrator then skips the HWM-delta assertion and
// keeps the behavioral + bytes-consumed assertions.
//
// Exit codes: 0 ran the full window; 64 bad usage; 65 cert load failed;
// 66 resolve() refused. OBSERVE_MS is measured from resolve() acceptance.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QNetworkProxy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTextStream>
#include <QTimer>
#include <vector>

#include "../YtInnertubeResolver.h"

// Pre-MEM-02 snapshots have no kMaxHomepageBytes. Report cap_bytes=none there
// so the SAME probe source builds against both sides of the fix (the
// orchestrator's fail-before gate compiles this probe against the pre-fix
// resolver); the behavioral invariants, not this constant, carry the gate.
template <typename T>
concept HasHomepageCap = requires { T::kMaxHomepageBytes; };

// Template so the discarded if-constexpr branch is not instantiated against
// pre-fix snapshots (in a non-template function it would still be name-checked).
template <typename T>
static void printMeta(QTextStream &out)
{
    out << "meta,timeout_ms=" << T::kTimeoutMs;
    if constexpr (HasHomepageCap<T>)
        out << ",cap_bytes=" << T::kMaxHomepageBytes;
    else
        out << ",cap_bytes=none";
    out << "\n";
}

static long rssKb(const char *key)
{
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly))
        return -1;
    const QList<QByteArray> rows = f.readAll().split('\n');
    const QByteArray prefix(key);
    for (const QByteArray &row : rows) {
        if (!row.startsWith(prefix))
            continue;
        const QList<QByteArray> fields = row.simplified().split(' ');
        return fields.size() > 1 ? fields[1].toLong() : -1;
    }
    return -1;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    if (argc != 5) {
        out << "usage: yt-visitordata-cap-probe PORT CERT_PATH KEY OBSERVE_MS\n";
        return 64;
    }
    const int port = QString::fromLocal8Bit(argv[1]).toInt();
    const QString certPath = QString::fromLocal8Bit(argv[2]);
    const QString key = QString::fromLocal8Bit(argv[3]);
    const int observeMs = QString::fromLocal8Bit(argv[4]).toInt();
    if (port <= 0 || port > 65535 || key.isEmpty() || observeMs <= 0) {
        out << "usage: yt-visitordata-cap-probe PORT CERT_PATH KEY OBSERVE_MS\n";
        return 64;
    }

    // Trust the ephemeral fixture cert (generated per run by the orchestrator)
    // on top of the system roots, and route every request through the loopback
    // CONNECT proxy — the resolver's dedicated QNAM and the vendored library
    // both honour the application proxy, so no endpoint override seam is
    // needed and no real network is reachable.
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

    printMeta<YtInnertubeResolver>(out);
    out.flush();

    YtInnertubeResolver resolver;
    QElapsedTimer wall;
    struct Event { qint64 wallMs; qint64 resolveMs; bool ok; QString reason; QString key; };
    std::vector<Event> events;

    QObject::connect(&resolver, &YtInnertubeResolver::finished, &app,
        [&](const YtResolveResult &r) {
            events.push_back({wall.elapsed(), r.elapsedMs, r.ok, r.reason, r.key});
            out << "event,wall_ms=" << wall.elapsed() << ",resolve_ms=" << r.elapsedMs
                << ",ok=" << (r.ok ? 1 : 0) << ",key=" << r.key
                << ",rss_kb=" << rssKb("VmRSS:") << ",hwm_kb=" << rssKb("VmHWM:")
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

    // Run the FULL window even after a terminal callback: exactly one result
    // per accepted resolve() is part of the contract, so a phantom second
    // result (e.g. a re-entrant finished() after abort()) must be observed.
    QTimer::singleShot(observeMs, &app, &QCoreApplication::quit);
    app.exec();

    out << "summary,events=" << int(events.size())
        << ",rss_kb=" << rssKb("VmRSS:") << ",hwm_kb=" << rssKb("VmHWM:") << "\n";
    out.flush();
    return 0;
}