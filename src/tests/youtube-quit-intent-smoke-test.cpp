// Offscreen QML smoke test for the R-2 YouTube launch-guard (SPEC-3).
//
// The YouTube Player's ESC/BS Back guard used to key off playbackStarted (the
// first mpv position event). A Back after doPlay() spawned mpv but before that
// first position took the goBack() branch and orphaned the child — no stop at
// all. The fix introduces launchDone (true the instant doPlay calls loadAndPlay,
// reset in preparePlayback/playAt) and keys the guard off it, so a post-launch /
// pre-playbackStarted Back routes through sendKey("ESC") — which the controller
// now guarantees delivery of (forceStop pre-connect) — instead of goBack().
//
// This test loads the REAL Player.qml with stub backends and asserts the routing
// behaviorally: which of goBack() vs mpvController.sendKey() fires for a Back in
// each launch phase. It does not snapshot source text; it dispatches real key
// events and observes the view's signals.
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlProperty>
#include <QMetaObject>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QColor>
#include <QUrl>
#include <QElapsedTimer>
#include <QThread>
#include <cstdio>

// ---- stub mpv: records loadAndPlay + sendKey, emits the playback signals ----
class StubMpv : public QObject {
    Q_OBJECT
public:
    int loadCalls = 0;
    QString lastUrl;
    QStringList keys;
    Q_INVOKABLE void loadAndPlay(const QString &url, float, int, int,
                                 const QStringList &, const QStringList &,
                                 bool, int, float, const QString &, bool,
                                 const QString &, bool, const QStringList &,
                                 float, bool, const QStringList &) {
        ++loadCalls;
        lastUrl = url;
    }
    // Model the fixed controller's guaranteed delivery: a quit-intent key
    // reaps the (stub) child and emits playbackEnded("stopped"), exactly as
    // MpvController::sendKey -> forceStop does pre-connect. Enabled per-case.
    bool emitEndedOnQuit = false;
    Q_INVOKABLE void sendKey(const QString &key) {
        keys.append(key);
        if (emitEndedOnQuit && (key == QLatin1String("ESC") || key == QLatin1String("BS")
                                || key == QLatin1String("STOP")))
            emit playbackEnded(0, 0, QStringLiteral("stopped"));
    }
    Q_INVOKABLE void setOscRanges(const QString &) {}
signals:
    void positionChanged(int);
    void durationChanged(int);
    void playbackEnded(int, int, const QString &);
};

// ---- stub youtube backend: warm pair on demand, else cold ----
class StubYoutube : public QObject {
    Q_OBJECT
public:
    bool warm = false;
    int savedPos = 0;
    Q_INVOKABLE QString cachedPlaybackUrl(const QString &, const QString &) const {
        return warm ? QStringLiteral("https://warm.example/video") : QString();
    }
    Q_INVOKABLE QString cachedPlaybackAudioUrl(const QString &, const QString &) const {
        return warm ? QStringLiteral("https://warm.example/audio") : QString();
    }
    Q_INVOKABLE bool cachedPlaybackSingle(const QString &, const QString &) const { return false; }
    Q_INVOKABLE void resolvePlayback(const QString &, const QString &) {}
    Q_INVOKABLE void requestSponsorSegments(const QString &) {}
    Q_INVOKABLE QVariantMap buildPlaybackEdl(const QString &, const QString &, const QVariantMap &) const {
        return {};
    }
    Q_INVOKABLE QVariantMap getSavedPosition(const QString &) const {
        QVariantMap m; m["pos"] = savedPos; return m;
    }
    int saveCalls = 0;
    Q_INVOKABLE void savePosition(const QString &, int, const QString &, const QString &) { ++saveCalls; }
    Q_INVOKABLE void invalidateCachedUrl(const QString &, const QString &) {}
    Q_INVOKABLE QString ytdlFormatForResolution(const QString &) const { return QStringLiteral("best"); }
signals:
    void playbackUrlReady(const QString &);
    void sponsorReady(const QString &);
};

class StubAppCore : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE QVariant get_setting(const QString &, const QString &key) const {
        if (key == QLatin1String("resume_playback")) return QStringLiteral("Ask");
        if (key == QLatin1String("playback_resolution")) return QStringLiteral("480p");
        if (key == QLatin1String("autoplay_playlist_items")) return QStringLiteral("OFF");
        return QStringLiteral("off");   // sponsor_* defaults
    }
    Q_INVOKABLE void note_state(const QString &, const QString &) {}
};

class StubModuleRoot : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString moduleId READ moduleId CONSTANT)
public:
    QString moduleId() const { return QStringLiteral("com.240mp.youtube"); }
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
        m["back"] = QStringLiteral("BACK"); m["navigate"] = QStringLiteral("NAV");
        m["select"] = QStringLiteral("SELECT");
        return m;
    }
};

static int errors = 0;
static void check(bool ok, const char *what) {
    if (!ok) { ++errors; std::printf("  FAIL  %s\n", what); }
    else     { std::printf("  ok    %s\n", what); }
}

static void sendKey(QObject *target, Qt::Key key) {
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &event);
}

// Spin the event loop for ms milliseconds so QML Timers (the 50 ms startTimer)
// fire and synchronous launch logic settles.
static void spin(int ms) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    }
    QCoreApplication::processEvents();
}

struct Fixture {
    QQmlEngine engine;
    StubAppCore core;
    StubModuleRoot module;
    StubRoot root;
    StubYoutube yt;
    StubMpv mpv;
    QObject *view = nullptr;
};

static Fixture *makeFixture(bool warm, int savedPos, const char *tag) {
    auto *f = new Fixture;
    f->yt.warm = warm;
    f->yt.savedPos = savedPos;
    auto *ctx = f->engine.rootContext();
    ctx->setContextProperty("appCore", QVariant::fromValue(&f->core));
    ctx->setContextProperty("moduleRoot", QVariant::fromValue(&f->module));
    ctx->setContextProperty("root", QVariant::fromValue(&f->root));
    ctx->setContextProperty("youtubeBackend", QVariant::fromValue(&f->yt));
    ctx->setContextProperty("mpvController", QVariant::fromValue(&f->mpv));

    const QString rootDir = QString::fromUtf8(MP240_REPO_ROOT);
    const QString viewsDir = rootDir + "/modules/youtube/views";
    QVariantMap item;
    item["url"] = QStringLiteral("https://www.youtube.com/watch?v=test123");
    item["videoId"] = QStringLiteral("test123");
    item["title"] = QStringLiteral("Test Video");
    item["channelName"] = QStringLiteral("Test Channel");
    QVariantMap nav;
    nav["item"] = item;
    nav["playlistItems"] = QVariantList{};
    nav["playlistIndex"] = -1;

    // Qt 6 removed QQmlComponent::create(initialProperties): wrap the real view
    // so navParams is bound before Component.onCompleted runs.
    auto *childCtx = new QQmlContext(&f->engine);
    childCtx->setContextProperty("initialNavParams", nav);
    QQmlComponent wrapper(&f->engine);
    wrapper.setData(("import QtQuick\nimport \"" + QUrl::fromLocalFile(viewsDir).toString() +
                     "\"\nPlayer { navParams: initialNavParams }\n").toUtf8(),
                    QUrl::fromLocalFile(viewsDir + "/PlayerQuitIntentWrapper.qml"));
    if (wrapper.status() == QQmlComponent::Error) {
        for (const auto &e : wrapper.errors())
            std::printf("  ERR  %s: %s\n", tag, e.description().toLocal8Bit().constData());
        check(false, "youtube Player.qml compiles");
        return f;
    }
    f->view = wrapper.create(childCtx);
    if (!f->view) {
        for (const auto &e : wrapper.errors())
            std::printf("  ERR  %s: %s\n", tag, e.description().toLocal8Bit().constData());
        check(false, "youtube Player.qml instantiates");
        return f;
    }
    // The wrapper component's root object IS the Player FocusScope.
    QMetaObject::invokeMethod(f->view, "forceActiveFocus");
    QCoreApplication::processEvents();
    return f;
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    // ---- A: warm launch, Back AFTER spawn but BEFORE first position ----
    // The orphan window. The key handler must route through sendKey("ESC")
    // (never a direct goBack()); the controller's guaranteed delivery then
    // reaps mpv and the ended handler returns the view. Full SPEC-3 chain.
    {
        Fixture *f = makeFixture(true, 0, "warm-preplayback");
        if (!f->view) return 1;
        spin(90);   // let the 50 ms startTimer fire doPlay -> loadAndPlay
        check(f->mpv.loadCalls == 1, "A: warm launch spawned mpv (loadAndPlay called)");
        check(QQmlProperty::read(f->view, "launchDone").toBool(), "A: launchDone set on spawn");
        check(!QQmlProperty::read(f->view, "playbackStarted").toBool(),
              "A: still pre-playbackStarted (no position event yet)");
        // Model the fixed controller: sendKey(ESC) reaps the child and emits
        // playbackEnded("stopped") -> the view's ended handler returns. SPEC-3:
        // no surviving mpv (stub reaped by construction), no resume write (pos 0),
        // view returned — and the key handler itself never called goBack().
        f->mpv.emitEndedOnQuit = true;
        QSignalSpy backSpy(f->view, SIGNAL(goBack()));
        sendKey(f->view, Qt::Key_Escape);
        QCoreApplication::processEvents();
        check(f->mpv.keys.contains(QStringLiteral("ESC")),
              "A: post-launch/pre-playbackStarted Back routes to sendKey(ESC)");
        check(backSpy.count() == 1, "A: view returned via playbackEnded (no orphan)");
        check(f->yt.saveCalls == 0, "A: early stop wrote no resume position");
        delete f;
    }

    // ---- B: cold resolve, Back BEFORE any launch ----
    // No mpv spawned yet: direct goBack(), no sendKey.
    {
        Fixture *f = makeFixture(false, 0, "cold-prelaunch");
        if (!f->view) return 1;
        spin(90);
        check(f->mpv.loadCalls == 0, "B: cold resolve has not spawned mpv");
        check(!QQmlProperty::read(f->view, "launchDone").toBool(), "B: launchDone false pre-launch");
        QSignalSpy backSpy(f->view, SIGNAL(goBack()));
        sendKey(f->view, Qt::Key_Escape);
        QCoreApplication::processEvents();
        check(backSpy.count() == 1, "B: pre-launch Back goes back directly");
        check(f->mpv.keys.isEmpty(), "B: pre-launch Back does not sendKey");
        delete f;
    }

    // ---- C: Back AFTER playbackStarted (normal playback) ----
    {
        Fixture *f = makeFixture(true, 0, "post-playback");
        if (!f->view) return 1;
        spin(90);
        emit f->mpv.positionChanged(1500);   // first position -> playbackStarted
        QCoreApplication::processEvents();
        check(QQmlProperty::read(f->view, "playbackStarted").toBool(), "C: playbackStarted set");
        QSignalSpy backSpy(f->view, SIGNAL(goBack()));
        sendKey(f->view, Qt::Key_Escape);
        QCoreApplication::processEvents();
        check(f->mpv.keys.contains(QStringLiteral("ESC")), "C: playback Back routes to sendKey(ESC)");
        check(backSpy.count() == 0, "C: playback Back does not goBack() directly");
        delete f;
    }

    // ---- D: resume overlay (pre-launch), Back dismisses via goBack ----
    {
        Fixture *f = makeFixture(true, 60000, "resume-overlay");
        if (!f->view) return 1;
        QCoreApplication::processEvents();
        check(QQmlProperty::read(f->view, "overlayVisible").toBool(), "D: resume overlay shown");
        check(f->mpv.loadCalls == 0, "D: overlay is pre-launch (no mpv)");
        QSignalSpy backSpy(f->view, SIGNAL(goBack()));
        sendKey(f->view, Qt::Key_Escape);
        QCoreApplication::processEvents();
        check(backSpy.count() == 1, "D: overlay Back goes back directly");
        check(f->mpv.keys.isEmpty(), "D: overlay Back does not sendKey");
        delete f;
    }

    // ---- E: Backspace after launch routes to sendKey(BS) ----
    {
        Fixture *f = makeFixture(true, 0, "bs-post-launch");
        if (!f->view) return 1;
        spin(90);
        QSignalSpy backSpy(f->view, SIGNAL(goBack()));
        sendKey(f->view, Qt::Key_Backspace);
        QCoreApplication::processEvents();
        check(f->mpv.keys.contains(QStringLiteral("BS")), "E: post-launch Backspace routes to sendKey(BS)");
        check(backSpy.count() == 0, "E: Backspace did not goBack()");
        delete f;
    }

    // ---- F: preparePlayback resets launchDone (relaunch clears prior intent) ----
    {
        Fixture *f = makeFixture(true, 0, "reset");
        if (!f->view) return 1;
        spin(90);
        check(QQmlProperty::read(f->view, "launchDone").toBool(), "F: launchDone true after launch");
        QMetaObject::invokeMethod(f->view, "preparePlayback");
        QCoreApplication::processEvents();
        check(!QQmlProperty::read(f->view, "launchDone").toBool(),
              "F: preparePlayback resets launchDone to false");
        delete f;
    }

    std::printf("%s (%d failure(s))\n", errors == 0 ? "YOUTUBE QUIT-INTENT OK" : "YOUTUBE QUIT-INTENT FAILED", errors);
    return errors == 0 ? 0 : 1;
}

#include "youtube-quit-intent-smoke-test.moc"