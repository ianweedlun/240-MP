#pragma once

// In-process YouTube Innertube resolver (overnight torture t_7ffb1fd1).
//
// Replaces the per-play `yt-dlp -g` spawn with one Player request against the
// VISIONOS client, using a page-derived visitorData. IOS-client URLs resolve
// fast but 403 on the open-ended `Range: bytes=0-` that ffmpeg sends, so they
// never play; VISIONOS URLs return 200 bare / 206 open-ended. The gate is mpv
// decode of BOTH streams (build-test/yt-resolver-probe), not a curl probe.
//
// Why VISIONOS: yt-dlp's JS-less default client. Its Player response carries
// direct googlevideo URLs (no signatureCipher, no n-transform) and those URLs
// stream with an open-ended Range. URLs are
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
#include <QByteArray>
#include <QElapsedTimer>

#include "innertube.h"

class QNetworkReply;

// One completed (or failed) in-process Innertube resolve.
struct YtResolveResult {
    QString videoId;
    QString resolution;    // as requested by the caller ("480p"/"720p"/"1080p")
    QString key;           // warm/cache token the resolve was launched for
    QString videoUrl;
    QString audioUrl;      // empty for a muxed single stream
    bool    single = false;
    bool    ok = false;
    QString reason;        // failure class: timeout / no-playable-pair / no-expiry / visitordata-size-cap / live-waiting / exception:...
    qint64  expireEpoch = 0;
    qint64  elapsedMs = 0;
    // #128 live route: ok=true with this set means the video is a LIVE event
    // and videoUrl/audioUrl are empty — the caller must fetch this HLS master
    // playlist, shape it (ythls::filterMaster) and play it as a single URL.
    // Progressive pairs from live responses are not demuxable by mpv.
    QString hlsManifestUrl;
};

Q_DECLARE_METATYPE(YtResolveResult)

class YtInnertubeResolver : public QObject
{
    Q_OBJECT
public:
    explicit YtInnertubeResolver(QObject *parent = nullptr);

    // Resolve one video. Returns false (and emits nothing) when a request is
    // already in flight or the id is empty — the caller treats that as
    // "busy" and takes another route.
    //
    // Terminal-result contract (ROB-07 — enforced, not aspirational):
    //  1. Exactly one terminal `finished` per accepted resolve(): ok=true,
    //     or ok=false with a reason (timeout / exception:... /
    //     no-playable-pair / no-expiry). emitResult() latches on m_inFlight,
    //     so any second terminal event for the same attempt is dropped.
    //  2. An attempt abandoned by the timeout never emits a result and never
    //     issues a downstream request: its late visitorData completion is
    //     dropped before startPlayer(), and any late Player reply/exception
    //     is dropped before emitResult().
    //  3. Every async continuation validates the generation captured when
    //     its request was issued — resolve() stamps the attempt and carries
    //     that stamp into the visitorData wait (m_pendingGeneration) and
    //     into startPlayer(generation). A continuation whose captured
    //     generation no longer equals m_generation belongs to an abandoned
    //     attempt and is dropped silently.
    bool resolve(const QString &videoId, const QString &resolution,
                 const QString &key);
    bool inFlight() const { return m_inFlight; }

    // Bound on a single attempt, milliseconds. The spike's worst observed
    // resolve was 385 ms; 4 s is the outer bound before the backend hands the
    // key to the yt-dlp fallback, which owns the tail risk.
    static constexpr int kTimeoutMs = 4000;

    // Bound on the visitorData homepage scan: inclusive maximum of cumulative
    // post-decode body bytes. 4 MiB = 4.80x headroom over the fresh live
    // measurement of 873,632 bytes (torture-t0/mem02-policy.md, 2026-11-19).
    static constexpr qint64 kMaxHomepageBytes = 4 * 1024 * 1024;

signals:
    void finished(const YtResolveResult &result);

private:
    void ensureClient();
    void fetchVisitorData();
    // `generation` is the originating attempt's, captured in resolve() and
    // carried through every continuation — never re-read m_generation at call
    // time (ROB-07: a late reply must stay bound to the attempt that issued it).
    void startPlayer(qint64 generation);
    // Bounded streaming scan of the homepage reply. Consumes at most
    // kVdChunkBytes per read into a rolling window (carry + chunk, bounded by
    // kVdChunkBytes + kVdCarryBytes), enforcing the inclusive kMaxHomepageBytes
    // cap on cumulative delivered bytes BEFORE any QString conversion. Never
    // materializes the whole page. Terminal paths: token found (early exit),
    // cap exceeded (clean failure into the backend's yt-dlp ladder), or return
    // with m_vdDone unset so finished() applies EOF/error semantics.
    void drainVisitorData(QNetworkReply *reply);
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
    // Generation-token pattern (ROB-07): m_generation is a monotonic attempt
    // counter. resolve() stamps each accepted attempt (++m_generation) and
    // every continuation carries that stamp; emitResult() bumps the counter
    // when the attempt terminates, invalidating every reply still in flight
    // for it. A reply is honored only while its captured stamp still equals
    // m_generation.
    //
    // startPlayer(generation) must inherit the originating attempt's stamp
    // rather than read m_generation at call time: a late visitorData
    // completion runs AFTER the timeout's emitResult() already bumped the
    // counter, so a call-time read would capture the post-timeout value and
    // re-bind the abandoned attempt's Player request to the live generation
    // — resurrecting it as a phantom second result (the ROB-07 defect).
    qint64  m_generation = 0;

    // VISIONOS is bot-walled unless visitorData came from a real YouTube page.
    // The vendored client's generateVisitorData() synthesises one and YouTube
    // rejects it (LOGIN_REQUIRED), so fetch it once per process and reuse it.
    QString m_visitorData;
    bool    m_vdStarted = false;
    bool    m_vdDone = false;
    bool    m_pendingStart = false;
    // Generation of the attempt waiting on the visitorData fetch. The fetch is
    // one-shot per process, so a re-resolve while it is in flight re-arms the
    // wait with its own generation; the completion chains only the live one.
    qint64  m_pendingGeneration = 0;
    // Bounded streaming scan state, live only while the one-shot fetch runs;
    // freed on every terminal path (token found, cap exceeded, EOF/error).
    QByteArray m_vdBuf;         // rolling scan window: carry + newest chunk
    qint64  m_vdBytes = 0;      // cumulative delivered body bytes (cap counter)
};
