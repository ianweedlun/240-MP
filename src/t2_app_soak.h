#pragma once
// Scratch-only T2 application observer. Included into a COPY of src/main.cpp;
// the product tree is never edited. QNetwork isolation is explicit: when a
// fixture port is supplied, every Qt request uses that loopback CONNECT proxy.
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlProperty>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTextStream>
#include <QTimer>
#include <QEvent>
#include <QVariant>
#include <cstdlib>
#include <malloc.h>

class T2AppSoak : public QObject {
public:
    static void configureNetworkFromEnv()
    {
        bool ok = false;
        const int port = qEnvironmentVariableIntValue("TORTURE_PROXY_PORT", &ok);
        if (!ok || port <= 0) return;
        QNetworkProxy::setApplicationProxy(
            QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), port));
        const QString caPath = qEnvironmentVariable("TORTURE_CA");
        const QList<QSslCertificate> certs = QSslCertificate::fromPath(caPath);
        if (!certs.isEmpty()) {
            QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
            QList<QSslCertificate> roots = ssl.caCertificates();
            roots.append(certs);
            ssl.setCaCertificates(roots);
            QSslConfiguration::setDefaultConfiguration(ssl);
        }
    }

    T2AppSoak(QQmlApplicationEngine *engine, QCoreApplication *app)
        : QObject(app), m_engine(engine), m_app(app),
          m_mode(qEnvironmentVariable("TORTURE_APP_SCENARIO")),
          m_root(qEnvironmentVariable("APP_ROOT"))
    {
        if (m_mode == QLatin1String("M1")) QTimer::singleShot(500, this, [this] { startM1(); });
        if (m_mode == QLatin1String("M4")) QTimer::singleShot(0, this, [this] { startM4(); });
    }

private:
    QQmlApplicationEngine *m_engine;
    QCoreApplication *m_app;
    QString m_mode, m_root;
    QObject *m_loader = nullptr;
    QStringList m_modules;
    QStringList m_ids;
    int m_transitions = 0;
    int m_errors = 0;
    int m_baselineRss = 0;
    int m_baselineObjects = 0;
    int m_baselineRoots = 0;
    int m_warmRss = 0;
    int m_target = 500;          // t_293db4ec: TORTURE_M1_TRANSITIONS env-driven stop
    bool m_control = false;      // t_293db4ec: TORTURE_M1_CONTROL=modulelist control
    QJsonArray m_samples;
    QElapsedTimer m_clock;

    static int rssKb()
    {
        QFile f(QStringLiteral("/proc/self/statm"));
        if (!f.open(QIODevice::ReadOnly)) return -1;
        const QList<QByteArray> fields = f.readAll().simplified().split(' ');
        bool ok = false;
        const qulonglong pages = fields.value(1).toULongLong(&ok);
        return ok ? int((pages * static_cast<qulonglong>(::sysconf(_SC_PAGESIZE))) / 1024) : -1;
    }
    static int fdCount()
    {
        return QDir(QStringLiteral("/proc/self/fd")).count() - 2;
    }
    int objectCount() const
    {
        int n = 0;
        const auto roots = m_engine->rootObjects();
        for (QObject *root : roots) n += 1 + root->findChildren<QObject *>().size();
        return n;
    }
    void collect()
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        m_engine->collectGarbage();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        // processEvents() can post fresh deferred deletes; a second flush keeps
        // the sampled QObject count from oscillating by one between baseline
        // and final (a timing artifact, not retention).
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        malloc_trim(0);
    }
    QJsonObject sample(const QString &label)
    {
        collect();
        QJsonObject row{
            {QStringLiteral("label"), label},
            {QStringLiteral("transitions"), m_transitions},
            {QStringLiteral("elapsed_ms"), qint64(m_clock.elapsed())},
            {QStringLiteral("rss_kb"), rssKb()},
            {QStringLiteral("fd_count"), fdCount()},
            {QStringLiteral("root_objects"), int(m_engine->rootObjects().size())},
            {QStringLiteral("window_qobjects"), objectCount()},
            {QStringLiteral("load_errors"), m_errors}
        };
        m_samples.append(row);
        QTextStream(stdout) << "T2_SAMPLE " << QJsonDocument(row).toJson(QJsonDocument::Compact) << Qt::endl;
        return row;
    }
    void finish(const QString &scenario, const QJsonObject &extra, bool pass)
    {
        QJsonObject result{{QStringLiteral("scenario"), scenario},
                           {QStringLiteral("pass"), pass},
                           {QStringLiteral("samples"), m_samples}};
        for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) result.insert(it.key(), it.value());
        QTextStream(stdout) << "T2_RESULT " << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
        m_app->exit(pass ? 0 : 2);
    }

    QObject *findMainLoader()
    {
        const auto roots = m_engine->rootObjects();
        if (roots.isEmpty()) return nullptr;
        const auto children = roots.first()->findChildren<QObject *>();
        for (QObject *o : children) {
            if (QLatin1String(o->metaObject()->className()) != QLatin1String("QQuickLoader")) continue;
            const QString source = QQmlProperty::read(o, QStringLiteral("source")).toString();
            if (source.contains(QLatin1String("views/ModuleList.qml"))) return o;
        }
        return nullptr;
    }
    // Owner Decision D3(b) (issue #135): the ~1.5-1.8 MB plain-build RSS rise
    // is one-time QtQml/QV4 warmup, not per-transition retention. Gate on the
    // POST-WARMUP SLOPE plus plateau evidence, never on the absolute delta.
    // A slope of 1 KB/transition over 500 transitions equals the original
    // 500 KB budget, so this re-specifies the measurement, it does not relax it.
    static constexpr double kSlopeLimitKbPerTransition = 1.0;
    static constexpr int kPlateauLimitKb = 128;   // late ~100-transition window
    // Literal defect-report gate: < 500 KB over the 500 measured transitions.
    // Measured from a fully warmed baseline, not from process start: the first
    // ~100 transitions are one-time QtQml/QV4 cache warmup (owner D3(b)).
    static constexpr int kRssDeltaLimitKb = 500;
    // Owner Decision D3(b) (issue #135): the one-time QtQml/QV4 cache warmup
    // is flat by ~t100. Warm five full passes over all 11 module roots before
    // the baseline so the measured window is steady-state, not the ramp.
    static constexpr int kWarmupCycles = 5;
    static constexpr int kWarmupSkipTransitions = 150;

    // Ordinary least-squares slope of rss_kb vs transition index over samples
    // at or beyond the warmup window. Returns KB per transition (~0 = plateau)
    // and writes the number of points used to *count* (too few => not gated).
    static double postWarmupSlope(const QJsonArray &samples, int *count = nullptr)
    {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int n = 0;
        for (const QJsonValue &v : samples) {
            const QJsonObject row = v.toObject();
            const int t = row.value(QStringLiteral("transitions")).toInt();
            if (t < kWarmupSkipTransitions) continue;
            const double x = double(t);
            const double y = row.value(QStringLiteral("rss_kb")).toDouble();
            sx += x; sy += y; sxx += x * x; sxy += x * y; ++n;
        }
        if (count) *count = n;
        if (n < 3) return 0.0;
        const double denom = n * sxx - sx * sx;
        return denom > 0 ? (n * sxy - sx * sy) / denom : 0.0;
    }

    // RSS growth across the last ~100 transitions: the plateau evidence that
    // distinguishes bounded warmup from a linear per-transition leak.
    static int lateWindowGrowth(const QJsonArray &samples)
    {
        int maxT = -1, lastRss = -1;
        for (const QJsonValue &v : samples) {
            const QJsonObject row = v.toObject();
            const int t = row.value(QStringLiteral("transitions")).toInt();
            if (t <= 0) continue;
            maxT = t; lastRss = row.value(QStringLiteral("rss_kb")).toInt();
        }
        if (maxT < 0 || lastRss < 0) return 0;
        const int windowStart = qMax(1, maxT - 100);
        int startRss = -1, best = maxT + 1;
        for (const QJsonValue &v : samples) {
            const QJsonObject row = v.toObject();
            const int t = row.value(QStringLiteral("transitions")).toInt();
            if (t <= 0 || t < windowStart || t >= best) continue;
            best = t; startRss = row.value(QStringLiteral("rss_kb")).toInt();
        }
        return startRss >= 0 ? lastRss - startRss : 0;
    }

    // t_293db4ec: extracted from the old stop-lambda so the startup-only
    // short-circuit and the timer path share one finalize implementation.
    void finishM1()
    {
        QQmlProperty::write(m_loader, QStringLiteral("source"),
                            QUrl::fromLocalFile(m_root + QStringLiteral("/views/ModuleList.qml")));
        const QJsonObject last = sample(QStringLiteral("final"));
        const int rssDelta = last.value(QStringLiteral("rss_kb")).toInt() - m_baselineRss;
        const int objectDelta = last.value(QStringLiteral("window_qobjects")).toInt() - m_baselineObjects;
        // D3(b) has two measurement legs:
        //   plain  -> RSS slope + plateau gate (what the owner ruling models)
        //   asan-q0 -> LSan leak gate; quarantine_size_mb=0 collapses the
        //             artifact, ASan's own allocator still drifts ~3 KB/trans
        //             so RSS-shape checks are informational on this leg.
        const bool asanQ0 = qEnvironmentVariable("TORTURE_M1_GATE_MODE") == QLatin1String("asan-q0");
        int slopePoints = 0;
        const double slopeKbPerTransition = postWarmupSlope(m_samples, &slopePoints);
        const int lateGrowthKb = lateWindowGrowth(m_samples);
        // The absolute delta is reported for context only (bounded Qt warmup).
        const bool slopeOk = asanQ0 || (slopePoints >= 3
            && slopeKbPerTransition <= kSlopeLimitKbPerTransition);
        const bool plateauOk = asanQ0 || lateGrowthKb <= kPlateauLimitKb;
        const bool deltaOk = asanQ0 || rssDelta <= kRssDeltaLimitKb;
        // The sampled QObject count is compared strictly: any net retention
        // fails. sample() flushes deferred deletes twice before reading it so
        // the point-instant count is stable across baseline and final.
        const bool objectOk = asanQ0 || objectDelta == 0;
        const bool pass = m_errors == 0 && slopeOk && plateauOk && deltaOk && objectOk
            && last.value(QStringLiteral("root_objects")).toInt() == m_baselineRoots;
        finish(QStringLiteral("M-1"), {
            {QStringLiteral("module_roots"), QJsonArray::fromStringList(m_modules)},
            {QStringLiteral("module_root_count"), int(m_modules.size())},
            {QStringLiteral("completed_transitions"), m_transitions},
            {QStringLiteral("target_transitions"), m_target},
            {QStringLiteral("control_mode"), m_control ? QStringLiteral("modulelist-only") : QStringLiteral("standard")},
            {QStringLiteral("baseline_rss_kb"), m_baselineRss},
            {QStringLiteral("final_rss_kb"), last.value(QStringLiteral("rss_kb"))},
            {QStringLiteral("rss_delta_kb"), rssDelta},
            {QStringLiteral("post_warmup_slope_kb_per_transition"), slopeKbPerTransition},
            {QStringLiteral("slope_window_start_transition"), kWarmupSkipTransitions},
            {QStringLiteral("slope_sample_count"), slopePoints},
            {QStringLiteral("late_window_growth_kb"), lateGrowthKb},
            {QStringLiteral("slope_limit_kb_per_transition"), kSlopeLimitKbPerTransition},
            {QStringLiteral("plateau_limit_kb"), kPlateauLimitKb},
            {QStringLiteral("slope_pass"), slopeOk},
            {QStringLiteral("plateau_pass"), plateauOk},
            {QStringLiteral("rss_delta_limit_kb"), kRssDeltaLimitKb},
            {QStringLiteral("rss_delta_pass"), deltaOk},
            {QStringLiteral("object_count_pass"), objectOk},
            {QStringLiteral("gate_mode"), asanQ0 ? QStringLiteral("asan-q0-leak-gate") : QStringLiteral("plain-slope-plateau-gate")},
            {QStringLiteral("gate_basis"), asanQ0 ? QStringLiteral("D3b-asan-q0-leak-only") : QStringLiteral("D3b-plain-slope-plateau")},
            {QStringLiteral("baseline_window_qobjects"), m_baselineObjects},
            {QStringLiteral("final_window_qobjects"), last.value(QStringLiteral("window_qobjects"))},
            {QStringLiteral("window_qobject_delta"), objectDelta},
            {QStringLiteral("load_errors"), m_errors},
            {QStringLiteral("method"), QStringLiteral("real Main.qml QQuickLoader; %1 source transitions; post-warmup OLS slope + late-window plateau; RSS is conservative JS-heap superset; ASAN_OPTIONS must set quarantine_size_mb=0 for instrumented runs").arg(m_target)}
        }, pass);
    }
    void startM1()
    {
        // t_293db4ec: env-driven loop count + ModuleList-only control mode.
        bool ok = false;
        const int envTarget = qEnvironmentVariableIntValue("TORTURE_M1_TRANSITIONS", &ok);
        if (ok && envTarget >= 0) m_target = envTarget;
        m_control = qEnvironmentVariable("TORTURE_M1_CONTROL") == QLatin1String("modulelist");
        m_clock.start();
        m_loader = findMainLoader();
        if (!m_loader) {
            finish(QStringLiteral("M-1"), {{QStringLiteral("setup_error"), QStringLiteral("main QQuickLoader not found")}}, false);
            return;
        }
        QDir dir(m_root + QStringLiteral("/modules"));
        const QStringList folders = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &folder : folders) {
            QFile f(dir.filePath(folder + QStringLiteral("/manifest.json")));
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QJsonObject manifest = QJsonDocument::fromJson(f.readAll()).object();
            const QString entry = manifest.value(QStringLiteral("entry_point_qml")).toString();
            if (!entry.isEmpty()) m_modules << QStringLiteral("modules/") + folder + QLatin1Char('/') + entry;
        }
        sample(QStringLiteral("initial"));
        if (m_target <= 0) {
            // Startup-only run: Main.qml + ModuleList already loaded; no module
            // view is ever loaded and no transition loop runs.
            const QJsonObject first = m_samples.last().toObject();
            m_baselineRss = first.value(QStringLiteral("rss_kb")).toInt();
            m_baselineObjects = first.value(QStringLiteral("window_qobjects")).toInt();
            m_baselineRoots = first.value(QStringLiteral("root_objects")).toInt();
            finishM1();
            return;
        }
        // Warm five full passes over every module Root before the baseline.
        // One pass was not enough: RSS was still climbing until ~t100, so the
        // measured window included the one-time warmup ramp. Control mode
        // never loads a module view, so it skips the warmup entirely.
        m_transitions = m_control ? 0 : -2 * m_modules.size() * kWarmupCycles;
        QTimer *timer = new QTimer(this);
        bool intervalOk = false;
        const int intervalMs = qEnvironmentVariableIntValue("TORTURE_M1_INTERVAL_MS", &intervalOk);
        timer->setInterval(intervalOk && intervalMs > 0 ? intervalMs : 4);
        connect(timer, &QTimer::timeout, this, [this, timer] {
            const int raw = m_transitions;
            const int absolute = raw < 0 ? -raw : raw;
            QString source;
            if (m_control) {
                source = QStringLiteral("views/ModuleList.qml");
            } else if (absolute % 2 == 1) {
                const int idx = ((absolute - 1) / 2) % m_modules.size();
                source = m_modules.at(idx);
            } else {
                source = QStringLiteral("views/ModuleList.qml");
            }
            QQmlProperty::write(m_loader, QStringLiteral("source"),
                                QUrl::fromLocalFile(m_root + QLatin1Char('/') + source));
            ++m_transitions;
            if (m_loader->property("status").toInt() == 3) ++m_errors;
            if (m_transitions > 0 && m_transitions % 50 == 0)
                sample(QStringLiteral("cycle-%1").arg(m_transitions));
            if (raw == 0) {
                const QJsonObject warm = sample(QStringLiteral("baseline-after-warmup"));
                m_baselineRss = warm.value(QStringLiteral("rss_kb")).toInt();
                m_baselineObjects = warm.value(QStringLiteral("window_qobjects")).toInt();
                m_baselineRoots = warm.value(QStringLiteral("root_objects")).toInt();
            }
            if (m_transitions >= m_target) {
                timer->stop();
                QTimer::singleShot(100, this, [this] { finishM1(); });
            }
        });
        timer->start();
    }

    void startM4()
    {
        m_clock.start();
        QFile idsFile(qEnvironmentVariable("DATA_ROOT") + QStringLiteral("/youtube_known_ids.json"));
        if (idsFile.open(QIODevice::ReadOnly)) {
            const QJsonArray ids = QJsonDocument::fromJson(idsFile.readAll()).array();
            for (const auto &v : ids) m_ids << v.toString();
        }
        const int durationSec = qEnvironmentVariableIntValue("TORTURE_M4_SECONDS");
        const int duration = durationSec > 0 ? durationSec : 240;
        QTimer *timer = new QTimer(this);
        timer->setInterval(1000);
        connect(timer, &QTimer::timeout, this, [this, timer, duration] {
            const int sec = int(m_clock.elapsed() / 1000);
            if (sec % 60 != 0 || (!m_samples.isEmpty()
                && m_samples.last().toObject().value(QStringLiteral("label")).toString()
                    == QStringLiteral("second-%1").arg(sec))) return;
            QJsonObject row = sample(QStringLiteral("second-%1").arg(sec));
            QObject *idle = m_engine->rootContext()->contextProperty(QStringLiteral("idleTracker")).value<QObject *>();
            QObject *yt = m_engine->rootContext()->contextProperty(QStringLiteral("youtubeBackend")).value<QObject *>();
            int cached = 0, fetching = 0;
            if (yt) for (const QString &id : m_ids) {
                bool value = false;
                QMetaObject::invokeMethod(yt, "hasSponsorSegments", Q_RETURN_ARG(bool, value), Q_ARG(QString, id));
                if (value) ++cached;
                value = false;
                QMetaObject::invokeMethod(yt, "sponsorFetchInFlight", Q_RETURN_ARG(bool, value), Q_ARG(QString, id));
                if (value) ++fetching;
            }
            row.insert(QStringLiteral("idle_seconds"), idle ? idle->property("idleSeconds").toInt() : -1);
            row.insert(QStringLiteral("sponsor_cached_ids"), cached);
            row.insert(QStringLiteral("sponsor_fetching_ids"), fetching);
            QFileInfo state(QStringLiteral("/tmp/240mp_state.log"));
            row.insert(QStringLiteral("state_log_bytes"), state.exists() ? state.size() : 0);
            m_samples.replace(m_samples.size() - 1, row);
            if (sec == 120) m_warmRss = row.value(QStringLiteral("rss_kb")).toInt();
            if (sec >= duration) {
                timer->stop();
                const int growth = row.value(QStringLiteral("rss_kb")).toInt() - m_warmRss;
                const bool pass = row.value(QStringLiteral("idle_seconds")).toInt() >= 60
                    && cached == m_ids.size() && !m_ids.isEmpty() && fetching == 0
                    && growth < 5120;
                finish(QStringLiteral("M-4"), {
                    {QStringLiteral("duration_seconds"), duration},
                    {QStringLiteral("known_id_count"), int(m_ids.size())},
                    {QStringLiteral("warmup_second"), 120},
                    {QStringLiteral("warmup_rss_kb"), m_warmRss},
                    {QStringLiteral("final_rss_kb"), row.value(QStringLiteral("rss_kb"))},
                    {QStringLiteral("post_warmup_rss_growth_kb"), growth},
                    {QStringLiteral("final_sponsor_cached_ids"), cached},
                    {QStringLiteral("final_sponsor_fetching_ids"), fetching},
                    {QStringLiteral("final_idle_seconds"), row.value(QStringLiteral("idle_seconds"))},
                    {QStringLiteral("state_log_final_bytes"), row.value(QStringLiteral("state_log_bytes"))}
                }, pass);
            }
        });
        timer->start();
    }
};
