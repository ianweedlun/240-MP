// Offscreen QML smoke test for the Plex Player's direct-play -> transcode
// fallback. When mpv exits "failed" on a direct-play URL, Player asks the
// backend for a transcode; the stream that comes back is a single-track HLS
// rendition with burned-in subtitles. The retry must therefore use the same
// track selection as every other transcode launch (audio 0 / sub -1 / no
// external sub files), not the original file's per-stream indices.
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
#include <QVariantList>
#include <QColor>
#include <QUrl>
#include <QTest>
#include <cstdio>

class StubAppCore : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE QVariant get_setting(const QString &, const QString &key) const {
        if (key == QLatin1String("resume_playback")) return QStringLiteral("yes");
        if (key == QLatin1String("autoplay_next_episode")) return false;
        return QVariant();
    }
    Q_INVOKABLE void note_state(const QString &, const QString &) {}
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
    QString moduleId() const { return QStringLiteral("com.240mp.plex"); }
};

class StubPlex : public QObject {
    Q_OBJECT
public:
    struct TranscodeRequest {
        QString audioId;
        QString subtitleId;
        int offsetMs;
    };
    QList<TranscodeRequest> transcodes;
    int buildCount = 0;
    Q_INVOKABLE void request_transcode(const QString &, const QString &, const QString &,
                                       const QString &audio, const QString &sub, int offset) {
        transcodes.append({audio, sub, offset});
    }
    Q_INVOKABLE void build_stream_url(const QString &, const QString &, const QString &) { ++buildCount; }
    Q_INVOKABLE void update_timeline(const QString &, const QString &, const QString &, int, int) {}
    Q_INVOKABLE void set_audio_stream(const QString &, const QString &) {}
    Q_INVOKABLE void set_subtitle_stream(const QString &, const QString &) {}
    Q_INVOKABLE void load_next_episode(const QString &) {}
    Q_INVOKABLE void load_queue_item(const QString &) {}
    Q_INVOKABLE void load_random_episode(const QString &) {}
signals:
    void streamUrlReady(const QString &, const QString &);
    void nextEpisodeReady(const QVariant &);
    void errorOccurred(const QString &);
};

class StubMpv : public QObject {
    Q_OBJECT
public:
    struct Play {
        QString url;
        float startSeconds;
        int audioTrack;
        int subTrack;
        QStringList subFiles;
        QString token;
    };
    QList<Play> plays;
    Q_INVOKABLE void loadAndPlay(const QString &url, float startSeconds,
                                 int audioTrack, int subTrack,
                                 const QStringList &subFiles, const QStringList &,
                                 bool, int, float, const QString &token,
                                 const QString & = {}, bool = false, const QString & = {},
                                 bool = false, const QStringList & = {}, float = 0.0f,
                                 bool = false, const QStringList & = {},
                                 const QString & = {}, const QStringList & = {}) {
        plays.append({url, startSeconds, audioTrack, subTrack, subFiles, token});
    }
    Q_INVOKABLE void stop() {}
    Q_INVOKABLE void sendKey(const QString &) {}
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
    StubPlex plex;
    StubMpv mpv;
    auto *ctx = engine.rootContext();
    ctx->setContextProperty("appCore", QVariant::fromValue(&core));
    ctx->setContextProperty("root", QVariant::fromValue(&root));
    ctx->setContextProperty("moduleRoot", QVariant::fromValue(&module));
    ctx->setContextProperty("plexBackend", QVariant::fromValue(&plex));
    ctx->setContextProperty("mpvController", QVariant::fromValue(&mpv));

    QVariantMap audio0; audio0["id"] = "10"; audio0["language"] = "eng";
    QVariantMap audio1; audio1["id"] = "11"; audio1["language"] = "fre";
    QVariantMap audio2; audio2["id"] = "12"; audio2["language"] = "spa";
    QVariantMap subOff; subOff["id"] = "0"; subOff["language"] = "";
    QVariantMap sub0; sub0["id"] = "20"; sub0["language"] = "eng";
    sub0["subUrl"] = "http://pms/sub/20";
    QVariantMap sub1; sub1["id"] = "21"; sub1["language"] = "eng";
    sub1["subUrl"] = "http://pms/sub/21";

    QVariantMap params;
    params["streamUrl"] = "http://pms/direct";
    params["ratingKey"] = "123";
    params["partKey"] = "/library/parts/55/file.mkv";
    params["partId"] = "55";
    params["sessionId"] = "SESSION1";
    params["viewOffset"] = 30000;
    params["title"] = "TEST EPISODE";
    params["audioStreams"] = QVariantList{audio0, audio1, audio2};
    params["subtitleStreams"] = QVariantList{subOff, sub0, sub1};
    params["selectedAudioId"] = "12";
    params["selectedSubtitleId"] = "21";
    params["isTranscoding"] = false;
    ctx->setContextProperty("smokeParams", params);

    QQmlComponent comp(&engine, QUrl::fromLocalFile(repoRoot + "/modules/plex/views/Player.qml"));
    if (comp.status() == QQmlComponent::Error) {
        for (const auto &e : comp.errors())
            std::printf("  ERR  %s\n", qPrintable(e.description()));
        return 1;
    }
    // Player.qml declares its own navParams, so a context property of that
    // name is shadowed. Inject it as an initial property before onCompleted.
    QObject *view = comp.beginCreate(engine.rootContext());
    if (!view) return 1;
    comp.setInitialProperties(view, {{QStringLiteral("navParams"), params}});
    comp.completeCreate();
    if (!view) return 1;
    // The deferred 50 ms startTimer can be delayed by font discovery and other
    // tests on a loaded CI runner. Wait for the observable launch, not a fixed
    // wall-clock slice; a missing launch still fails after the bounded deadline.
    int directPlayWaitMs = 0;
    while (mpv.plays.isEmpty() && directPlayWaitMs < 2000) {
        QTest::qWait(20);
        directPlayWaitMs += 20;
    }
    check(mpv.plays.size() == 1, "direct play launches once on completion");
    if (mpv.plays.isEmpty()) return 1;

    // mpv fails to decode the direct URL -> Player asks for a transcode at 0.
    bool ok = QMetaObject::invokeMethod(&mpv, "playbackEnded",
                                        Q_ARG(int, 0), Q_ARG(int, 0),
                                        Q_ARG(QString, QStringLiteral("failed")));
    QCoreApplication::processEvents();
    check(ok && plex.transcodes.size() == 1, "failed direct play requests one transcode");
    if (plex.transcodes.isEmpty()) return 1;
    check(plex.transcodes.first().offsetMs == 0, "retry transcode requests the full timeline");
    check(plex.transcodes.first().audioId == QStringLiteral("12") &&
          plex.transcodes.first().subtitleId == QStringLiteral("21"),
          "retry transcode keeps the user's audio and subtitle choice");

    // The backend resolves the HLS URL; Player must relaunch with transcode tracks.
    ok = QMetaObject::invokeMethod(&plex, "streamUrlReady",
                                   Q_ARG(QString, QStringLiteral("http://pms/transcode/master.m3u8")),
                                   Q_ARG(QString, QStringLiteral("TOK")));
    QCoreApplication::processEvents();
    check(ok && mpv.plays.size() == 2, "transcode URL relaunches mpv");
    check(QQmlProperty::read(view, "isTranscoding").toBool(), "player switches to transcode mode");
    if (mpv.plays.size() < 2) return failures ? 1 : 0;

    const StubMpv::Play &retry = mpv.plays.at(1);
    std::printf("  retry: start=%.3f audio=%d sub=%d subUrls=%d token=%s\n",
                retry.startSeconds, retry.audioTrack, retry.subTrack,
                int(retry.subFiles.size()), qPrintable(retry.token));
    check(qAbs(retry.startSeconds - 30.0f) < 0.01f, "retry seeks to the 30s resume point");
    check(retry.url.startsWith(QStringLiteral("http://pms/transcode")), "retry plays the transcode URL");
    check(retry.token == QStringLiteral("TOK"), "retry passes the plex token");
    // THE BUG: this uses the original file's stream indices (audio 3) and its
    // external subtitle files, which a burned-in HLS transposition cannot have.
    check(retry.audioTrack == 0, "transcode retry uses mpv audio track 0");
    check(retry.subTrack == -1, "transcode retry disables mpv subtitle tracks");
    check(retry.subFiles.isEmpty(), "transcode retry carries no external subtitle files");

    delete view;
    std::printf("%s (%d failure(s))\n", failures == 0 ? "PLEX PLAYER OK" : "PLEX PLAYER FAILED", failures);
    return failures == 0 ? 0 : 1;
}

#include "plex-player-smoke-test.moc"
