// Standalone QML smoke test for the preset add-flow views. Loads PresetRecipe
// and PresetSubject offscreen with stubbed appCore / virtualChannelsBackend /
// root context, and asserts they instantiate, populate their OptionList, and
// fire their nav/create signals.
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlProperty>
#include <QMetaMethod>
#include <QGuiApplication>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QColor>
#include <QMetaObject>
#include <QUrl>
#include <cstdio>

// ---- stub backend: bypasses network, records the create call ----
class StubBackend : public QObject {
    Q_OBJECT
public:
    int createdKind = -1; QString createdId, createdName; int createdRet = 7;
    QString createdWants;
    int seededChannel = -1;
    Q_INVOKABLE QVariantList list_channels() { return {}; }
    Q_INVOKABLE void load_catalog(const QString &kind) {
        QVariantList rows;
        QVariantMap a; a["id"] = "389"; a["label"] = "AMC"; rows.append(a);
        QVariantMap b; b["id"] = "402"; b["label"] = "HBO"; rows.append(b);
        emit catalogReady(kind, rows);
    }
    Q_INVOKABLE int create_preset_channel(int kind, const QString &id, const QString &name,
                                          const QString &wants) {
        createdKind = kind; createdId = id; createdName = name; createdWants = wants;
        return createdRet;
    }
    // The real backend answers the seed asynchronously; here the answer lands
    // on the call, which is the same signal path the view listens on.
    Q_INVOKABLE void seed_preset_channel(int ch) {
        seededChannel = ch;
        emit presetSeeded(ch, true, QStringLiteral("5 shows"));
    }
signals:
    void catalogReady(const QString &kind, const QVariantList &options);
    void catalogFailed(const QString &kind, const QString &reason);
    void presetSeeded(int channelNumber, bool ok, const QString &message);
};

// ---- stub appCore: get_module_info + settings ----
class StubAppCore : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE QVariantMap get_module_info(const QString &) {
        QVariantMap m; m["icon"] = QString(); return m;
    }
    Q_INVOKABLE QVariantMap get_setting(const QString &) {
        QVariantMap m; m["value"] = false; return m;
    }
    Q_INVOKABLE void save_setting(const QString &, const QVariantMap &) {}
};

// ---- stub root: the app's root.* context the views + OptionList bind to ----
class StubRoot : public QObject {
    Q_OBJECT
    Q_PROPERTY(double sh READ sh CONSTANT)
    Q_PROPERTY(double sw READ sw CONSTANT)
    Q_PROPERTY(QString globalFont READ globalFont CONSTANT)
    Q_PROPERTY(QColor surfaceColor READ surfaceColorRead CONSTANT)
    Q_PROPERTY(QColor primaryColor READ primaryColorRead CONSTANT)
    Q_PROPERTY(QColor secondaryColor READ secondaryColorRead CONSTANT)
    Q_PROPERTY(QColor tertiaryColor READ tertiaryColorRead CONSTANT)
    Q_PROPERTY(QColor accentColor READ accentColorRead CONSTANT)
    Q_PROPERTY(QVariantMap hints READ hintsRead CONSTANT)
public:
    double sh() const { return 720; }
    double sw() const { return 1280; }
    QString globalFont() const { return QString("VCR OSD Mono"); }
    QColor surfaceColorRead()   const { return QColor("#101018"); }
    QColor primaryColorRead()   const { return QColor("#e6e6e6"); }
    QColor secondaryColorRead() const { return QColor("#8a8a9a"); }
    QColor tertiaryColorRead()  const { return QColor("#5a5a6a"); }
    QColor accentColorRead()    const { return QColor("#ffcc00"); }
    QVariantMap hintsRead() const {
        QVariantMap m;
        m["back"]   = QString("BACK");
        m["navigate"] = QString("NAVIGATE");
        m["change"] = QString("CHANGE");
        m["select"] = QString("SELECT");
        return m;
    }
};

// Records the path a view navigates to, so the smoke test can assert the
// navigateTo signal fired without relying on QSignalSpy (QML signals aren't in
// the C++ signal table in this Qt build).
class NavRecorder : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE void record(const QString &path) { m_path = path; called = true; }
    bool called = false; QString m_path;
};

static int errors = 0;
static void check(bool ok, const char *what) {
    if (!ok) { ++errors; std::printf("  FAIL  %s\n", what); }
    else     { std::printf("  ok    %s\n", what); }
}

int main(int argc, char **argv) {
    // Headless CI has no X server; the offscreen platform lets the QML engine's
    // QGuiApplication construct without a display (same Qt quirk as the appcore
    // weather-proxy test). Set before the app is created.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    QQmlEngine engine;
    engine.addImportPath(QString::fromUtf8(MP240_REPO_ROOT) + "/views");

    StubBackend backend; StubAppCore appCore; StubRoot root;

    auto *ctx = engine.rootContext();
    ctx->setContextProperty("appCore", QVariant::fromValue(&appCore));
    ctx->setContextProperty("virtualChannelsBackend", QVariant::fromValue(&backend));
    ctx->setContextProperty("root", QVariant::fromValue(&root));

    const QString repoRoot(QString::fromUtf8(MP240_REPO_ROOT));
    const QUrl recipeUrl = QUrl::fromLocalFile(
        repoRoot + "/modules/virtual_channels/views/PresetRecipe.qml");
    const QUrl subjectUrl = QUrl::fromLocalFile(
        repoRoot + "/modules/virtual_channels/views/PresetSubject.qml");

    std::printf("STEP engine+ctx done\n"); std::fflush(stdout);
    // ---- Test 1: PresetRecipe instantiates and exposes 3 recipe rows ----
    {
        QQmlComponent comp(&engine, recipeUrl);
        std::printf("STEP recipe comp status=%d\n", (int)comp.status()); std::fflush(stdout);
        if (comp.status() == QQmlComponent::Error) {
            for (auto &e : comp.errors())
                std::printf("  ERR  PresetRecipe: %s\n",
                             e.description().toStdString().c_str());
            check(false, "PresetRecipe instantiates");
        } else {
            QObject *obj = comp.create();
            std::printf("STEP recipe created obj=%p\n", (void*)obj); std::fflush(stdout);
            check(obj != nullptr, "PresetRecipe object created");
            QVariantList recipes = QQmlProperty::read(obj, "recipes").toList();
            check(recipes.size() == 3, "PresetRecipe exposes 3 recipe rows");
            QVariantList openRows = QQmlProperty::read(obj, "rows").toList();
            check(openRows.size() == 2, "PresetRecipe opens with the 2 kind rows");
            if (obj) delete obj;
        }
    }

    // ---- Test 2: PresetSubject loads catalog and populates rows ----
    {
        QQmlComponent comp(&engine, subjectUrl);
        if (comp.status() == QQmlComponent::Error) {
            for (auto &e : comp.errors())
                std::printf("  ERR  PresetSubject: %s\n",
                             e.description().toStdString().c_str());
            check(false, "PresetSubject instantiates");
        } else {
            ctx->setContextProperty("navParams", [] {
                QVariantMap m; m["recipe"] = QString("genre");
                m["wants"] = QString("tv");
                return m;
            }());
            QObject *obj = comp.create();
            std::printf("STEP subject created obj=%p\n", (void*)obj); std::fflush(stdout);
            check(obj != nullptr, "PresetSubject object created");
            // Component.onCompleted -> load_catalog -> catalogReady fires
            // synchronously and the view's Connections sets subjectRoot.rows.
            QVariantList rows = QQmlProperty::read(obj, "rows").toList();
            check(rows.size() == 2, "PresetSubject populated 2 catalog rows");

            // ---- Test 3: activate -> create_preset_channel + navigateTo ----
            // Wire a recorder to the view's navigateTo signal (string-based
            // connect, which works where QSignalSpy can't find QML signals here).
            app.processEvents();
            QObject *optObj = nullptr;
            for (QObject *c : obj->findChildren<QObject *>())
                if (QString(c->metaObject()->className()).contains("OptionList")) { optObj = c; break; }
            check(optObj != nullptr, "PresetSubject OptionList found");

            NavRecorder recorder;
            int sIdx = obj->metaObject()->indexOfMethod("navigateTo(QString,QVariant,QVariant)");
            int mIdx = recorder.metaObject()->indexOfMethod("record(QString)");
            QMetaObject::connect(obj, sIdx, &recorder, mIdx, Qt::DirectConnection);

            std::printf("STEP about-to-invoke activate\n"); std::fflush(stdout);
            QMetaObject::invokeMethod(optObj, "activate", Q_ARG(int, 0));
            std::printf("STEP invoke-activate done\n"); std::fflush(stdout);
            app.processEvents();

            check(backend.createdKind == 0 && backend.createdId == "389"
                  && backend.createdName == "AMC" && backend.createdWants == "tv",
                  "activate calls create_preset_channel(0, 389, AMC, tv)");
            check(backend.seededChannel == backend.createdRet,
                  "activate seeds the channel it just made");
            check(recorder.called && recorder.m_path.contains("ChannelSources"),
                  "the seeded channel navigates to ChannelSources");
            if (obj) delete obj;
        }
    }

    std::printf("\n%s (errors=%d)\n", errors ? "SMOKE FAILED" : "SMOKE PASSED", errors);
    return errors ? 1 : 0;
}

#include "preset-smoke-test.moc"
