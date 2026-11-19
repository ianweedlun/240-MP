// Offscreen QML smoke for the YouTube Player's held-key wiring: taps and
// synthesized auto-repeats must reach MpvController.sendKey WITH the repeat
// flag, and key-up must reach sendKeyUp — the direct-IPC path gets the same
// contract from InputManager. SeekHoldPolicy (unit-tested separately) turns
// those into a speed ramp / coalesced rewind instead of a seek machine-gun.
// Known-bad guard: before the fix, Player.qml called sendKey("RIGHT") with no
// flag, so every repeat arrived as a fresh tap — the repeat assertions fail
// on that code.
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickView>
#include <QQuickItem>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QColor>
#include <QUrl>
#include <QTest>
#include <cstdio>

class StubAppCore : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE QVariant get_setting(const QString &, const QString &key) const {
        if (key == QLatin1String("resume_playback")) return QStringLiteral("Never");
        if (key == QLatin1String("playback_resolution")) return QStringLiteral("480p");
        if (key.startsWith(QLatin1String("sponsor_"))) return QStringLiteral("off");
        return QVariant();
    }
    Q_INVOKABLE void note_state(const QString &, const QString &) {}
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
    Q_PROPERTY(QString moduleId READ moduleId CONSTANT)
public:
    QString moduleId() const { return QStringLiteral("com.240mp.youtube"); }
};

class StubYoutube : public QObject {
    Q_OBJECT
public:
    // Warm cache: preparePlayback takes the direct path and play() launches
    // immediately, so the key handlers run in their playback branch.
    Q_INVOKABLE QString cachedPlaybackUrl(const QString &, const QString &) const {
        return QStringLiteral("https://stream.invalid/v");
    }
    Q_INVOKABLE QString cachedPlaybackAudioUrl(const QString &, const QString &) const {
        return QStringLiteral("https://stream.invalid/a");
    }
    Q_INVOKABLE bool cachedPlaybackSingle(const QString &, const QString &) const { return false; }
    Q_INVOKABLE QVariantMap getSavedPosition(const QString &) const {
        return QVariantMap{{QStringLiteral("pos"), 0}};
    }
    Q_INVOKABLE QString ytdlFormatForResolution(const QString &) const { return QString(); }
    Q_INVOKABLE QVariantMap buildPlaybackEdl(const QString &, const QString &, const QVariantMap &) const { return {}; }
    Q_INVOKABLE void requestSponsorSegments(const QString &) {}
    Q_INVOKABLE void resolvePlayback(const QString &, const QString &) {}
    Q_INVOKABLE void invalidateCachedUrl(const QString &, const QString &) {}
    Q_INVOKABLE void savePosition(const QString &, int, const QString &, const QString &) {}
    Q_INVOKABLE bool hasSponsorSegments(const QString &) const { return false; }
    Q_INVOKABLE bool sponsorFetchInFlight(const QString &) const { return false; }
signals:
    void playbackUrlReady(const QString &);
    void sponsorReady(const QString &);
    void playbackResolveFailed(const QString &videoId, const QString &reason);
};

class StubMpv : public QObject {
    Q_OBJECT
public:
    QList<QPair<QString, bool>> keys;   // (key, autoRepeat) as the view delivered them
    QStringList ups;
    int plays = 0;
    // Mirrors MpvController::loadAndPlay exactly (src/player/MpvController.h) —
    // a divergent stub signature silently drops the launch (TypeError) and the
    // key handlers never run in their playback branch.
    Q_INVOKABLE void loadAndPlay(const QString &url, float startSeconds,
                                 int audioTrack, int subTrack,
                                 const QStringList &subFiles = {},
                                 const QStringList &subLangs = {},
                                 bool loop = false,
                                 int playlistStart = -1,
                                 float transcodeOffsetSec = 0.0f,
                                 const QString &plexToken = {},
                                 bool muteAudio = false,
                                 const QString &oscMode = {},
                                 bool shuffle = false,
                                 const QStringList &subTitles = {},
                                 float imageDurationSec = 0.0f,
                                 bool imageContent = false,
                                 const QStringList &extraArgs = {},
                                 const QString &jellyfinToken = {},
                                 const QStringList &extraUrls = {}) { ++plays; }
    Q_INVOKABLE void stop() {}
    Q_INVOKABLE void setOscRanges(const QString &) {}
    Q_INVOKABLE void sendKey(const QString &key, bool autoRepeat = false) {
        keys.append({key, autoRepeat});
    }
    Q_INVOKABLE void sendKeyUp(const QString &key) { ups.append(key); }
signals:
    void positionChanged(int);
    void durationChanged(int);
    void playbackEnded(int, int, const QString &);
    void playbackStalled(int);
    void skipRequested();
    void volumeChanged(int);
    void subtitleCycleRequested();
    void audioCycleRequested();
};

static int failures = 0;
static void check(bool ok, const char *what) {
    if (!ok) { ++failures; std::printf("  FAIL  %s\n", what); }
    else     { std::printf("  ok    %s\n", what); }
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    const QString repoRoot(QString::fromUtf8(MP240_REPO_ROOT));

    QQmlEngine engine;
    engine.addImportPath(repoRoot + "/views");
    StubAppCore core;
    StubRoot root;
    StubModule module;
    StubYoutube yt;
    StubMpv mpv;
    auto *ctx = engine.rootContext();
    ctx->setContextProperty("appCore", QVariant::fromValue(&core));
    ctx->setContextProperty("root", QVariant::fromValue(&root));
    ctx->setContextProperty("moduleRoot", QVariant::fromValue(&module));
    ctx->setContextProperty("youtubeBackend", QVariant::fromValue(&yt));
    ctx->setContextProperty("mpvController", QVariant::fromValue(&mpv));

    QVariantMap item;
    item["url"] = QStringLiteral("https://stream.invalid/v");
    item["videoId"] = QStringLiteral("smokevid");
    item["title"] = QStringLiteral("SMOKE");
    QVariantMap params;
    params["item"] = item;
    params["playlistItems"] = QVariantList{};
    params["playlistIndex"] = -1;

    QQmlComponent comp(&engine, QUrl::fromLocalFile(repoRoot + "/modules/youtube/views/Player.qml"));
    if (comp.status() == QQmlComponent::Error) {
        for (const auto &e : comp.errors())
            std::printf("  ERR  %s\n", qPrintable(e.description()));
        return 1;
    }
    QObject *obj = comp.beginCreate(engine.rootContext());
    if (!obj) return 1;
    comp.setInitialProperties(obj, {{QStringLiteral("navParams"), params}});
    comp.completeCreate();

    // Key events need a window with an activeFocusItem; reparent the created
    // FocusScope into a QQuickView (offscreen) and give it focus.
    QQuickView window;
    window.resize(640, 480);
    auto *qi = qobject_cast<QQuickItem *>(obj);
    if (!qi) { std::printf("  ERR  root is not a QQuickItem\n"); return 1; }
    qi->setParentItem(window.contentItem());
    qi->setWidth(640);
    qi->setHeight(480);
    qi->setFocus(true);
    window.show();
    QTest::qWait(150);

    check(mpv.plays == 1, "warm direct path launches once on completion");
    check(QQmlProperty::read(obj, "errorMessage").toString().isEmpty(), "no error overlay");
    check(!QQmlProperty::read(obj, "overlayVisible").toBool(), "no resume overlay");

    auto key = [&](int k, bool repeat, QEvent::Type type) {
        QKeyEvent ev(type, k, Qt::NoModifier, 0, 0, 0, QString(), repeat);
        QCoreApplication::sendEvent(&window, &ev);
        QTest::qWait(30);
    };
    auto lastKey = [&](const QString &k, bool repeat) {
        return !mpv.keys.isEmpty() && mpv.keys.last().first == k && mpv.keys.last().second == repeat;
    };

    // Right: tap, then a held burst (the measured 10 presses/s shape), release.
    key(Qt::Key_Right, false, QEvent::KeyPress);
    check(lastKey("RIGHT", false), "right tap reaches sendKey as a tap");
    for (int i = 0; i < 3; ++i)
        key(Qt::Key_Right, true, QEvent::KeyPress);
    check(lastKey("RIGHT", true), "right auto-repeat keeps the repeat flag (known-bad: old code sent taps)");
    key(Qt::Key_Right, false, QEvent::KeyRelease);
    check(mpv.ups.contains("RIGHT"), "right key-up reaches sendKeyUp");

    // Left: same contract.
    key(Qt::Key_Left, false, QEvent::KeyPress);
    check(lastKey("LEFT", false), "left tap reaches sendKey as a tap");
    key(Qt::Key_Left, true, QEvent::KeyPress);
    check(lastKey("LEFT", true), "left auto-repeat keeps the repeat flag");
    key(Qt::Key_Left, false, QEvent::KeyRelease);
    check(mpv.ups.contains("LEFT"), "left key-up reaches sendKeyUp");

    // Up: repeats flagged too (policy drops them downstream).
    key(Qt::Key_Up, true, QEvent::KeyPress);
    check(lastKey("UP", true), "up auto-repeat keeps the repeat flag");
    key(Qt::Key_Up, false, QEvent::KeyRelease);

    // Enter: unchanged passthrough.
    key(Qt::Key_Return, false, QEvent::KeyPress);
    check(lastKey("ENTER", false), "enter still passes through");
    key(Qt::Key_Return, false, QEvent::KeyRelease);

    // Leaving the view mid-hold must flush: destruction sends both key-ups.
    const int upsBefore = mpv.ups.size();
    delete qi;
    QTest::qWait(30);
    check(mpv.ups.size() >= upsBefore + 2, "destruction flushes RIGHT and LEFT key-ups");

    std::printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

#include "youtube-seek-smoke-test.moc"
