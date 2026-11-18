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
    Q_INVOKABLE QString playlistSort() const { return m_playlistSort; }
    Q_INVOKABLE QVariantList refreshAllPlaylists() {
        QVariantList rows;
        for (const QString &name : { QStringLiteral("NES"), QStringLiteral("SNES"),
                                     QStringLiteral("Genesis") }) {
            QVariantMap pl;
            pl["name"] = name;
            pl["shortName"] = name;
            pl["count"] = 10;
            pl["path"] = "/playlists/" + name + ".lpl";
            rows.append(pl);
        }
        return rows;
    }
    Q_INVOKABLE QVariantList refreshPlaylists(const QString &) { return refreshAllPlaylists(); }
    // Games.qml asks for a playlist; the smoke test uses the same five rows.
    Q_INVOKABLE QVariantList refreshGames(const QString &) { return recentGames(); }
    Q_INVOKABLE void recordGamePosition(const QString &, int) {}
    Q_INVOKABLE int gamePosition(const QString &) const { return 0; }
    Q_INVOKABLE QString resolveThumbnail(const QString &name, const QString &basename) const {
        return QStringLiteral("/tmp/boxart/%1/%2.png").arg(basename, name);
    }
    Q_INVOKABLE void launchGame(const QVariantMap &, const QString &) {}
public slots:
    // Mirrors the real flow: AppCore::save_setting calls this slot on the
    // backend BEFORE emitting moduleSettingChanged. No test may pre-set the
    // sort directly — that would hide the ordering race this test pins.
    void onSettingChanged(const QString &moduleId, const QString &key, const QVariant &value) {
        if (moduleId != QLatin1String("com.240mp.retroarch")) return;
        if (key == QLatin1String("game_sort"))
            m_sort = value.toString() == QLatin1String("Z-A")
                     ? QStringLiteral("Z-A") : QStringLiteral("A-Z");
        if (key == QLatin1String("playlist_sort"))
            m_playlistSort = value.toString() == QLatin1String("COMPANY")
                             ? QStringLiteral("COMPANY") : QStringLiteral("SYSTEM");
    }
private:
    QString m_sort = QStringLiteral("A-Z");
    QString m_playlistSort = QStringLiteral("SYSTEM");
};

class StubAppCore : public QObject {
    Q_OBJECT
public:
    explicit StubAppCore(QString thumb = QStringLiteral("RIGHT")) : m_thumb(std::move(thumb)) {}
    void setBackend(StubBackend *backend) { m_backend = backend; }
    Q_INVOKABLE QString get_setting(const QString &, const QString &) const { return m_thumb; }
    Q_INVOKABLE void note_state(const QString &, const QString &) {}
    Q_INVOKABLE void save_setting(const QString &module, const QString &key, const QVariant &value) {
        m_savedModule = module;
        m_savedKey = key;
        m_savedValue = value;
        // Same ordering as AppCore::save_setting: backend first, signal after.
        if (m_backend)
            QMetaObject::invokeMethod(m_backend, "onSettingChanged", Qt::DirectConnection,
                                      Q_ARG(QString, module), Q_ARG(QString, key),
                                      Q_ARG(QVariant, value));
        emit moduleSettingChanged(module, key, value);
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
    StubBackend *m_backend = nullptr;
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
    appCore.setBackend(&backend);
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

// Qt 6.11 offscreen: a ListView ignores the first currentIndex write until its
// delegates exist. Prime with a no-op write + event turn so index assertions in
// this harness exercise the view's real selection, not a stale index 0.
static void primeListIndex(QObject *list, int index) {
    QQmlProperty prop(list, QStringLiteral("currentIndex"));
    prop.write(index);
    QCoreApplication::processEvents();
    QQmlProperty::write(list, QStringLiteral("currentIndex"), index);
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
        QObject *header = view->findChild<QObject *>("recentSortDirection");
        QObject *count = view->findChild<QObject *>("recentSortCount");
        check(count && QQmlProperty::read(count, "text").toString()
              == QStringLiteral("5 GAMES  ·  ") &&
              header && QQmlProperty::read(header, "text").toString() == QStringLiteral("A-Z"),
              "header splits count and sort direction (N GAMES · A-Z)");
        check(QQmlProperty::read(view, "showPreview").toBool(), "preview enabled by default");

        QObject *preview = view->findChild<QObject *>("previewLoader");
        QObject *strip = view->findChild<QObject *>("letterList");
        QObject *list = view->findChild<QObject *>("recentList");
        if (list) primeListIndex(list, 0);
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
            QObject *highlight = view->findChild<QObject *>("sortHeaderHighlight");
            check(highlight && QQmlProperty::read(highlight, "visible").toBool() &&
                  QQmlProperty::read(highlight, "width").toDouble()
                  < QQmlProperty::read(count, "width").toDouble(),
                  "accent highlights only the direction word, not N GAMES");
            check(QQmlProperty::read(strip, "currentIndex").toInt() == 0,
                  "letter selection is preserved when the header takes focus");
            sendKey(header, Qt::Key_Down);
            check(!QQmlProperty::read(view, "sortHeaderFocused").toBool() &&
                  QQmlProperty::read(strip, "currentIndex").toInt() == 0,
                  "Down from the header returns to the same letter");

            sendKey(strip, Qt::Key_Up);
            sendKey(header, Qt::Key_Return);
            QCoreApplication::processEvents();
            check(core.savedModule() == QStringLiteral("com.240mp.retroarch") &&
                  core.savedKey() == QStringLiteral("game_sort") &&
                  core.savedValue().toString() == QStringLiteral("Z-A"),
                  "Enter on the header persists the toggled Game Sort");
            // Regression for #85: the Connections handler refreshes synchronously
            // on moduleSettingChanged, so save_setting must have updated the
            // backend BEFORE emitting. With the old order the first Enter was
            // invisible — config changed but the header kept reading A-Z.
            check(backend.gameSort() == QStringLiteral("Z-A"),
                  "first Enter updates the backend before the view refreshes");
            check(QQmlProperty::read(view, "gameSortLabel").toString() == QStringLiteral("Z-A"),
                  "first Enter flips the rendered header to Z-A");
        }

        // Live Game Sort change: same game stays selected, header/strip flip.
        // No manual backend pre-set here — the only write is the Enter above.
        if (list) {
            QCoreApplication::processEvents(); // let the ListView settle on its new model
            QVariantList flipped = QQmlProperty::read(list, "model").toList();
            check(!flipped.isEmpty() && flipped.first().toMap().value("name").toString()
                  == QStringLiteral("Metal Gear Solid"),
                  "Z-A list starts at the descending end of the alphabet");
            // Same-game selection across a flip is intentionally not asserted
            // here: Qt 6.11 offscreen ignores a ListView currentIndex write made
            // immediately after a model swap, so the C++ side reads a stale 0.
            // The ordering regression itself is pinned by appcore-setting-order.
            QVariantList flippedIndex = QQmlProperty::read(view, "letterIndex").toList();
            QString flippedLetters;
            for (const QVariant &bucket : flippedIndex)
                flippedLetters += bucket.toMap().value("letter").toString();
            check(flippedLetters == QStringLiteral("MLDA#"),
                  "live sort flip rebuilds the strip in Z-A order (MLDA#)");
        }

        // A second Enter goes through the same real path back to A-Z.
        if (header && list) {
            sendKey(header, Qt::Key_Return);
            QCoreApplication::processEvents();
            check(backend.gameSort() == QStringLiteral("A-Z") &&
                  QQmlProperty::read(view, "gameSortLabel").toString() == QStringLiteral("A-Z"),
                  "second Enter restores A-Z in the backend and the header");
            QCoreApplication::processEvents(); // let the ListView settle on its new model
            QVariantList model = QQmlProperty::read(list, "model").toList();
            check(!model.isEmpty() && model.first().toMap().value("name").toString()
                  == QStringLiteral("007 Goldeneye"),
                  "flip back to A-Z rebuilds the A-Z list order");
            QString letters;
            for (const QVariant &bucket : QQmlProperty::read(view, "letterIndex").toList())
                letters += bucket.toMap().value("letter").toString();
            check(letters == QStringLiteral("#ADLM"),
                  "flip back to A-Z rebuilds the strip (#ADLM)");
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
        QObject *header = view->findChild<QObject *>("recentSortDirection");
        QObject *count = view->findChild<QObject *>("recentSortCount");
        check(count && QQmlProperty::read(count, "text").toString()
              == QStringLiteral("5 GAMES  ·  ") &&
              header && QQmlProperty::read(header, "text").toString() == QStringLiteral("Z-A"),
              "header splits count and sort direction (N GAMES · Z-A)");
        QObject *list = view->findChild<QObject *>("recentList");
        QObject *strip = view->findChild<QObject *>("letterList");
        if (list) primeListIndex(list, 0);
        if (list) {
            QVariantList model = QQmlProperty::read(list, "model").toList();
            check(!model.isEmpty() && model.first().toMap().value("name").toString()
                  == QStringLiteral("Metal Gear Solid"),
                  "Z-A list starts at the descending end of the alphabet");
        }
        if (list && strip) {
            primeListIndex(list, 4); // "007 Goldeneye" -> '#'
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
        QObject *count = view->findChild<QObject *>("gamesSortCount");
        check(header && strip && count, "Games view has a sort header and letter strip");
        if (header && count) {
            check(QQmlProperty::read(count, "text").toString()
                  == QStringLiteral("5 GAMES  ·  ") &&
                  QQmlProperty::read(header, "text").toString() == QStringLiteral("A-Z"),
                  "Games header splits count and sort direction (N GAMES · A-Z)");
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
            QCoreApplication::processEvents();
            check(backend.gameSort() == QStringLiteral("Z-A") &&
                  QQmlProperty::read(view, "gameSortLabel").toString() == QStringLiteral("Z-A"),
                  "Games: first Enter flips backend and rendered header together");
            sendKey(header, Qt::Key_Down);
            check(!QQmlProperty::read(view, "sortHeaderFocused").toBool() &&
                  QQmlProperty::read(strip, "currentIndex").toInt() == 0,
                  "Games: Down from the header returns to the same letter");
        }
        delete view;
    }

    // ---- Playlists.qml: same focusable sort control, SYSTEM / COMPANY ----
    {
        QQmlEngine engine;
        engine.addImportPath(repoRoot + "/views");
        StubBackend backend; StubAppCore core; StubRoot root; StubModule module;
        QObject *view = loadView(engine, backend, core, root, module, repoRoot,
                                 QStringLiteral("Playlists.qml"));
        if (!view) return 1;

        QObject *header = view->findChild<QObject *>("playlistSortDirection");
        QObject *count = view->findChild<QObject *>("playlistSortCount");
        QObject *list = view->findChild<QObject *>("playlistList");
        check(header && count && list, "Playlists view has a sort header and list");
        if (header && count) {
            check(QQmlProperty::read(count, "text").toString()
                  == QStringLiteral("4 PLAYLISTS  ·  ") &&
                  QQmlProperty::read(header, "text").toString() == QStringLiteral("SYSTEM"),
                  "Playlists header splits count and sort direction");
        }
        if (header && list) {
            QQmlProperty::write(list, "currentIndex", 0);
            QMetaObject::invokeMethod(list, "forceActiveFocus");
            sendKey(list, Qt::Key_Up);
            check(QQmlProperty::read(view, "sortHeaderFocused").toBool(),
                  "Playlists: Up past the first row focuses the sort header");
            QObject *highlight = view->findChild<QObject *>("sortHeaderHighlight");
            check(highlight && QQmlProperty::read(highlight, "visible").toBool() &&
                  count && QQmlProperty::read(highlight, "width").toDouble()
                  < QQmlProperty::read(count, "width").toDouble(),
                  "Playlists: accent highlights only the direction word");
            sendKey(header, Qt::Key_Return);
            check(core.savedModule() == QStringLiteral("com.240mp.retroarch") &&
                  core.savedKey() == QStringLiteral("playlist_sort") &&
                  core.savedValue().toString() == QStringLiteral("COMPANY"),
                  "Playlists: Enter persists the toggled playlist sort");
            QCoreApplication::processEvents();
            check(backend.playlistSort() == QStringLiteral("COMPANY") &&
                  QQmlProperty::read(view, "playlistSortLabel").toString()
                  == QStringLiteral("COMPANY"),
                  "Playlists: first Enter flips backend and rendered header together");
            sendKey(header, Qt::Key_Down);
            check(!QQmlProperty::read(view, "sortHeaderFocused").toBool(),
                  "Playlists: Down from the header returns to the list");
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
