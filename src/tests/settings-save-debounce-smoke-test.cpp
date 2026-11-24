// CFG-01 follow-up N1 — offscreen QML smoke for Settings.qml save debounce.
//
// The per-keypress storm lives here: LEFT/RIGHT on a list_single row used to
// call appCore.save_setting() on every press. With the atomic writer that is a
// full fsync(file)+fsync(dir)+bak cycle per press. This test pins the
// coalescing contract: N rapid presses on one row produce ZERO calls until a
// trailing-edge flush, then exactly ONE durable write carrying the last value.
// save_setting itself is untouched (synchronous bool, disk read-your-writes,
// degraded refusal), so the cfg01 G7 C++ gate keeps its exact contract.
//
// Two-sided: on the pre-fix code every press calls save_setting, so the
// "zero calls before flush" assertions fail by construction.
#include <QGuiApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QColor>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char *what) {
    if (!ok) { ++failures; std::printf("  FAIL  %s\n", what); }
    else     { std::printf("  ok    %s\n", what); }
}

class StubAppCore : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool configCorrupted READ configCorrupted CONSTANT)
    Q_PROPERTY(QString configNotice READ configNotice CONSTANT)
public:
    bool corrupted = false;
    bool failSave = false;
    int saveCalls = 0;
    QString lastKey;
    QVariant lastValue;

    bool configCorrupted() const { return corrupted; }
    QString configNotice() const { return QString(); }

    Q_INVOKABLE QVariantMap get_settings() const {
        QVariantMap app;
        app["color_scheme"] = QStringLiteral("Video 1");
        app["screensaver_timeout"] = QStringLiteral("OFF");
        QVariantMap cfg;
        cfg["app"] = app;
        return cfg;
    }
    Q_INVOKABLE QVariantList get_installed_modules() const { return {}; }
    Q_INVOKABLE bool isAutostartSession() const { return false; }
    Q_INVOKABLE QVariantMap getCustomColorSchemes() const { return {}; }
    Q_INVOKABLE QVariantMap getCustomColorScheme() const { return {}; }
    Q_INVOKABLE void note_state(const QString &, const QString &) {}
    Q_INVOKABLE QString localIpAddress() const { return QString(); }
    Q_INVOKABLE bool resetCorruptedConfig() { return false; }

    // Mirrors the real shape: get_setting re-reads disk. The stub's "disk" is
    // only updated by a successful save_setting — never by the optimistic UI.
    Q_INVOKABLE QVariant get_setting(const QString &, const QString &key) const {
        return stored.value(key, QVariant(QStringLiteral("Video 1")));
    }
    Q_INVOKABLE bool save_setting(const QString &, const QString &key, const QVariant &value) {
        ++saveCalls;
        lastKey = key;
        lastValue = value;
        if (failSave) return false;
        stored[key] = value;
        return true;
    }
signals:
    void moduleSettingChanged(const QString &, const QString &, const QVariant &);
    void appSettingChanged(const QString &, const QString &);
    void configHealthChanged();

private:
    QVariantMap stored;
};

// Old-style SIGNAL() needs a real slot on the receiving side; a bare lambda
// is only accepted by the typed connect overload.
class GoBackRecorder : public QObject {
    Q_OBJECT
public:
    int count = 0;
public slots:
    void record() { ++count; }
};

class StubRoot : public QObject {
    Q_OBJECT
    Q_PROPERTY(double sh READ sh CONSTANT)
    Q_PROPERTY(double sw READ sw CONSTANT)
    Q_PROPERTY(QString globalFont READ globalFont CONSTANT)
    Q_PROPERTY(QColor surfaceColor READ surfaceColor CONSTANT)
    Q_PROPERTY(QColor primaryColor READ primaryColor CONSTANT)
    Q_PROPERTY(QColor secondaryColor READ secondaryColor CONSTANT)
    Q_PROPERTY(QColor tertiaryColor READ tertiaryColor CONSTANT)
    Q_PROPERTY(QColor accentColor READ accentColor CONSTANT)
    Q_PROPERTY(QVariantMap hints READ hints CONSTANT)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
public:
    double sh() const { return 480; }
    double sw() const { return 640; }
    QString globalFont() const { return QStringLiteral("VCR OSD Mono"); }
    QColor surfaceColor() const { return QColor("#101018"); }
    QColor primaryColor() const { return QColor("#e6e6e6"); }
    QColor secondaryColor() const { return QColor("#8a8a9a"); }
    QColor tertiaryColor() const { return QColor("#5a5a6a"); }
    QColor accentColor() const { return QColor("#ffcc00"); }
    QString appVersion() const { return QStringLiteral("N1-SMOKE"); }
    QVariantMap hints() const {
        QVariantMap m;
        m["back"] = "BACK"; m["navigate"] = "NAV"; m["select"] = "SELECT";
        m["change"] = "CHANGE"; m["browse"] = "BROWSE";
        return m;
    }
};

static void spinFor(int ms) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

static void sendKey(QObject *target, Qt::Key key) {
    QKeyEvent ev(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &ev);
}

static QObject *load(QQmlEngine &engine, StubAppCore &core, StubRoot &root,
                     const QString &repoRoot, QObject **listOut) {
    auto *ctx = engine.rootContext();
    ctx->setContextProperty("appCore", QVariant::fromValue(&core));
    ctx->setContextProperty("root", QVariant::fromValue(&root));
    QQmlComponent comp(&engine, QUrl::fromLocalFile(repoRoot + "/views/Settings.qml"));
    if (comp.status() == QQmlComponent::Error) {
        for (const auto &e : comp.errors())
            std::printf("  ERR  %s\n", qPrintable(e.description()));
        check(false, "Settings.qml instantiates");
        return nullptr;
    }
    QObject *view = comp.create();
    check(view != nullptr, "Settings.qml object created");
    if (!view) return nullptr;
    QCoreApplication::processEvents();
    *listOut = view->findChild<QObject *>("settingsList");
    check(*listOut != nullptr, "settingsList present");
    // ListView ignores the first currentIndex write until delegates exist.
    QQmlProperty::write(*listOut, "currentIndex", 0);
    QCoreApplication::processEvents();
    QQmlProperty::write(*listOut, "currentIndex", 0);
    return view;
}

static QString firstValue(QObject *view) {
    const QVariantList items = QQmlProperty::read(view, "settingsItems").toList();
    // Degraded model prepends a reset action, so locate the row by key.
    for (const QVariant &item : items) {
        const QVariantMap row = item.toMap();
        if (row.value("key").toString() == QLatin1String("color_scheme"))
            return row.value("value").toString();
    }
    return QString();
}

static void flush(QObject *view) {
    bool ok = QMetaObject::invokeMethod(view, "flushPendingSettingSave", Qt::DirectConnection);
    check(ok, "flushPendingSettingSave is invokable");
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    // Env override lets the SAME binary run against a pre-fix detached tree
    // for the negative control (no second build, no source divergence).
    const QByteArray rootOverride = qgetenv("MP240_REPO_ROOT_OVERRIDE");
    const QString repoRoot = rootOverride.isEmpty()
        ? QString::fromUtf8(MP240_REPO_ROOT)
        : QString::fromUtf8(rootOverride);

    // ---- 1. N rapid presses on one row -> 0 writes, then exactly 1 ----
    {
        QQmlEngine engine; engine.addImportPath(repoRoot + "/views");
        StubAppCore core; StubRoot root; QObject *list = nullptr;
        QObject *view = load(engine, core, root, repoRoot, &list);
        if (!view || !list) return 1;
        QQmlProperty::write(view, "saveDebounceMs", 60000); // freeze the timer
        for (int i = 0; i < 4; ++i) sendKey(list, Qt::Key_Right);
        check(core.saveCalls == 0, "4 rapid RIGHTs cause ZERO save_setting calls");
        check(firstValue(view) == QStringLiteral("T-120"),
              "optimistic display advanced to the last value (T-120)");
        check(!QQmlProperty::read(view, "pendingSettingSave").isNull(),
              "a pending save is queued");
        flush(view);
        check(core.saveCalls == 1, "one flush performs exactly ONE save_setting");
        check(core.lastValue.toString() == QStringLiteral("T-120"),
              "the single write carries the final value");
        check(QQmlProperty::read(view, "pendingSettingSave").isNull(),
              "pending cleared after flush");
    }

    // ---- 2. different row commits the previous one first ----
    {
        QQmlEngine engine; engine.addImportPath(repoRoot + "/views");
        StubAppCore core; StubRoot root; QObject *list = nullptr;
        QObject *view = load(engine, core, root, repoRoot, &list);
        if (!view || !list) return 1;
        QQmlProperty::write(view, "saveDebounceMs", 60000);
        sendKey(list, Qt::Key_Right);
        sendKey(list, Qt::Key_Right);
        check(core.saveCalls == 0, "row-0 presses stay queued");
        sendKey(list, Qt::Key_Down);
        check(core.saveCalls == 1, "changing row commits the pending row-0 write");
        check(core.lastValue.toString() == QStringLiteral("Synthwave"),
              "committed value is the last row-0 value");
        check(QQmlProperty::read(view, "pendingSettingSave").isNull(),
              "no pending write survives the row change");
    }

    // ---- 3. Back flushes before goBack ----
    {
        QQmlEngine engine; engine.addImportPath(repoRoot + "/views");
        StubAppCore core; StubRoot root; QObject *list = nullptr;
        QObject *view = load(engine, core, root, repoRoot, &list);
        if (!view || !list) return 1;
        QQmlProperty::write(view, "saveDebounceMs", 60000);
        GoBackRecorder recorder;
        QObject::connect(view, SIGNAL(goBack()), &recorder, SLOT(record()));
        sendKey(list, Qt::Key_Right);
        sendKey(list, Qt::Key_Escape);
        check(core.saveCalls == 1, "Back commits the pending write");
        check(recorder.count == 1, "goBack still emitted");
    }

    // ---- 4. degraded state refuses immediately, no optimistic display ----
    {
        QQmlEngine engine; engine.addImportPath(repoRoot + "/views");
        StubAppCore core; StubRoot root; QObject *list = nullptr;
        core.corrupted = true;
        QObject *view = load(engine, core, root, repoRoot, &list);
        if (!view || !list) return 1;
        // Degraded model puts the reset action at row 0; cycle to the first
        // list_single row (Color Scheme) before pressing RIGHT.
        QQmlProperty::write(list, "currentIndex", 1);
        QCoreApplication::processEvents();
        check(firstValue(view) == QStringLiteral("Video 1"), "degraded baseline value");
        sendKey(list, Qt::Key_Right);
        check(core.saveCalls == 0, "degraded RIGHT causes no save call");
        check(firstValue(view) == QStringLiteral("Video 1"),
              "degraded RIGHT does not change the display");
        check(QQmlProperty::read(view, "pendingSettingSave").isNull(),
              "degraded RIGHT queues nothing");
    }

    // ---- 5. failed write reverts the optimistic display to disk value ----
    {
        QQmlEngine engine; engine.addImportPath(repoRoot + "/views");
        StubAppCore core; StubRoot root; QObject *list = nullptr;
        QObject *view = load(engine, core, root, repoRoot, &list);
        if (!view || !list) return 1;
        QQmlProperty::write(view, "saveDebounceMs", 60000);
        sendKey(list, Qt::Key_Right);
        check(firstValue(view) == QStringLiteral("Late Night"), "optimistic value shown");
        core.failSave = true;
        flush(view);
        check(firstValue(view) == QStringLiteral("Video 1"),
              "refused write reverts display to the on-disk value");
    }

    // ---- 6. the real timer path (short interval) performs the write ----
    {
        QQmlEngine engine; engine.addImportPath(repoRoot + "/views");
        StubAppCore core; StubRoot root; QObject *list = nullptr;
        QObject *view = load(engine, core, root, repoRoot, &list);
        if (!view || !list) return 1;
        QQmlProperty::write(view, "saveDebounceMs", 20);
        sendKey(list, Qt::Key_Right);
        check(core.saveCalls == 0, "no immediate write before the timer fires");
        spinFor(250);
        check(core.saveCalls == 1, "trailing-edge timer performed the write");
        check(core.lastValue.toString() == QStringLiteral("Late Night"),
              "timer wrote the final value");
    }

    if (failures) {
        std::printf("settings-save-debounce-smoke: %d FAILURES\n", failures);
        return 1;
    }
    std::printf("settings-save-debounce-smoke: ALL PASS\n");
    return 0;
}
#include "settings-save-debounce-smoke-test.moc"
