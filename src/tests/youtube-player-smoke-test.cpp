// Offscreen QML smoke test for the YouTube Player's launch state machine.
//
// Guards the SponsorBlock decoupling: the launch depends ONLY on the stream URL
// pair. A segment window that is already cached rides the launch as an EDL; a
// window that is still being fetched (or a sponsor.ajay.app that never answers)
// must add ZERO wait. Before the decoupling the cold path held the launch for
// up to 6 s waiting for sponsorReady, so the timing assertions below are the
// regression gate: they fail if any non-essential service re-gates first frame.
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlProperty>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QColor>
#include <QElapsedTimer>
#include <QUrl>
#include <QTest>
#include <cstdio>

class StubAppCore : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE QVariant get_setting(const QString &, const QString &key) const {
        if (key == QLatin1String("playback_resolution")) return QStringLiteral("480p");
        if (key == QLatin1String("resume_playback"))     return QStringLiteral("Ask");
        if (key == QLatin1String("autoplay_playlist_items")) return false;
        if (key.startsWith(QStringLiteral("sponsor_")))  return QStringLiteral("off");
        return QVariant();
    }
    Q_INVOKABLE void note_state(const QString &, const QString &) {}
    Q_INVOKABLE bool telemetryEnabled() const { return false; }
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

// Stand-in for YouTubeBackend. `segmentsCached` models a window already on
// file (requestSponsorSegments answers inline); when false the request is a
// black hole — exactly the dead-sponsor.ajay.app case the decoupling must
// survive without delaying first frame.
class StubYoutube : public QObject {
    Q_OBJECT
public:
    bool warmPair = false;
    bool segmentsCached = false;
    int sponsorRequests = 0;
    QStringList requestedIds;

    Q_INVOKABLE void requestSponsorSegments(const QString &id) {
        ++sponsorRequests;
        requestedIds << id;
        if (segmentsCached)
            emit sponsorReady(id);
    }
    Q_INVOKABLE QString cachedPlaybackUrl(const QString &, const QString &) const {
        return warmPair ? QStringLiteral("http://yt/video.mp4") : QString();
    }
    Q_INVOKABLE QString cachedPlaybackAudioUrl(const QString &, const QString &) const {
        return warmPair ? QStringLiteral("http://yt/audio.mp4") : QString();
    }
    Q_INVOKABLE bool cachedPlaybackSingle(const QString &, const QString &) const { return false; }
    Q_INVOKABLE void resolvePlayback(const QString &, const QString &) {}
    Q_INVOKABLE void invalidateCachedUrl(const QString &, const QString &) {}
    Q_INVOKABLE void savePosition(const QString &, int, const QString &, const QString &) {}
    Q_INVOKABLE QVariantMap getSavedPosition(const QString &) const { return {}; }
    Q_INVOKABLE QString ytdlFormatForResolution(const QString &) const {
        return QStringLiteral("bv+ba");
    }
    Q_INVOKABLE QVariantMap buildPlaybackEdl(const QString &, const QString &,
                                             const QVariantMap &) const {
        QVariantMap m;
        if (!segmentsCached)
            return m;
        m.insert(QStringLiteral("videoEdl"),
                 QStringLiteral("edl://!empty,10-20;http://yt/video.mp4"));
        m.insert(QStringLiteral("audioEdl"),
                 QStringLiteral("edl://!empty,10-20;http://yt/audio.mp4"));
        m.insert(QStringLiteral("ranges"), QStringLiteral("10-20:Sponsor:skip;"));
        return m;
    }
    Q_INVOKABLE bool hasSponsorSegments(const QString &) const { return segmentsCached; }
    Q_INVOKABLE bool sponsorFetchInFlight(const QString &) const { return false; }
signals:
    void playbackUrlReady(const QString &videoId);
    void sponsorReady(const QString &videoId);
};

class StubMpv : public QObject {
    Q_OBJECT
public:
    struct Play {
        QString url;
        float startSeconds;
        QStringList extraArgs;
    };
    QList<Play> plays;
    Q_INVOKABLE void loadAndPlay(const QString &url, float startSeconds,
                                 int audioTrack, int subTrack,
                                 const QStringList &subFiles = {},
                                 const QStringList &subLangs = {},
                                 bool loop = false, int playlistStart = -1,
                                 float transcodeOffsetSec = 0.0f,
                                 const QString &plexToken = {},
                                 bool muteAudio = false, const QString &oscMode = {},
                                 bool shuffle = false, const QStringList &subTitles = {},
                                 float imageDurationSec = 0.0f, bool imageContent = false,
                                 const QStringList &extraArgs = {},
                                 const QString &jellyfinToken = {},
                                 const QStringList &extraUrls = {}) {
        (void)audioTrack; (void)subTrack; (void)subFiles; (void)subLangs;
        (void)loop; (void)playlistStart; (void)transcodeOffsetSec; (void)plexToken;
        (void)muteAudio; (void)oscMode; (void)shuffle; (void)subTitles;
        (void)imageDurationSec; (void)imageContent; (void)jellyfinToken;
        (void)extraUrls;
        plays.append({url, startSeconds, extraArgs});
    }
    Q_INVOKABLE void setOscRanges(const QString &) {}
    Q_INVOKABLE void sendKey(const QString &) {}
signals:
    void positionChanged(int);
    void durationChanged(int);
    void playbackEnded(int, int, const QString &);
};

static int failures = 0;
static void check(bool ok, const char *what) {
    if (!ok) { ++failures; std::printf("  FAIL  %s\n", what); }
    else     { std::printf("  ok    %s\n", what); }
}

static QObject *loadPlayer(QQmlEngine &engine, StubAppCore *core, StubRoot *root,
                           StubModule *module, StubYoutube *yt, StubMpv *mpv,
                           const QString &videoId, const QString &repoRoot,
                           bool *loadError) {
    *loadError = false;
    auto *ctx = engine.rootContext();
    ctx->setContextProperty("appCore", QVariant::fromValue(core));
    ctx->setContextProperty("root", QVariant::fromValue(root));
    ctx->setContextProperty("moduleRoot", QVariant::fromValue(module));
    ctx->setContextProperty("youtubeBackend", QVariant::fromValue(yt));
    ctx->setContextProperty("mpvController", QVariant::fromValue(mpv));

    QQmlComponent comp(&engine,
                       QUrl::fromLocalFile(repoRoot + "/modules/youtube/views/Player.qml"));
    if (comp.status() == QQmlComponent::Error) {
        for (const auto &e : comp.errors())
            std::printf("  ERR  %s\n", qPrintable(e.description()));
        *loadError = true;
        return nullptr;
    }
    QVariantMap item;
    item["url"] = QStringLiteral("http://yt/watch");
    item["videoId"] = videoId;
    item["title"] = QStringLiteral("SMOKE VIDEO");
    item["channelName"] = QStringLiteral("SMOKE CHANNEL");
    QVariantMap params;
    params["item"] = item;
    QObject *view = comp.beginCreate(engine.rootContext());
    if (!view) { *loadError = true; return nullptr; }
    comp.setInitialProperties(view, {{QStringLiteral("navParams"), params}});
    comp.completeCreate();
    return view;
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

    // --- Case 1: warm pair + cached segments -> EDL launch, no hold ---------
    {
        StubYoutube yt;
        StubMpv mpv;
        yt.warmPair = true;
        yt.segmentsCached = true;
        bool loadError = false;
        QObject *view = loadPlayer(engine, &core, &root, &module, &yt, &mpv,
                                   QStringLiteral("VID1"), repoRoot, &loadError);
        check(!loadError && view != nullptr, "case1: Player.qml loads offscreen");
        if (!view) return 1;
        QElapsedTimer t;
        t.start();
        QTest::qWait(250);
        check(mpv.plays.size() == 1, "case1: warm launch happens once");
        if (!mpv.plays.isEmpty()) {
            check(mpv.plays[0].url.startsWith(QStringLiteral("edl://")),
                  "case1: cached segments ride the launch as an EDL");
            check(mpv.plays[0].startSeconds == 0.0f, "case1: starts from 0");
        }
        check(t.elapsed() < 1000, "case1: warm launch well under any hold");
        check(yt.sponsorRequests >= 1 && yt.requestedIds.contains(QStringLiteral("VID1")),
              "case1: segment window requested for the played video");
        delete view;
    }

    // --- Case 2: cold pair, resolve lands, segments cached -------------------
    {
        StubYoutube yt;
        StubMpv mpv;
        yt.warmPair = false;
        yt.segmentsCached = true;
        bool loadError = false;
        QObject *view = loadPlayer(engine, &core, &root, &module, &yt, &mpv,
                                   QStringLiteral("VID2"), repoRoot, &loadError);
        check(!loadError && view != nullptr, "case2: Player.qml loads offscreen");
        if (!view) return 1;
        QTest::qWait(150);
        check(mpv.plays.isEmpty(), "case2: cold open waits for the resolve");
        yt.warmPair = true;
        QElapsedTimer t;
        t.start();
        emit yt.playbackUrlReady(QStringLiteral("VID2"));
        QTest::qWait(250);
        check(mpv.plays.size() == 1, "case2: resolve landing launches once");
        if (!mpv.plays.isEmpty())
            check(mpv.plays[0].url.startsWith(QStringLiteral("edl://")),
                  "case2: resolve-time cached segments ride the launch");
        check(t.elapsed() < 1000,
              "case2: no sponsor wait on the cold path (was a 6s hold)");
        delete view;
    }

    // --- Case 3: cold pair, resolve lands, sponsor never answers -------------
    {
        StubYoutube yt;
        StubMpv mpv;
        yt.warmPair = false;
        yt.segmentsCached = false;   // requestSponsorSegments never emits
        bool loadError = false;
        QObject *view = loadPlayer(engine, &core, &root, &module, &yt, &mpv,
                                   QStringLiteral("VID3"), repoRoot, &loadError);
        check(!loadError && view != nullptr, "case3: Player.qml loads offscreen");
        if (!view) return 1;
        QTest::qWait(150);
        check(mpv.plays.isEmpty(), "case3: cold open waits for the resolve");
        yt.warmPair = true;
        QElapsedTimer t;
        t.start();
        emit yt.playbackUrlReady(QStringLiteral("VID3"));
        QTest::qWait(250);
        check(mpv.plays.size() == 1, "case3: dead sponsor domain still launches");
        if (!mpv.plays.isEmpty()) {
            check(mpv.plays[0].url == QStringLiteral("http://yt/video.mp4"),
                  "case3: empty cache falls back to the plain URL");
        }
        check(t.elapsed() < 1000,
              "case3: unreachable sponsor adds zero wait to first frame");
        delete view;
    }

    // --- Case 4: warm pair, sponsor never answers ---------------------------
    {
        StubYoutube yt;
        StubMpv mpv;
        yt.warmPair = true;
        yt.segmentsCached = false;
        bool loadError = false;
        QObject *view = loadPlayer(engine, &core, &root, &module, &yt, &mpv,
                                   QStringLiteral("VID4"), repoRoot, &loadError);
        check(!loadError && view != nullptr, "case4: Player.qml loads offscreen");
        if (!view) return 1;
        QElapsedTimer t;
        t.start();
        QTest::qWait(250);
        check(mpv.plays.size() == 1, "case4: warm launch happens with sponsor dead");
        if (!mpv.plays.isEmpty())
            check(mpv.plays[0].url == QStringLiteral("http://yt/video.mp4"),
                  "case4: plain URL when no window is cached");
        check(t.elapsed() < 1000, "case4: warm launch not gated on sponsor");
        delete view;
    }

    std::printf(failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}

#include "youtube-player-smoke-test.moc"
