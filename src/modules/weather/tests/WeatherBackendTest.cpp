// Adversarial weather-backend tests: real WeatherBackend talking HTTP to a
// loopback fake of the four weather APIs. The fake is deliberately dumb (fixed
// payloads, per-route delay) — it exists to make races and cache invalidation
// reproducible, not to emulate Open-Meteo or the NWS.
#include <QtTest/QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QThread>
#include <QFile>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <sys/resource.h>
#include <csignal>

#include "modules/weather/WeatherBackend.h"

namespace {

QByteArray forecastPayload(double lat, bool extrasOnly) {
    QJsonObject cur;
    cur.insert("temperature_2m", lat < 40.0 ? 18.0 : 30.0);
    cur.insert("relative_humidity_2m", 50);
    cur.insert("dew_point_2m", 8.0);
    cur.insert("pressure_msl", 1010.0);
    cur.insert("wind_speed_10m", 12.0);
    cur.insert("wind_direction_10m", 180);
    cur.insert("visibility", 10000);
    cur.insert("weather_code", 2);
    cur.insert("is_day", 1);
    cur.insert("uv_index", 5.0);
    QJsonObject root;
    root.insert("current", cur);
    root.insert("utc_offset_seconds", lat < 40.0 ? -14400 : 32400);
    if (!extrasOnly) {
        QJsonArray times, mins, maxs, codes, sunrise, sunset;
        for (int i = 0; i < 3; ++i) {
            times.append(QStringLiteral("2026-11-1%1").arg(i));
            mins.append(10.0 + i);
            maxs.append(20.0 + i);
            codes.append(2);
            sunrise.append(QStringLiteral("2026-11-1%1T06:00").arg(i));
            sunset.append(QStringLiteral("2026-11-1%1T18:00").arg(i));
        }
        QJsonObject daily;
        daily.insert("time", times);
        daily.insert("temperature_2m_min", mins);
        daily.insert("temperature_2m_max", maxs);
        daily.insert("weather_code", codes);
        daily.insert("sunrise", sunrise);
        daily.insert("sunset", sunset);
        root.insert("daily", daily);
    }
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray observationPayload(const QString &station) {
    QJsonObject temp, dew, humidity, dir, speed, visibility, pressure;
    temp.insert("value", station == QStringLiteral("KOLD") ? 5.0 : 25.0);
    dew.insert("value", 3.0);
    humidity.insert("value", 60.0);
    dir.insert("value", 270.0);
    speed.insert("value", 10.0);
    visibility.insert("value", 10000.0);
    pressure.insert("value", 101300.0);
    QJsonArray layers, present;
    QJsonObject layer;
    layer.insert("amount", "BKN");
    layers.append(layer);
    QJsonObject props;
    props.insert("timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    props.insert("temperature", temp);
    props.insert("dewpoint", dew);
    props.insert("relativeHumidity", humidity);
    props.insert("windDirection", dir);
    props.insert("windSpeed", speed);
    props.insert("visibility", visibility);
    props.insert("seaLevelPressure", pressure);
    props.insert("cloudLayers", layers);
    props.insert("presentWeather", present);
    QJsonObject root;
    root.insert("properties", props);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace

class FakeWeatherServer : public QObject {
    Q_OBJECT
public:
    explicit FakeWeatherServer(QObject *parent = nullptr) : QObject(parent) {
        QVERIFY2(m_server.listen(QHostAddress::LocalHost, 0), "loopback listen failed");
        connect(&m_server, &QTcpServer::newConnection, this, &FakeWeatherServer::onNewConnection);
    }
    quint16 port() const { return m_server.serverPort(); }
    QString base() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }
    void setDelay(const QString &path, int ms) { m_delays[path] = ms; }
    int forecastRequests() const { return m_forecastRequests; }

private slots:
    void onNewConnection() {
        while (m_server.hasPendingConnections()) {
            QTcpSocket *s = m_server.nextPendingConnection();
            s->setParent(this);
            connect(s, &QTcpSocket::readyRead, this, [this, s]() { serve(s); });
        }
    }

private:
    void serve(QTcpSocket *s) {
        const QByteArray data = s->readAll();
        const QList<QByteArray> head = data.split('\n');
        if (head.isEmpty()) return;
        const QList<QByteArray> parts = head.first().trimmed().split(' ');
        if (parts.size() < 2) return;
        const QUrl req(QStringLiteral("http://127.0.0.1") + QString::fromLatin1(parts.at(1)));
        const QString path = req.path();
        const QUrlQuery query(req.query());
        QByteArray body;

        if (path == QStringLiteral("/forecast")) {
            ++m_forecastRequests;
            const bool extrasOnly = !query.hasQueryItem(QStringLiteral("forecast_days"));
            const double lat = query.queryItemValue(QStringLiteral("latitude"))
                                   .section(',', 0, 0).toDouble();
            if (extrasOnly) {
                QJsonArray arr;
                const QStringList lats = query.queryItemValue(QStringLiteral("latitude")).split(',');
                for (int i = 0; i < lats.size(); ++i) {
                    QJsonObject cur;
                    cur.insert("temperature_2m", 30.0);
                    cur.insert("weather_code", 2);
                    cur.insert("wind_speed_10m", 10.0);
                    cur.insert("wind_direction_10m", 180);
                    cur.insert("is_day", 1);
                    QJsonObject entry;
                    entry.insert("current", cur);
                    arr.append(entry);
                }
                body = QJsonDocument(arr).toJson(QJsonDocument::Compact);
            } else {
                body = forecastPayload(lat, false);
            }
        } else if (path == QStringLiteral("/air-quality")) {
            QJsonObject cur;
            cur.insert("european_aqi", 42.0);
            cur.insert("us_aqi", 103.0);
            QJsonObject root;
            root.insert("current", cur);
            body = QJsonDocument(root).toJson(QJsonDocument::Compact);
        } else if (path == QStringLiteral("/geocode")) {
            const QString name = query.queryItemValue(QStringLiteral("name")).toUpper();
            QJsonObject place;
            place.insert("name", name);
            if (name.contains(QStringLiteral("TOKYO"))) {
                place.insert("latitude", 35.6762);
                place.insert("longitude", 139.6503);
            } else {
                place.insert("latitude", 42.3601);
                place.insert("longitude", -71.0589);
            }
            QJsonArray results;
            results.append(place);
            QJsonObject root;
            root.insert("results", results);
            body = QJsonDocument(root).toJson(QJsonDocument::Compact);
        } else if (path.startsWith(QStringLiteral("/points/"))) {
            const QString coord = path.mid(QStringLiteral("/points/").size());
            const double lat = coord.section(',', 0, 0).toDouble();
            const QString station = lat < 40.0 ? QStringLiteral("KOLD") : QStringLiteral("KNEW");
            QJsonObject props;
            props.insert("observationStations", base() + QStringLiteral("/stations/") + station);
            QJsonObject root;
            root.insert("properties", props);
            body = QJsonDocument(root).toJson(QJsonDocument::Compact);
        } else if (path.startsWith(QStringLiteral("/stations/"))
                   && !path.endsWith(QStringLiteral("/latest"))) {
            const QString station = path.mid(QStringLiteral("/stations/").size());
            QJsonObject idObj;
            idObj.insert("stationIdentifier", station);
            QJsonObject props;
            props.insert("stationIdentifier", station);
            QJsonArray coords;
            coords.append(-77.0);
            coords.append(38.8);
            QJsonObject geom;
            geom.insert("coordinates", coords);
            QJsonObject feature;
            feature.insert("properties", props);
            feature.insert("geometry", geom);
            QJsonArray features;
            features.append(feature);
            QJsonObject root;
            root.insert("features", features);
            body = QJsonDocument(root).toJson(QJsonDocument::Compact);
            Q_UNUSED(idObj)
        } else if (path.endsWith(QStringLiteral("/observations/latest"))) {
            const QString station = path.section('/', -3, -3);
            body = observationPayload(station);
        } else {
            body = QByteArray("{}");
        }

        const QByteArray response =
            QByteArray("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ")
            + QByteArray::number(body.size())
            + QByteArray("\r\nConnection: close\r\n\r\n") + body;
        QTimer::singleShot(m_delays.value(path, 0), s, [s, response]() {
            if (s->state() != QAbstractSocket::UnconnectedState) {
                s->write(response);
                s->flush();
                s->disconnectFromHost();
            }
        });
    }

    QTcpServer m_server;
    QHash<QString, int> m_delays;
    int m_forecastRequests = 0;
};

class WeatherBackendTest : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    FakeWeatherServer m_server;

    void writeFile(const QString &name, const QString &content) {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
        const QByteArray bytes = content.toUtf8();
        f.write(bytes);
        // QFile is closed by its destructor at end of scope; flush explicitly.
        f.close();
    }

    void writeConfig(const QString &units) {
        writeFile(QStringLiteral("config.json"),
                  QStringLiteral("{\"modules\":{\"com.240mp.weather\":{"
                                 "\"units\":\"%1\",\"music\":false,"
                                 "\"screen_time\":\"10\",\"hours_format\":\"24-hour\"}}}")
                      .arg(units));
    }

    void writeLocation(const QString &primary, const QString &extras = QString()) {
        QString text = primary + QStringLiteral("\n");
        if (!extras.isEmpty()) text += extras + QStringLiteral("\n");
        writeFile(QStringLiteral("weather_location.txt"), text);
    }

    template <class F>
    bool waitFor(F predicate, int ms = 3000) {
        QElapsedTimer t;
        t.start();
        while (!predicate() && t.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(5);
        }
        return predicate();
    }
    void settle(int ms) { waitFor([] { return false; }, ms); }

private slots:
    void initTestCase() {
        qputenv("MP240_WEATHER_FORECAST_URL",
                (m_server.base() + QStringLiteral("/forecast")).toUtf8());
        qputenv("MP240_WEATHER_GEOCODE_URL",
                (m_server.base() + QStringLiteral("/geocode")).toUtf8());
        qputenv("MP240_WEATHER_AIR_QUALITY_URL",
                (m_server.base() + QStringLiteral("/air-quality")).toUtf8());
        qputenv("MP240_WEATHER_NWS_URL", m_server.base().toUtf8());
        QVERIFY(m_dir.isValid());
    }

    void init() { m_server.setDelay(QStringLiteral("/geocode"), 0); }

    // Notebook hypothesis #1: the coordinate gate checks range, but QString's
    // toDouble() also accepts "nan" — and NaN fails both range comparisons.
    void nanCoordinatesAreRejected() {
        WeatherBackend wb(QString(), m_dir.path());
        QVERIFY(wb.looksLikeCoordinates(QStringLiteral("38.8, -77.0")));
        QVERIFY(!wb.looksLikeCoordinates(QStringLiteral("999.9, -999.9")));
        QVERIFY(!wb.looksLikeCoordinates(QStringLiteral("nan, nan")));
        QVERIFY(!wb.looksLikeCoordinates(QStringLiteral("NaN, NaN, LABEL")));

        writeLocation(QStringLiteral("38.8, -77.0, OLD"));
        wb.setLocation(QStringLiteral("nan, nan, BAD"));
        QCOMPARE(wb.primaryLocationLine(), QStringLiteral("38.8, -77.0, OLD"));
    }

    // Changing LOCATION must retire the old location's station list and last
    // observation. Otherwise the new forecast is silently overlaid with a
    // reading from a station near the previous city.
    void locationChangeDropsOldObservation() {
        writeConfig(QStringLiteral("Metric"));
        writeLocation(QStringLiteral("38.8, -77.0, OLD"));
        WeatherBackend wb(QString(), m_dir.path());
        wb.start();
        QVERIFY(waitFor([&] {
            return wb.hasData()
                && wb.current().value(QStringLiteral("temperature")).toString()
                       == QStringLiteral("5°");
        }));
        QCOMPARE(wb.current().value(QStringLiteral("temperature")).toString(),
                 QStringLiteral("5°")); // old station's observation, by design

        wb.setLocation(QStringLiteral("35.6762, 139.6503, NEW"));
        const bool switched = waitFor([&] {
            return wb.locationName() == QStringLiteral("NEW")
                && wb.current().value(QStringLiteral("temperature")).toString()
                       == QStringLiteral("18°");
        });
        if (!switched) {
            qInfo() << "RED: after LOCATION switch, current =" << wb.current()
                    << "name =" << wb.locationName();
        }
        QVERIFY(switched);
        // Give any straggling old reply a chance to land.
        settle(200);
        QCOMPARE(wb.current().value(QStringLiteral("temperature")).toString(),
                 QStringLiteral("18°"));
        wb.stop();
    }

    // Air quality is presented on a scale chosen by the Units setting, so a
    // units flip must reformat the cached reading, not keep the old scale's
    // number under a new °F/°C heading.
    void unitsChangeRefreshesAirQualityScale() {
        writeConfig(QStringLiteral("Metric"));
        writeLocation(QStringLiteral("38.8, -77.0, OLD"));
        WeatherBackend wb(QString(), m_dir.path());
        wb.start();
        const bool metricShown = waitFor([&] {
            return wb.current().contains(QStringLiteral("airQuality"))
                && wb.current().value(QStringLiteral("airQuality")).toString()
                       == QStringLiteral("42.0 HIGH");
        });
        if (!metricShown) {
            qInfo() << "RED: metric air quality =" << wb.current().value(QStringLiteral("airQuality"));
        }
        QVERIFY(metricShown);
        QCOMPARE(wb.tempUnitLabel(), QStringLiteral("°C"));

        writeConfig(QStringLiteral("US"));
        wb.onSettingChanged(QStringLiteral("com.240mp.weather"),
                            QStringLiteral("units"), QStringLiteral("US"));
        QVERIFY(waitFor([&] {
            return wb.current().value(QStringLiteral("airQuality")).toString()
                       == QStringLiteral("103 SENSITIVE");
        }));
        QCOMPARE(wb.tempUnitLabel(), QStringLiteral("°F"));
        wb.stop();
    }

    // A geocode reply from a superseded location must not write into the new
    // location's extras list (the callback indexes slots by position).
    void staleExtraGeocodeCannotCorruptNewRows() {
        writeConfig(QStringLiteral("Metric"));
        writeLocation(QStringLiteral("38.8, -77.0, OLD"), QStringLiteral("BOSTON"));
        WeatherBackend wb(QString(), m_dir.path());
        m_server.setDelay(QStringLiteral("/geocode"), 600);
        wb.start();
        // Supersede before the first geocode answers.
        QVERIFY(waitFor([&] { return m_server.forecastRequests() >= 1; }, 2000));
        m_server.setDelay(QStringLiteral("/geocode"), 0);
        writeLocation(QStringLiteral("35.6762, 139.6503, NEW"), QStringLiteral("TOKYO"));
        wb.setLocation(QStringLiteral("35.6762, 139.6503, NEW"));
        QVERIFY(waitFor([&] {
            return wb.hasOtherLocations()
                && wb.otherLocations().first().toMap().value(QStringLiteral("name")).toString()
                       == QStringLiteral("TOKYO");
        }, 4000));
        settle(800);
        const QVariantList rows = wb.otherLocations();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("TOKYO"));
        wb.stop();
    }

    // Pressure must be present and self-labelled in both unit systems. A US
    // bare number was easy to mistake for an absent field on the CRT row.
    void pressureVisibleInBothUnitSystems() {
        for (const QString &units : { QStringLiteral("Metric"), QStringLiteral("US") }) {
            writeConfig(units);
            writeLocation(QStringLiteral("38.8, -77.0, OLD"));
            WeatherBackend wb(QString(), m_dir.path());
            wb.start();
            QVERIFY(waitFor([&] { return wb.hasData(); }));
            settle(200);
            const QString pressure = wb.current().value(QStringLiteral("pressure")).toString();
            QVERIFY2(!pressure.isEmpty(), qPrintable(units + QStringLiteral(" pressure present")));
            if (units == QLatin1String("US")) {
                QVERIFY2(pressure.contains(QStringLiteral("INHG")), "US pressure names its scale");
                const double inHg = pressure.split(QLatin1Char(' ')).first().toDouble();
                QVERIFY(inHg > 29.0 && inHg < 30.5);
            } else {
                QVERIFY2(pressure.contains(QStringLiteral("MB")), "metric pressure names its scale");
                const double mb = pressure.split(QLatin1Char(' ')).first().toDouble();
                QVERIFY(mb > 1000.0 && mb < 1020.0);
            }

        }

    }

    // Notebook hypothesis #4: a failed write must not change the file and must
    // surface "unwritable_location" instead of silently promoting an
    // unwritten change. RLIMIT_FSIZE=0 refuses any write that grows the file;
    // with SIGXFSZ ignored the failing write returns -1 and the backend answers
    // with the error, leaving the seed content intact.
    void setLocationWriteFailureLeavesFileUnchanged()
    {
        writeLocation(QStringLiteral("38.8, -77.0, OLD"));
        WeatherBackend wb(QString(), m_dir.path());

        QFile beforeFile(m_dir.filePath(QStringLiteral("weather_location.txt")));
        QVERIFY(beforeFile.open(QIODevice::ReadOnly));
        const QByteArray fileBefore = beforeFile.readAll();
        beforeFile.close();

        // Refuse any file growth so the rewrite cannot land.
        std::signal(SIGXFSZ, SIG_IGN);
        struct rlimit rl;
        QVERIFY(::getrlimit(RLIMIT_FSIZE, &rl) == 0);
        const rlim_t oldCur = rl.rlim_cur;
        const rlim_t oldMax = rl.rlim_max;
        rl.rlim_cur = 0;
        // Keep rlim_max intact: an unprivileged process cannot raise a hard
        // limit it lowered, so zeroing it would poison every later write in
        // the suite with EFBIG. Lowering only the soft limit still makes
        // QSaveFile::commit() fail here, and restoring it always succeeds.
        QVERIFY(::setrlimit(RLIMIT_FSIZE, &rl) == 0);

        struct Recorder {
            QStringList reasons;
            void record(const QString &r) { reasons << r; }
            bool wasEmitted(const QString &r) const { return reasons.contains(r); }
        };
        Recorder recorder;
        QObject::connect(&wb, &WeatherBackend::locationError,
                         [&recorder](const QString &r) { recorder.record(r); });

        wb.setLocation(QStringLiteral("10.0000, 20.0000, NEW"));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

        // Restore the ambient limits before asserting on the file.
        rl.rlim_cur = oldCur;
        rl.rlim_max = oldMax;
        QVERIFY(::setrlimit(RLIMIT_FSIZE, &rl) == 0);
        std::signal(SIGXFSZ, SIG_DFL);

        QVERIFY(recorder.wasEmitted(QStringLiteral("unwritable_location")));
        QFile afterFile(m_dir.filePath(QStringLiteral("weather_location.txt")));
        QVERIFY(afterFile.open(QIODevice::ReadOnly));
        QCOMPARE(afterFile.readAll(), fileBefore);
    }

    // Comment and blank lines in weather_location.txt must survive a location
    // update: only the first non-comment, non-blank line (the coordinate) is
    // replaced, so a comment sitting before it is preserved on write-back.
    void commentsInLocationFileSurviveUpdate()
    {
        const QString seed =
            QStringLiteral("# documented list\n")
            + QStringLiteral("\n")
            + QStringLiteral("42.3601, -71.0589, BOSTON\n")
            + QStringLiteral("# extra note\n")
            + QStringLiteral("40.7128, -74.0060, NYC\n");
        writeFile(QStringLiteral("weather_location.txt"), seed);

        WeatherBackend wb(QString(), m_dir.path());
        {
            QFileInfo fi(m_dir.filePath(QStringLiteral("weather_location.txt")));
            qInfo("[DBG] after-writeFile exists=%d bytes=%lld",
                  fi.exists() ? 1 : 0, fi.size());
            QFile probe(m_dir.filePath(QStringLiteral("weather_location.txt")));
            QVERIFY(probe.open(QIODevice::ReadOnly));
            qInfo("[DBG] after-writeFile probeSize=%lld", (long long)probe.size());
        }
        {
            qInfo("[DBG] m_dir.path=%s", qUtf8Printable(m_dir.path()));
            QFile probe(m_dir.filePath(QStringLiteral("weather_location.txt")));
            QVERIFY(probe.open(QIODevice::ReadOnly));
            qInfo("[DBG] seed-after-ctor bytes=%lld", (long long)probe.size());
            qInfo("[DBG] seed-after-ctor content=%s", qUtf8Printable(probe.readAll()));
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

        wb.setLocation(QStringLiteral("5.0000, 6.0000, NEW"));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

        QFile after(m_dir.filePath(QStringLiteral("weather_location.txt")));
        QVERIFY(after.open(QIODevice::ReadOnly));
        const QString afterText = QString::fromUtf8(after.readAll());

        // DEBUG dump of the file right after setLocation, before assertions.
        {
            QFile dbg(m_dir.filePath(QStringLiteral("weather_location.txt")));
            QVERIFY(dbg.open(QIODevice::ReadOnly));
            qInfo("[DBG] comments file bytes=%lld", (long long)dbg.size());
            qInfo("[DBG] comments file content=%s", qUtf8Printable(dbg.readAll()));
        }
        QVERIFY(afterText.contains(QStringLiteral("# documented list")));
        QVERIFY(afterText.contains(QStringLiteral("# extra note")));
        QVERIFY(afterText.contains(QStringLiteral("40.7128, -74.0060, NYC")));
        QCOMPARE(afterText,
                 QStringLiteral("# documented list\n"
                                "\n"
                                "5.0000, 6.0000, NEW\n"
                                "# extra note\n"
                                "40.7128, -74.0060, NYC\n"));
    }

    // A successful update rewrites the primary line and keeps the extra
    // locations and comments in order.
    void successfulUpdateKeepsExtrasAndComments()
    {
        const QString seed =
            QStringLiteral("42.3601, -71.0589, BOSTON\n")
            + QStringLiteral("# note about NYC\n")
            + QStringLiteral("40.7128, -74.0060, NYC\n")
            + QStringLiteral("34.0522, -118.2437, LA\n");
        writeFile(QStringLiteral("weather_location.txt"), seed);

        WeatherBackend wb(QString(), m_dir.path());
        wb.setLocation(QStringLiteral("5.0000, 6.0000, NEW"));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

        QFile after(m_dir.filePath(QStringLiteral("weather_location.txt")));
        QVERIFY(after.open(QIODevice::ReadOnly));
        const QString afterText = QString::fromUtf8(after.readAll());

        QVERIFY(afterText.contains(QStringLiteral("40.7128, -74.0060, NYC")));
        QVERIFY(afterText.contains(QStringLiteral("34.0522, -118.2437, LA")));
        QVERIFY(afterText.contains(QStringLiteral("# note about NYC")));
        qInfo("[DBG] commentsInLocationFileSurviveUpdate path = %s", qUtf8Printable(m_dir.filePath(QStringLiteral("weather_location.txt"))));
        bool af2 = after.open(QIODevice::ReadOnly);
        qInfo("[DBG] commentsInLocationFileSurviveUpdate after.open() = %d", af2 ? 1 : 0);
        const QString afterText2 = af2 ? QString::fromUtf8(after.readAll()) : QStringLiteral("(open failed)");
        qInfo("[DBG] commentsInLocationFileSurviveUpdate afterText2 = %s", qUtf8Printable(afterText2));
        QCOMPARE(afterText,
                 QStringLiteral("5.0000, 6.0000, NEW\n"
                                "# note about NYC\n"
                                "40.7128, -74.0060, NYC\n"
                                "34.0522, -118.2437, LA\n"));
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    WeatherBackendTest testobject;
    return QTest::qExec(&testobject, argc, argv);
}

#include "WeatherBackendTest.moc"
