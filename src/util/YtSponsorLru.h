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

} // namespace ytlru
