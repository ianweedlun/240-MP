#pragma once
// SponsorBlock fetch gate: pure clock math for "should this video's segment
// window be requested again". Split out of YouTubeBackend so the cache/TTL
// decision is unit-testable with no network, no QNetworkAccessManager and no
// backend instance.
//
// Doctrine: a confirmed-empty window (4xx = prefix unknown) is a cache HIT and
// suppresses retries forever; a transient failure (DNS, timeout, 5xx) is
// negative-cached for a short TTL so a dead sponsor.ajay.app costs one request
// per video per window instead of one request per play.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QHash>
#include <QString>
#include <QtGlobal>

namespace ytsb {

// Long enough that a whole list prefetch plus the plays that follow it share a
// single failure; short enough that a real outage does not pin a video's
// segments away for the rest of the session.
inline constexpr qint64 kFailureTtlMs = 10 * 60 * 1000;

// True while a failure stamped at `failedAtMs` is still inside its window.
// A stamp of 0 (absent / default-constructed) never blocks.
inline bool failureActive(qint64 failedAtMs, qint64 nowMs,
                          qint64 ttlMs = kFailureTtlMs)
{
    return failedAtMs > 0 && (nowMs - failedAtMs) < ttlMs;
}

// Drop entries whose window has elapsed, in place. Returns how many went away.
// Called before a decision so a long-running session's map stays bounded.
inline int pruneFailures(QHash<QString, qint64> &failedAt, qint64 nowMs,
                         qint64 ttlMs = kFailureTtlMs)
{
    int pruned = 0;
    for (auto it = failedAt.begin(); it != failedAt.end();) {
        if (!failureActive(it.value(), nowMs, ttlMs)) {
            it = failedAt.erase(it);
            ++pruned;
        } else {
            ++it;
        }
    }
    return pruned;
}

enum class FetchDecision {
    ServeCache,      // window already on file (possibly a confirmed empty)
    AlreadyFetching, // a request for this video is in flight
    Suppressed,      // transient failure inside its TTL — do not retry yet
    Fetch            // nothing known: issue the request
};

// Order matters: a cached window outranks an in-flight request, which outranks
// a recent failure. Only when nothing is known do we hit the network.
inline FetchDecision decideFetch(bool cached, bool inFlight, bool failureBlocks)
{
    if (cached)
        return FetchDecision::ServeCache;
    if (inFlight)
        return FetchDecision::AlreadyFetching;
    if (failureBlocks)
        return FetchDecision::Suppressed;
    return FetchDecision::Fetch;
}

} // namespace ytsb
