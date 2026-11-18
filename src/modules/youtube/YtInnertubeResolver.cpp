#include "YtInnertubeResolver.h"

#include "innertube/endpoints/innertubeendpoints.h"
#include "innertube/itc-objects/innertubeclient.h"
#include "innertube/objects/player/streamingformat.h"
#include "innertube/responses/video/playerresponse.h"

#include "../../util/YtStreamSelect.h"
#include "../../util/YtUrlFresh.h"

#include <QDateTime>

namespace {
using PlayerEndpoint = InnertubeEndpoints::Player;
using PlayerReply = InnertubeReply<PlayerEndpoint>;
}

YtInnertubeResolver::YtInnertubeResolver(QObject *parent)
    : QObject(parent)
{
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this]() {
        YtResolveResult r;
        r.videoId = m_videoId;
        r.resolution = m_resolution;
        r.key = m_key;
        r.ok = false;
        r.reason = QStringLiteral("timeout");
        r.elapsedMs = m_timer.elapsed();
        emitResult(r);
    });
}

void YtInnertubeResolver::ensureClient()
{
    if (m_clientReady)
        return;
    m_clientReady = true;
    if (!m_it)
        m_it = InnerTube::instance();
    // Locked client pair from the spike (t_4e50d23f): the IOS client, version
    // 20.10.4. Its User-Agent is what the googlevideo URLs are minted for.
    m_it->createClient(InnertubeClient(InnertubeClient::ClientType::IOS,
                                       QStringLiteral("20.10.4")));
}

bool YtInnertubeResolver::resolve(const QString &videoId, const QString &resolution,
                                  const QString &key)
{
    if (m_inFlight || videoId.isEmpty())
        return false;
    ensureClient();
    m_inFlight = true;
    m_videoId = videoId;
    m_resolution = resolution;
    m_key = key;
    m_maxHeight = ytstream::maxHeightForResolution(resolution);
    const qint64 generation = ++m_generation;
    m_timer.start();
    m_timeout->start(kTimeoutMs);

    PlayerReply *reply = nullptr;
    try {
        reply = m_it->get<PlayerEndpoint>(videoId);
    } catch (const InnertubeException &ex) {
        YtResolveResult r;
        r.videoId = videoId;
        r.resolution = resolution;
        r.key = key;
        r.ok = false;
        r.reason = QStringLiteral("exception:") + ex.message();
        r.elapsedMs = m_timer.elapsed();
        emitResult(r);
        return false;
    }

    connect(reply, &PlayerReply::finished, this,
            [this, generation](const PlayerEndpoint &player) {
        if (generation != m_generation)
            return; // a timeout already reported this attempt
        m_timeout->stop();

        const InnertubeObjects::StreamingData &sd = player.response.streamingData;
        QVector<ytstream::Format> formats;
        formats.reserve(sd.formats.size() + sd.adaptiveFormats.size());
        for (const InnertubeObjects::StreamingFormat &f : sd.formats)
            formats.append({f.itag, f.height, f.mimeType, f.url});
        for (const InnertubeObjects::StreamingFormat &f : sd.adaptiveFormats)
            formats.append({f.itag, f.height, f.mimeType, f.url});

        const ytstream::Pair pair = ytstream::selectPair(formats, m_maxHeight);

        YtResolveResult r;
        r.videoId = m_videoId;
        r.resolution = m_resolution;
        r.key = m_key;
        r.elapsedMs = m_timer.elapsed();
        if (!pair.ok) {
            r.reason = QStringLiteral("no-playable-pair");
            emitResult(r);
            return;
        }

        qint64 expire = yturl::expireEpochFromUrl(pair.videoUrl);
        if (!pair.audioUrl.isEmpty()) {
            const qint64 audioExpire = yturl::expireEpochFromUrl(pair.audioUrl);
            if (audioExpire > 0 && (expire <= 0 || audioExpire < expire))
                expire = audioExpire;
        }
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        expire = ytstream::expiryEpoch(expire, now, sd.expiresInSeconds.toInt());
        if (expire <= now) {
            r.reason = QStringLiteral("no-expiry");
            emitResult(r);
            return;
        }

        r.videoUrl = pair.videoUrl;
        r.audioUrl = pair.audioUrl;
        r.single = pair.single;
        r.ok = true;
        r.expireEpoch = expire;
        emitResult(r);
    });

    connect(reply, &PlayerReply::exception, this,
            [this, generation](const InnertubeException &ex) {
        if (generation != m_generation)
            return;
        m_timeout->stop();
        YtResolveResult r;
        r.videoId = m_videoId;
        r.resolution = m_resolution;
        r.key = m_key;
        r.ok = false;
        r.reason = QStringLiteral("exception:") + ex.message();
        r.elapsedMs = m_timer.elapsed();
        emitResult(r);
    });

    return true;
}

void YtInnertubeResolver::emitResult(const YtResolveResult &result)
{
    ++m_generation;    // invalidate any reply still in flight
    m_inFlight = false;
    m_timeout->stop();
    emit finished(result);
}
