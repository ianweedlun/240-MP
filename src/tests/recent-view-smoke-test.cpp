// Offscreen QML smoke test for the Recently-played view layout contract.
// Loads Recent.qml with stubbed backends and asserts its full parity with a
// Games list: thumbnail preview pane, `N GAMES · A-Z/Z-A` header, and an A-Z
// letter strip that follows the active sort direction. Article-stripped
// bucketing and zone geometry that cannot overlap the strip are covered too.
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlProperty>
#include <QMetaObject>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QColor>
#include <QUrl>
#include <algorithm>
#include <cstdio>

class StubBackend : public QObject {
    Q_OBJECT
public:
    explicit StubBackend(QString sort = QStringLiteral("A-Z"), QObject *parent = nullptr)
        : QObject(parent), m_sort(std::move(sort)) {}
    Q_INVOKABLE QVariantList recentGames() {
        QVariantList games;
        // Same five rows the real store would return; the real backend applies
        // Game Sort, so mirror that here for both directions.
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
        // Mirror the real backend: the RECENT view receives games already
        // ordered by Game Sort (articles stripped), not raw recency order.
        const auto sortKey = [](const QVariant &game) {
            QString name = game.toMap().value(QStringLiteral("name")).toString().trimmed();
            for (const QString &article : { QStringLiteral("the "), QStringLiteral("a "),
                                            QStringLiteral("an ") }) {
                if (name.startsWith(article, Qt::CaseInsensitive)) {
                    name = name.mid(article.size()).trimmed();
                    break;
                }
            }
            return name.toCaseFolded();
        };
        const bool descending = m_sort == QLatin1String("Z-A");
        std::sort(games.begin(), games.end(), [&](const QVariant &a, const QVariant &b) {
            return descending ? sortKey(a) > sortKey(b) : sortKey(a) < sortKey(b);
        });
        return games;
    }
    Q_INVOKABLE QString gameSort() const { return m_sort; }
    void setSort(const QString &sort) { m_sort = sort; }
    // Games.qml asks for a playlist; the smoke test uses the same five rows.
    Q_INVOKABLE QVariantList refreshGames(const QString &) { return recentGames(); }
    Q_INVOKABLE void recordGamePosition(const QString &, int) {}
    Q_INVOKABLE int gamePosition(const QString &) const { return 0; }
    Q_INVOKABLE QString resolveThumbnail(const QString &name, const QString &basename) const {
        return QStringLiteral("/tmp/boxart/%1/%2.png").arg(basename, name);
    }
    Q_INVOKABLE void launchGame(const QVariantMap &, const QString &) {}
private:
    QString m_sort;
};

class StubAppCore : public QObject {
    Q_OBJECT
public:
    explicit StubAppCore(QString thumb = QStringLiteral("RIGHT")) : m_thumb(std::move(thumb)) {}
    Q_INVOKABLE QString get_setting(const QString &, const QString &) const { return m_thumb; }
    Q_INVOKABLE void note_state(const QString &, const QString &) {}
    Q_INVOKABLE void save_setting(const QString &module, const QString &key, const QVariant &value) {
        m_savedModule = module;
        m_savedKey = key;
        m_savedValue = value;
    }
    QString savedModule() const { return m_savedModule; }
    QString savedKey() const { return m_savedKey; }
    QVariant savedValue() const { return m_savedValue; }
signals:
    void moduleSettingChanged(const QString &, const QString &, const QVariant &);
private:
    QString m_thumb;
    QString m_savedModule;
    QString m_savedKey;
    QVariant m_savedValue;
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
    Q_PROPERTY(QString moduleId READ moduleId CONSTANT)
public:
    QString icon() const { return QString(); }
    QString name() const { return QStringLiteral("RETROARCH"); }
    QString moduleId() const { return QStringLiteral("com.240mp.retroarch"); }
};

static int errors = 0;
static void check(bool ok, const char *what) {
    if (!ok) { ++errors; std::printf("  FAIL  %s\n", what); }
    else     { std::printf("  ok    %s\n", what); }
}

static QObject *loadView(QQmlEngine &engine, StubBackend &backend,
                         StubAppCore &appCore, StubRoot &root, StubModule &module,
                         const QString &repoRoot, const QString &leaf = QStringLiteral("Recent.qml")) {
    auto *ctx = engine.rootContext();
    ctx->setContextProperty("appCore", QVariant::fromValue(&appCore));
    ctx->setContextProperty("retroarchBackend", QVariant::fromValue(&backend));
    ctx->setContextProperty("root", QVariant::fromValue(&root));
    ctx->setContextProperty("moduleRoot", QVariant::fromValue(&module));

    QQmlComponent comp(&engine, QUrl::fromLocalFile(
        repoRoot + "/modules/retroarch/views/" + leaf));
    if (comp.status() == QQmlComponent::Error) {
        for (const auto &e : comp.errors())
            std::printf("  ERR  %s: %s\n", qPrintable(leaf), e.description().toStdString().c_str());
        check(false, qPrintable(leaf + QStringLiteral(" instantiates")));
        return nullptr;
    }
    QObject *obj = comp.create();
    check(obj != nullptr, qPrintable(leaf + QStringLiteral(" object created")));
    return obj;
}

static void sendKey(QObject *target, Qt::Key key) {
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &event);
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
        // Game Sort parity: A-Z means alphabetical by name (articles stripped),
        // so the strip reads # A D L M with '#' leading, exactly like Games.qml.
        check(letters == QStringLiteral("#ADLM"),
              "A-Z letter strip is alphabetical with articles stripped (#ADLM)");
        check(QQmlProperty::read(view, "gameSortLabel").toString() == QStringLiteral("A-Z"),
              "view carries the A-Z sort label");
        QObject *header = view->findChild<QObject *>("recentCountHeader");
        check(header && QQmlProperty::read(header, "text").toString()
              == QStringLiteral("5 GAMES  ·  A-Z"),
              "count header matches the games list (N GAMES · A-Z)");
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
            QQmlProperty::write(list, "currentIndex", 0); // the "#" bucket in A-Z
            bool invoked = QMetaObject::invokeMethod(view, "syncLetterToGame");
            check(invoked && QQmlProperty::read(strip, "currentIndex").toInt() == 0,
                  "row selection syncs the strip to the matching bucket");
        }

        // Up at the first letter leaves letter mode and focuses the sort header;
        // Down returns to the same letter. Enter persists the toggled setting.
        if (header && strip) {
            QQmlProperty::write(strip, "currentIndex", 0);
            QMetaObject::invokeMethod(strip, "forceActiveFocus");
            sendKey(strip, Qt::Key_Up);
            QCoreApplication::processEvents();
            check(QQmlProperty::read(view, "sortHeaderFocused").toBool(),
                  "Up past the first letter focuses the sort header");
            check(QQmlProperty::read(strip, "currentIndex").toInt() == 0,
                  "letter selection is preserved when the header takes focus");
            sendKey(header, Qt::Key_Down);
            check(!QQmlProperty::read(view, "sortHeaderFocused").toBool() &&
                  QQmlProperty::read(strip, "currentIndex").toInt() == 0,
                  "Down from the header returns to the same letter");

            sendKey(strip, Qt::Key_Up);
            sendKey(header, Qt::Key_Return);
            check(core.savedModule() == QStringLiteral("com.240mp.retroarch") &&
                  core.savedKey() == QStringLiteral("game_sort") &&
                  core.savedValue().toString() == QStringLiteral("Z-A"),
                  "Enter on the header persists the toggled Game Sort");
        }

        // Live Game Sort change: same game stays selected, header/strip flip.
        if (list) {
            QQmlProperty::write(list, "currentIndex", 4); // Metal Gear Solid in A-Z
            backend.setSort(QStringLiteral("Z-A"));
            emit core.moduleSettingChanged(QStringLiteral("com.240mp.retroarch"),
                                           QStringLiteral("game_sort"),
                                           QStringLiteral("Z-A"));
            QCoreApplication::processEvents();
            QVariantList flipped = QQmlProperty::read(list, "model").toList();
            const int idx = QQmlProperty::read(list, "currentIndex").toInt();
            check(QQmlProperty::read(view, "gameSortLabel").toString() == QStringLiteral("Z-A"),
                  "live Game Sort change updates the view label");
            check(idx >= 0 && idx < flipped.size() &&
                  flipped.at(idx).toMap().value("name").toString()
                  == QStringLiteral("Metal Gear Solid"),
                  "live sort flip keeps the same game selected");
            QVariantList flippedIndex = QQmlProperty::read(view, "letterIndex").toList();
            QString flippedLetters;
            for (const QVariant &bucket : flippedIndex)
                flippedLetters += bucket.toMap().value("letter").toString();
            check(flippedLetters == QStringLiteral("MLDA#"),
                  "live sort flip rebuilds the strip in Z-A order (MLDA#)");
        }
        delete view;
    }

    // ---- Z-A: header, list and letter strip all reverse together ----
    {
        QQmlEngine engine;
        engine.addImportPath(repoRoot + "/views");
        StubBackend backend(QStringLiteral("Z-A")); StubAppCore core;
        StubRoot root; StubModule module;
        QObject *view = loadView(engine, backend, core, root, module, repoRoot);
        if (!view) return 1;

        QVariantList index = QQmlProperty::read(view, "letterIndex").toList();
        QString letters;
        for (const QVariant &bucket : index)
            letters += bucket.toMap().value("letter").toString();
        check(letters == QStringLiteral("MLDA#"),
              "Z-A letter strip reverses with '#' trailing (MLDA#)");
        check(QQmlProperty::read(view, "gameSortLabel").toString() == QStringLiteral("Z-A"),
              "view carries the Z-A sort label");
        QObject *header = view->findChild<QObject *>("recentCountHeader");
        check(header && QQmlProperty::read(header, "text").toString()
              == QStringLiteral("5 GAMES  ·  Z-A"),
              "count header matches the games list (N GAMES · Z-A)");
        QObject *list = view->findChild<QObject *>("recentList");
        QObject *strip = view->findChild<QObject *>("letterList");
        if (list) {
            QVariantList model = QQmlProperty::read(list, "model").toList();
            check(!model.isEmpty() && model.first().toMap().value("name").toString()
                  == QStringLiteral("Metal Gear Solid"),
                  "Z-A list starts at the descending end of the alphabet");
        }
        if (list && strip) {
            QQmlProperty::write(list, "currentIndex", 4); // "007 Goldeneye" -> '#'
            bool invoked = QMetaObject::invokeMethod(view, "syncLetterToGame");
            check(invoked && QQmlProperty::read(strip, "currentIndex").toInt() == 4,
                  "Z-A row selection syncs the strip to the matching bucket");
        }
        delete view;
    }

    // ---- Games.qml gets the same focusable sort header ----
    {
        QQmlEngine engine;
        engine.addImportPath(repoRoot + "/views");
        StubBackend backend; StubAppCore core; StubRoot root; StubModule module;
        QObject *view = loadView(engine, backend, core, root, module, repoRoot,
                                 QStringLiteral("Games.qml"));
        if (!view) return 1;

        QObject *header = view->findChild<QObject *>("sortHeader");
        QObject *strip = view->findChild<QObject *>("letterList");
        check(header && strip, "Games view has a sort header and letter strip");
        if (header) {
            check(QQmlProperty::read(header, "text").toString()
                  == QStringLiteral("5 GAMES  ·  A-Z"),
                  "Games header reads N GAMES · A-Z");
        }
        if (header && strip) {
            QQmlProperty::write(strip, "currentIndex", 0);
            QMetaObject::invokeMethod(strip, "forceActiveFocus");
            sendKey(strip, Qt::Key_Up);
            check(QQmlProperty::read(view, "sortHeaderFocused").toBool(),
                  "Games: Up past the first letter focuses the sort header");
            sendKey(header, Qt::Key_Return);
            check(core.savedModule() == QStringLiteral("com.240mp.retroarch") &&
                  core.savedKey() == QStringLiteral("game_sort") &&
                  core.savedValue().toString() == QStringLiteral("Z-A"),
                  "Games: Enter on the header persists the toggled Game Sort");
            sendKey(header, Qt::Key_Down);
            check(!QQmlProperty::read(view, "sortHeaderFocused").toBool() &&
                  QQmlProperty::read(strip, "currentIndex").toInt() == 0,
                  "Games: Down from the header returns to the same letter");
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
