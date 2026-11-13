#pragma once
#include <QObject>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

// The slice of PlexBackend the channels backend finds by introspection. Answers
// arrive on the next turn of the event loop, as the server's do; the two the
// backend waits on are left for the test to emit, so it can hold a stream
// request exactly where the backend does.
class FakePlexServer : public QObject {
    Q_OBJECT
public:
    explicit FakePlexServer(QObject *parent = nullptr) : QObject(parent) {}

    QStringList calls;
    QVariantMap detail;                       // what load_item_detail answers with
    QVariantList libraries;                   // what load_libraries answers with
    QHash<QString, QVariantList> libraryItems; // sectionId -> load_library_all items
    QHash<QString, QVariantList> childrenByKey;// ratingKey -> load_children items
    QStringList transcodeSubtitleIds;         // subtitleId of each request_transcode
    QStringList transcodeAudioIds;            // audioId of each

    QString quality = QStringLiteral("2000");   // "auto" makes every stream a direct play
    Q_INVOKABLE QString video_quality() const { return quality; }
    Q_INVOKABLE QString get_access_token() const { return QStringLiteral("tok"); }

    Q_INVOKABLE void load_item_detail(const QString &ratingKey) {
        calls << QStringLiteral("load_item_detail:") + ratingKey;
        later([this] { emit itemLoaded(detail); });
    }
    Q_INVOKABLE void load_libraries() {
        calls << QStringLiteral("load_libraries");
        const QVariantList libs = libraries;
        later([this, libs] { emit librariesLoaded(libs); });
    }
    Q_INVOKABLE void load_library_all(const QString &sectionId) {
        calls << QStringLiteral("load_library_all:") + sectionId;
        const QVariantList items = libraryItems.value(sectionId);
        later([this, items] { emit itemsLoaded(items); });
    }
    Q_INVOKABLE void load_children(const QString &ratingKey) {
        calls << QStringLiteral("load_children:") + ratingKey;
        const QVariantList ch = childrenByKey.value(ratingKey);
        later([this, ch] { emit childrenLoaded(ch); });
    }
    Q_INVOKABLE void build_stream_url(const QString &, const QString &, const QString &) {
        calls << QStringLiteral("build_stream_url");
        later([this] { emit streamUrlReady(QStringLiteral("http://plex/file.mkv"), QStringLiteral("tok")); });
    }
    Q_INVOKABLE void request_transcode(const QString &, const QString &, const QString &,
                                       const QString &audioId, const QString &subtitleId, int) {
        calls << QStringLiteral("request_transcode");
        transcodeSubtitleIds << subtitleId;
        transcodeAudioIds << audioId;
        later([this] { emit streamUrlReady(QStringLiteral("http://plex/session/index.m3u8"), QStringLiteral("tok")); });
    }
    Q_INVOKABLE void stop_transcode(const QString &sessionId) {
        calls << QStringLiteral("stop_transcode:") + sessionId;
    }
    Q_INVOKABLE void set_audio_stream(const QString &streamId, const QString &partId) {
        calls << QStringLiteral("set_audio_stream:") + streamId + QLatin1Char('@') + partId;
    }
    Q_INVOKABLE void set_subtitle_stream(const QString &streamId, const QString &partId) {
        calls << QStringLiteral("set_subtitle_stream:") + streamId + QLatin1Char('@') + partId;
    }

    template <typename F> void later(F f) { QMetaObject::invokeMethod(this, f, Qt::QueuedConnection); }

    int count(const QString &prefix) const {
        int n = 0;
        for (const QString &c : calls) if (c.startsWith(prefix)) ++n;
        return n;
    }

signals:
    void itemLoaded(const QVariant &detail);
    void librariesLoaded(const QVariant &libraries);
    void itemsLoaded(const QVariant &items);
    void childrenLoaded(const QVariant &items);
    void streamUrlReady(const QString &url, const QString &plexToken);
    void subtitleStreamSet(const QString &partId);
    void audioStreamSet(const QString &partId);
    void transcodeStopped(const QString &sessionId);
};
