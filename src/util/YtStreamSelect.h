#pragma once

// Pure stream-selection logic for the in-process YouTube Innertube resolver
// (overnight torture t_7ffb1fd1). The Innertube Player response hands back a
// flat list of progressive + adaptive formats; picking the playable pair is a
// pure function of that list and the resolution cap, so it is unit-tested
// without a network, a Qt event loop, or the vendored library.
//
// Rules (locked by the spike card t_4e50d23f):
//   * video — the tallest H.264 (avc1) track at or below the cap. H.264 first
//     because the Pi 4 decodes it in hardware; a non-avc1 track is only used
//     when no avc1 track fits the cap.
//   * audio — itag 140 (m4a) is preferred: it is the indexed AAC track, so
//     seeking does not re-read the file (see the fast-forward fix). Any other
//     audio track is a fallback.
//   * muxed — when the list carries no audio-only track (live broadcasts,
//     combined progressive formats), a track whose mimeType names BOTH a video
//     and an audio codec plays as one stream (single=1), matching the yt-dlp
//     "one URL" cache shape.
//   * expiry — googlevideo URLs carry their own expire= epoch; the Player
//     response's expiresInSeconds is the fallback when a URL omits it.
//
// Copyright 2026 Ian Weedlund
// This file is part of 240-mp and is licensed under the terms of the 240-mp Textual Artifact License (TAG).
// See the LICENSE file in the project root, or run: <git tag --get-at-tag artifact>

#include <QString>
#include <QVector>
#include <QtGlobal>

namespace ytstream {

struct Format {
    int     itag = 0;
    int     height = 0;      // 0 = unknown (audio-only tracks report none)
    QString mimeType;
    QString url;
};

struct Pair {
    QString videoUrl;
    QString audioUrl;        // empty for a muxed single stream
    bool    single = false;
    bool    ok = false;
};

// Maps the playback_resolution setting to the tallest stream we will accept.
// Single source for BOTH the yt-dlp format string and the Innertube picker.
inline int maxHeightForResolution(const QString &resolution)
{
    if (resolution == QLatin1String("720p"))
        return 720;
    if (resolution == QLatin1String("1080p"))
        return 1080;
    return 480; // "480p" and every unknown value
}

inline Pair selectPair(const QVector<Format> &formats, int maxHeight)
{
    Pair out;
    if (formats.isEmpty() || maxHeight <= 0)
        return out;

    const Format *video = nullptr;
    bool videoAvc1 = false;
    for (const Format &f : formats) {
        if (f.url.isEmpty() || !f.mimeType.contains(QLatin1String("video")))
            continue;
        if (f.height <= 0 || f.height > maxHeight)
            continue;
        const bool avc1 = f.mimeType.contains(QLatin1String("avc1"));
        if (!video) {
            video = &f;
            videoAvc1 = avc1;
        } else if (avc1 && !videoAvc1) {
            video = &f;            // H.264 always wins on the Pi
            videoAvc1 = true;
        } else if (avc1 == videoAvc1 && f.height > video->height) {
            video = &f;            // same codec family: take the taller one
        }
    }
    if (!video)
        return out;

    const Format *audio = nullptr;
    bool audioMp4a = false;
    for (const Format &f : formats) {
        if (f.url.isEmpty() || !f.mimeType.contains(QLatin1String("audio")))
            continue;
        if (f.itag == 140) {       // indexed m4a — the locked audio pick
            audio = &f;
            break;
        }
        const bool mp4a = f.mimeType.contains(QLatin1String("mp4a"));
        if (!audio) {
            audio = &f;
            audioMp4a = mp4a;
        } else if (mp4a && !audioMp4a) {
            audio = &f;
        }
    }

    if (audio) {
        out.videoUrl = video->url;
        out.audioUrl = audio->url;
        out.ok = true;
        return out;
    }

    // No audio-only track: a muxed progressive format (video + audio codecs in
    // one mimeType) is a complete stream on its own.
    if (video->mimeType.contains(QLatin1String("mp4a"))
        || video->mimeType.contains(QLatin1String("audio"))) {
        out.videoUrl = video->url;
        out.single = true;
        out.ok = true;
    }
    return out;
}

// A signed URL's own expire= epoch wins; the Player response's
// expiresInSeconds is the fallback. 0 = no usable expiry (caller rejects).
inline qint64 expiryEpoch(qint64 urlExpire, qint64 nowEpoch, int expiresInSeconds)
{
    if (urlExpire > nowEpoch)
        return urlExpire;
    if (expiresInSeconds > 0)
        return nowEpoch + expiresInSeconds;
    return 0;
}

} // namespace ytstream
