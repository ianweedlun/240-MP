#pragma once

// In-process YouTube Innertube resolver (overnight torture t_7ffb1fd1).
//
// Replaces the per-play `yt-dlp -g` spawn with one Player request against the
// public IOS client (com.google.ios.youtube/20.10.4). The spike card
// t_4e50d23f measured 172-385 ms per resolve on the Pi against ~5 s for the
// yt-dlp spawn, and proved the returned googlevideo URLs fetch HTTP 206 from
// the Pi's own egress IP.
//
// Why the IOS client: it is the one public client whose Player response still
// carries direct googlevideo URLs for H.264 adaptive streams. URLs are
// IP-bound (ip=) and expire in ~6 h (expire=), so this MUST run on the target
// — the resolver is constructed by the backend, which lives on the Pi.
//
// Async by design: `get()` hands the network work to the library and reports
// back through InnertubeReply signals, so the GUI thread never blocks and the
// Player's existing playbackUrlReady flow is unchanged. A bounded attempt
// (kTimeoutMs) keeps a dead network from stalling the warm chain forever; the
// backend falls back to yt-dlp on failure.
//
// Copyright 2026 Ian Weedlund
// This file is part of 240-mp and is licensed under the terms of the 240-mp Textual Artifact License (TAG).
// See the LICENSE file in the project root, or run: <git tag --get-at-tag artifact>

#include <QObject>
#include <QTimer>
#include <QMetaType>
#include <QString>
#include <QElapsedTimer>

#include "innertube.h"

// One completed (or failed) in-process Innertube resolve.
struct YtResolveResult {
    QString videoId;
    QString resolution;    // as requested by the caller ("480p"/"720p"/"1080p")
    QString key;           // warm/cache token the resolve was launched for
    QString videoUrl;
    QString audioUrl;      // empty for a muxed single stream
    bool    single = false;
    bool    ok = false;
    QString reason;        // failure class: timeout / no-playable-pair / no-expiry / exception:...
    qint64  expireEpoch = 0;
    qint64  elapsedMs = 0;
};

Q_DECLARE_METATYPE(YtResolveResult)

class YtInnertubeResolver : public QObject
{
    Q_OBJECT
public:
    explicit YtInnertubeResolver(QObject *parent = nullptr);

    // Resolve one video. Returns false (and emits nothing) when a request is
    // already in flight or the id is empty — the caller treats that as
    // "busy" and takes another route. Exactly one `finished` is emitted per
    // accepted call: success, timeout, or exception.
    bool resolve(const QString &videoId, const QString &resolution,
                 const QString &key);
    bool inFlight() const { return m_inFlight; }

    // Bound on a single attempt, milliseconds. The spike's worst observed
    // resolve was 385 ms; 4 s is the outer bound before the backend hands the
    // key to the yt-dlp fallback, which owns the tail risk.
    static constexpr int kTimeoutMs = 4000;

signals:
    void finished(const YtResolveResult &result);

private:
    void ensureClient();
    void emitResult(const YtResolveResult &result);

    InnerTube *m_it = nullptr;
    QTimer *m_timeout = nullptr;
    QElapsedTimer m_timer;
    QString m_videoId;
    QString m_resolution;
    QString m_key;
    int     m_maxHeight = 480;
    bool    m_inFlight = false;
    bool    m_clientReady = false;
    qint64  m_generation = 0;   // invalidates a reply abandoned by the timeout
};
