// Offscreen smoke test for the Local Files Player view contract.
//
// Player.qml decides loop, shuffle and resume entirely from settings + history.
// Those branches cannot be reached from a pure C++ unit test, so this harness
// loads the real view with stub backends and inspects the exact loadAndPlay()
// call that would reach mpv. Covers two user-visible regressions:
//   * legacy string toggle "OFF" must not turn looping ON (!!"OFF" is true in JS);
//   * a saved playlist item with pos=0, plPos>0 must resume that item, not restart
//     the playlist from the first entry.
#include <QGuiApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantHash>
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
    QVariantMap saved;
    Q_INVOKABLE bool isPlaylist(const QString &path) const {
        return path.endsWith(QLatin1String(".m3u"), Qt::CaseInsensitive);
    }
    Q_INVOKABLE bool isImage(const QString &) const { return false; }
    Q_INVOKABLE bool playlistContainsImages(const QString &) const { return false; }
    Q_INVOKABLE QVariantMap getSavedPosition(const QString &) const { return saved; }
    Q_INVOKABLE void savePosition(const QString &, int, int) {}
    Q_INVOKABLE void clearPosition(const QString &) {}
signals:
    void dynamicOptionsReady(const QString &, const QVariant &);
};

class StubMpv : public QObject {
    Q_OBJECT
public:
    int calls = 0;
    float startSeconds = -1.0f;
    bool loop = false;
    int playlistStart = -99;
    bool shuffle = false;

    Q_INVOKABLE void loadAndPlay(const QString &, float startSeconds_, int, int,
                                 const QStringList &, const QStringList &,
                                 bool loop_, int playlistStart_, float,
                                 const QString &, bool, const QString &,
                                 bool shuffle_, const QStringList &, float, bool) {
        ++calls;
        startSeconds = startSeconds_;
        loop = loop_;
        playlistStart = playlistStart_;
        shuffle = shuffle_;
    }
    Q_INVOKABLE void sendKey(const QString &) {}
signals:
    void positionChanged(int);
    void durationChanged(int);
    void playlistPosChanged(int);
    void playbackEnded(int, int, const QString &);
};

struct Fixture {
    QQmlEngine engine;
    StubAppCore core;
    StubModuleRoot module;
    StubRoot root;
    StubLocalFiles localFiles;
    StubMpv mpv;
    QObject *view = nullptr;
};

static Fixture *makeFixture(const QString &filePath, const QVariantMap &settings,
                            const QVariantMap &saved, const QString &tag) {
    auto *f = new Fixture;
    f->core.settings = settings;
    f->localFiles.saved = saved;
    auto *ctx = f->engine.rootContext();
    ctx->setContextProperty("appCore", QVariant::fromValue(&f->core));
    ctx->setContextProperty("moduleRoot", QVariant::fromValue(&f->module));
    ctx->setContextProperty("root", QVariant::fromValue(&f->root));
    ctx->setContextProperty("localFilesBackend", QVariant::fromValue(&f->localFiles));
    ctx->setContextProperty("mpvController", QVariant::fromValue(&f->mpv));

    const QString rootDir = QString::fromUtf8(MP240_REPO_ROOT);
    QQmlComponent comp(&f->engine, QUrl::fromLocalFile(
        rootDir + "/modules/local_files/views/Player.qml"));
    if (comp.status() == QQmlComponent::Error) {
        for (const auto &e : comp.errors())
            std::printf("  ERR  %s: %s\n", qPrintable(tag), e.description().toLocal8Bit().constData());
        check(false, qPrintable(tag + ": Player.qml compiles"));
        return f;
    }
    QVariantMap nav;
    nav["filePath"] = filePath;
    nav["title"] = QStringLiteral("Test Playlist");
    // Qt 6 removed QQmlComponent::create(initialProperties); wrap the real view
    // in a tiny inline component so navParams is set before onCompleted runs.
    auto *childCtx = new QQmlContext(&f->engine);
    childCtx->setContextProperty("initialNavParams", nav);
    const QString viewsUrl = QUrl::fromLocalFile(rootDir + "/modules/local_files/views").toString();
    QQmlComponent wrapper(&f->engine);
    wrapper.setData(("import QtQuick\nimport \"" + viewsUrl +
                     "\"\nPlayer { navParams: initialNavParams }\n").toUtf8(),
                    QUrl::fromLocalFile(rootDir + "/modules/local_files/views/PlayerSmokeWrapper.qml"));
    if (wrapper.status() == QQmlComponent::Error) {
        for (const auto &e : wrapper.errors())
            std::printf("  ERR  %s: %s\n", qPrintable(tag), e.description().toLocal8Bit().constData());
    }
    f->view = wrapper.create(childCtx);
    if (!f->view) check(false, qPrintable(tag + ": Player.qml instantiates"));
    return f;
}

static void sendKey(QObject *target, Qt::Key key) {
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &event);
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    const QString list = QStringLiteral("/srv/media/favorites.m3u");
    const QVariantMap base = {
        {"resume_playback", "yes"}, {"shuffle_playback", "no"},
        {"auto_subtitles", "forced"}, {"sub_lang", "-"},
        {"image_duration", 5}, {"loop_playback", false}
    };

    // 1. Legacy string toggles. ModuleSettings still accepts "ON"/"OFF" strings
    // from older config.json files, but Player.qml's !!() turns "OFF" into true.
    {
        QVariantMap s = base; s["loop_playback"] = QStringLiteral("OFF");
        auto *f = makeFixture(list, s, {}, "loop-off-string");
        if (f->view) {
            check(f->mpv.calls == 1, "loop OFF (legacy string) still starts playback");
            check(!f->mpv.loop, "loop OFF (legacy string) does not enable mpv loop");
            delete f->view;
        }
        delete f;
    }
    {
        QVariantMap s = base; s["loop_playback"] = QStringLiteral("ON");
        auto *f = makeFixture(list, s, {}, "loop-on-string");
        if (f->view) check(f->mpv.loop, "loop ON (legacy string) enables mpv loop");
        delete f->view; delete f;
    }
    {
        auto *f = makeFixture(list, base, {}, "loop-off-bool");
        if (f->view) check(!f->mpv.loop, "loop OFF (bool) does not enable mpv loop");
        delete f->view; delete f;
    }

    // 2. Playlist resume with zero elapsed time but a saved item index.
    // Saving (pos=0, plPos=2) happens when the user quits within the first
    // second of item 3: onPlaylistPosChanged resets lastKnownPositionMs to 0
    // and onPositionChanged ignores ms<=0. The next open must still pick item 3.
    {
        QVariantMap s = base; s["resume_playback"] = "ask";
        auto *f = makeFixture(list, s, {{"pos", 0}, {"plPos", 2}}, "plpos-only");
        if (f->view) {
            check(QQmlProperty::read(f->view, "overlayVisible").toBool(),
                  "pos=0 plPos=2 offers a resume choice");
            QVariantList choices = QQmlProperty::read(f->view, "choices").toList();
            check(!choices.isEmpty() &&
                  choices.first().toMap().value("plPos").toInt() == 2,
                  "pos=0 plPos=2 resume choice carries item index 2");
            sendKey(f->view, Qt::Key_Return);
            check(f->mpv.calls == 1 && f->mpv.playlistStart == 2,
                  "pos=0 plPos=2 resumes playlist at item 2");
            delete f->view;
        }
        delete f;
    }
    // Same state with Resume=Always: resume directly, no dialog.
    {
        auto *f = makeFixture(list, base, {{"pos", 0}, {"plPos", 2}}, "plpos-always");
        if (f->view) {
            check(f->mpv.calls == 1 && f->mpv.playlistStart == 2 &&
                  f->mpv.startSeconds == 0.0f,
                  "Resume=Always with pos=0 plPos=2 starts item 2 from 0s");
            delete f->view;
        }
        delete f;
    }
    // Control: normal time + item resume still works after the fix.
    {
        QVariantMap s = base; s["resume_playback"] = "ask";
        auto *f = makeFixture(list, s, {{"pos", 65000}, {"plPos", 3}}, "full-resume");
        if (f->view) {
            check(QQmlProperty::read(f->view, "overlayVisible").toBool(),
                  "pos>0 plPos=3 offers a resume choice");
            sendKey(f->view, Qt::Key_Return);
            check(f->mpv.calls == 1 && f->mpv.playlistStart == 3 &&
                  f->mpv.startSeconds > 64.0f && f->mpv.startSeconds < 66.0f,
                  "pos>0 plPos=3 resumes at 65s on item 3");
            delete f->view;
        }
        delete f;
    }
    // Control: resume=Never ignores saved item index too.
    {
        QVariantMap s = base; s["resume_playback"] = "no";
        auto *f = makeFixture(list, s, {{"pos", 0}, {"plPos", 2}}, "resume-never");
        if (f->view) {
            check(f->mpv.calls == 1 && f->mpv.playlistStart == -1,
                  "resume=Never restarts the playlist");
            delete f->view;
        }
        delete f;
    }

    if (failures) {
        std::printf("local-files-player-smoke: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("local-files-player-smoke: all checks passed\n");
    return 0;
}

#include "local-files-player-smoke-test.moc"
