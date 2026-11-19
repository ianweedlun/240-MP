#pragma once
// Bounded-eviction helpers for the YouTube ID-keyed state. Pure
// container math, split out of YouTubeBackend so the cap/recency policy is
// unit-testable with no network, no QNetworkAccessManager and no backend.
//
// Doctrine: the existing TTLs (14-day success / 10-min failure) decide WHEN a
// window refetches; these caps decide HOW MANY windows may stay resident. The
// two are orthogonal — a fresh entry can be evicted under pressure and a stale
// one can stay resident until touched again. Eviction is capacity-only and
// never schedules a refetch: an evicted id behaves exactly like a cold id.
//
// One recency index (MRU at the front) co-evicts m_sponsorCache and
// m_sponsorFetchedAt together, so the two key sets are structurally identical
// and a timestamp can never outlive its segments. The known-id pool has its
// own independent index; it is sized <= the sponsor pool so idle fill cannot
// churn the sponsor cache once the known pool stops growing.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QHash>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <algorithm>

#include "YtSponsorTtl.h"

namespace ytlru {

// Move `key` to the MRU front; insert it at the front when absent. Called on
// cache HIT as well as insert so hot entries survive eviction pressure.
// O(index size) — the index is capped, so this is bounded work.
inline void touch(QStringList &lru, const QString &key)
{
    lru.removeOne(key);
    lru.prepend(key);
}

// Append `key` at the LRU tail unless already present. Load-path rebuild:
// file order is the recency signal (writes are MRU-first), and legacy files
// with duplicate ids keep their first occurrence deterministically.
inline void appendIfAbsent(QStringList &lru, const QString &key)
{
    if (!key.isEmpty() && !lru.contains(key))
        lru.append(key);
}

// Evict LRU tails until the sponsor index holds at most `cap` keys, removing
// each evicted key from BOTH the segment map and the timestamp map. Returns
// the evicted keys in eviction order (oldest first).
template <typename SegmentVector>
inline QStringList evictSponsorToCap(QStringList &lru,
                                     QHash<QString, SegmentVector> &segments,
                                     QHash<QString, qint64> &fetchedAt,
                                     int cap)
{
    QStringList evicted;
    while (lru.size() > cap) {
        const QString key = lru.takeLast();
        segments.remove(key);
        fetchedAt.remove(key);
        evicted.append(key);
    }
    return evicted;
}

// Known-id mirror of evictSponsorToCap: one index, one set, same tail rule.
inline QStringList evictKnownToCap(QStringList &lru, QSet<QString> &ids, int cap)
{
    QStringList evicted;
    while (lru.size() > cap) {
        const QString key = lru.takeLast();
        ids.remove(key);
        evicted.append(key);
    }
    return evicted;
}

// Count cap for the transient-failure stamp map (defense-in-depth beside its
// 10-minute TTL prune). Drops the OLDEST stamps first — they are closest to
// expiring anyway; the key breaks ties so the prune is deterministic.
// Returns how many stamps went away.
inline int capFailures(QHash<QString, qint64> &failedAt, int cap)
{
    if (failedAt.size() <= cap)
        return 0;
    QVector<QPair<qint64, QString>> rows;
    rows.reserve(failedAt.size());
    for (auto it = failedAt.cbegin(); it != failedAt.end(); ++it)
        rows.append(qMakePair(it.value(), it.key()));
    std::sort(rows.begin(), rows.end(),
              [](const QPair<qint64, QString> &a, const QPair<qint64, QString> &b) {
                  return a.first != b.first ? a.first < b.first : a.second < b.second;
              });
    const int excess = failedAt.size() - cap;
    for (int i = 0; i < excess; ++i)
        failedAt.remove(rows[i].second);
    return excess;
}

// Load-path defense: a timestamp whose segment entry is absent is an orphan
// (a legacy row carried fetchedAt but no usable segments array). Drop it so
// the pair is co-keyed before the recency index is trusted. Returns the count.
template <typename SegmentVector>
inline int dropOrphanStamps(const QHash<QString, SegmentVector> &segments,
                            QHash<QString, qint64> &stamps)
{
    int dropped = 0;
    for (auto it = stamps.begin(); it != stamps.end();) {
        if (!segments.contains(it.key())) {
            it = stamps.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    return dropped;
}

// Deterministic load-time prune of an OVERSIZED (legacy) sponsor cache; a
// bounded file is a no-op, so stale-but-resident entries keep serving playback
// until their refetch lands, exactly as before. When the index exceeds `cap`:
//   1. drop TTL-stale entries first (a stamp of 0 is never stale — pre-stamp
//      entries get the benefit of the doubt, per ytsb::successStale),
//   2. if still over cap, keep the newest stamps. Equal stamps keep the
//      earlier file position; stampless (0) entries sort LAST, because a
//      known-fresh window beats an unknown-age one.
// The index is rebuilt MRU-first in surviving order (newest stamp = the MRU
// proxy; a legacy file has no hit-recency). Original stamps are never
// rewritten here — the caller stamps pre-stamp survivors AFTER pruning, so
// the ordering decision can never be biased by a re-stamp to now.
// Returns how many entries were dropped.
template <typename SegmentVector>
inline int pruneSponsorOnLoad(QStringList &lru,
                              QHash<QString, SegmentVector> &segments,
                              QHash<QString, qint64> &stamps,
                              qint64 nowMs, int cap)
{
    if (lru.size() <= cap)
        return 0;
    int dropped = 0;
    for (auto it = lru.begin(); it != lru.end();) {
        if (ytsb::successStale(stamps.value(*it), nowMs)) {
            segments.remove(*it);
            stamps.remove(*it);
            it = lru.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    if (lru.size() <= cap)
        return dropped;
    QVector<QPair<qint64, int>> rows; // (original stamp, file position)
    rows.reserve(lru.size());
    for (int i = 0; i < lru.size(); ++i)
        rows.append(qMakePair(stamps.value(lru.at(i)), i));
    std::sort(rows.begin(), rows.end(),
              [](const QPair<qint64, int> &a, const QPair<qint64, int> &b) {
                  return a.first != b.first ? a.first > b.first : a.second < b.second;
              });
    QStringList survivors;
    survivors.reserve(cap);
    for (int i = 0; i < cap; ++i)
        survivors.append(lru.at(rows.at(i).second));
    for (int i = cap; i < rows.size(); ++i) {
        const QString key = lru.at(rows.at(i).second);
        segments.remove(key);
        stamps.remove(key);
        ++dropped;
    }
    lru = survivors;
    return dropped;
}

} // namespace ytlru
