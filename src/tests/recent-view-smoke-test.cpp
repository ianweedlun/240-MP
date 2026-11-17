// Offscreen QML smoke test for the Recently-played view layout contract.
// Loads Recent.qml with stubbed backends and asserts the two affordances the
// owner flagged as missing on the CRT: the thumbnail preview pane and the
// letter-strip quick-scroll (including article-stripped, display-order buckets
// and zone geometry that cannot overlap the strip).
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlProperty>
#include <QMetaObject>
#include <QGuiApplication>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QColor>
#include <QUrl>
#include <cstdio>

class StubBackend : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE QVariantList recentGames() {
        QVariantList games;
        // Newest first, deliberately NOT alphabetical. "The Legend of Zelda"
        // must index under Z, not T (article stripping).
        const char *names[] = { "The Legend of Zelda", "Metal Gear Solid",
                                "Angry Birds", "007 Goldeneye", "Donkey Kong" };
        for (const char *name : names) {
            QVariantMap g;
            g["name"] = QString::fromLatin1(name);
            g["shortName"] = QStringLiteral("NES");
            g["playlistBasename"] = QStringLiteral("Nintendo - NES");
            g["systemId"] = QStringLiteral("nes");
            games.append(g);
        }
        return games;
    }
    Q_INVOKABLE QString resolveThumbnail(const QString &name, const QString &basename) const {
        return QStringLiteral("/tmp/boxart/%1/%2.png").arg(basename, name);
    }
    Q_INVOKABLE void launchGame(const QVariantMap &, const QString &) {}
};

class StubAppCore : public QObject {
    Q_OBJECT
public:
    explicit StubAppCore(QString thumb = QStringLiteral("RIGHT")) : m_thumb(std::move(thumb)) {}
    Q_INVOKABLE QString get_setting(const QString &, const QString &) const { return m_thumb; }
    Q_INVOKABLE void note_state(const QString &, const QString &) {}
private:
    QString m_thumb;
};

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
    double sh() const { return 480; }
    double sw() const { return 640; }
    QString globalFont() const { return QStringLiteral("VCR OSD Mono"); }
    QColor surfaceColorRead() const { return QColor("#101018"); }
    QColor primaryColorRead() const { return QColor("#e6e6e6"); }
    QColor secondaryColorRead() const { return QColor("#8a8a9a"); }
    QColor tertiaryColorRead() const { return QColor("#5a5a6a"); }
    QColor accentColorRead() const { return QColor("#ffcc00"); }
    QVariantMap hintsRead() const {
        QVariantMap m;
        m["back"] = QStringLiteral("BACK");
        m["navigate"] = QStringLiteral("NAV");
        m["browse"] = QStringLiteral("BROWSE");
        m["select"] = QStringLiteral("SELECT");
        return m;
    }
};

class StubModule : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString moduleIcon READ icon CONSTANT)
    Q_PROPERTY(QString moduleName READ name CONSTANT)
public:
    QString icon() const { return QString(); }
    QString name() const { return QStringLiteral("RETROARCH"); }
};

static int errors = 0;
static void check(bool ok, const char *what) {
    if (!ok) { ++errors; std::printf("  FAIL  %s\n", what); }
    else     { std::printf("  ok    %s\n", what); }
}

static QObject *loadView(QQmlEngine &engine, StubBackend &backend,
                         StubAppCore &appCore, StubRoot &root, StubModule &module,
                         const QString &repoRoot) {
    auto *ctx = engine.rootContext();
    ctx->setContextProperty("appCore", QVariant::fromValue(&appCore));
    ctx->setContextProperty("retroarchBackend", QVariant::fromValue(&backend));
    ctx->setContextProperty("root", QVariant::fromValue(&root));
    ctx->setContextProperty("moduleRoot", QVariant::fromValue(&module));

    QQmlComponent comp(&engine, QUrl::fromLocalFile(
        repoRoot + "/modules/retroarch/views/Recent.qml"));
    if (comp.status() == QQmlComponent::Error) {
        for (const auto &e : comp.errors())
            std::printf("  ERR  Recent.qml: %s\n", e.description().toStdString().c_str());
        check(false, "Recent.qml instantiates");
        return nullptr;
    }
    QObject *obj = comp.create();
    check(obj != nullptr, "Recent.qml object created");
    return obj;
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    const QString repoRoot(QString::fromUtf8(MP240_REPO_ROOT));

    // ---- Default setting (RIGHT): preview + strip, article-stripped buckets ----
    {
        QQmlEngine engine;
        engine.addImportPath(repoRoot + "/views");
        StubBackend backend; StubAppCore core; StubRoot root; StubModule module;
        QObject *view = loadView(engine, backend, core, root, module, repoRoot);
        if (!view) return 1;

        QVariantList index = QQmlProperty::read(view, "letterIndex").toList();
        QString letters;
        for (const QVariant &bucket : index)
            letters += bucket.toMap().value("letter").toString();
        // "The Legend of Zelda" must index under L, not T (article stripping);
        // order stays newest-first: L, M, A, #, D — no alphabetical re-sort.
        check(letters == QStringLiteral("LMA#D"),
              "letter strip is display-order with articles stripped (LMA#D)");
        check(QQmlProperty::read(view, "showPreview").toBool(), "preview enabled by default");

        QObject *preview = view->findChild<QObject *>("previewLoader");
        QObject *strip = view->findChild<QObject *>("letterList");
        QObject *list = view->findChild<QObject *>("recentList");
        check(preview != nullptr && strip != nullptr && list != nullptr,
              "preview / strip / list objects exist");
        if (preview) {
            QObject *item = preview->property("item").value<QObject *>();
            check(item != nullptr, "GamePreview.qml loaded into the preview column");
            if (item) {
                QVariantMap game = item->property("game").toMap();
                check(!game.value("thumbnail").toString().isEmpty(),
                      "selected game's boxart resolved from stored playlist basename");
            }
            double px = QQmlProperty::read(preview, "x").toDouble();
            double pw = QQmlProperty::read(preview, "width").toDouble();
            double sx = strip ? QQmlProperty::read(strip, "x").toDouble() : 0.0;
            check(px + pw <= sx + 0.5, "preview column does not overlap the letter strip");
        }
        if (strip) {
            check(QQmlProperty::read(strip, "visible").toBool(), "letter strip visible with 5 buckets");
            check(QQmlProperty::read(strip, "count").toInt() == 5, "letter strip has 5 buckets");
        }
        if (list && strip) {
            QQmlProperty::write(list, "currentIndex", 3); // the "#" bucket
            bool invoked = QMetaObject::invokeMethod(view, "syncLetterToGame");
            check(invoked && QQmlProperty::read(strip, "currentIndex").toInt() == 3,
                  "row selection syncs the strip to the matching bucket");
        }
        delete view;
    }

    // ---- OFF: preview gone, strip still there (setting must stay OFF) ----
    {
        QQmlEngine engine;
        engine.addImportPath(repoRoot + "/views");
        StubBackend backend; StubAppCore core(QStringLiteral("OFF"));
        StubRoot root; StubModule module;
        QObject *view = loadView(engine, backend, core, root, module, repoRoot);
        if (!view) return 1;
        check(!QQmlProperty::read(view, "showPreview").toBool(), "thumbnail_display=OFF stays OFF");
        QObject *preview = view->findChild<QObject *>("previewLoader");
        QObject *strip = view->findChild<QObject *>("letterList");
        check(preview && !QQmlProperty::read(preview, "visible").toBool(),
              "preview hidden when thumbnail_display=OFF");
        check(strip && QQmlProperty::read(strip, "visible").toBool(),
              "letter strip still works with thumbnails off");
        delete view;
    }

    std::printf("%s (%d failure(s))\n", errors == 0 ? "RECENT VIEW OK" : "RECENT VIEW FAILED", errors);
    return errors == 0 ? 0 : 1;
}

#include "recent-view-smoke-test.moc"
