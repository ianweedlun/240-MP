// Offscreen smoke test for the Local Files Player empty-filePath launch guard (R10).
//
// Player.qml's Component.onCompleted used to silently `return` on an empty
// filePath: no mpv child was ever spawned, and Back/ESC (sendKey-only) had
// nothing to stop — pre-connect the key is dropped, and even a guaranteed
// forceStop is a no-op with no child, so the user was stranded on a black
// view. The guard must emit goBack() at the launch entry instead, mirroring
// YouTube's empty-URL guard (modules/youtube/views/Player.qml).
//
// Two-sided by design: on the pre-fix snapshot the goBack checks FAIL (the
// signal never fires); with the guard they pass. The valid-path control keeps
// the guard honest — a normal launch must still reach mpv and must not goBack.
//
// goBack fires during Component.onCompleted — i.e. inside QQmlComponent::create()
// — so a spy connected after creation would miss it. The wrapper QML routes the
// signal into a stub recorder (onGoBack: goBackRecorder.record()), synchronous
// with the emission. Same stub-backend + real-view pattern as
// local-files-player-smoke-test.cpp.
#include <QGuiApplication>
#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QColor>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char *what) {
    if (!ok) { ++failures; std::printf("  FAIL  %s\n", what); }
    else     { std::printf("  ok    %s\n", what); }
}

class StubAppCore : public QObject {
    Q_OBJECT
public:
    QVariantMap settings;
    Q_INVOKABLE QVariant get_setting(const QString &, const QString &key) const {
        return settings.value(key);
    }
signals:
    void moduleSettingChanged(const QString &, const QString &, const QVariant &);
};

class StubModuleRoot : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString moduleId READ moduleId CONSTANT)
public:
    QString moduleId() const { return QStringLiteral("com.240mp.local_files"); }
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
public:
    double sh() const { return 480; }
    double sw() const { return 640; }
    QString globalFont() const { return QStringLiteral("VCR OSD Mono"); }
    QColor surfaceColor() const { return QColor("#101018"); }
    QColor primaryColor() const { return QColor("#e6e6e6"); }
    QColor secondaryColor() const { return QColor("#8a8a9a"); }
    QColor tertiaryColor() const { return QColor("#5a5a6a"); }
    QColor accentColor() const { return QColor("#ffcc00"); }
    QVariantMap hints() const {
        QVariantMap m;
        m["back"] = "BACK"; m["navigate"] = "NAV";
        m["select"] = "SELECT"; m["browse"] = "BROWSE";
        return m;
    }
};

class StubLocalFiles : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE bool isPlaylist(const QString &path) const {
        return path.endsWith(QLatin1String(".m3u"), Qt::CaseInsensitive);
    }
    Q_INVOKABLE bool isImage(const QString &) const { return false; }
    Q_INVOKABLE bool playlistContainsImages(const QString &) const { return false; }
    Q_INVOKABLE QVariantMap getSavedPosition(const QString &) const { return {}; }
    Q_INVOKABLE void savePosition(const QString &, int, int) {}
    Q_INVOKABLE void clearPosition(const QString &) {}
signals:
    void dynamicOptionsReady(const QString &, const QVariant &);
};

class StubMpv : public QObject {
    Q_OBJECT
public:
    int calls = 0;
    Q_INVOKABLE void loadAndPlay(const QString &, float, int, int,
                                 const QStringList &, const QStringList &,
                                 bool, int, float,
                                 const QString &, bool, const QString &,
                                 bool, const QStringList &, float, bool) {
        ++calls;
    }
    Q_INVOKABLE void sendKey(const QString &) {}
signals:
    void positionChanged(int);
    void durationChanged(int);
    void playlistPosChanged(int);
    void playbackEnded(int, int, const QString &);
};

// Counts goBack emissions. The wrapper QML calls record() from the view's
// onGoBack handler, so emissions during Component.onCompleted are captured.
class GoBackRecorder : public QObject {
    Q_OBJECT
public:
    int count = 0;
    Q_INVOKABLE void record() { ++count; }
};

struct Fixture {
    QQmlEngine engine;
    StubAppCore core;
    StubModuleRoot module;
    StubRoot root;
    StubLocalFiles localFiles;
    StubMpv mpv;
    GoBackRecorder back;
    QObject *view = nullptr;
};

static Fixture *makeFixture(const QVariantMap &nav, const QString &tag) {
    auto *f = new Fixture;
    // Resume=Always so a valid path launches directly, no overlay decision.
    f->core.settings = {
        {"resume_playback", "yes"}, {"shuffle_playback", "no"},
        {"auto_subtitles", "forced"}, {"sub_lang", "-"},
        {"image_duration", 5}, {"loop_playback", false}
    };
    auto *ctx = f->engine.rootContext();
    ctx->setContextProperty("appCore", QVariant::fromValue(&f->core));
    ctx->setContextProperty("moduleRoot", QVariant::fromValue(&f->module));
    ctx->setContextProperty("root", QVariant::fromValue(&f->root));
    ctx->setContextProperty("localFilesBackend", QVariant::fromValue(&f->localFiles));
    ctx->setContextProperty("mpvController", QVariant::fromValue(&f->mpv));
    ctx->setContextProperty("goBackRecorder", QVariant::fromValue(&f->back));

    const QString rootDir = QString::fromUtf8(MP240_REPO_ROOT);
    // Qt 6 removed QQmlComponent::create(initialProperties); wrap the real view
    // in a tiny inline component so navParams is set before onCompleted runs.
    // The onGoBack handler captures the launch-entry emission (see header).
    auto *childCtx = new QQmlContext(&f->engine);
    childCtx->setContextProperty("initialNavParams", nav);
    const QString viewsUrl = QUrl::fromLocalFile(rootDir + "/modules/local_files/views").toString();
    QQmlComponent wrapper(&f->engine);
    wrapper.setData(("import QtQuick\nimport \"" + viewsUrl +
                     "\"\nPlayer { navParams: initialNavParams; onGoBack: goBackRecorder.record() }\n").toUtf8(),
                    QUrl::fromLocalFile(rootDir + "/modules/local_files/views/EmptyPathSmokeWrapper.qml"));
    if (wrapper.status() == QQmlComponent::Error) {
        for (const auto &e : wrapper.errors())
            std::printf("  ERR  %s: %s\n", qPrintable(tag), e.description().toLocal8Bit().constData());
        check(false, qPrintable(tag + ": Player.qml compiles"));
        return f;
    }
    f->view = wrapper.create(childCtx);
    if (!f->view) check(false, qPrintable(tag + ": Player.qml instantiates"));
    return f;
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    // 1. Explicit empty filePath: the launch entry must goBack(), not strand
    //    the view. Pre-fix this FAILS: onCompleted returns silently, no signal.
    {
        auto *f = makeFixture({{"filePath", ""}, {"title", "Empty"}}, "empty-string");
        if (f->view) {
            check(f->back.count == 1, "empty filePath emits goBack exactly once");
            check(f->mpv.calls == 0, "empty filePath spawns no mpv playback");
            delete f->view;
        }
        delete f;
    }

    // 2. navParams without a filePath key at all: `navParams.filePath || ""`
    //    coerces undefined to the same empty-string edge.
    {
        auto *f = makeFixture({{"title", "Missing"}}, "missing-key");
        if (f->view) {
            check(f->back.count == 1, "missing filePath key emits goBack exactly once");
            check(f->mpv.calls == 0, "missing filePath key spawns no mpv playback");
            delete f->view;
        }
        delete f;
    }

    // 3. Control: a valid filePath still launches and does NOT goBack — the
    //    guard must not swallow normal playback.
    {
        auto *f = makeFixture({{"filePath", "/srv/media/video.mp4"}, {"title", "Valid"}}, "valid-path");
        if (f->view) {
            check(f->mpv.calls == 1, "valid filePath still starts playback");
            check(f->back.count == 0, "valid filePath does not goBack at launch");
            delete f->view;
        }
        delete f;
    }

    if (failures) {
        std::printf("local-files-empty-path-smoke: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("local-files-empty-path-smoke: all checks passed\n");
    return 0;
}

#include "local-files-empty-path-smoke-test.moc"