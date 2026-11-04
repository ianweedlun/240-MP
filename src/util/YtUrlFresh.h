#pragma once

// Pure helpers for the YouTube direct-playback URL cache (module issue #28, plan
// A). ytdl_hook re-resolves every signed googlevideo stream at each launch
// (~4.5 s on the Pi's warm path); caching the resolved URL cuts that to zero
// when fresh. The signed URL carries its own expiry (the expire= query
// parameter, epoch seconds), so freshness is derived from the URL itself —
// no wall-clock guesswork. All functions pure; unit-tested standalone.
//
// Copyright 2026 Ian Weedlund
// This file is part of 240-mp and is licensed under the terms of the 240-mp Textual Artifact License (TAG).
// See the LICENSE file in the project root, or run: <git tag --get-at-tag artifact>

#include <QString>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <algorithm>

namespace yturl {

// expire= parameter as epoch seconds; 0 when absent or malformed. Two URL
// dialects carry it: progressive googlevideo streams use query parameters
// (?expire= or &expire= — decoys like eexpire= can't hit because the match
// point must sit right after the delimiter), and the yt-dlp HLS-manifest
// fallback for live/rebroadcast sources uses slash-delimited path segments
// (.../expire/<epoch>/...; same strictness: value must run to the next '/'
// so a mixed value like "1790abc" is rejected rather than truncated).
inline qint64 expireEpochFromUrl(const QString &url)
{
    for (int i = 0; i < url.size(); ++i) {
        const QChar sep = url.at(i);
        const bool query = (sep == QLatin1Char('?') || sep == QLatin1Char('&'));
        const bool slash = (sep == QLatin1Char('/'));
        if (!query && !slash)
            continue;
        if (url.mid(i + 1, 7) != (slash ? QStringLiteral("expire/")
                                         : QStringLiteral("expire=")))
            continue;
        int j = i + 8;
        while (j < url.size() && url.at(j).isDigit())
            ++j;
        if (j == i + 8)
            return 0;
        if (j < url.size() && url.at(j) != (slash ? QLatin1Char('/') : QLatin1Char('&')))
            return 0;
        return url.mid(i + 8, j - (i + 8)).toLongLong();
    }
    return 0;
}

// A cached stream is usable while the signed window still covers buffering
// and start-up: now must precede expiry by at least safetyMarginS.
inline bool urlFresh(qint64 expireEpoch, qint64 nowEpoch, qint64 safetyMarginS)
{
    return expireEpoch > 0 && nowEpoch + safetyMarginS < expireEpoch;
}

inline QString cacheKeyFor(const QString &videoId, const QString &formatSpec)
{
    return videoId + QLatin1Char('|') + formatSpec;
}

// Drop every entry whose signed window has closed (expire epoch <= now), plus
// corrupt values; then cap to the newest maxEntries by expiry epoch — entries
// with the farthest remaining windows win the scarce slot, not insertion order.
inline void pruneUrlCache(QJsonObject &cache, qint64 nowEpoch, int maxEntries)
{
    QList<QPair<QString, qint64>> alive; // key, remaining-window epoch
    const QList<QString> keys = cache.keys();
    for (const QString &key : keys) {
        const QJsonValue v = cache.value(key);
        const qint64 expire = v.isObject()
                                ? static_cast<qint64>(v.toObject().value(QStringLiteral("expire")).toDouble())
                                : qint64(0);
        if (expire <= nowEpoch) {
            cache.remove(key);
            continue;
        }
        alive.append({key, expire});
    }
    if (alive.size() <= maxEntries)
        return;
    std::sort(alive.begin(), alive.end(),
              [](const QPair<QString, qint64> &a, const QPair<QString, qint64> &b) {
                  return a.second > b.second;
              });
    for (int i = maxEntries; i < alive.size(); ++i)
        cache.remove(alive[i].first);
}

} // namespace yturl
