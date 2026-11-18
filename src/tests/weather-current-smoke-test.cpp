// Offscreen QML smoke test for the current-conditions row budget.
// The pixel-font row set must show the same fields in Metric and US units.
// A fixed-width "1em per character" estimate used to reserve two slots for
// the longer US air-quality label, pushing PRESSURE off the visible prefix.
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlProperty>
#include <QMetaObject>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QColor>
#include <QUrl>
#include <cstdio>

class StubRoot : public QObject {
    Q_OBJECT
    Q_PROPERTY(double sh READ sh CONSTANT)
    Q_PROPERTY(double sw READ sw CONSTANT)
    Q_PROPERTY(QString globalFont READ globalFont CONSTANT)
    Q_PROPERTY(QColor surfaceColor READ surfaceColorRead CONSTANT)
    Q_PROPERTY(QColor primaryColor READ primaryColorRead CONSTANT)
    Q_PROPERTY(QColor secondaryColor READ secondaryColorRead CONSTANT)
    Q_PROPERTY(QColor accentColor READ accentColorRead CONSTANT)
public:
    double sh() const { return 480; }
    double sw() const { return 720; }
    QString globalFont() const { return QStringLiteral("monospace"); }
    QColor surfaceColorRead() const { return QColor("#101018"); }
    QColor primaryColorRead() const { return QColor("#e6e6e6"); }
    QColor secondaryColorRead() const { return QColor("#8a8a9a"); }
    QColor accentColorRead() const { return QColor("#ffcc00"); }
};

static int errors = 0;
static void check(bool ok, const char *what) {
    if (!ok) { ++errors; std::printf("  FAIL  %s\n", what); }
    else     { std::printf("  ok    %s\n", what); }
}

static int callInt(QObject *obj, const char *method) {
    QVariant result;
    if (!QMetaObject::invokeMethod(obj, method, Q_RETURN_ARG(QVariant, result)))
        check(false, "QML method invocation");
    return result.toInt();
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
#ifndef MP240_REPO_ROOT
#define MP240_REPO_ROOT "."
#endif
    const QString repoRoot = QString::fromLocal8Bit(MP240_REPO_ROOT);
    StubRoot root;

    struct Case { const char *tag; QVariantMap wx; };
    QVariantMap metric;
    metric[QStringLiteral("airQuality")] = QStringLiteral("41.0 HIGH");
    metric[QStringLiteral("uvIndex")]    = QStringLiteral("3 MODERATE");
    metric[QStringLiteral("humidity")]   = QStringLiteral("45%");
    metric[QStringLiteral("dewPoint")]   = QStringLiteral("1°");
    metric[QStringLiteral("pressure")]   = QStringLiteral("1013.0 MB");
    QVariantMap us;
    us[QStringLiteral("airQuality")] = QStringLiteral("57 MODERATE");
    us[QStringLiteral("uvIndex")]    = QStringLiteral("3 MODERATE");
    us[QStringLiteral("humidity")]   = QStringLiteral("45%");
    us[QStringLiteral("dewPoint")]   = QStringLiteral("33°");
    us[QStringLiteral("pressure")]   = QStringLiteral("29.91 INHG");

    for (const Case &c : { Case{ "METRIC", metric }, Case{ "US", us } }) {
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("root"), &root);
        QQmlComponent comp(&engine, QUrl::fromLocalFile(
            repoRoot + QStringLiteral("/modules/weather/views/screens/CurrentConditions.qml")));
        if (comp.status() == QQmlComponent::Error) {
            for (const auto &e : comp.errors())
                std::printf("  ERR  %s\n", e.description().toLocal8Bit().constData());
            check(false, "CurrentConditions.qml instantiates");
            continue;
        }
        QObject *view = comp.create();
        check(view != nullptr, "CurrentConditions.qml object created");
        if (!view) continue;
        QQmlProperty::write(view, QStringLiteral("wx"), c.wx);
        QQmlProperty::write(view, QStringLiteral("locationName"), QStringLiteral("CATONSVILLE"));
        // Loader normally sizes the loaded item; a bare QQmlComponent has no
        // geometry, which would make rowArea.height zero and rowFit()==0.
        QQmlProperty::write(view, QStringLiteral("width"), 540);
        QQmlProperty::write(view, QStringLiteral("height"), 360);
        QCoreApplication::processEvents();

        const int fit = callInt(view, "rowFit");
        check(fit >= 5, (QByteArray("row budget shows at least the pressure row (")
                         + c.tag + ")").constData());
        // reservedLines(0) = 1 means the air-quality row does not falsely
        // reserve a second line and displace PRESSURE (index 4).
        QVariant aqReserved;
        if (QMetaObject::invokeMethod(view, "reservedLines", Q_RETURN_ARG(QVariant, aqReserved),
                                      Q_ARG(QVariant, 0))) {
            check(aqReserved.toInt() == 1,
                  (QByteArray("air quality fits one line (") + c.tag + ")").constData());
        }
        QStringList rows = QQmlProperty::read(view, QStringLiteral("rowTexts")).toStringList();
        (void)rows;
        std::printf("  %s fit=%d\n", c.tag, fit);
        delete view;
    }

    std::printf("%s (%d failure(s))\n",
                errors == 0 ? "WEATHER CURRENT OK" : "WEATHER CURRENT FAILED", errors);
    return errors == 0 ? 0 : 1;
}

#include "weather-current-smoke-test.moc"
