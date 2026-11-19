// YouTube list-selection telemetry smoke. Loads each real primary list view
// offscreen with local stub data, sends an ordinary Down key, and requires the
// shared row tracker to name the newly highlighted row. No network or playback.
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <cstdio>
#include <functional>
#include <memory>

class StubAppCore : public QObject {
    Q_OBJECT
public:
    QStringList states;
    QVariantMap settings;

    Q_INVOKABLE QVariant get_setting(const QString &, const QString &key) const {
        if (key == QLatin1String("playback_resolution")) return QStringLiteral("480p");
        if (key == QLatin1String("display_shorts")) return QStringLiteral("ON");
        return settings.value(key);
    }
    Q_INVOKABLE void save_setting(const QString &, const QString &key, const QVariant &value) {
        settings[key] = value;
    }
    Q_INVOKABLE QVariantMap get_module_info(const QString &) const {
        return {{QStringLiteral("name"), QStringLiteral("YouTube")},
                {QStringLiteral("icon"), QString() }};
    }
    Q_INVOKABLE void note_state(const QString &event, const QString &detail) {
        states << event + QLatin1Char(' ') + detail;
    }
    Q_INVOKABLE bool telemetryEnabled() const { return true; }
signals:
    void moduleSettingChanged(const QString &, const QString &, const QVariant &);
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
    QColor surfaceColorRead() const { return QColor(QStringLiteral("#101018")); }
    QColor primaryColorRead() const { return QColor(QStringLiteral("#e6e6e6")); }
    QColor secondaryColorRead() const { return QColor(QStringLiteral("#8a8a9a")); }
    QColor tertiaryColorRead() const { return QColor(QStringLiteral("#5a5a6a")); }
    QColor accentColorRead() const { return QColor(QStringLiteral("#ffcc00")); }
    QVariantMap hintsRead() const {
        return {{QStringLiteral("back"), QStringLiteral("BACK")},
                {QStringLiteral("navigate"), QStringLiteral("NAV")},
                {QStringLiteral("select"), QStringLiteral("SELECT")},
                {QStringLiteral("left"), QStringLiteral("LEFT")},
                {QStringLiteral("right"), QStringLiteral("RIGHT")}};
    }
};

class StubModule : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString moduleId READ moduleId CONSTANT)
    Q_PROPERTY(QString moduleName READ moduleName CONSTANT)
    Q_PROPERTY(QString moduleIcon READ moduleIcon CONSTANT)
public:
    QString moduleId() const { return QStringLiteral("com.240mp.youtube"); }
    QString moduleName() const { return QStringLiteral("YouTube"); }
    QString moduleIcon() const { return QString(); }
};

class StubYouTube : public QObject {
    Q_OBJECT
public:
    QVariantList watchLater;
    QStringList warmedIds;
    int warmCalls = 0;

    StubYouTube() {
        watchLater = {
            QVariantMap{{"videoId", "first"}, {"title", "First Fixture"}, {"channelName", "Lab"}},
            QVariantMap{{"videoId", "second"}, {"title", "Second Fixture"}, {"channelName", "Lab"}}
        };
    }
    Q_INVOKABLE QVariantMap check_subscriptions() const {
        return {{QStringLiteral("ok"), true}, {QStringLiteral("fileExists"), true},
                {QStringLiteral("error"), QString()}, {QStringLiteral("channelCount"), 2}};
    }
    Q_INVOKABLE QVariantMap check_playlists() const {
        return {{QStringLiteral("ok"), true}, {QStringLiteral("fileExists"), true},
                {QStringLiteral("error"), QString()}, {QStringLiteral("playlistCount"), 2}};
    }
    Q_INVOKABLE QVariantMap check_signin() const {
        return {{QStringLiteral("signedIn"), false}};
    }
    Q_INVOKABLE QVariantList getWatchLater() const { return watchLater; }
    Q_INVOKABLE QVariantList getHistory() const { return {}; }
    Q_INVOKABLE bool isInWatchLater(const QString &) const { return true; }
    Q_INVOKABLE void addToWatchLater(const QString &, const QString &, const QString &) {}
    Q_INVOKABLE void removeFromWatchLater(const QString &) {}
    Q_INVOKABLE void warmListPlayback(const QVariant &ids, const QString &) {
        ++warmCalls;
        warmedIds = ids.toStringList();
    }
    Q_INVOKABLE void requestSponsorSegments(const QString &) {}
    Q_INVOKABLE void resolvePlayback(const QString &, const QString &) {}
    Q_INVOKABLE void load_playlists() {}
    Q_INVOKABLE void load_channels() {}

signals:
    void subscriptionsFeedLoaded(const QVariant &items);
    void channelVideosLoaded(const QVariant &items);
    void playlistVideosLoaded(const QVariant &items);
    void playlistsLoaded(const QVariant &items);
    void channelsLoaded(const QVariant &items);
    void accountFeedLoaded(const QVariant &items);
    void accountPlaylistsLoaded(const QVariant &items);
    void accountHistoryLoaded(const QVariant &items);
    void accountWatchLaterLoaded(const QVariant &items);
    void errorOccurred(const QString &message);
};

static int failures = 0;
static void check(bool ok, const char *what) {
    if (!ok) { ++failures; std::fprintf(stderr, "  FAIL  %s\n", what); }
    else     { std::printf("  ok    %s\n", what); }
}

static bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 5000) {
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    while (!predicate() && QDateTime::currentMSecsSinceEpoch() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(10);
    }
    return predicate();
}

static bool hasState(const StubAppCore &core, const QString &expected) {
    return core.states.contains(expected);
}

static std::unique_ptr<QQuickView> makeView(const QString &file, const QVariantMap &nav,
                                            StubAppCore &core, StubRoot &root,
                                            StubModule &module, StubYouTube &youtube,
                                            const QString &repoRoot) {
    auto view = std::make_unique<QQuickView>();
    view->engine()->addImportPath(repoRoot + QStringLiteral("/views"));
    auto *ctx = view->engine()->rootContext();
    ctx->setContextProperty(QStringLiteral("appCore"), &core);
    ctx->setContextProperty(QStringLiteral("root"), &root);
    ctx->setContextProperty(QStringLiteral("moduleRoot"), &module);
    ctx->setContextProperty(QStringLiteral("youtubeBackend"), &youtube);
    view->setInitialProperties({{QStringLiteral("navParams"), nav}});
    view->setResizeMode(QQuickView::SizeRootObjectToView);
    view->resize(640, 480);
    view->setSource(QUrl::fromLocalFile(repoRoot + QStringLiteral("/modules/youtube/views/") + file));
    if (view->status() == QQuickView::Error) {
        for (const auto &error : view->errors())
            std::fprintf(stderr, "  QML  %s\n", qPrintable(error.description()));
        return view;
    }
    view->show();
    view->requestActivate();
    if (view->rootObject()) view->rootObject()->forceActiveFocus();
    waitUntil([&view] { return view->isActive(); });
    return view;
}

static void downAndExpect(QQuickView *view, StubAppCore &core,
                          const QString &expected, const char *what) {
    core.states.clear();
    QTest::keyClick(view, Qt::Key_Down);
    check(waitUntil([&core, &expected] { return hasState(core, expected); }), what);
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication app(argc, argv);
    const QString repoRoot = QString::fromUtf8(MP240_REPO_ROOT);
    StubAppCore core;
    StubRoot root;
    StubModule module;
    StubYouTube youtube;

    {
        auto view = makeView(QStringLiteral("Items.qml"), {}, core, root, module, youtube, repoRoot);
        check(view->rootObject() != nullptr, "Items.qml instantiates");
        if (view->rootObject())
            downAndExpect(view.get(), core, QStringLiteral("select Channels idx=1"),
                          "YouTube Items logs the highlighted string row");
    }
    {
        auto view = makeView(QStringLiteral("Subscriptions.qml"),
                             {{QStringLiteral("mode"), QStringLiteral("watchlater")}},
                             core, root, module, youtube, repoRoot);
        check(view->rootObject() != nullptr, "Subscriptions.qml instantiates");
        check(youtube.warmCalls == 1 && youtube.warmedIds == QStringList({"first", "second"}),
              "Watch Later still warms both local rows");
        if (view->rootObject())
            downAndExpect(view.get(), core, QStringLiteral("select Second Fixture idx=1"),
                          "YouTube video list logs the highlighted title");
    }
    {
        auto view = makeView(QStringLiteral("Playlists.qml"), {}, core, root, module, youtube, repoRoot);
        check(view->rootObject() != nullptr, "Playlists.qml instantiates");
        if (view->rootObject()) {
            emit youtube.playlistsLoaded(QVariantList{
                QVariantMap{{"title", "One"}, {"playlistId", "PL1"}},
                QVariantMap{{"title", "Two"}, {"playlistId", "PL2"}}});
            check(waitUntil([&core] { return core.states.contains(QStringLiteral("yt-rows view=playlists path=file rows=2")); }),
                  "playlist fixture rows load");
            downAndExpect(view.get(), core, QStringLiteral("select Two idx=1"),
                          "YouTube playlists log the highlighted title");
        }
    }
    {
        auto view = makeView(QStringLiteral("Channels.qml"), {}, core, root, module, youtube, repoRoot);
        check(view->rootObject() != nullptr, "Channels.qml instantiates");
        if (view->rootObject()) {
            emit youtube.channelsLoaded(QVariantList{
                QVariantMap{{"title", "Alpha"}, {"channelId", "UC1"}},
                QVariantMap{{"title", "Beta"}, {"channelId", "UC2"}}});
            check(waitUntil([&core] { return core.states.contains(QStringLiteral("yt-rows view=channels path=file rows=2")); }),
                  "channel fixture rows load");
            downAndExpect(view.get(), core, QStringLiteral("select Beta idx=1"),
                          "YouTube channels log the highlighted title");
        }
    }
    {
        auto view = makeView(QStringLiteral("SponsorSettings.qml"), {}, core, root, module, youtube, repoRoot);
        check(view->rootObject() != nullptr, "SponsorSettings.qml instantiates");
        if (view->rootObject())
            downAndExpect(view.get(), core, QStringLiteral("select SELF PROMO idx=1"),
                          "YouTube settings list logs the highlighted label");
    }

    std::printf("YouTube telemetry smoke: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

#include "youtube-telemetry-smoke-test.moc"
