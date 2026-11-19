#pragma once

// Durable negative cache for YouTube resolve failures whose own response says
// retrying cannot help (age/login gate or unavailable/claimed content). Keep
// transient failures out: a timeout, network error or 5xx must still retry on
// the next request. Entries live beside youtube_url_cache.json, keyed only by
// video id so every resolution shares the negative result.
//
// Copyright 2026 Ian Weedlund
// This file is part of 240-mp and is licensed under the terms of the 240-mp Textual Artifact License (TAG).
// See the LICENSE file in the project root, or run: <git tag --get-at-tag artifact>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

namespace ytneg {

// Permanent failures may be retried after one day. This is deliberately a
// named constant: tests pin the exact TTL boundary and operators can see how
// long a cached LOGIN_REQUIRED / UNPLAYABLE result suppresses resolver work.
inline constexpr qint64 kNegativeCacheTtlS = 24 * 60 * 60;

// Return the canonical permanent class evidenced by a resolver reason, or an
// empty string for every transient/other class. Do not broaden this by
// guessing: no-playable-pair, no-expiry and subprocess empty output are not
// evidence that the video itself is permanently unavailable.
inline QString permanentFailureClass(const QString &reason)
{
    if (reason.contains(QStringLiteral("LOGIN_REQUIRED")))
        return QStringLiteral("LOGIN_REQUIRED");
    if (reason.contains(QStringLiteral("UNPLAYABLE")))
        return QStringLiteral("UNPLAYABLE");
    return QString();
}

// Record only permanent failures. Returns the stored class, or empty when the
// reason was transient and no cache mutation was made.
inline QString recordFailure(QJsonObject &cache, const QString &videoId,
                             const QString &reason, qint64 nowEpoch)
{
    const QString failureClass = permanentFailureClass(reason);
    if (videoId.isEmpty() || failureClass.isEmpty())
        return QString();

    QJsonObject entry;
    entry.insert(QStringLiteral("failedAt"), nowEpoch);
    entry.insert(QStringLiteral("reason"), failureClass);
    cache.insert(videoId, entry);
    return failureClass;
}

// Canonical class while the entry is inside its TTL; empty means the caller
// must make a fresh resolve attempt. Expiry is strict at the 24 h boundary.
inline QString freshFailureClass(const QJsonObject &cache, const QString &videoId,
                                 qint64 nowEpoch)
{
    const QJsonObject entry = cache.value(videoId).toObject();
    const qint64 failedAt = static_cast<qint64>(
        entry.value(QStringLiteral("failedAt")).toDouble());
    const QString failureClass = entry.value(QStringLiteral("reason")).toString();
    if (failedAt <= 0 || permanentFailureClass(failureClass) != failureClass
        || nowEpoch >= failedAt + kNegativeCacheTtlS)
        return QString();
    return failureClass;
}

inline void pruneNegativeCache(QJsonObject &cache, qint64 nowEpoch)
{
    const QList<QString> keys = cache.keys();
    for (const QString &key : keys)
        if (freshFailureClass(cache, key, nowEpoch).isEmpty())
            cache.remove(key);
}

inline QJsonObject readNegativeCache(const QString &path, qint64 nowEpoch)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QJsonObject cache = QJsonDocument::fromJson(file.readAll()).object();
    pruneNegativeCache(cache, nowEpoch);
    return cache;
}

inline bool writeNegativeCache(const QString &path, const QJsonObject &cache)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(QJsonDocument(cache).toJson(QJsonDocument::Compact)) < 0)
        return false;
    return file.commit();
}

} // namespace ytneg
