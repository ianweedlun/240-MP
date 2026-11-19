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
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace ytsb {

// Long enough that a whole list prefetch plus the plays that follow it share a
// single failure; short enough that a real outage does not pin a video's
// segments away for the rest of the session.
inline constexpr qint64 kFailureTtlMs = 10 * 60 * 1000;

// A confirmed window is durable, but not eternal: new segments get submitted
// to SponsorBlock after a video has been cached. Without a success TTL an
// idle-filled cache would keep skipping to boundaries submitted months ago.
// Two weeks is long enough that a normal viewing habit never refetches, short
// enough that a durable cache self-heals.
inline constexpr qint64 kSuccessTtlMs = 14LL * 24 * 60 * 60 * 1000;

// True when a window fetched at `fetchedAtMs` should be refreshed. A stamp of
// 0 (entry predates the stamp, or was never fetched) is NOT stale: stamping
// happens at load, so an upgraded cache ages out gradually instead of
// triggering a refetch storm on first boot.
inline bool successStale(qint64 fetchedAtMs, qint64 nowMs,
                         qint64 ttlMs = kSuccessTtlMs)
{
    return fetchedAtMs > 0 && (nowMs - fetchedAtMs) >= ttlMs;
}

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

// Idle-fill selection: which candidate ids still need a window confirmed.
// Pure so the ordering, dedupe, cap and skip rules are unit-testable without a
// backend or a network. Input order is preserved (callers pass newest-first).
inline QStringList idleSelect(const QStringList &candidates,
                              const QHash<QString, qint64> &fetchedAt,
                              const QSet<QString> &cached,
                              const QSet<QString> &fetching,
                              const QHash<QString, qint64> &failedAt,
                              qint64 nowMs, int cap,
                              qint64 successTtlMs = kSuccessTtlMs,
                              qint64 failureTtlMs = kFailureTtlMs)
{
    QStringList out;
    QSet<QString> seen;
    for (const QString &id : candidates) {
        if (id.isEmpty() || seen.contains(id))
            continue;
        seen.insert(id);
        if (fetching.contains(id))
            continue;
        if (cached.contains(id) && !successStale(fetchedAt.value(id), nowMs, successTtlMs))
            continue;
        if (failureActive(failedAt.value(id), nowMs, failureTtlMs))
            continue;
        out.append(id);
        if (out.size() >= cap)
            break;
    }
    return out;
}

} // namespace ytsb
