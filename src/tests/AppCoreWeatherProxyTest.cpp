// Verifies issue #53's fix: app-level views (ModuleSettings, LocationPicker)
// load outside the weather module's QQml scope, where the module-scope
// "weatherBackend" context property is null. The weather module's location
// reads are proxied through appCore, which holds every backend in m_backends
// and is available at app-level scope. These tests exercise that proxy directly.
#include <QTest>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QFile>

#include "AppCore.h"
#include "modules/weather/WeatherBackend.h"

class AppCoreWeatherProxyTest : public QObject {
    Q_OBJECT

private:
    void registerWeather(AppCore &core, WeatherBackend &wb) {
        // Register without a QML scope: the proxy reads m_backends directly,
        // so a null QQmlContext is fine here.
        core.registerModule(QStringLiteral("com.240mp.weather"), QString(), &wb, nullptr);
    }

    void writeLocation(const QString &path, const QString &line) {
        QFile f(path + QStringLiteral("/weather_location.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(line.toUtf8());
        f.write("\n");
    }

private Q_SLOTS:
    // No location file -> empty line, no throw.
    void testNoLocationFile() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        WeatherBackend wb(QString(), dir.path());
        AppCore core(QString(), dir.path());
        registerWeather(core, wb);
        QCOMPARE(core.weatherPrimaryLocationLine(), QString());
    }

    // The proxy reads the file the same way the module does.
    void testReadsLocationFile() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeLocation(dir.path(), QStringLiteral("38.8,-77.0  Arlington, VA"));
        WeatherBackend wb(QString(), dir.path());
        AppCore core(QString(), dir.path());
        registerWeather(core, wb);
        QCOMPARE(core.weatherPrimaryLocationLine(),
                 QStringLiteral("38.8,-77.0  Arlington, VA"));
    }

    // Coordinate-format gate: explicit coords pass, place names don't.
    void testLooksLikeCoordinates() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        WeatherBackend wb(QString(), dir.path());
        AppCore core(QString(), dir.path());
        registerWeather(core, wb);
        QVERIFY(core.weatherLooksLikeCoordinates(QStringLiteral("38.8,-77.0")));
        QVERIFY(!core.weatherLooksLikeCoordinates(QStringLiteral("Catsville, MD")));
    }

    // No backend registered -> safe defaults, no throw (the app-level guard).
    void testNoBackendRegistered() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AppCore core(QString(), dir.path());
        QCOMPARE(core.weatherPrimaryLocationLine(), QString());
        QCOMPARE(core.weatherLooksLikeCoordinates(QStringLiteral("38.8,-77.0")), false);
    }
};

// Headless CI has no X server; QTEST_MAIN would build a QGuiApplication that
// can't construct without a display. Set the offscreen platform before the app
// is created, then run the test the way QTEST_MAIN would.
int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    AppCoreWeatherProxyTest testobject;
    return QTest::qExec(&testobject, argc, argv);
}

#include "AppCoreWeatherProxyTest.moc"
